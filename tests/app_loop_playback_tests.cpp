#include "AppPlaybackActions.h"
#include "AppLoopActions.h"

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <cstdlib>
#endif

#include <atomic>
#include <chrono>
#include <iostream>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
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
        if (!installResult.success) {
            return installResult;
        }
        installedStartSamples.push_back(plan->playbackStartSample);
        snapshot_.realtime.projectSamplePosition = plan->playbackStartSample;
        snapshot_.realtime.renderedSampleCount = 0;
        snapshot_.realtime.state = trackloom::RealtimePlaybackState::Playing;
        installedPlan = std::move(plan);
        return installResult;
    }

    bool requestStop() noexcept override
    {
        std::scoped_lock lock(mutex_);
        ++stopCallCount;
        if (requestStopResult) {
            snapshot_.realtime.state = trackloom::RealtimePlaybackState::Stopping;
        }
        return requestStopResult;
    }

    void serviceNonRealtime() override
    {
        std::scoped_lock lock(mutex_);
        ++serviceCallCount;
    }

    void hardStopAndReset() noexcept override
    {
        std::scoped_lock lock(mutex_);
        ++hardResetCallCount;
        snapshot_.realtime.state = trackloom::RealtimePlaybackState::Stopped;
    }

    trackloom::RealtimePlaybackHostSnapshot snapshot() const override
    {
        std::scoped_lock lock(mutex_);
        return snapshot_;
    }

    void setRealtimePosition(std::int64_t sample)
    {
        std::scoped_lock lock(mutex_);
        snapshot_.realtime.projectSamplePosition = sample;
        snapshot_.realtime.renderedSampleCount = static_cast<std::uint64_t>(sample);
    }

    void completeStop()
    {
        std::scoped_lock lock(mutex_);
        snapshot_.realtime.state = trackloom::RealtimePlaybackState::Stopped;
    }

    void setFormatGeneration(std::uint64_t generation)
    {
        std::scoped_lock lock(mutex_);
        snapshot_.format.generation = generation;
    }

    void setDeviceAvailable(bool available)
    {
        std::scoped_lock lock(mutex_);
        snapshot_.format.available = available;
    }

    trackloom::RealtimePlaybackHostResult installResult {
        true, trackloom::RealtimePlaybackHostFailureReason::None, "started"
    };
    bool requestStopResult = true;
    int installCallCount = 0;
    int stopCallCount = 0;
    int serviceCallCount = 0;
    int hardResetCallCount = 0;
    std::vector<std::int64_t> installedStartSamples;
    std::unique_ptr<const trackloom::PreparedMidiPlaybackPlan> installedPlan;

private:
    mutable std::mutex mutex_;
    trackloom::RealtimePlaybackHostSnapshot snapshot_;
};

trackloom::PreparedMidiPlaybackPlanBuildResult makeFakePreparedPlan(
    const trackloom::PreparedMidiPlaybackPlanBuildRequest& request)
{
    auto plan = std::make_unique<trackloom::PreparedMidiPlaybackPlan>();
    plan->sampleRate = request.sampleRate;
    plan->maximumBlockFrames = request.maximumBlockFrames;
    plan->outputChannelCount = request.outputChannelCount;
    plan->outputChannelMask = request.outputChannelMask;
    plan->playbackStartSample = request.playbackStartSample;
    return { trackloom::PreparedMidiPlaybackPlanBuildFailureReason::None, std::move(plan) };
}

void pollUntilSettled(
    trackloom::AppPlaybackController& playback,
    const trackloom::AppProjectSession& session,
    const trackloom::AppLoopPlaybackState& loopState)
{
    for (int attempt = 0; attempt < 10000; ++attempt) {
        playback.poll(session, loopState);
        const auto state = playback.status().state;
        if (state != trackloom::AppPlaybackState::Preparing
            && state != trackloom::AppPlaybackState::Stopping) {
            return;
        }
        std::this_thread::yield();
    }
    require(false, "playback worker must settle within the bounded polling loop");
}

void setProjectLoop(
    trackloom::AppProjectSession& session,
    trackloom::PlaybackLoopRange range)
{
    require(session.editProject().setPlaybackLoopRange(range),
        "loop playback fixture must accept its project range");
}

