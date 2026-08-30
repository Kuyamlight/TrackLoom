#include "AppProjectReplacementActions.h"
#include "AppLoopActions.h"
#include "AppMainMenu.h"
#include "AppPlaybackActions.h"
#include "AppRecentProjects.h"
#include "Command.h"
#include "ProjectSerializer.h"

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <cstdlib>
#endif

#include <filesystem>
#include <iostream>
#include <latch>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

void configureTestFailureOutput()
{
#if defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_error_mode(_OUT_TO_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
}

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << message << '\n';
        throw std::runtime_error(message);
    }
}

std::filesystem::path testWorkspace()
{
    return std::filesystem::current_path()
        / "build" / "test-output" / "app-project-replacement";
}

void resetTestWorkspace()
{
    std::error_code error;
    std::filesystem::remove_all(testWorkspace(), error);
    std::filesystem::create_directories(testWorkspace(), error);
    require(!error, "replacement test workspace must be writable");
}

class FakeRealtimePlaybackHost final : public trackloom::RealtimePlaybackHost {
public:
    FakeRealtimePlaybackHost()
    {
        snapshot_.format = {
            1, "fake-device", "Fake Speakers", 48000.0, 256, 2, 3, true
        };
        snapshot_.realtime.state = trackloom::RealtimePlaybackState::Stopped;
    }

    trackloom::AudioDeviceFormatSnapshot deviceFormatSnapshot() const override
    {
        std::scoped_lock lock(mutex_);
        return snapshot_.format;
    }

    trackloom::RealtimePlaybackHostResult installAndStart(
        std::unique_ptr<const trackloom::PreparedMidiPlaybackPlan> plan) override
    {
        std::scoped_lock lock(mutex_);
        ++installCallCount;
        installedStartSamples.push_back(plan->playbackStartSample);
        installedPlan_ = std::move(plan);
        snapshot_.realtime.state = trackloom::RealtimePlaybackState::Playing;
        snapshot_.callbackRunning = true;
        snapshot_.planInstalled = true;
        return { true, trackloom::RealtimePlaybackHostFailureReason::None, "started" };
    }

    bool requestStop() noexcept override
    {
        std::scoped_lock lock(mutex_);
        snapshot_.realtime.state = trackloom::RealtimePlaybackState::Stopping;
        return true;
    }

    void serviceNonRealtime() override
    {
    }

    void hardStopAndReset() noexcept override
    {
        std::scoped_lock lock(mutex_);
        ++hardResetCallCount;
        if (clearCallbackOnReset) {
            snapshot_.callbackRunning = false;
        }
        if (clearPlanOnReset) {
            snapshot_.planInstalled = false;
            installedPlan_.reset();
        }
        if (snapshot_.realtime.state != trackloom::RealtimePlaybackState::Faulted) {
            snapshot_.realtime.state = trackloom::RealtimePlaybackState::Stopped;
        }
    }

    trackloom::RealtimePlaybackHostSnapshot snapshot() const override
    {
        std::scoped_lock lock(mutex_);
        if (snapshotsUntilThrow_ == 0) {
            snapshotsUntilThrow_ = -1;
            throw std::runtime_error("injected host snapshot failure");
        }
        if (snapshotsUntilThrow_ > 0) {
            --snapshotsUntilThrow_;
        }
        return snapshot_;
    }

    void throwAfterSuccessfulSnapshots(int count)
    {
        std::scoped_lock lock(mutex_);
        snapshotsUntilThrow_ = count;
    }

    void setDeviceAvailable(bool available)
    {
        std::scoped_lock lock(mutex_);
        snapshot_.format.available = available;
    }

    void setRealtimeState(trackloom::RealtimePlaybackState state)
    {
        std::scoped_lock lock(mutex_);
        snapshot_.realtime.state = state;
    }

    void completeStop()
    {
        std::scoped_lock lock(mutex_);
        snapshot_.realtime.state = trackloom::RealtimePlaybackState::Stopped;
        snapshot_.callbackRunning = false;
    }

    void setRealtimePosition(std::int64_t sample)
    {
        std::scoped_lock lock(mutex_);
        snapshot_.realtime.projectSamplePosition = sample;
        snapshot_.realtime.renderedSampleCount = static_cast<std::uint64_t>(sample);
    }

    void setQuiescenceEvidence(bool callbackRunning, bool planInstalled)
    {
        std::scoped_lock lock(mutex_);
        snapshot_.callbackRunning = callbackRunning;
        snapshot_.planInstalled = planInstalled;
    }

