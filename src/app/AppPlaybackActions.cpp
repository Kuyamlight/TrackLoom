#include "AppPlaybackActions.h"

#include "PlaybackControl.h"

#include <iomanip>
#include <sstream>
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

AppPlaybackActionFeedback noOpFeedback(std::string message)
{
    AppPlaybackActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppPlaybackActionFeedbackKind::NoOp;
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

std::string secondsText(double seconds)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3) << seconds;
    return stream.str();
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

bool AppPlaybackController::rewindToStart(const Project& project)
{
    SeekPlaybackCommand command(defaultAppPlaybackStartSample, defaultAppPlaybackReleaseSampleOffset);
    return command.execute(playbackSession_, transport_, project).success;
}

bool AppPlaybackController::advanceOneUiBlock(const Project& project)
{
    if (!playbackSession_.isPrepared()) {
        return false;
    }

    const auto channelCount = playbackSession_.channelCount();
    const auto frameCount = defaultAppPlaybackUiBlockFrames;
    scratchAudioBuffer_.resize(static_cast<std::size_t>(channelCount * frameCount));

    // UI tick 使用应用层持有的静音缓冲区推动已测试的核心播放会话。
    // 它不会打开声卡；真实音频设备接入后应由设备回调提供 AudioBlock。
    AudioBlock block(scratchAudioBuffer_.data(), channelCount, frameCount);
    const auto result = playbackSession_.renderNextBlock(transport_, block, project);
    return result.renderSucceeded;
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

AppPlaybackActionFeedback toggleAppPlayback(
    AppPlaybackController& playback,
    const Project& project)
{
    // 已经在播放时必须走标准停止入口，因为停止入口会复用核心安全命令。
    if (playback.isPlaying()) {
        return stopAppPlayback(playback, project);
    }

    // 停止态必须走标准开始入口，确保 prepare、Transport 和反馈消息保持一致。
    return startAppPlayback(playback, project);
}

AppPlaybackActionFeedback rewindAppPlaybackToStart(
    AppPlaybackController& playback,
    const Project& project)
{
    if (!playback.ensurePrepared(project)) {
        return failureFeedback(
            AppPlaybackActionFeedbackKind::PrepareFailed,
            "无法回到开头：播放运行态准备失败。");
    }

    if (!playback.rewindToStart(project)) {
        return failureFeedback(
            AppPlaybackActionFeedbackKind::SeekFailed,
            "无法回到开头：安全跳转命令被拒绝。");
    }

    return successFeedback("已回到开头。");
}

AppPlaybackActionFeedback advanceAppPlaybackForUiTick(
    AppPlaybackController& playback,
    const Project& project)
{
    if (!playback.isPlaying()) {
        return noOpFeedback("播放未运行，本次界面刷新不推进播放位置。");
    }

    if (!playback.ensurePrepared(project)) {
        return failureFeedback(
            AppPlaybackActionFeedbackKind::PrepareFailed,
            "无法推进播放：播放运行态准备失败。");
    }

    if (!playback.advanceOneUiBlock(project)) {
        return failureFeedback(
            AppPlaybackActionFeedbackKind::RenderFailed,
            "无法推进播放：播放 block 渲染失败。");
    }

    return successFeedback("播放位置已推进。");
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
        + "，位置 " + std::to_string(status.currentSample)
        + " samples，约 " + secondsText(status.currentSeconds) + " 秒。";
    return status;
}

}
