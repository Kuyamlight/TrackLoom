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
        return snapshot_;
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

    void setQuiescenceEvidence(bool callbackRunning, bool planInstalled)
    {
        std::scoped_lock lock(mutex_);
        snapshot_.callbackRunning = callbackRunning;
        snapshot_.planInstalled = planInstalled;
    }

    bool clearCallbackOnReset = true;
    bool clearPlanOnReset = true;
    int hardResetCallCount = 0;

private:
    mutable std::mutex mutex_;
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

    bool operator==(const SessionObservation&) const = default;
};

SessionObservation observe(
    const trackloom::AppProjectSession& session,
    const trackloom::AppLoopPlaybackState& loopState)
{
    return {
        trackloom::saveProjectToText(session.project()),
        session.currentProjectPath(),
        session.isDirty(),
        session.canUndoProjectEdit(),
        session.canRedoProjectEdit(),
        session.projectEditGeneration(),
        loopState.enabled()
    };
}

void requirePreserved(
    const SessionObservation& before,
    const trackloom::AppProjectSession& session,
    const trackloom::AppLoopPlaybackState& loopState,
    const std::string& scenario)
{
    require(observe(session, loopState) == before,
        scenario + " must preserve project, path, dirty, history, generation, and loop state");
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
    preparePreservedSession(session, loopState, testWorkspace() / "preparing.trackloom");
    require(trackloom::startAppPlayback(playback, session, loopState).success,
        "preparing rejection fixture must start its worker");
    entered.wait();
    const auto before = observe(session, loopState);

    const auto result = trackloom::createNewAppProjectIfSafe(
        session, playback, loopState, "Rejected");
    release.count_down();

    require(!result.success
            && result.failureReason
                == trackloom::AppProjectReplacementFailureReason::PreparationWorkerActive,
        "an unfinished preparation worker must win replacement classification");
    require(host.hardResetCallCount == 0,
        "worker rejection must happen before host reset");
    requirePreserved(before, session, loopState, "preparing rejection");
}

void playingAndStoppingRejectReplacementWithoutMutation()
{
    resetTestWorkspace();
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host, makePreparedPlan);
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;
    preparePreservedSession(session, loopState, testWorkspace() / "active.trackloom");
    startPlaying(playback, session, loopState);
    const auto playingBefore = observe(session, loopState);

    const auto playingResult = trackloom::createNewAppProjectIfSafe(
        session, playback, loopState, "Rejected Playing");
    require(!playingResult.success
            && playingResult.failureReason
                == trackloom::AppProjectReplacementFailureReason::PlaybackActive,
        "Playing must reject replacement as active playback");
    requirePreserved(playingBefore, session, loopState, "playing rejection");

    require(trackloom::stopAppPlayback(playback).success,
        "stopping rejection fixture must accept stop");
    const auto stoppingBefore = observe(session, loopState);
    const auto stoppingResult = trackloom::createNewAppProjectIfSafe(
        session, playback, loopState, "Rejected Stopping");
    require(!stoppingResult.success
            && stoppingResult.failureReason
                == trackloom::AppProjectReplacementFailureReason::PlaybackActive,
        "Stopping must reject replacement as active playback");
    requirePreserved(stoppingBefore, session, loopState, "stopping rejection");
}

void stoppedControllerRejectsAStillRunningCallbackWithoutMutation()
{
    resetTestWorkspace();
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host, makePreparedPlan);
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;
    preparePreservedSession(session, loopState, testWorkspace() / "callback.trackloom");
    host.setQuiescenceEvidence(true, true);
    const auto before = observe(session, loopState);

    const auto result = trackloom::createNewAppProjectIfSafe(
        session, playback, loopState, "Rejected Callback");

    require(!result.success
            && result.failureReason
                == trackloom::AppProjectReplacementFailureReason::HostNotQuiescent,
        "Stopped controller with a running callback must reject replacement");
    require(host.hardResetCallCount == 0,
        "a running callback must be rejected rather than synchronously reset from Stopped");
    requirePreserved(before, session, loopState, "running callback rejection");
}

void dirtyReadOnlyCheckPrecedesHostQuiescence()
{
    resetTestWorkspace();
    FakeRealtimePlaybackHost host;
    host.setQuiescenceEvidence(false, true);
    trackloom::AppPlaybackController playback(host, makePreparedPlan);
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;
    preparePreservedSession(session, loopState, testWorkspace() / "dirty.trackloom");
    session.editProject().rename("Dirty Preserved Project");
    const auto before = observe(session, loopState);

    const auto result = trackloom::createNewAppProjectIfSafe(
        session, playback, loopState, "Rejected Dirty");

    require(!result.success
            && result.failureReason
                == trackloom::AppProjectReplacementFailureReason::DirtyProject,
        "dirty project must be classified before playback and host gates");
    require(host.hardResetCallCount == 0,
        "dirty read-only rejection must not release the host plan");
    requirePreserved(before, session, loopState, "dirty rejection");
}

