#include "AppPlaybackActions.h"

#include <iomanip>
#include <sstream>
#include <utility>

namespace trackloom {
namespace {

AppPlaybackActionFeedback feedback(
    bool success,
    AppPlaybackActionFeedbackKind kind,
    AppPlaybackFailureReason reason,
    std::string message)
{
    return { success, kind, reason, std::move(message) };
}

std::string secondsText(double seconds)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3) << seconds;
    return stream.str();
}

const char* stateLabel(AppPlaybackState state)
{
    switch (state) {
    case AppPlaybackState::Unavailable: return "不可用";
    case AppPlaybackState::Stopped: return "已停止";
    case AppPlaybackState::Preparing: return "准备中";
    case AppPlaybackState::Playing: return "播放中";
    case AppPlaybackState::Stopping: return "停止中";
    case AppPlaybackState::Faulted: return "故障";
    }
    return "故障";
}

}

void detail::runAppPlaybackPreparationBuild(
    const std::shared_ptr<AppPlaybackPreparationCompletionState>& completionState,
    PreparedMidiPlaybackPlanBuildRequest request,
    AppPlaybackPreparationKey key,
    AppPreparedPlanBuildOperation build,
    std::stop_token stopToken) noexcept
{
    PreparedMidiPlaybackPlanBuildResult result;
    try {
        result = build(std::move(request), stopToken);
    } catch (...) {
        result.failureReason = PreparedMidiPlaybackPlanBuildFailureReason::InvalidOutputFormat;
    }
    {
        std::scoped_lock lock(completionState->mailboxMutex);
        completionState->mailbox.emplace(
            AppPlaybackPreparationCompletion { std::move(key), std::move(result) });
    }
    completionState->completionPublished.store(true, std::memory_order_release);
}

AppPlaybackController::AppPlaybackController(
    RealtimePlaybackHost& host,
    AppPreparedPlanBuildOperation build)
    : host_(host)
    , build_(build ? std::move(build) : AppPreparedPlanBuildOperation(buildPreparedMidiPlaybackPlan))
    , completionState_(std::make_shared<detail::AppPlaybackPreparationCompletionState>())
{
    const auto hostSnapshot = host_.snapshot();
    status_.state = hostSnapshot.format.available
        ? AppPlaybackState::Stopped
        : AppPlaybackState::Unavailable;
    status_.failureReason = hostSnapshot.format.available
        ? AppPlaybackFailureReason::None
        : AppPlaybackFailureReason::NoAudioDevice;
    updateStatusFromHost(hostSnapshot);
}

AppPlaybackController::~AppPlaybackController()
{
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
}

AppPlaybackActionFeedback AppPlaybackController::start(
    AppProjectPlaybackSnapshot project,
    std::optional<PlaybackLoopRange> loopRange)
{
    const auto hostSnapshot = host_.snapshot();
    updateStatusFromHost(hostSnapshot);
    if (!hostSnapshot.format.available) {
        status_.state = AppPlaybackState::Unavailable;
        status_.failureReason = AppPlaybackFailureReason::NoAudioDevice;
        updateStatusText();
        return feedback(false, AppPlaybackActionFeedbackKind::PrepareFailed,
            AppPlaybackFailureReason::NoAudioDevice, "无法开始播放：音频设备不可用。");
    }
    if (status_.state != AppPlaybackState::Stopped
        || hostSnapshot.realtime.state != RealtimePlaybackState::Stopped
        || worker_.joinable()) {
        return feedback(false, AppPlaybackActionFeedbackKind::PrepareFailed,
            AppPlaybackFailureReason::HostRejected, "无法开始播放：播放控制器当前不能启动。");
    }

    const auto requestedStartSample = nextPlaybackStartSample_.value_or(
        hostSnapshot.realtime.projectSamplePosition);
    nextPlaybackStartSample_.reset();
    playbackStartSample_ = requestedStartSample;
    loopRange_ = loopRange;
    const AppPlaybackPreparationKey key {
        project.projectEditGeneration,
        hostSnapshot.format.generation,
        playbackStartSample_,
        loopRange_
    };
    activePreparationKey_ = key;
    PreparedMidiPlaybackPlanBuildRequest request {
        std::move(project.project),
        hostSnapshot.format.sampleRate,
        hostSnapshot.format.maximumBlockFrames,
        hostSnapshot.format.outputChannelCount,
        hostSnapshot.format.outputChannelMask,
        playbackStartSample_,
        loopRange_
    };

    {
        std::scoped_lock lock(completionState_->mailboxMutex);
        completionState_->mailbox.reset();
    }
    completionState_->completionPublished.store(false, std::memory_order_release);
    status_.state = AppPlaybackState::Preparing;
    status_.failureReason = AppPlaybackFailureReason::None;
    status_.canStart = false;
    updateStatusText();
    const auto build = build_;
    const auto completionState = completionState_;
    worker_ = std::jthread(
        [completionState, request = std::move(request), key, build](std::stop_token stopToken) mutable {
            detail::runAppPlaybackPreparationBuild(
                completionState,
                std::move(request),
                key,
                build,
                stopToken);
        });
    return feedback(true, AppPlaybackActionFeedbackKind::Success,
        AppPlaybackFailureReason::None, "已开始准备播放。");
}

