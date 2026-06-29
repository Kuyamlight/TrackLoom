#pragma once

#include "Project.h"
#include "ProjectPlaybackSession.h"
#include "Transport.h"

#include <cstdint>
#include <string>
#include <vector>

namespace trackloom {

// 首屏播放控制先使用固定、安全的本地运行参数。
// 真实音频设备接入后，这些值会由设备回调配置覆盖。
inline constexpr double defaultAppPlaybackSampleRate = 44100.0;
inline constexpr int defaultAppPlaybackChannelCount = 2;
inline constexpr int defaultAppPlaybackMaxBlockFrames = 512;
inline constexpr int defaultAppPlaybackUiBlockFrames = 512;
inline constexpr int defaultAppPlaybackReleaseSampleOffset = 0;
inline constexpr std::int64_t defaultAppPlaybackStartSample = 0;

// AppPlaybackActionFeedbackKind 给 UI 和测试提供稳定分支。
// 播放控制是运行态动作，不应进入工程撤销栈，也不应标脏工程。
enum class AppPlaybackActionFeedbackKind {
    Success,
    NoOp,
    PrepareFailed,
    StopFailed,
    SeekFailed,
    RenderFailed
};

struct AppPlaybackActionFeedback {
    bool success = false;
    AppPlaybackActionFeedbackKind kind = AppPlaybackActionFeedbackKind::PrepareFailed;
    std::string message;
};

// AppPlaybackStatus 是首屏读取播放状态的只读快照。
// UI 应读取结构化字段，不要解析 summary 文案。
struct AppPlaybackStatus {
    bool prepared = false;
    bool playing = false;
    std::int64_t currentSample = 0;
    double currentSeconds = 0.0;
    std::string stateLabel;
    std::string summary;
};

// AppPlaybackController 保存桌面壳的运行态播放对象。
// 它不属于 Project 文件格式；打开、新建或关闭工程时可按应用需要重置。
class AppPlaybackController {
public:
    bool ensurePrepared(const Project& project);

    bool isPrepared() const;
    bool isPlaying() const;
    std::int64_t currentSample() const;
    double currentSeconds() const;

    void start();
    bool stop(const Project& project);
    bool rewindToStart(const Project& project);
    bool advanceOneUiBlock(const Project& project);

private:
    ProjectPlaybackSession playbackSession_;
    Transport transport_;
    std::vector<float> scratchAudioBuffer_;
};

AppPlaybackActionFeedback startAppPlayback(
    AppPlaybackController& playback,
    const Project& project);

AppPlaybackActionFeedback stopAppPlayback(
    AppPlaybackController& playback,
    const Project& project);

// 播放/停止切换是给按钮、快捷键、菜单和 AI 工具复用的统一入口；
// UI 不需要自己判断 isPlaying 后再复制开始、停止和错误反馈规则。
AppPlaybackActionFeedback toggleAppPlayback(
    AppPlaybackController& playback,
    const Project& project);

AppPlaybackActionFeedback rewindAppPlaybackToStart(
    AppPlaybackController& playback,
    const Project& project);

AppPlaybackActionFeedback advanceAppPlaybackForUiTick(
    AppPlaybackController& playback,
    const Project& project);

AppPlaybackStatus describeAppPlayback(const AppPlaybackController& playback);

}