    bool clearCallbackOnReset = true;
    bool clearPlanOnReset = true;
    int installCallCount = 0;
    int hardResetCallCount = 0;
    std::vector<std::int64_t> installedStartSamples;

private:
    mutable std::mutex mutex_;
    mutable int snapshotsUntilThrow_ = -1;
    trackloom::RealtimePlaybackHostSnapshot snapshot_;
    std::unique_ptr<const trackloom::PreparedMidiPlaybackPlan> installedPlan_;
};

trackloom::PreparedMidiPlaybackPlanBuildResult makePreparedPlan(
    trackloom::PreparedMidiPlaybackPlanBuildRequest request,
    std::stop_token = {})
{
    auto plan = std::make_unique<trackloom::PreparedMidiPlaybackPlan>();
    plan->sampleRate = request.sampleRate;
    plan->maximumBlockFrames = request.maximumBlockFrames;
    plan->outputChannelCount = request.outputChannelCount;
    plan->outputChannelMask = request.outputChannelMask;
    plan->playbackStartSample = request.playbackStartSample;
    return { trackloom::PreparedMidiPlaybackPlanBuildFailureReason::None,
        std::move(plan) };
}

trackloom::LoadProjectResult throwDuringProjectLoad(const std::filesystem::path&)
{
    throw std::runtime_error("injected project load failure");
}

void startPlaying(
    trackloom::AppPlaybackController& playback,
    trackloom::AppProjectSession& session,
    trackloom::AppLoopPlaybackState& loopState)
{
    std::latch completionPublished(1);
    trackloom::detail::setAppPlaybackPreparationPublishedCallbackForTesting(
        playback, [&] { completionPublished.count_down(); });
    require(trackloom::startAppPlayback(playback, session, loopState).success,
        "playing fixture must start preparation");
    completionPublished.wait();
    playback.poll(session, loopState);
    require(playback.status().state == trackloom::AppPlaybackState::Playing,
        "playing fixture must deterministically install its plan");
}

struct SessionObservation {
    std::string canonicalProject;
    std::optional<std::filesystem::path> path;
    bool dirty = false;
    bool canUndo = false;
    bool canRedo = false;
    std::uint64_t generation = 0;
    bool loopEnabled = false;
    trackloom::AppPlaybackState playbackState = trackloom::AppPlaybackState::Stopped;
    trackloom::AppPlaybackFailureReason playbackFailure =
        trackloom::AppPlaybackFailureReason::None;
    bool deviceAvailable = false;
    bool canStart = false;
    std::int64_t projectSamplePosition = 0;
    std::uint64_t renderedSampleCount = 0;
    double projectSeconds = 0.0;
    trackloom::AppPlaybackLoopIntentStatus loopIntentStatus =
        trackloom::AppPlaybackLoopIntentStatus::None;
    std::string loopIntentMessage;
    std::string stateLabel;
    std::string summary;

    bool operator==(const SessionObservation&) const = default;
};

SessionObservation observe(
    const trackloom::AppProjectSession& session,
    const trackloom::AppPlaybackController& playback,
    const trackloom::AppLoopPlaybackState& loopState)
{
    const auto& playbackStatus = playback.status();
    return {
        trackloom::saveProjectToText(session.project()),
        session.currentProjectPath(),
        session.isDirty(),
        session.canUndoProjectEdit(),
        session.canRedoProjectEdit(),
        session.projectEditGeneration(),
        loopState.enabled(),
        playbackStatus.state,
        playbackStatus.failureReason,
        playbackStatus.deviceAvailable,
        playbackStatus.canStart,
        playbackStatus.projectSamplePosition,
        playbackStatus.renderedSampleCount,
        playbackStatus.projectSeconds,
        playbackStatus.loopIntentStatus,
        playbackStatus.loopIntentMessage,
        playbackStatus.stateLabel,
        playbackStatus.summary
    };
}

struct SelectionFixture {
    std::string trackId = "selected-track";
    std::string audioTrackId = "selected-audio-track";
    std::string audioClipId = "selected-audio-clip";
    std::string midiClipId = "selected-midi-clip";

    trackloom::AppProjectObjectSelection selection()
    {
        return { trackId, audioTrackId, audioClipId, midiClipId };
    }

    bool unchanged() const
    {
        return trackId == "selected-track"
            && audioTrackId == "selected-audio-track"
            && audioClipId == "selected-audio-clip"
            && midiClipId == "selected-midi-clip";
    }

    bool empty() const
    {
        return trackId.empty() && audioTrackId.empty()
            && audioClipId.empty() && midiClipId.empty();
    }
};

void requirePreserved(
    const SessionObservation& before,
    const trackloom::AppProjectSession& session,
    const trackloom::AppPlaybackController& playback,
    const trackloom::AppLoopPlaybackState& loopState,
    const std::string& scenario)
{
    require(observe(session, playback, loopState) == before,
        scenario + " must preserve project, path, dirty, history, generation, loop, and controller status");
}