AppPlaybackActionFeedback AppPlaybackController::stop()
{
    if (status_.state == AppPlaybackState::Preparing) {
        if (worker_.joinable()) {
            worker_.request_stop();
        }
        status_.state = AppPlaybackState::Stopping;
        status_.failureReason = AppPlaybackFailureReason::None;
        updateStatusText();
        return feedback(true, AppPlaybackActionFeedbackKind::Success,
            AppPlaybackFailureReason::None, "正在取消播放准备。");
    }
    if (status_.state == AppPlaybackState::Playing) {
        if (!host_.requestStop()) {
            status_.state = AppPlaybackState::Faulted;
            status_.failureReason = AppPlaybackFailureReason::HostRejected;
            updateStatusText();
            return feedback(false, AppPlaybackActionFeedbackKind::StopFailed,
                AppPlaybackFailureReason::HostRejected, "无法停止播放：主机拒绝停止请求。");
        }
        status_.state = AppPlaybackState::Stopping;
        status_.failureReason = AppPlaybackFailureReason::None;
        updateStatusText();
        return feedback(true, AppPlaybackActionFeedbackKind::Success,
            AppPlaybackFailureReason::None, "正在停止播放。");
    }
    if (status_.state == AppPlaybackState::Stopped
        || status_.state == AppPlaybackState::Unavailable) {
        return feedback(true, AppPlaybackActionFeedbackKind::NoOp,
            status_.failureReason, "播放已经停止。");
    }
    return feedback(false, AppPlaybackActionFeedbackKind::StopFailed,
        AppPlaybackFailureReason::HostRejected, "无法停止播放：控制器状态不允许停止。");
}

AppPlaybackActionFeedback AppPlaybackController::rewindToStart()
{
    if (status_.state == AppPlaybackState::Playing) {
        if (!host_.requestStop()) {
            status_.state = AppPlaybackState::Faulted;
            status_.failureReason = AppPlaybackFailureReason::HostRejected;
            updateStatusText();
            return feedback(false, AppPlaybackActionFeedbackKind::SeekFailed,
                AppPlaybackFailureReason::HostRejected, "无法回到开头：主机拒绝停止请求。");
        }
        rewindAfterStop_ = true;
        status_.state = AppPlaybackState::Stopping;
        updateStatusText();
        return feedback(true, AppPlaybackActionFeedbackKind::Success,
            AppPlaybackFailureReason::None, "正在停止并回到开头。");
    }
    if (status_.state == AppPlaybackState::Stopped) {
        nextPlaybackStartSample_ = defaultAppPlaybackStartSample;
        playbackStartSample_ = defaultAppPlaybackStartSample;
        status_.projectSamplePosition = defaultAppPlaybackStartSample;
        status_.renderedSampleCount = 0;
        status_.projectSeconds = 0.0;
        updateStatusText();
        return feedback(true, AppPlaybackActionFeedbackKind::Success,
            AppPlaybackFailureReason::None, "已回到开头。");
    }
    return feedback(false, AppPlaybackActionFeedbackKind::SeekFailed,
        AppPlaybackFailureReason::HostRejected, "无法回到开头：控制器状态不允许跳转。");
}