void enableProjectLoop(
    trackloom::AppProjectSession& session,
    trackloom::AppLoopPlaybackState& loopState,
    trackloom::PlaybackLoopRange range)
{
    setProjectLoop(session, range);
    require(loopState.setEnabled(session.project(), true),
        "loop playback fixture must enable its session loop");
}

void loopStartResolutionUsesTheHalfOpenConvertedSampleRange()
{
    trackloom::Project project;
    const trackloom::PlaybackLoopRange loop { 960, 3840 };

    const auto inside = trackloom::resolveAppPlaybackStartSample(
        project, 48000.0, 48000, loop);
    const auto left = trackloom::resolveAppPlaybackStartSample(
        project, 48000.0, 23999, loop);
    const auto right = trackloom::resolveAppPlaybackStartSample(
        project, 48000.0, 96001, loop);
    const auto atRightBoundary = trackloom::resolveAppPlaybackStartSample(
        project, 48000.0, 96000, loop);

    require(inside.success && inside.sample == 48000,
        "a current sample inside [loop start, loop end) must be preserved");
    require(left.success && left.sample == 24000,
        "a current sample left of the loop must start at the converted loop start");
    require(right.success && right.sample == 24000,
        "a current sample right of the loop must start at the converted loop start");
    require(atRightBoundary.success && atRightBoundary.sample == 24000,
        "the converted loop end is outside the half-open playback range");
}

void loopStartResolutionUsesTheProjectTempoMap()
{
    trackloom::Project project;
    require(project.createTempoEvent(960, 60.0).has_value(),
        "tempo-map fixture must add the second tempo segment");

    const trackloom::PlaybackLoopRange loop { 1920, 2880 };
    const auto insideChangedTempoSegment = trackloom::resolveAppPlaybackStartSample(
        project, 48000.0, 100000, loop);
    const auto atChangedTempoRightBoundary = trackloom::resolveAppPlaybackStartSample(
        project, 48000.0, 120000, loop);

    require(insideChangedTempoSegment.success
            && insideChangedTempoSegment.sample == 100000,
        "a current sample inside the slower tempo segment must be preserved");
    require(atChangedTempoRightBoundary.success
            && atChangedTempoRightBoundary.sample == 72000,
        "the slower segment's converted right boundary must remain half-open");
}

void loopStartResolutionClassifiesInvalidSampleRatesFirst()
{
    const trackloom::Project project;
    const double invalidRates[] {
        0.0,
        -48000.0,
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()
    };

    for (const auto sampleRate : invalidRates) {
        const auto result = trackloom::resolveAppPlaybackStartSample(
            project, sampleRate, 123, trackloom::PlaybackLoopRange { -1, -1 });
        require(!result.success
                && result.failureReason
                    == trackloom::AppPlaybackStartPositionFailureReason::InvalidSampleRate,
            "invalid sample rates must win over invalid loop-range classification");
    }
}

void loopStartResolutionRejectsInvalidTickRanges()
{
    const trackloom::Project project;
    const trackloom::PlaybackLoopRange invalidRanges[] {
        { -1, 960 },
        { 960, 960 },
        { 1920, 960 }
    };

    for (const auto range : invalidRanges) {
        const auto result = trackloom::resolveAppPlaybackStartSample(
            project, 48000.0, 123, range);
        require(!result.success
                && result.failureReason
                    == trackloom::AppPlaybackStartPositionFailureReason::InvalidLoopRange,
            "negative, empty, and reversed tick ranges must be InvalidLoopRange");
    }
}

void loopStartResolutionDistinguishesOverflowFromRoundedCollapse()
{
    const trackloom::Project project;

    const auto overflow = trackloom::resolveAppPlaybackStartSample(
        project,
        std::numeric_limits<double>::max(),
        0,
        trackloom::PlaybackLoopRange { 0, std::numeric_limits<std::int64_t>::max() });
    require(!overflow.success
            && overflow.failureReason
                == trackloom::AppPlaybackStartPositionFailureReason::SamplePositionOverflow,
        "a valid tick range whose sample conversion overflows must be classified separately");

    const auto collapsed = trackloom::resolveAppPlaybackStartSample(
        project, 1.0, 0, trackloom::PlaybackLoopRange { 0, 1 });
    require(!collapsed.success
            && collapsed.failureReason
                == trackloom::AppPlaybackStartPositionFailureReason::CollapsedLoop,
        "distinct ticks that round to the same sample must be rejected as a collapsed loop");
}

