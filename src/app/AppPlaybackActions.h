#pragma once

#include "AppProjectSession.h"
#include "PreparedMidiPlaybackPlan.h"
#include "RealtimePlaybackHost.h"

#include <atomic>
#include <cstdint>
#include <functional>
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

enum class AppPlaybackFailureReason {
    None,
    NoAudioDevice,
    PreparationCancelled,
    PreparationFailed,
    StalePreparation,
    HostRejected,
    DeviceFault
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

using AppPreparedPlanBuildOperation = std::function<
    PreparedMidiPlaybackPlanBuildResult(
        PreparedMidiPlaybackPlanBuildRequest,
        std::stop_token)>;

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
    void poll(const AppProjectSession& session);
    AppPlaybackStatus status() const;
    bool isPlaying() const noexcept;
    std::int64_t currentSample() const noexcept;
    double currentSeconds() const noexcept;

private:
    struct CompletedPreparation {
        AppPlaybackPreparationKey key;
        PreparedMidiPlaybackPlanBuildResult result;
    };

    void updateStatusFromHost(const RealtimePlaybackHostSnapshot& hostSnapshot);
    void updateStatusText();
    void finishPreparation(const AppProjectSession& session);

    RealtimePlaybackHost& host_;
    AppPreparedPlanBuildOperation build_;
    AppPlaybackStatus status_;
    std::int64_t playbackStartSample_ = defaultAppPlaybackStartSample;
    std::optional<PlaybackLoopRange> loopRange_;
    bool rewindAfterStop_ = false;
    std::jthread worker_;
    mutable std::mutex mailboxMutex_;
    std::optional<CompletedPreparation> mailbox_;
    std::atomic<bool> workerCompleted_ { false };
};

AppPlaybackActionFeedback startAppPlayback(
    AppPlaybackController& playback,
    const AppProjectSession& session,
    std::optional<PlaybackLoopRange> loopRange = std::nullopt);
AppPlaybackActionFeedback stopAppPlayback(AppPlaybackController& playback);
AppPlaybackActionFeedback toggleAppPlayback(
    AppPlaybackController& playback,
    const AppProjectSession& session);
AppPlaybackActionFeedback rewindAppPlaybackToStart(
    AppPlaybackController& playback);

AppPlaybackStatus describeAppPlayback(const AppPlaybackController& playback);

}