void stoppedAndUnavailableQuiescentControllersAllowReplacement()
{
    {
        FakeRealtimePlaybackHost host;
        trackloom::AppPlaybackController playback(host, makePreparedPlan);
        trackloom::AppProjectSession session;
        trackloom::AppLoopPlaybackState loopState;

        const auto result = trackloom::createNewAppProjectIfSafe(
            session, playback, loopState, "Stopped Replacement");

        require(result.success && session.project().name() == "Stopped Replacement",
            "quiescent Stopped controller must allow a new project");
        require(playback.status().state == trackloom::AppPlaybackState::Stopped,
            "successful replacement must reset controller presentation to Stopped");
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
        preparePreservedSession(
            session,
            loopState,
            testWorkspace() / (std::string(resetCase.name) + ".trackloom"));
        host.setRealtimeState(trackloom::RealtimePlaybackState::Faulted);
        host.setQuiescenceEvidence(true, true);
        playback.poll(session, loopState);
        const auto before = observe(session, loopState);

        const auto result = trackloom::createNewAppProjectIfSafe(
            session, playback, loopState, "Rejected Reset");

        require(!result.success
                && result.failureReason
                    == trackloom::AppProjectReplacementFailureReason::HostResetFailed,
            std::string("hard reset must reject a remaining ") + resetCase.name);
        require(host.hardResetCallCount == 1,
            "faulted replacement must attempt exactly one hard reset");
        requirePreserved(
            before, session, loopState,
            std::string("hard reset ") + resetCase.name + " failure");
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
    preparePreservedSession(session, loopState, testWorkspace() / "fault-worker.trackloom");
    require(trackloom::startAppPlayback(playback, session, loopState).success,
        "faulted worker fixture must start preparation");
    entered.wait();
    host.setRealtimeState(trackloom::RealtimePlaybackState::Faulted);
    host.setQuiescenceEvidence(true, true);
    playback.poll(session, loopState);
    const auto before = observe(session, loopState);

    const auto result = trackloom::createNewAppProjectIfSafe(
        session, playback, loopState, "Rejected Fault Worker");
    release.count_down();

    require(!result.success
            && result.failureReason
                == trackloom::AppProjectReplacementFailureReason::PreparationWorkerActive,
        "Faulted with an unfinished worker must reject before hard reset");
    require(host.hardResetCallCount == 0,
        "worker-first rejection must not touch the faulted host");
    requirePreserved(before, session, loopState, "faulted worker rejection");
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
    preparePreservedSession(session, loopState, testWorkspace() / "async-current.trackloom");
    require(session.save().success, "async current fixture must become clean before chooser gate");
    const auto firstGate = trackloom::prepareAppProjectReplacement(playback);
    require(firstGate.safe, "chooser launch gate must allow the initially stopped host");

    startPlaying(playback, session, loopState);
    const auto before = observe(session, loopState);
    const auto secondGate = trackloom::openAppProjectIfSafe(
        session, playback, loopState, targetPath);

    require(!secondGate.success
            && secondGate.failureReason
                == trackloom::AppProjectReplacementFailureReason::PlaybackActive,
        "load-time gate must reject playback started while the chooser was open");
    requirePreserved(before, session, loopState, "asynchronous second-gate rejection");
}

void openFailurePreservesSessionAndLoopAfterReleasingAResidualPlan()
{
    resetTestWorkspace();
    FakeRealtimePlaybackHost host;
    host.setQuiescenceEvidence(false, true);
    trackloom::AppPlaybackController playback(host, makePreparedPlan);
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;
    preparePreservedSession(session, loopState, testWorkspace() / "load-failure.trackloom");
    require(session.save().success, "load failure fixture must be clean before open");
    const auto before = observe(session, loopState);

    const auto result = trackloom::openAppProjectIfSafe(
        session, playback, loopState, testWorkspace() / "missing.trackloom");

    require(!result.success
            && result.failureReason
                == trackloom::AppProjectReplacementFailureReason::OpenFailed,
        "missing project must report OpenFailed after host quiescence");
    require(host.hardResetCallCount == 1,
        "Stopped host with only a residual plan must hard reset before load");
    requirePreserved(before, session, loopState, "open failure");
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
    recent.record(existingPath);
    recent.record(missingPath);
    const auto beforeFailure = recent.paths();
    const auto sessionBeforeFailure = observe(session, loopState);

    const auto failed = trackloom::openAppRecentProjectByNumber(
        session, playback, loopState, recent, 1, settingsPath);
    require(!failed.success
            && failed.kind == trackloom::AppRecentProjectOpenFeedbackKind::OpenFailed,
        "missing recent project must fail through the replacement gate");
    require(recent.paths() == beforeFailure,
        "failed recent open must not change in-memory ordering");
    requirePreserved(
        sessionBeforeFailure, session, loopState, "failed recent open");

    const auto succeeded = trackloom::openAppRecentProjectByNumber(
        session, playback, loopState, recent, 2, settingsPath);
    require(succeeded.success && session.project().name() == "Recent Success",
        "existing recent project must open through the replacement gate");
    require(recent.paths().size() == 2 && recent.paths()[0] == existingPath,
        "only a successful recent open may promote its path");
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
        recentProjectsReorderOnlyAfterARealSuccessfulOpen();
        fileMenuDisablesEveryReplacementEntryDuringObviousActiveStates();
    } catch (const std::exception& error) {
        std::cerr << "App project replacement test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "App project replacement tests passed\n";
    return 0;
}
