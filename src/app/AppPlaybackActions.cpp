#include "AppPlaybackActions.h"

#include "PlaybackControl.h"

#include <string>
#include <utility>

namespace trackloom {
namespace {

AppPlaybackActionFeedback successFeedback(std::string message)
{
    AppPlaybackActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppPlaybackActionFeedbackKind::Success;
    feedback.message = std::move(message);
    return feedback;
}

AppPlaybackActionFeedback failureFeedback(
    AppPlaybackActionFeedbackKind kind,
    std::string message)
{
    AppPlaybackActionFeedback feedback;
    feedback.success = false;
    feedback.kind = kind;
    feedback.message = std::move(message);
    return feedback;
}

}

bool AppPlaybackController::ensurePrepared(const Project& project)
{
    if (playbackSession_.isPrepared()) {
        return true;
    }

    if (!transport_.setSampleRate(defaultAppPlaybackSampleRate)) {
        return false;
    }

    if (!playbackSession_.prepare(
            defaultAppPlaybackSampleRate,
            defaultAppPlaybackChannelCount,
            defaultAppPlaybackMaxBlockFrames)) {
        return false;
    }

    // 现阶段首屏尚未绑定真实音源或 MIDI 输出设备；先用空图建立稳定运行态边界。
    return playbackSession_.rebuildAudioGraph(project, {})
        && playbackSession_.rebuildMidiOutput(project, {});
}

bool AppPlaybackController::isPrepared() const
{
    return playbackSession_.isPrepared();
}

bool AppPlaybackController::isPlaying() const
{
    return transport_.isPlaying();
}

std::int64_t AppPlaybackController::currentSample() const
{
    return transport_.currentSample();
}

double AppPlaybackController::currentSeconds() const
{
    return transport_.currentSeconds();
}

void AppPlaybackController::start()
{
    // 起播后的第一帧需要 chase 已经持续中的 MIDI 音符；未准备时请求会失败，但 startAppPlayback 会先准备。
    playbackSession_.requestMidiChaseOnNextBlock();
    transport_.play();
}

bool AppPlaybackController::stop(const Project& project)
{
    StopPlaybackCommand command(defaultAppPlaybackReleaseSampleOffset);
    return command.execute(playbackSession_, transport_, project).success;
}

AppPlaybackActionFeedback startAppPlayback(
    AppPlaybackController& playback,
    const Project& project)
{
    if (!playback.ensurePrepared(project)) {
        return failureFeedback(
            AppPlaybackActionFeedbackKind::PrepareFailed,
            "无法开始播放：播放运行态准备失败。");
    }

    playback.start();
    return successFeedback("已开始播放。");
}

AppPlaybackActionFeedback stopAppPlayback(
    AppPlaybackController& playback,
    const Project& project)
{
    if (!playback.ensurePrepared(project)) {
        return failureFeedback(
            AppPlaybackActionFeedbackKind::PrepareFailed,
            "无法停止播放：播放运行态准备失败。");
    }

    if (!playback.stop(project)) {
        return failureFeedback(
            AppPlaybackActionFeedbackKind::StopFailed,
            "无法停止播放：安全停止命令被拒绝。");
    }

    return successFeedback("已停止播放。");
}

AppPlaybackStatus describeAppPlayback(const AppPlaybackController& playback)
{
    AppPlaybackStatus status;
    status.prepared = playback.isPrepared();
    status.playing = playback.isPlaying();
    status.currentSample = playback.currentSample();
    status.currentSeconds = playback.currentSeconds();
    status.stateLabel = status.playing ? "播放中" : "已停止";
    status.summary = "播放状态：" + status.stateLabel
        + "，位置 " + std::to_string(status.currentSample) + " samples。";
    return status;
}

}
