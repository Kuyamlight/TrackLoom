#pragma once

#include "AppProjectSession.h"

#include <cstdint>
#include <string>

namespace trackloom {

// 首屏“添加音符”使用保守的默认 MIDI 音符。
// C4、四分音符、正常力度和 1 通道适合做入门可听结果；后续钢琴卷帘再提供精细编辑。
inline constexpr std::int64_t defaultAppMidiNoteLengthTick = Project::ticksPerQuarterNote;
inline constexpr int defaultAppMidiNoteNumber = 60;
inline constexpr int defaultAppMidiNoteVelocity = 100;
inline constexpr int defaultAppMidiNoteVelocityStep = 8;
inline constexpr int defaultAppMidiNoteChannel = 1;

// AppMidiNoteActionFeedbackKind 给 UI、快捷键和测试提供稳定失败分类。
// 文案可调整，但 enum 不应随界面措辞变化。
enum class AppMidiNoteActionFeedbackKind {
    Success,
    MissingClip,
    IncompatibleClipType,
    ClipFull,
    EmptyClip,
    PitchFailed,
    VelocityFailed,
    DeleteFailed,
    CreateFailed
};

// AppMidiNoteActionFeedback 是默认 MIDI 音符创建动作的展示结果。
// noteId 只在 success 为 true 时有效，便于后续 UI 自动选中新音符。
struct AppMidiNoteActionFeedback {
    bool success = false;
    AppMidiNoteActionFeedbackKind kind = AppMidiNoteActionFeedbackKind::CreateFailed;
    std::string message;
    std::string noteId;
};

// createDefaultMidiNoteInClip 在目标 MIDI 片段里追加一个默认音符。
// 它不会创建片段，也不会改变音高或长度规则；这些属于后续编辑器入口。
AppMidiNoteActionFeedback createDefaultMidiNoteInClip(
    AppProjectSession& session,
    const std::string& clipId);

// deleteLastMidiNoteInClip 删除目标 MIDI 片段里时间位置最后的音符。
// 这是首屏的安全撤回入口，不等同于任意音符选择或钢琴卷帘删除。
AppMidiNoteActionFeedback deleteLastMidiNoteInClip(
    AppProjectSession& session,
    const std::string& clipId);

// raiseLastMidiNotePitchInClip / lowerLastMidiNotePitchInClip 只调整目标片段里的末尾音符音高。
// 当前没有任意音符选择 UI，因此沿用“时间位置最后”的安全目标选择规则。
AppMidiNoteActionFeedback raiseLastMidiNotePitchInClip(
    AppProjectSession& session,
    const std::string& clipId);

AppMidiNoteActionFeedback lowerLastMidiNotePitchInClip(
    AppProjectSession& session,
    const std::string& clipId);

// increaseLastMidiNoteVelocityInClip / decreaseLastMidiNoteVelocityInClip 只调整末尾音符力度。
// MIDI velocity 0 通常表示 Note Off，因此可保存发声音符保持在 1-127。
AppMidiNoteActionFeedback increaseLastMidiNoteVelocityInClip(
    AppProjectSession& session,
    const std::string& clipId);

AppMidiNoteActionFeedback decreaseLastMidiNoteVelocityInClip(
    AppProjectSession& session,
    const std::string& clipId);

}