void playAndSpaceCaptureTheSameEffectiveLoopBuildRequest()
{
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;
    const trackloom::PlaybackLoopRange loop { 960, 3840 };
    enableProjectLoop(session, loopState, loop);

    FakeRealtimePlaybackHost playHost;
    FakeRealtimePlaybackHost spaceHost;
    playHost.setRealtimePosition(120000);
    spaceHost.setRealtimePosition(120000);
    std::vector<trackloom::PreparedMidiPlaybackPlanBuildRequest> playRequests;
    std::vector<trackloom::PreparedMidiPlaybackPlanBuildRequest> spaceRequests;
    trackloom::AppPlaybackController play(
        playHost,
        [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
            playRequests.push_back(request);
            return makeFakePreparedPlan(request);
        });
    trackloom::AppPlaybackController space(
        spaceHost,
        [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
            spaceRequests.push_back(request);
            return makeFakePreparedPlan(request);
        });

    require(trackloom::startAppPlayback(play, session, loopState).success,
        "Play must accept the enabled project loop");
    require(trackloom::toggleAppPlayback(space, session, loopState).success,
        "Space must accept the same enabled project loop");
    pollUntilSettled(play, session, loopState);
    pollUntilSettled(space, session, loopState);

    require(playRequests.size() == 1 && spaceRequests.size() == 1,
        "Play and Space must each build exactly one request");
    require(playRequests[0].loopRange == loop
            && spaceRequests[0].loopRange == loop,
        "Play and Space must capture the same effective loop range");
    require(playRequests[0].playbackStartSample == 24000
            && spaceRequests[0].playbackStartSample == 24000,
        "Play and Space must apply the same loop-start policy");
}

void preparingProjectLoopChangeCancelsAndRejectsTheOldCompletion()
{
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;
    const trackloom::PlaybackLoopRange first { 960, 3840 };
    const trackloom::PlaybackLoopRange second { 3840, 7680 };
    enableProjectLoop(session, loopState, first);
    FakeRealtimePlaybackHost host;
    std::latch entered(1);
    std::latch release(1);
    std::atomic<bool> stopObserved { false };
    std::atomic<int> buildCount { 0 };
    trackloom::AppPlaybackController playback(
        host,
        [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token stopToken) {
            ++buildCount;
            entered.count_down();
            release.wait();
            stopObserved.store(stopToken.stop_requested());
            return makeFakePreparedPlan(request);
        });

    require(trackloom::startAppPlayback(playback, session, loopState).success,
        "range-change fixture must enter Preparing");
    entered.wait();
    setProjectLoop(session, second);
    host.setFormatGeneration(2);
    playback.poll(session, loopState);
    const auto invalidated = playback.status();
    require(invalidated.loopIntentStatus
            == trackloom::AppPlaybackLoopIntentStatus::PreparationInvalidated,
        "a changed effective range must outrank simultaneous project and format staleness");
    require(invalidated.loopIntentMessage.find("循环设置已变化，请重新播放")
            != std::string::npos,
        "loop invalidation must expose the required user guidance");

    release.count_down();
    pollUntilSettled(playback, session, loopState);
    require(stopObserved.load(), "loop invalidation must request worker cancellation");
    require(host.installCallCount == 0 && buildCount.load() == 1,
        "the old completion must neither install nor trigger automatic replay");
    require(playback.status().state == trackloom::AppPlaybackState::Stopped,
        "an invalidated preparation must converge to Stopped for explicit replay");
}

void preparingSessionToggleCancelsWithoutRelyingOnProjectGeneration()
{
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;
    enableProjectLoop(session, loopState, { 960, 3840 });
    const auto generation = session.projectEditGeneration();
    FakeRealtimePlaybackHost host;
    std::latch entered(1);
    std::latch release(1);
    std::atomic<bool> stopObserved { false };
    trackloom::AppPlaybackController playback(
        host,
        [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token stopToken) {
            entered.count_down();
            release.wait();
            stopObserved.store(stopToken.stop_requested());
            return makeFakePreparedPlan(request);
        });

    require(trackloom::startAppPlayback(playback, session, loopState).success,
        "session-toggle fixture must enter Preparing");
    entered.wait();
    require(loopState.setEnabled(session.project(), false),
        "session-toggle fixture must disable the loop without editing the project");
    require(session.projectEditGeneration() == generation,
        "the session-only switch must not advance project generation");
    playback.poll(session, loopState);
    require(playback.status().loopIntentStatus
            == trackloom::AppPlaybackLoopIntentStatus::PreparationInvalidated,
        "pure session loop changes must invalidate the active preparation key");

    release.count_down();
    pollUntilSettled(playback, session, loopState);
    require(stopObserved.load() && host.installCallCount == 0,
        "the cancelled session-only completion must not install");
}