void preparePreservedSession(
    trackloom::AppProjectSession& session,
    trackloom::AppLoopPlaybackState& loopState,
    const std::filesystem::path& path)
{
    session.createNewProject("Preserved Project");
    require(session.saveAs(path).success, "preserved fixture must establish a project path");
    require(session.executeProjectCommand(std::make_unique<trackloom::AddTrackCommand>(
                "Undoable Track", trackloom::TrackType::Instrument)).success,
        "preserved fixture must establish undo history");
    require(session.executeProjectCommand(
                std::make_unique<trackloom::SetProjectPlaybackLoopCommand>(
                    trackloom::PlaybackLoopRange { 960, 3840 })).success,
        "preserved fixture must establish a project loop");
    require(loopState.setEnabled(session.project(), true),
        "preserved fixture must enable its session loop");
    require(session.save().success,
        "preserved fixture must be clean while retaining its command history");
}

void preparingWorkerRejectsReplacementBeforeTouchingTheSession()
{
    resetTestWorkspace();
    FakeRealtimePlaybackHost host;
    std::latch entered(1);
    std::latch release(1);
    trackloom::AppPlaybackController playback(
        host,
        [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
            entered.count_down();
            release.wait();
            return makePreparedPlan(request);
        });
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;
    SelectionFixture selection;
    preparePreservedSession(session, loopState, testWorkspace() / "preparing.trackloom");
    std::latch completionPublished(1);
    trackloom::detail::setAppPlaybackPreparationPublishedCallbackForTesting(
        playback, [&] { completionPublished.count_down(); });
    require(trackloom::startAppPlayback(playback, session, loopState).success,
        "preparing rejection fixture must start its worker");
    entered.wait();
    const auto before = observe(session, playback, loopState);

    const auto result = trackloom::createNewAppProjectIfSafe(
        session, playback, loopState, selection.selection(), "Rejected");
    release.count_down();
    completionPublished.wait();

    require(!result.success
            && result.failureReason
                == trackloom::AppProjectReplacementFailureReason::PreparationWorkerActive,
        "an unfinished preparation worker must win replacement classification");
    require(host.hardResetCallCount == 0,
        "worker rejection must happen before host reset");
    requirePreserved(before, session, playback, loopState, "preparing rejection");
    require(selection.unchanged(), "preparing rejection must preserve Main selection");
    playback.poll(session, loopState);
    require(playback.status().state == trackloom::AppPlaybackState::Playing,
        "preparing rejection must preserve active key and published completion mailbox");
}

void playingAndStoppingRejectReplacementWithoutMutation()
{
    resetTestWorkspace();
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host, makePreparedPlan);
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;
    SelectionFixture selection;
    preparePreservedSession(session, loopState, testWorkspace() / "active.trackloom");
    startPlaying(playback, session, loopState);
    const auto playingBefore = observe(session, playback, loopState);

    const auto playingResult = trackloom::createNewAppProjectIfSafe(
        session, playback, loopState, selection.selection(), "Rejected Playing");
    require(!playingResult.success
            && playingResult.failureReason
                == trackloom::AppProjectReplacementFailureReason::PlaybackActive,
        "Playing must reject replacement as active playback");
    requirePreserved(playingBefore, session, playback, loopState, "playing rejection");
    require(selection.unchanged(), "playing rejection must preserve Main selection");

    require(trackloom::stopAppPlayback(playback).success,
        "stopping rejection fixture must accept stop");
    const auto stoppingBefore = observe(session, playback, loopState);
    const auto stoppingResult = trackloom::createNewAppProjectIfSafe(
        session, playback, loopState, selection.selection(), "Rejected Stopping");
    require(!stoppingResult.success
            && stoppingResult.failureReason
                == trackloom::AppProjectReplacementFailureReason::PlaybackActive,
        "Stopping must reject replacement as active playback");
    requirePreserved(stoppingBefore, session, playback, loopState, "stopping rejection");
    require(selection.unchanged(), "stopping rejection must preserve Main selection");
}

void stoppedControllerRejectsAStillRunningCallbackWithoutMutation()
{
    resetTestWorkspace();
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host, makePreparedPlan);
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;
    SelectionFixture selection;
    preparePreservedSession(session, loopState, testWorkspace() / "callback.trackloom");
    host.setQuiescenceEvidence(true, true);
    const auto before = observe(session, playback, loopState);

    const auto result = trackloom::createNewAppProjectIfSafe(
        session, playback, loopState, selection.selection(), "Rejected Callback");

    require(!result.success
            && result.failureReason
                == trackloom::AppProjectReplacementFailureReason::HostNotQuiescent,
        "Stopped controller with a running callback must reject replacement");
    require(host.hardResetCallCount == 0,
        "a running callback must be rejected rather than synchronously reset from Stopped");
    requirePreserved(before, session, playback, loopState, "running callback rejection");
    require(selection.unchanged(), "callback rejection must preserve Main selection");
}

