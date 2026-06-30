#pragma once

#include "AppProjectSession.h"

#include <cstdint>
#include <string>

namespace trackloom {

// 首屏“创建音频片段”先使用固定一小节占位长度。
// 它只是时间线外壳，后续音频文件导入稳定后再绑定真实素材、波形和素材偏移。
inline constexpr std::int64_t defaultAppAudioClipLengthTick = Project::ticksPerQuarterNote * 4;

// AppAudioClipActionFeedbackKind 给 UI、快捷键和测试提供稳定分支。
// 中文 message 只负责展示，业务逻辑不要解析 message 文本。
enum class AppAudioClipActionFeedbackKind {
    Success,
    MissingTrack,
    MissingClip,
    IncompatibleTrackType,
    IncompatibleClipType,
    DeleteFailed,
    CreateFailed
};

// AppAudioClipActionFeedback 是桌面音频片段动作的统一返回值。
// clipId 只在 success 为 true 时有效，便于 UI 后续选中新建片段。
struct AppAudioClipActionFeedback {
    bool success = false;
    AppAudioClipActionFeedbackKind kind = AppAudioClipActionFeedbackKind::CreateFailed;
    std::string message;
    std::string clipId;
};

// createDefaultAudioClipOnTrack 在目标音频轨末尾追加一个默认空音频片段。
// 它不导入文件、不创建波形、不承诺播放；失败校验必须在 editProject() 之前完成。
AppAudioClipActionFeedback createDefaultAudioClipOnTrack(
    AppProjectSession& session,
    const std::string& trackId);

// deleteAudioClipById 删除一个已存在的空音频片段外壳。
// 它只接受 Audio 片段；MIDI 片段和不存在的片段会在 dirty 之前被拒绝。
AppAudioClipActionFeedback deleteAudioClipById(
    AppProjectSession& session,
    const std::string& clipId);

}