void preparingStalenessUsesLoopProjectThenDevicePriority()
{
    {
        trackloom::AppProjectSession session;
        trackloom::AppLoopPlaybackState loopState;
        enableProjectLoop(session, loopState, { 960, 3840 });
        FakeRealtimePlaybackHost host;
        std::latch entered(1);
        std::latch release(1);
        trackloom::AppPlaybackController playback(
            host,
            [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
                entered.count_down();
                release.wait();
                return makeFakePreparedPlan(request);
            });
        require(trackloom::startAppPlayback(playback, session, loopState).success,
            "project-stale fixture must start preparation");
        entered.wait();
        session.editProject().rename("generation only");
        host.setDeviceAvailable(false);
        playback.poll(session, loopState);
        require(playback.status().failureReason
                == trackloom::AppPlaybackFailureReason::StalePreparation,
            "project generation staleness must outrank simultaneous device loss");
        require(playback.status().loopIntentStatus
                == trackloom::AppPlaybackLoopIntentStatus::None,
            "ordinary project generation changes must not be reported as loop changes");
        release.count_down();
        pollUntilSettled(playback, session, loopState);
        require(host.installCallCount == 0,
            "a stale project completion must not install");
    }
    {
        trackloom::AppProjectSession session;
        trackloom::AppLoopPlaybackState loopState;
        FakeRealtimePlaybackHost host;
        std::latch entered(1);
        std::latch release(1);
        trackloom::AppPlaybackController playback(
            host,
            [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
                entered.count_down();
                release.wait();
                return makeFakePreparedPlan(request);
            });
        require(trackloom::startAppPlayback(playback, session, loopState).success,
            "format-stale fixture must start preparation");
        entered.wait();
        host.setFormatGeneration(2);
        playback.poll(session, loopState);
        require(playback.status().failureReason
                == trackloom::AppPlaybackFailureReason::StalePreparation,
            "changed available device format must retain stale-device preparation semantics");
        require(playback.status().loopIntentStatus
                == trackloom::AppPlaybackLoopIntentStatus::None,
            "device format invalidation must not publish a loop intent");
        release.count_down();
        pollUntilSettled(playback, session, loopState);
        require(host.installCallCount == 0,
            "a stale device completion must not install");
    }
    {
        trackloom::AppProjectSession session;
        trackloom::AppLoopPlaybackState loopState;
        FakeRealtimePlaybackHost host;
        std::latch entered(1);
        std::latch release(1);
        trackloom::AppPlaybackController playback(
            host,
            [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
                entered.count_down();
                release.wait();
                return makeFakePreparedPlan(request);
            });
        require(trackloom::startAppPlayback(playback, session, loopState).success,
            "device-loss fixture must start preparation");
        entered.wait();
        host.setDeviceAvailable(false);
        playback.poll(session, loopState);
        require(playback.status().failureReason
                == trackloom::AppPlaybackFailureReason::NoAudioDevice,
            "device loss must retain NoAudioDevice invalidation semantics");
        release.count_down();
        pollUntilSettled(playback, session, loopState);
        require(host.installCallCount == 0,
            "a completion prepared for a lost device must not install");
    }
}