void dirtyReadOnlyCheckPrecedesHostQuiescence()
{
    resetTestWorkspace();
    FakeRealtimePlaybackHost host;
    host.setQuiescenceEvidence(false, true);
    trackloom::AppPlaybackController playback(host, makePreparedPlan);
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;
    SelectionFixture selection;
    preparePreservedSession(session, loopState, testWorkspace() / "dirty.trackloom");
    session.editProject().rename("Dirty Preserved Project");
    const auto before = observe(session, playback, loopState);

    const auto result = trackloom::createNewAppProjectIfSafe(
        session, playback, loopState, selection.selection(), "Rejected Dirty");

    require(!result.success
            && result.failureReason
                == trackloom::AppProjectReplacementFailureReason::DirtyProject,
        "dirty project must be classified before playback and host gates");
    require(host.hardResetCallCount == 0,
        "dirty read-only rejection must not release the host plan");
    requirePreserved(before, session, playback, loopState, "dirty rejection");
    require(selection.unchanged(), "dirty rejection must preserve Main selection");
}

void stoppedAndUnavailableQuiescentControllersAllowReplacement()
{
    {
        FakeRealtimePlaybackHost host;
        trackloom::AppPlaybackController playback(host, makePreparedPlan);
        trackloom::AppProjectSession session;
        trackloom::AppLoopPlaybackState loopState;
        SelectionFixture selection;

        const auto result = trackloom::createNewAppProjectIfSafe(
            session, playback, loopState, selection.selection(), "Stopped Replacement");

        require(result.success && session.project().name() == "Stopped Replacement",
            "quiescent Stopped controller must allow a new project");
        require(playback.status().state == trackloom::AppPlaybackState::Stopped,
            "successful replacement must reset controller presentation to Stopped");
        require(selection.empty(),
            "successful replacement must clear every Main selection field in the commit");
    }

    FakeRealtimePlaybackHost unavailableHost;
    unavailableHost.setDeviceAvailable(false);
    trackloom::AppPlaybackController unavailable(unavailableHost, makePreparedPlan);
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;
    const auto sourcePath = testWorkspace() / "v11-loop.trackloom";
    trackloom::AppProjectSession source;
    source.createNewProject("Loaded V11 Loop");
    require(source.editProject().setPlaybackLoopRange(
                trackloom::PlaybackLoopRange { 1920, 7680 }),
        "loaded v11 fixture must accept its persisted loop");
    require(source.saveAs(sourcePath).success,
        "loaded v11 fixture must save");

    const auto opened = trackloom::openAppProjectIfSafe(
        session, unavailable, loopState, sourcePath);

    require(opened.success, "Unavailable without callback or worker must allow open");
    require(session.project().playbackLoopRange()
            == trackloom::PlaybackLoopRange { 1920, 7680 },
        "successful v11 open must retain the persisted project loop range");
    require(!loopState.enabled(),
        "successful project replacement must disable session loop playback");
    require(unavailable.status().state == trackloom::AppPlaybackState::Unavailable,
        "successful replacement without a device must remain usable and Unavailable");
}

void faultedControllerHardResetsBeforeSuccessfulReplacement()
{
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host, makePreparedPlan);
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;
    host.setRealtimeState(trackloom::RealtimePlaybackState::Faulted);
    host.setQuiescenceEvidence(true, true);
    playback.poll(session, loopState);
    require(playback.status().state == trackloom::AppPlaybackState::Faulted,
        "fault reset fixture must publish controller Faulted");

    const auto result = trackloom::createNewAppProjectIfSafe(
        session, playback, loopState, "Recovered Project");

    require(result.success && host.hardResetCallCount == 1,
        "Faulted replacement must hard reset the host before replacing");
    const auto hostSnapshot = host.snapshot();
    require(!hostSnapshot.callbackRunning && !hostSnapshot.planInstalled,
        "successful fault recovery must prove callback and plan quiescence");
    require(playback.status().state == trackloom::AppPlaybackState::Faulted
            && playback.status().loopIntentStatus
                == trackloom::AppPlaybackLoopIntentStatus::None,
        "successful faulted replacement must clear old intent while retaining host diagnostics");
}

