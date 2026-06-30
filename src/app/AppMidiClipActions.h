#pragma once

#include "AppProjectSession.h"

#include <cstdint>
#include <string>

namespace trackloom {

// 首屏“创建 MIDI 片段”使用一个固定的一小节 starter 长度。
// 这里以 4/4、每拍 Project::ticksPerQuarterNote 为基础；后续节拍号 UI 稳定后再让默认长度跟随工程拍号。
inline constexpr std::int64_t defaultAppMidiClipLengthTick = Project::ticksPerQuarterNote * 4;

// AppMidiClipActionFeedbackKind 给 UI 和测试提供稳定分支。
// message 只负责显示，不应被当作业务判断依据。
enum class AppMidiClipActionFeedbackKind {
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
    CreateFailed
};

// AppMidiClipActionFeedback 是 MIDI 片段创建/删除动作的展示结果。
// clipId 只在 success 为 true 时有效，便于后续 UI 自动选中新片段或清理已删选择。
struct AppMidiClipActionFeedback {
    bool success = false;
    AppMidiClipActionFeedbackKind kind = AppMidiClipActionFeedbackKind::CreateFailed;
    std::string message;
    std::string clipId;
};

// createDefaultMidiClipOnTrack 只处理桌面入口的默认片段创建。
// 它先验证目标轨道，只有确认能创建时才请求 editable project，从而避免失败校验污染 dirty 状态。
AppMidiClipActionFeedback createDefaultMidiClipOnTrack(
    AppProjectSession& session,
    const std::string& trackId);

// deleteMidiClipById 删除一个已存在的 MIDI 片段。
// 它只接受 MIDI 片段 ID；音频片段和不存在的片段都会在 dirty 之前被拒绝。
AppMidiClipActionFeedback deleteMidiClipById(
    AppProjectSession& session,
    const std::string& clipId);

// duplicateMidiClipAfterItself 把目标 MIDI 片段复制到同一轨道、原片段结束位置。
// 这是首屏复制入口的最小能力；跨轨复制、拖拽定位和冲突处理属于后续时间线编辑器。
AppMidiClipActionFeedback duplicateMidiClipAfterItself(
    AppProjectSession& session,
    const std::string& clipId);

// renameMidiClipById 修改目标 MIDI 片段名称。
// 名称会先去掉首尾空白；空名称、音频片段和缺失片段都不会触碰 editable project。
AppMidiClipActionFeedback renameMidiClipById(
    AppProjectSession& session,
    const std::string& clipId,
    std::string name);

// splitMidiClipAtMidpoint 把目标 MIDI 片段从当前长度中点拆成左右两段。
// 这是首屏“拆分片段”的无输入入口；任意切点、刀片工具和跨音符拆分规则属于后续时间线编辑器。
AppMidiClipActionFeedback splitMidiClipAtMidpoint(
    AppProjectSession& session,
    const std::string& clipId);

}