void completionReadyStalenessIsRejectedBeforeInstallationInPriorityOrder()
{
    using namespace std::chrono_literals;

    {
        trackloom::AppProjectSession session;
        trackloom::AppLoopPlaybackState loopState;
        enableProjectLoop(session, loopState, { 960, 3840 });
        FakeRealtimePlaybackHost host;
        std::latch buildReturning(1);
        std::atomic<int> buildCount { 0 };
        trackloom::AppPlaybackController playback(
            host,
            [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
                ++buildCount;
                auto result = makeFakePreparedPlan(request);
                buildReturning.count_down();
                return result;
            });
        require(trackloom::startAppPlayback(playback, session, loopState).success,
            "completion-ready loop-priority fixture must start preparation");
        buildReturning.wait();
        std::this_thread::sleep_for(20ms);
        setProjectLoop(session, { 3840, 7680 });
        host.setFormatGeneration(2);

        playback.poll(session, loopState);

        const auto status = playback.status();
        require(status.loopIntentStatus
                == trackloom::AppPlaybackLoopIntentStatus::PreparationInvalidated,
            "a published completion must still classify loop changes before project and device changes");
        require(status.failureReason
                == trackloom::AppPlaybackFailureReason::PreparationCancelled,
            "completion-ready loop invalidation must retain its loop cancellation reason");
        require(host.installCallCount == 0 && buildCount.load() == 1,
            "a completion published before loop invalidation is polled must not install or auto replay");
    }
    {
        trackloom::AppProjectSession session;
        trackloom::AppLoopPlaybackState loopState;
        FakeRealtimePlaybackHost host;
        std::latch buildReturning(1);
        std::atomic<int> buildCount { 0 };
        trackloom::AppPlaybackController playback(
            host,
            [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
                ++buildCount;
                auto result = makeFakePreparedPlan(request);
                buildReturning.count_down();
                return result;
            });
        require(trackloom::startAppPlayback(playback, session, loopState).success,
            "completion-ready project-priority fixture must start preparation");
        buildReturning.wait();
        std::this_thread::sleep_for(20ms);
        session.editProject().rename("completion-ready generation");
        host.setFormatGeneration(2);

        playback.poll(session, loopState);

        const auto status = playback.status();
        require(status.failureReason
                == trackloom::AppPlaybackFailureReason::StalePreparation,
            "a published completion must classify project generation before device format changes");
        require(status.loopIntentStatus
                == trackloom::AppPlaybackLoopIntentStatus::None,
            "completion-ready project staleness must not be reported as loop intent");
        require(host.installCallCount == 0 && buildCount.load() == 1,
            "a completion published before project staleness is polled must not install or auto replay");
    }
    {
        trackloom::AppProjectSession session;
        trackloom::AppLoopPlaybackState loopState;
        FakeRealtimePlaybackHost host;
        std::latch buildReturning(1);
        std::atomic<int> buildCount { 0 };
        trackloom::AppPlaybackController playback(
            host,
            [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
                ++buildCount;
                auto result = makeFakePreparedPlan(request);
                buildReturning.count_down();
                return result;
            });
        require(trackloom::startAppPlayback(playback, session, loopState).success,
            "completion-ready device-priority fixture must start preparation");
        buildReturning.wait();
        std::this_thread::sleep_for(20ms);
        host.setFormatGeneration(2);

        playback.poll(session, loopState);

        const auto status = playback.status();
        require(status.failureReason
                == trackloom::AppPlaybackFailureReason::StalePreparation,
            "a published completion must retain changed-format invalidation semantics");
        require(status.loopIntentStatus
                == trackloom::AppPlaybackLoopIntentStatus::None,
            "completion-ready device staleness must not publish loop intent");
        require(host.installCallCount == 0 && buildCount.load() == 1,
            "a completion published before device staleness is polled must not install or auto replay");
    }
}

void playingLoopPendingIsDerivedFromTheInstalledRangeAndIsReversible()
{
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;
    const trackloom::PlaybackLoopRange installed { 960, 3840 };
    const trackloom::PlaybackLoopRange changed { 3840, 7680 };
    enableProjectLoop(session, loopState, installed);
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(
        host,
        [](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
            return makeFakePreparedPlan(request);
        });
    require(trackloom::startAppPlayback(playback, session, loopState).success,
        "pending fixture must start the installed loop");
    pollUntilSettled(playback, session, loopState);

    setProjectLoop(session, changed);
    playback.poll(session, loopState);
    require(playback.isPlaying()
            && playback.status().loopIntentStatus
                == trackloom::AppPlaybackLoopIntentStatus::PendingNextPlayback,
        "changing the effective range while Playing must keep playing and publish pending");
    require(playback.status().loopIntentMessage.find("停止并重新播放后生效")
            != std::string::npos,
        "pending loop changes must explain their next-playback boundary");

    setProjectLoop(session, installed);
    playback.poll(session, loopState);
    require(playback.isPlaying()
            && playback.status().loopIntentStatus
                == trackloom::AppPlaybackLoopIntentStatus::None
            && playback.status().loopIntentMessage.empty(),
        "restoring the installed effective range must clear pending immediately");

    session.editProject().rename("unrelated generation");
    playback.poll(session, loopState);
    require(playback.status().loopIntentStatus
            == trackloom::AppPlaybackLoopIntentStatus::None,
        "an unrelated project generation change must not create loop pending");
}