void failedHardResetRejectsReplacementWithoutMutation()
{
    resetTestWorkspace();
    struct ResetCase {
        bool clearCallback;
        bool clearPlan;
        const char* name;
    };
    const ResetCase cases[] {
        { false, true, "callback residual" },
        { true, false, "plan residual" }
    };

    for (const auto& resetCase : cases) {
        FakeRealtimePlaybackHost host;
        host.clearCallbackOnReset = resetCase.clearCallback;
        host.clearPlanOnReset = resetCase.clearPlan;
        trackloom::AppPlaybackController playback(host, makePreparedPlan);
        trackloom::AppProjectSession session;
        trackloom::AppLoopPlaybackState loopState;
        SelectionFixture selection;
        preparePreservedSession(
            session,
            loopState,
            testWorkspace() / (std::string(resetCase.name) + ".trackloom"));
        host.setRealtimeState(trackloom::RealtimePlaybackState::Faulted);
        host.setQuiescenceEvidence(true, true);
        playback.poll(session, loopState);
        const auto before = observe(session, playback, loopState);

        const auto result = trackloom::createNewAppProjectIfSafe(
            session, playback, loopState, selection.selection(), "Rejected Reset");

        require(!result.success
                && result.failureReason
                    == trackloom::AppProjectReplacementFailureReason::HostResetFailed,
            std::string("hard reset must reject a remaining ") + resetCase.name);
        require(host.hardResetCallCount == 1,
            "faulted replacement must attempt exactly one hard reset");
        requirePreserved(
            before, session, playback, loopState,
            std::string("hard reset ") + resetCase.name + " failure");
        require(selection.unchanged(),
            std::string("hard reset ") + resetCase.name
                + " failure must preserve Main selection");
    }
}

void faultedControllerWithUnfinishedWorkerRejectsBeforeHardReset()
{
    resetTestWorkspace();
    FakeRealtimePlaybackHost host;
    std::latch entered(1);
    std::latch release(1);
    trackloom::AppPlaybackController playback(
        host,
        [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
            entered.count_down();
            release.wait();
            return makePreparedPlan(request);
        });
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;
    SelectionFixture selection;
    preparePreservedSession(session, loopState, testWorkspace() / "fault-worker.trackloom");
    require(trackloom::startAppPlayback(playback, session, loopState).success,
        "faulted worker fixture must start preparation");
    entered.wait();
    host.setRealtimeState(trackloom::RealtimePlaybackState::Faulted);
    host.setQuiescenceEvidence(true, true);
    playback.poll(session, loopState);
    const auto before = observe(session, playback, loopState);

    const auto result = trackloom::createNewAppProjectIfSafe(
        session, playback, loopState, selection.selection(), "Rejected Fault Worker");
    release.count_down();

    require(!result.success
            && result.failureReason
                == trackloom::AppProjectReplacementFailureReason::PreparationWorkerActive,
        "Faulted with an unfinished worker must reject before hard reset");
    require(host.hardResetCallCount == 0,
        "worker-first rejection must not touch the faulted host");
    requirePreserved(before, session, playback, loopState, "faulted worker rejection");
    require(selection.unchanged(), "faulted worker rejection must preserve Main selection");
}

void asynchronousOpenPerformsASecondGateAtLoadTime()
{
    resetTestWorkspace();
    const auto targetPath = testWorkspace() / "async-target.trackloom";
    trackloom::AppProjectSession target;
    target.createNewProject("Async Target");
    require(target.saveAs(targetPath).success, "async target fixture must save");

    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host, makePreparedPlan);
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;
    SelectionFixture selection;
    preparePreservedSession(session, loopState, testWorkspace() / "async-current.trackloom");
    require(session.save().success, "async current fixture must become clean before chooser gate");
    const auto firstGate = trackloom::prepareAppProjectReplacement(playback);
    require(firstGate.safe, "chooser launch gate must allow the initially stopped host");

    startPlaying(playback, session, loopState);
    const auto before = observe(session, playback, loopState);
    const auto secondGate = trackloom::openAppProjectIfSafe(
        session, playback, loopState, selection.selection(), targetPath);

    require(!secondGate.success
            && secondGate.failureReason
                == trackloom::AppProjectReplacementFailureReason::PlaybackActive,
        "load-time gate must reject playback started while the chooser was open");
    requirePreserved(before, session, playback, loopState, "asynchronous second-gate rejection");
    require(selection.unchanged(), "async second-gate rejection must preserve Main selection");
}