void AppPlaybackController::poll(const AppProjectSession& session)
{
    host_.serviceNonRealtime();
    auto hostSnapshot = host_.snapshot();
    if (hostSnapshot.realtime.state == RealtimePlaybackState::Faulted) {
        if (worker_.joinable()) {
            worker_.request_stop();
        }
        status_.state = AppPlaybackState::Faulted;
        status_.failureReason = AppPlaybackFailureReason::DeviceFault;
        updateStatusFromHost(hostSnapshot);
        return;
    }
    if (completionState_->completionPublished.load(std::memory_order_acquire)) {
        finishPreparation(session);
        hostSnapshot = host_.snapshot();
    }

    if (status_.state == AppPlaybackState::Faulted
        && status_.failureReason == AppPlaybackFailureReason::DeviceFault
        && hostSnapshot.format.available
        && hostSnapshot.realtime.state == RealtimePlaybackState::Stopped
        && !worker_.joinable()) {
        activePreparationKey_.reset();
        {
            std::scoped_lock lock(completionState_->mailboxMutex);
            completionState_->mailbox.reset();
        }
        completionState_->completionPublished.store(false, std::memory_order_release);
        status_.state = AppPlaybackState::Stopped;
        status_.failureReason = AppPlaybackFailureReason::None;
    }

    if (status_.state == AppPlaybackState::Playing
        && hostSnapshot.realtime.state == RealtimePlaybackState::Stopped) {
        status_.state = AppPlaybackState::Stopped;
        status_.failureReason = AppPlaybackFailureReason::None;
    } else if (status_.state == AppPlaybackState::Stopping
        && !worker_.joinable()
        && hostSnapshot.realtime.state == RealtimePlaybackState::Stopped) {
        status_.state = AppPlaybackState::Stopped;
        status_.failureReason = AppPlaybackFailureReason::None;
        if (rewindAfterStop_) {
            rewindAfterStop_ = false;
            nextPlaybackStartSample_ = defaultAppPlaybackStartSample;
            playbackStartSample_ = defaultAppPlaybackStartSample;
            status_.projectSamplePosition = defaultAppPlaybackStartSample;
            status_.renderedSampleCount = 0;
            start(session.capturePlaybackSnapshot(), loopRange_);
            hostSnapshot = host_.snapshot();
        }
    } else if ((status_.state == AppPlaybackState::Stopped
            || status_.state == AppPlaybackState::Unavailable)
        && hostSnapshot.realtime.state == RealtimePlaybackState::Stopped) {
        status_.state = hostSnapshot.format.available
            ? AppPlaybackState::Stopped
            : AppPlaybackState::Unavailable;
        if (!hostSnapshot.format.available) {
            status_.failureReason = AppPlaybackFailureReason::NoAudioDevice;
        }
    }
    updateStatusFromHost(hostSnapshot);
}

