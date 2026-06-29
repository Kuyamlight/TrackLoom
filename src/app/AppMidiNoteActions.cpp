#include "AppMidiNoteActions.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace trackloom {
namespace {

AppMidiNoteActionFeedback successFeedback(const MidiNoteEvent& note)
{
    AppMidiNoteActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppMidiNoteActionFeedbackKind::Success;
    feedback.noteId = note.id;
    feedback.message = "已添加默认 MIDI 音符。";
    return feedback;
}

AppMidiNoteActionFeedback failureFeedback(
    AppMidiNoteActionFeedbackKind kind,
    std::string message)
{
    AppMidiNoteActionFeedback feedback;
    feedback.success = false;
    feedback.kind = kind;
    feedback.message = std::move(message);
    return feedback;
}

std::int64_t nextNoteStartTick(const TimelineClip& clip)
{
    std::int64_t nextStartTick = 0;

    for (const auto& note : clip.midiNotes) {
        nextStartTick = std::max(nextStartTick, note.startTick + note.lengthTick);
    }

    return nextStartTick;
}

bool canFitDefaultNoteAt(const TimelineClip& clip, std::int64_t startTick)
{
    return clip.lengthTick >= defaultAppMidiNoteLengthTick
        && startTick <= clip.lengthTick - defaultAppMidiNoteLengthTick;
}

}

AppMidiNoteActionFeedback createDefaultMidiNoteInClip(
    AppProjectSession& session,
    const std::string& clipId)
{
    const auto targetClip = session.project().findClipById(clipId);
    if (!targetClip.has_value()) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::MissingClip,
            "无法添加 MIDI 音符：目标片段不存在。");
    }

    if (targetClip->type != ClipType::Midi) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::IncompatibleClipType,
            "无法添加 MIDI 音符：只能向 MIDI 片段添加音符。");
    }

    const auto startTick = nextNoteStartTick(*targetClip);
    if (!canFitDefaultNoteAt(*targetClip, startTick)) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::ClipFull,
            "无法添加 MIDI 音符：当前片段已经没有足够空间容纳默认音符。");
    }

    // 失败路径必须在这里之前返回；editProject 会把会话标记为 dirty。
    auto createdNote = session.editProject().createMidiNote(
        clipId,
        startTick,
        defaultAppMidiNoteLengthTick,
        defaultAppMidiNoteNumber,
        defaultAppMidiNoteVelocity,
        defaultAppMidiNoteChannel);

    if (!createdNote.has_value()) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::CreateFailed,
            "无法添加 MIDI 音符：工程模型拒绝了这次音符创建。");
    }

    return successFeedback(*createdNote);
}

}