void openFailurePreservesSessionAndLoopAfterReleasingAResidualPlan()
{
    resetTestWorkspace();
    FakeRealtimePlaybackHost host;
    host.setQuiescenceEvidence(false, true);
    trackloom::AppPlaybackController playback(host, makePreparedPlan);
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;
    SelectionFixture selection;
    preparePreservedSession(session, loopState, testWorkspace() / "load-failure.trackloom");
    require(session.save().success, "load failure fixture must be clean before open");
    const auto before = observe(session, playback, loopState);

    const auto result = trackloom::openAppProjectIfSafe(
        session,
        playback,
        loopState,
        selection.selection(),
        testWorkspace() / "missing.trackloom");

    require(!result.success
            && result.failureReason
                == trackloom::AppProjectReplacementFailureReason::OpenFailed,
        "missing project must report OpenFailed after host quiescence");
    require(host.hardResetCallCount == 1,
        "Stopped host with only a residual plan must hard reset before load");
    requirePreserved(before, session, playback, loopState, "open failure");
    require(selection.unchanged(), "open failure must preserve Main selection");
    const auto canonicalBeforeHistoryProbe = trackloom::saveProjectToText(session.project());
    require(session.undoProjectEdit(),
        "failed open must retain a real undo command, not only canUndo metadata");
    require(trackloom::saveProjectToText(session.project()) != canonicalBeforeHistoryProbe,
        "failed open undo must change the project using the retained command payload");
    require(session.redoProjectEdit(),
        "failed open must retain a real redo command payload");
    require(trackloom::saveProjectToText(session.project()) == canonicalBeforeHistoryProbe,
        "failed open redo must restore the exact pre-failure project");
}

void playingFailurePreservesInstalledLoopIntentAndSelection()
{
    resetTestWorkspace();
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host, makePreparedPlan);
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;
    SelectionFixture selection;
    require(session.editProject().setPlaybackLoopRange(
                trackloom::PlaybackLoopRange { 960, 3840 }),
        "installed intent fixture must establish its first loop");
    require(loopState.setEnabled(session.project(), true),
        "installed intent fixture must enable loop playback");
    startPlaying(playback, session, loopState);

    const auto rejected = trackloom::createNewAppProjectIfSafe(
        session, playback, loopState, selection.selection(), "Rejected Playing Intent");
    require(!rejected.success, "playing intent fixture must be rejected");
    require(selection.unchanged(), "playing rejection must preserve Main selection");

    require(session.editProject().setPlaybackLoopRange(
                trackloom::PlaybackLoopRange { 1920, 5760 }),
        "installed intent fixture must accept a changed next-play loop");
    playback.poll(session, loopState);
    require(playback.status().loopIntentStatus
            == trackloom::AppPlaybackLoopIntentStatus::PendingNextPlayback,
        "playing rejection must preserve installed preparation key and high-level intent");
}

void stoppingFailurePreservesRewindFlagsAndSelection()
{
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host, makePreparedPlan);
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;
    SelectionFixture selection;
    startPlaying(playback, session, loopState);
    require(playback.rewindToStart().success,
        "rewind preservation fixture must enter Stopping");

    const auto rejected = trackloom::createNewAppProjectIfSafe(
        session, playback, loopState, selection.selection(), "Rejected Rewind");
    require(!rejected.success, "Stopping rewind replacement must be rejected");
    require(selection.unchanged(), "Stopping rejection must preserve Main selection");

    std::latch replayPublished(1);
    trackloom::detail::setAppPlaybackPreparationPublishedCallbackForTesting(
        playback, [&] { replayPublished.count_down(); });
    host.completeStop();
    playback.poll(session, loopState);
    replayPublished.wait();
    playback.poll(session, loopState);
    require(playback.status().state == trackloom::AppPlaybackState::Playing
            && host.installCallCount == 2
            && host.installedStartSamples.back() == 0,
        "Stopping rejection must preserve rewind flags and execute the queued replay");
}

void successfulReplacementClearsNextStartAndUsesTheNewCompletionState()
{
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host, makePreparedPlan);
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;
    SelectionFixture selection;
    host.setRealtimePosition(4096);
    playback.poll(session, loopState);
    require(playback.rewindToStart().success,
        "success reset fixture must establish a zero next-start intent");

    const auto replaced = trackloom::createNewAppProjectIfSafe(
        session, playback, loopState, selection.selection(), "Replacement Clears Intent");
    require(replaced.success && selection.empty(),
        "successful replacement must atomically clear project selection");

    std::latch newCompletionPublished(1);
    trackloom::detail::setAppPlaybackPreparationPublishedCallbackForTesting(
        playback, [&] { newCompletionPublished.count_down(); });
    require(trackloom::startAppPlayback(playback, session, loopState).success,
        "replacement must accept preparation through its fresh completion state");
    newCompletionPublished.wait();
    playback.poll(session, loopState);
    require(playback.status().state == trackloom::AppPlaybackState::Playing
            && host.installedStartSamples.back() == 4096,
        "successful replacement must clear the old zero next-start intent");
}