void stopThenReplayBuildsTheLatestRangeAndClearsPending()
{
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;
    const trackloom::PlaybackLoopRange first { 960, 3840 };
    const trackloom::PlaybackLoopRange second { 3840, 7680 };
    enableProjectLoop(session, loopState, first);
    FakeRealtimePlaybackHost host;
    host.setRealtimePosition(30000);
    std::vector<trackloom::PreparedMidiPlaybackPlanBuildRequest> requests;
    trackloom::AppPlaybackController playback(
        host,
        [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
            requests.push_back(request);
            return makeFakePreparedPlan(request);
        });
    require(trackloom::startAppPlayback(playback, session, loopState).success,
        "replay fixture must start the first range");
    pollUntilSettled(playback, session, loopState);
    setProjectLoop(session, second);
    playback.poll(session, loopState);
    require(playback.status().loopIntentStatus
            == trackloom::AppPlaybackLoopIntentStatus::PendingNextPlayback,
        "changed range must be pending before stop");

    require(trackloom::stopAppPlayback(playback).success,
        "replay fixture must accept stop");
    host.completeStop();
    playback.poll(session, loopState);
    require(playback.status().state == trackloom::AppPlaybackState::Stopped
            && playback.status().loopIntentStatus
                == trackloom::AppPlaybackLoopIntentStatus::None,
        "normal stop completion must clear the installed-key pending presentation");
    require(trackloom::startAppPlayback(playback, session, loopState).success,
        "explicit replay must accept the latest range");
    pollUntilSettled(playback, session, loopState);

    require(requests.size() == 2 && requests[1].loopRange == second,
        "replay must build exactly once from the latest effective range");
    require(requests[1].playbackStartSample == 96000,
        "a stopped sample left of the latest loop must replay from its converted start");
}

void rewindAfterStopReReadsTheLatestRangeAtTheActualReplayPoint()
{
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState loopState;
    const trackloom::PlaybackLoopRange first { 960, 3840 };
    const trackloom::PlaybackLoopRange latest { 3840, 7680 };
    enableProjectLoop(session, loopState, first);
    FakeRealtimePlaybackHost host;
    std::vector<trackloom::PreparedMidiPlaybackPlanBuildRequest> requests;
    trackloom::AppPlaybackController playback(
        host,
        [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
            requests.push_back(request);
            return makeFakePreparedPlan(request);
        });
    require(trackloom::startAppPlayback(playback, session, loopState).success,
        "rewind fixture must start the first range");
    pollUntilSettled(playback, session, loopState);
    require(trackloom::rewindAppPlaybackToStart(playback).success,
        "rewind must enter an orderly stopping phase");

    setProjectLoop(session, latest);
    host.completeStop();
    playback.poll(session, loopState);
    pollUntilSettled(playback, session, loopState);

    require(requests.size() == 2 && requests[1].loopRange == latest,
        "rewind replay must recapture the latest effective loop after stop completes");
    require(requests[1].playbackStartSample == 96000,
        "rewind sample zero outside the latest loop must resolve to that loop start");
}

}

int main()
{
    configureTestFailureOutput();
    try {
        loopStartResolutionUsesTheHalfOpenConvertedSampleRange();
        loopStartResolutionUsesTheProjectTempoMap();
        loopStartResolutionClassifiesInvalidSampleRatesFirst();
        loopStartResolutionRejectsInvalidTickRanges();
        loopStartResolutionDistinguishesOverflowFromRoundedCollapse();
        playAndSpaceCaptureTheSameEffectiveLoopBuildRequest();
        preparingProjectLoopChangeCancelsAndRejectsTheOldCompletion();
        preparingSessionToggleCancelsWithoutRelyingOnProjectGeneration();
        preparingStalenessUsesLoopProjectThenDevicePriority();
        completionReadyStalenessIsRejectedBeforeInstallationInPriorityOrder();
        playingLoopPendingIsDerivedFromTheInstalledRangeAndIsReversible();
        stopThenReplayBuildsTheLatestRangeAndClearsPending();
        rewindAfterStopReReadsTheLatestRangeAtTheActualReplayPoint();
    } catch (const std::exception& error) {
        std::cerr << "App loop playback test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "All app loop playback tests passed\n";
    return 0;
}