void AppPlaybackController::finishPreparation(const AppProjectSession& session)
{
    if (worker_.joinable()) {
        worker_.join();
    }
    completionState_->completionPublished.store(false, std::memory_order_release);
    std::optional<detail::AppPlaybackPreparationCompletion> completed;
    {
        std::scoped_lock lock(completionState_->mailboxMutex);
        completed = std::move(completionState_->mailbox);
        completionState_->mailbox.reset();
    }
    if (!completed.has_value()) {
        activePreparationKey_.reset();
        status_.state = AppPlaybackState::Faulted;
        status_.failureReason = AppPlaybackFailureReason::PreparationFailed;
        updateStatusText();
        return;
    }
    const auto acceptedKey = activePreparationKey_;
    activePreparationKey_.reset();
    if (status_.state == AppPlaybackState::Stopping) {
        status_.state = AppPlaybackState::Stopped;
        status_.failureReason = AppPlaybackFailureReason::PreparationCancelled;
        updateStatusText();
        return;
    }
    if (status_.state == AppPlaybackState::Faulted) {
        status_.failureReason = AppPlaybackFailureReason::DeviceFault;
        updateStatusText();
        return;
    }

    const auto format = host_.deviceFormatSnapshot();
    const auto completedWrongIntent = !acceptedKey.has_value()
        || completed->key != *acceptedKey;
    const auto acceptedIntentIsStale = !acceptedKey.has_value()
        || acceptedKey->projectEditGeneration != session.projectEditGeneration()
        || acceptedKey->deviceFormatGeneration != format.generation;
    if (completedWrongIntent || acceptedIntentIsStale || !format.available) {
        status_.state = format.available
            ? AppPlaybackState::Stopped
            : AppPlaybackState::Unavailable;
        status_.failureReason = format.available
            ? AppPlaybackFailureReason::StalePreparation
            : AppPlaybackFailureReason::NoAudioDevice;
        updateStatusText();
        return;
    }
    if (completed->result.failureReason != PreparedMidiPlaybackPlanBuildFailureReason::None
        || completed->result.plan == nullptr) {
        if (completed->result.failureReason == PreparedMidiPlaybackPlanBuildFailureReason::Cancelled) {
            status_.state = AppPlaybackState::Stopped;
            status_.failureReason = AppPlaybackFailureReason::PreparationCancelled;
        } else {
            status_.state = AppPlaybackState::Faulted;
            status_.failureReason = AppPlaybackFailureReason::PreparationFailed;
        }
        updateStatusText();
        return;
    }

    const auto installed = host_.installAndStart(std::move(completed->result.plan));
    if (!installed.success) {
        status_.state = AppPlaybackState::Faulted;
        status_.failureReason = AppPlaybackFailureReason::HostRejected;
        updateStatusText();
        return;
    }
    status_.state = AppPlaybackState::Playing;
    status_.failureReason = AppPlaybackFailureReason::None;
    updateStatusFromHost(host_.snapshot());
}

void AppPlaybackController::updateStatusFromHost(
    const RealtimePlaybackHostSnapshot& hostSnapshot)
{
    status_.deviceAvailable = hostSnapshot.format.available;
    status_.projectSamplePosition = hostSnapshot.realtime.projectSamplePosition;
    status_.renderedSampleCount = hostSnapshot.realtime.renderedSampleCount;
    status_.projectSeconds = hostSnapshot.format.sampleRate > 0.0
        ? static_cast<double>(status_.projectSamplePosition) / hostSnapshot.format.sampleRate
        : 0.0;
    status_.canStart = status_.state == AppPlaybackState::Stopped
        && hostSnapshot.format.available
        && hostSnapshot.realtime.state == RealtimePlaybackState::Stopped
        && !worker_.joinable();
    updateStatusText();
}

void AppPlaybackController::updateStatusText()
{
    status_.stateLabel = stateLabel(status_.state);
    status_.summary = "播放状态：" + status_.stateLabel
        + "，位置 " + std::to_string(status_.projectSamplePosition)
        + " samples，约 " + secondsText(status_.projectSeconds) + " 秒。";
}

AppPlaybackStatus AppPlaybackController::status() const
{
    return status_;
}

bool AppPlaybackController::isPlaying() const noexcept
{
    return status_.state == AppPlaybackState::Playing;
}

std::int64_t AppPlaybackController::currentSample() const noexcept
{
    return status_.projectSamplePosition;
}

double AppPlaybackController::currentSeconds() const noexcept
{
    return status_.projectSeconds;
}

AppPlaybackActionFeedback startAppPlayback(
    AppPlaybackController& playback,
    const AppProjectSession& session,
    std::optional<PlaybackLoopRange> loopRange)
{
    return playback.start(session.capturePlaybackSnapshot(), loopRange);
}

AppPlaybackActionFeedback stopAppPlayback(AppPlaybackController& playback)
{
    return playback.stop();
}

AppPlaybackActionFeedback toggleAppPlayback(
    AppPlaybackController& playback,
    const AppProjectSession& session)
{
    const auto state = playback.status().state;
    if (state == AppPlaybackState::Preparing
        || state == AppPlaybackState::Playing) {
        return stopAppPlayback(playback);
    }
    return startAppPlayback(playback, session);
}

AppPlaybackActionFeedback rewindAppPlaybackToStart(AppPlaybackController& playback)
{
    return playback.rewindToStart();
}

AppPlaybackStatus describeAppPlayback(const AppPlaybackController& playback)
{
    return playback.status();
}

}