void successfulReplacementClearsAQueuedRewindBeforeLaterPlaybackStops()
{
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host, makePreparedPlan);
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;
    SelectionFixture selection;
    startPlaying(playback, session, loopState);
    require(playback.rewindToStart().success,
        "successful rewind reset fixture must queue a replay");

    host.setRealtimeState(trackloom::RealtimePlaybackState::Faulted);
    playback.poll(session, loopState);
    require(playback.status().state == trackloom::AppPlaybackState::Faulted,
        "successful rewind reset fixture must enter the replaceable Faulted state");

    const auto replaced = trackloom::createNewAppProjectIfSafe(
        session, playback, loopState, selection.selection(), "Replacement Clears Rewind");
    require(replaced.success && selection.empty(),
        "Faulted replacement must succeed and clear Main selection");

    host.setRealtimeState(trackloom::RealtimePlaybackState::Stopped);
    playback.poll(session, loopState);
    require(playback.status().state == trackloom::AppPlaybackState::Stopped,
        "the reset controller must recover to Stopped when the host recovers");
    startPlaying(playback, session, loopState);
    require(trackloom::stopAppPlayback(playback).success,
        "post-replacement playback must accept a normal stop");
    host.completeStop();
    playback.poll(session, loopState);
    require(playback.status().state == trackloom::AppPlaybackState::Stopped
            && host.installCallCount == 2,
        "successful replacement must clear stale rewind flags instead of replaying later");
}

void controllerResetStagingExceptionPreservesTheEntireLiveReplacementState()
{
    resetTestWorkspace();
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host, makePreparedPlan);
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;
    preparePreservedSession(
        session, loopState, testWorkspace() / "reset-staging-exception.trackloom");
    const auto before = observe(session, playback, loopState);
    std::string selectedTrackId = "track-selection";
    std::string selectedAudioTrackId = "audio-track-selection";
    std::string selectedAudioClipId = "audio-clip-selection";
    std::string selectedMidiClipId = "midi-clip-selection";
    trackloom::AppProjectObjectSelection selection {
        selectedTrackId,
        selectedAudioTrackId,
        selectedAudioClipId,
        selectedMidiClipId
    };

    // prepareForProjectReplacement consumes the first snapshot. The next snapshot
    // is the controller success-reset staging read and must occur before commit.
    host.throwAfterSuccessfulSnapshots(1);
    const auto result = trackloom::createNewAppProjectIfSafe(
        session, playback, loopState, selection, "Must Not Commit");

    require(!result.success
            && result.failureReason
                == trackloom::AppProjectReplacementFailureReason::OpenFailed,
        "a recoverable controller reset staging exception must become failure feedback");
    requirePreserved(before, session, playback, loopState, "controller reset staging exception");
    require(selectedTrackId == "track-selection"
            && selectedAudioTrackId == "audio-track-selection"
            && selectedAudioClipId == "audio-clip-selection"
            && selectedMidiClipId == "midi-clip-selection",
        "replacement staging failure must preserve every Main selection field");
}

void loadExceptionPreservesTheEntireLiveReplacementState()
{
    resetTestWorkspace();
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host, makePreparedPlan);
    trackloom::AppProjectSession session(
        trackloom::saveProjectToFileAtomically, throwDuringProjectLoad);
    trackloom::AppLoopPlaybackState loopState;
    SelectionFixture selection;
    preparePreservedSession(
        session, loopState, testWorkspace() / "load-exception.trackloom");
    const auto before = observe(session, playback, loopState);

    const auto result = trackloom::openAppProjectIfSafe(
        session,
        playback,
        loopState,
        selection.selection(),
        testWorkspace() / "throwing-load.trackloom");

    require(!result.success
            && result.failureReason
                == trackloom::AppProjectReplacementFailureReason::OpenFailed,
        "a recoverable loader exception must become failure feedback");
    requirePreserved(before, session, playback, loopState, "loader exception");
    require(selection.unchanged(),
        "loader exception must preserve every Main selection field");

    const auto canonicalBeforeHistoryProbe = trackloom::saveProjectToText(session.project());
    require(session.undoProjectEdit(),
        "loader exception must retain the real undo command payload");
    require(trackloom::saveProjectToText(session.project()) != canonicalBeforeHistoryProbe,
        "loader exception undo must execute the retained command");
    require(session.redoProjectEdit(),
        "loader exception must retain the real redo command payload");
    require(trackloom::saveProjectToText(session.project()) == canonicalBeforeHistoryProbe,
        "loader exception redo must restore the exact project");
}

