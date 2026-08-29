#pragma once

#include "AppLoopActions.h"
#include "AppProjectSession.h"
#include "PreparedMidiPlaybackPlan.h"
#include "RealtimePlaybackHost.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

namespace trackloom {

inline constexpr std::int64_t defaultAppPlaybackStartSample = 0;

enum class AppPlaybackActionFeedbackKind {
    Success,
    NoOp,
    PrepareFailed,
    StopFailed,
    SeekFailed,
    RenderFailed
};

enum class AppPlaybackState {
    Unavailable,
    Stopped,
    Preparing,
    Playing,
    Stopping,
    Faulted
};

enum class AppPlaybackLoopIntentStatus {
    None,
    PreparationInvalidated,
    PendingNextPlayback
};

enum class AppPlaybackFailureReason {
    None,
    NoAudioDevice,
    PreparationCancelled,
    PreparationFailed,
    StalePreparation,
    HostRejected,
    DeviceFault
};

enum class AppPlaybackStartPositionFailureReason {
    None,
    InvalidSampleRate,
    InvalidLoopRange,
    SamplePositionOverflow,
    CollapsedLoop
};

struct AppPlaybackStartPositionResult {
    bool success = false;
    AppPlaybackStartPositionFailureReason failureReason =
        AppPlaybackStartPositionFailureReason::InvalidLoopRange;
    std::int64_t sample = 0;
};

struct AppPlaybackActionFeedback {
    bool success = false;
    AppPlaybackActionFeedbackKind kind = AppPlaybackActionFeedbackKind::PrepareFailed;
    AppPlaybackFailureReason failureReason = AppPlaybackFailureReason::None;
    std::string message;
};

struct AppPlaybackStatus {
    AppPlaybackState state = AppPlaybackState::Stopped;
    AppPlaybackFailureReason failureReason = AppPlaybackFailureReason::None;
    bool deviceAvailable = false;
    bool canStart = false;
    std::int64_t projectSamplePosition = 0;
    std::uint64_t renderedSampleCount = 0;
    double projectSeconds = 0.0;
    AppPlaybackLoopIntentStatus loopIntentStatus =
        AppPlaybackLoopIntentStatus::None;
    std::string loopIntentMessage;
    std::string stateLabel;
    std::string summary;
};

struct AppPlaybackPreparationKey {
    std::uint64_t projectEditGeneration = 0;
    std::uint64_t deviceFormatGeneration = 0;
    std::int64_t playbackStartSample = 0;
    std::optional<PlaybackLoopRange> loopRange;
    bool operator==(const AppPlaybackPreparationKey&) const = default;
};

AppPlaybackStartPositionResult resolveAppPlaybackStartSample(
    const Project& project,
    double sampleRate,
    std::int64_t currentSample,
    std::optional<PlaybackLoopRange> effectiveLoopRange);

using AppPreparedPlanBuildOperation = std::function<
    PreparedMidiPlaybackPlanBuildResult(
        PreparedMidiPlaybackPlanBuildRequest,
        std::stop_token)>;

namespace detail {

struct AppPlaybackPreparationCompletion {
    AppPlaybackPreparationKey key;
    PreparedMidiPlaybackPlanBuildResult result;
};

struct AppPlaybackPreparationCompletionState {
    mutable std::mutex mailboxMutex;
    std::optional<AppPlaybackPreparationCompletion> mailbox;
    std::atomic<bool> completionPublished { false };
};

void runAppPlaybackPreparationBuild(
    const std::shared_ptr<AppPlaybackPreparationCompletionState>& completionState,
    PreparedMidiPlaybackPlanBuildRequest request,
    AppPlaybackPreparationKey key,
    AppPreparedPlanBuildOperation build,
    std::stop_token stopToken) noexcept;

}

class AppPlaybackController final {
public:
    explicit AppPlaybackController(
        RealtimePlaybackHost& host,
        AppPreparedPlanBuildOperation build = buildPreparedMidiPlaybackPlan);
    ~AppPlaybackController();

    AppPlaybackController(const AppPlaybackController&) = delete;
    AppPlaybackController& operator=(const AppPlaybackController&) = delete;

    AppPlaybackActionFeedback start(
        AppProjectPlaybackSnapshot project,
        std::optional<PlaybackLoopRange> loopRange = std::nullopt);
    AppPlaybackActionFeedback stop();
    AppPlaybackActionFeedback rewindToStart();
    void poll(
        const AppProjectSession& session,
        const AppLoopPlaybackState& loopState);
    AppPlaybackStatus status() const;
    bool isPlaying() const noexcept;
    std::int64_t currentSample() const noexcept;
    double currentSeconds() const noexcept;

private:
    friend AppPlaybackActionFeedback startAppPlayback(
        AppPlaybackController& playback,
        const AppProjectSession& session,
        const AppLoopPlaybackState& loopState);

    AppPlaybackActionFeedback startWithLoopIntent(
        AppProjectPlaybackSnapshot project,
        std::optional<PlaybackLoopRange> loopRange,
        bool usesHighLevelLoopIntent);
    void updateStatusFromHost(const RealtimePlaybackHostSnapshot& hostSnapshot);
    void updateStatusText();
    void finishPreparation(
        const AppProjectSession& session,
        const AppLoopPlaybackState& loopState);

    RealtimePlaybackHost& host_;
    AppPreparedPlanBuildOperation build_;
    AppPlaybackStatus status_;
    std::int64_t playbackStartSample_ = defaultAppPlaybackStartSample;
    std::optional<PlaybackLoopRange> loopRange_;
    std::optional<std::int64_t> nextPlaybackStartSample_;
    std::optional<AppPlaybackPreparationKey> activePreparationKey_;
    std::optional<AppPlaybackPreparationKey> installedPreparationKey_;
    bool activePreparationUsesLoopIntent_ = false;
    bool installedPreparationUsesLoopIntent_ = false;
    bool rewindAfterStop_ = false;
    bool rewindAfterStopUsesLoopIntent_ = false;
    std::shared_ptr<detail::AppPlaybackPreparationCompletionState> completionState_;
    std::jthread worker_;
};

AppPlaybackActionFeedback startAppPlayback(
    AppPlaybackController& playback,
    const AppProjectSession& session,
    const AppLoopPlaybackState& loopState);
AppPlaybackActionFeedback stopAppPlayback(AppPlaybackController& playback);
AppPlaybackActionFeedback toggleAppPlayback(
    AppPlaybackController& playback,
    const AppProjectSession& session,
    const AppLoopPlaybackState& loopState);
AppPlaybackActionFeedback rewindAppPlaybackToStart(
    AppPlaybackController& playback);

AppPlaybackStatus describeAppPlayback(const AppPlaybackController& playback);

}
