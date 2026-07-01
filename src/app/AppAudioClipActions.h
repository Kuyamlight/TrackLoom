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
    EmptyName,
    RenameFailed,
    DeleteFailed,
    DuplicateFailed,
    SplitFailed,
    MoveFailed,
    TrimFailed,
    ExtendFailed,
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

// duplicateAudioClipAfterItself 把目标空音频片段复制到同一轨道、原片段结束位置。
// 它只复制片段外壳；不复制或创建音频文件、波形、素材引用和素材偏移。
AppAudioClipActionFeedback duplicateAudioClipAfterItself(
    AppProjectSession& session,
    const std::string& clipId);

// splitAudioClipAtMidpoint 把目标空音频片段从长度中点拆成左右两段。
// 它只拆时间线外壳；真实音频切点、交叉淡化和素材偏移属于音频导入后的独立规则。
AppAudioClipActionFeedback splitAudioClipAtMidpoint(
    AppProjectSession& session,
    const std::string& clipId);

// renameAudioClipById 修改一个已存在的空音频片段外壳名称。
// 名称会先去掉首尾空白；空名称、MIDI 片段和不存在的片段会在 dirty 之前被拒绝。
AppAudioClipActionFeedback renameAudioClipById(
    AppProjectSession& session,
    const std::string& clipId,
    std::string name);

// moveAudioClipLeftOneBeat / moveAudioClipRightOneBeat 用固定一拍步长移动空音频片段外壳。
// 它只移动片段起点，不处理素材偏移、波形、重叠冲突或真实音频播放。
AppAudioClipActionFeedback moveAudioClipLeftOneBeat(
    AppProjectSession& session,
    const std::string& clipId);

AppAudioClipActionFeedback moveAudioClipRightOneBeat(
    AppProjectSession& session,
    const std::string& clipId);

// trimAudioClipEndEarlierOneBeat 把空音频片段右边界向左缩短一拍。
// 当前只改变外壳长度；不会裁剪真实素材，因为素材引用和偏移尚未实现。
AppAudioClipActionFeedback trimAudioClipEndEarlierOneBeat(
    AppProjectSession& session,
    const std::string& clipId);

// extendAudioClipEndLaterOneBeat 把空音频片段右边界向右延长一拍。
// 它只增加空白时间线长度，不移动片段起点，也不创建音频内容。
AppAudioClipActionFeedback extendAudioClipEndLaterOneBeat(
    AppProjectSession& session,
    const std::string& clipId);

}