void recentProjectsReorderOnlyAfterARealSuccessfulOpen()
{
    resetTestWorkspace();
    const auto existingPath = testWorkspace() / "existing.trackloom";
    const auto missingPath = testWorkspace() / "missing-recent.trackloom";
    const auto settingsPath = testWorkspace() / "settings" / "recent-projects.txt";
    trackloom::AppProjectSession existing;
    existing.createNewProject("Recent Success");
    require(existing.saveAs(existingPath).success, "recent success fixture must save");

    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host, makePreparedPlan);
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;
    trackloom::AppRecentProjects recent;
    SelectionFixture selection;
    recent.record(existingPath);
    recent.record(missingPath);
    const auto beforeFailure = recent.paths();
    const auto sessionBeforeFailure = observe(session, playback, loopState);

    const auto failed = trackloom::openAppRecentProjectByNumber(
        session, playback, loopState, selection.selection(), recent, 1, settingsPath);
    require(!failed.success
            && failed.kind == trackloom::AppRecentProjectOpenFeedbackKind::OpenFailed,
        "missing recent project must fail through the replacement gate");
    require(recent.paths() == beforeFailure,
        "failed recent open must not change in-memory ordering");
    requirePreserved(
        sessionBeforeFailure, session, playback, loopState, "failed recent open");
    require(selection.unchanged(), "failed recent open must preserve Main selection");

    const auto succeeded = trackloom::openAppRecentProjectByNumber(
        session, playback, loopState, selection.selection(), recent, 2, settingsPath);
    require(succeeded.success && session.project().name() == "Recent Success",
        "existing recent project must open through the replacement gate");
    require(recent.paths().size() == 2 && recent.paths()[0] == existingPath,
        "only a successful recent open may promote its path");
    require(selection.empty(), "successful recent open must clear Main selection");
}

void fileMenuDisablesEveryReplacementEntryDuringObviousActiveStates()
{
    const auto requireDisabledFileReplacementEntries = [](
            const trackloom::AppMainMenuStatus& menu,
            const std::string& stateName) {
        require(menu.groups.size() >= 1 && menu.groups[0].items.size() >= 6,
            stateName + " menu fixture must contain file replacement entries");
        require(!menu.groups[0].items[0].enabled
                && !menu.groups[0].items[1].enabled
                && !menu.groups[0].items[5].enabled,
            stateName + " must disable new, open, and recent project entries");
    };

    trackloom::AppRecentProjects recent;
    recent.record(testWorkspace() / "menu-recent.trackloom");
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;

    {
        FakeRealtimePlaybackHost host;
        std::latch entered(1);
        std::latch release(1);
        trackloom::AppPlaybackController playback(
            host,
            [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
                entered.count_down();
                release.wait();
                return makePreparedPlan(std::move(request));
            });
        require(trackloom::startAppPlayback(playback, session, loopState).success,
            "Preparing menu fixture must start its worker");
        entered.wait();
        const auto menu = trackloom::describeAppMainMenu(session, playback, recent);
        release.count_down();
        requireDisabledFileReplacementEntries(menu, "Preparing");
    }

    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host, makePreparedPlan);
    startPlaying(playback, session, loopState);
    requireDisabledFileReplacementEntries(
        trackloom::describeAppMainMenu(session, playback, recent), "Playing");
    require(trackloom::stopAppPlayback(playback).success,
        "Stopping menu fixture must accept stop");
    requireDisabledFileReplacementEntries(
        trackloom::describeAppMainMenu(session, playback, recent), "Stopping");
}

}

int main()
{
    configureTestFailureOutput();
    try {
        preparingWorkerRejectsReplacementBeforeTouchingTheSession();
        playingAndStoppingRejectReplacementWithoutMutation();
        stoppedControllerRejectsAStillRunningCallbackWithoutMutation();
        dirtyReadOnlyCheckPrecedesHostQuiescence();
        stoppedAndUnavailableQuiescentControllersAllowReplacement();
        faultedControllerHardResetsBeforeSuccessfulReplacement();
        failedHardResetRejectsReplacementWithoutMutation();
        faultedControllerWithUnfinishedWorkerRejectsBeforeHardReset();
        asynchronousOpenPerformsASecondGateAtLoadTime();
        openFailurePreservesSessionAndLoopAfterReleasingAResidualPlan();
        playingFailurePreservesInstalledLoopIntentAndSelection();
        stoppingFailurePreservesRewindFlagsAndSelection();
        successfulReplacementClearsNextStartAndUsesTheNewCompletionState();
        successfulReplacementClearsAQueuedRewindBeforeLaterPlaybackStops();
        controllerResetStagingExceptionPreservesTheEntireLiveReplacementState();
        loadExceptionPreservesTheEntireLiveReplacementState();
        recentProjectsReorderOnlyAfterARealSuccessfulOpen();
        fileMenuDisablesEveryReplacementEntryDuringObviousActiveStates();
    } catch (const std::exception& error) {
        std::cerr << "App project replacement test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "App project replacement tests passed\n";
    return 0;
}
