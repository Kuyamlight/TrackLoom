#include "AppMidiNoteActions.h"

#include "Command.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

AppMidiNoteActionFeedback deleteSuccessFeedback(const MidiNoteEvent& note)
{
    AppMidiNoteActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppMidiNoteActionFeedbackKind::Success;
    feedback.noteId = note.id;
    feedback.message = "已删除末尾 MIDI 音符。";
    return feedback;
}

AppMidiNoteActionFeedback duplicateSuccessFeedback(const MidiNoteEvent& note)
{
    AppMidiNoteActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppMidiNoteActionFeedbackKind::Success;
    feedback.noteId = note.id;
    feedback.message = "已复制末尾 MIDI 音符。";
    return feedback;
}

AppMidiNoteActionFeedback pitchSuccessFeedback(const MidiNoteEvent& note, const std::string& directionLabel)
{
    AppMidiNoteActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppMidiNoteActionFeedbackKind::Success;
    feedback.noteId = note.id;
    feedback.message = "已" + directionLabel + "末尾 MIDI 音符。";
    return feedback;
}

AppMidiNoteActionFeedback velocitySuccessFeedback(const MidiNoteEvent& note, const std::string& directionLabel)
{
    AppMidiNoteActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppMidiNoteActionFeedbackKind::Success;
    feedback.noteId = note.id;
    feedback.message = "已" + directionLabel + "末尾 MIDI 音符力度。";
    return feedback;
}

AppMidiNoteActionFeedback lengthSuccessFeedback(const MidiNoteEvent& note, const std::string& directionLabel)
{
    AppMidiNoteActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppMidiNoteActionFeedbackKind::Success;
    feedback.noteId = note.id;
    feedback.message = "已" + directionLabel + "末尾 MIDI 音符长度。";
    return feedback;
}

AppMidiNoteActionFeedback timingSuccessFeedback(const MidiNoteEvent& note, const std::string& directionLabel)
{
    AppMidiNoteActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppMidiNoteActionFeedbackKind::Success;
    feedback.noteId = note.id;
    feedback.message = "已" + directionLabel + "末尾 MIDI 音符起点。";
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

std::vector<std::string> currentMidiNoteIds(const TimelineClip& clip)
{
    std::vector<std::string> ids;
    ids.reserve(clip.midiNotes.size());
    for (const auto& note : clip.midiNotes) {
        ids.push_back(note.id);
    }
    return ids;
}

bool containsNoteId(const std::vector<std::string>& ids, const std::string& noteId)
{
    return std::find(ids.begin(), ids.end(), noteId) != ids.end();
}

std::optional<MidiNoteEvent> findNoteCreatedAfterCommand(
    const Project& project,
    const std::string& clipId,
    const std::vector<std::string>& previousNoteIds)
{
    const auto clip = project.findClipById(clipId);
    if (!clip.has_value()) {
        return std::nullopt;
    }

    for (const auto& note : clip->midiNotes) {
        if (!containsNoteId(previousNoteIds, note.id)) {
            return note;
        }
    }

    return std::nullopt;
}

const MidiNoteEvent& lastNoteInTimelineOrder(const TimelineClip& clip)
{
    return *std::max_element(
        clip.midiNotes.begin(),
        clip.midiNotes.end(),
        [](const MidiNoteEvent& left, const MidiNoteEvent& right) {
            const auto leftEndTick = left.startTick + left.lengthTick;
            const auto rightEndTick = right.startTick + right.lengthTick;
            if (leftEndTick != rightEndTick) {
                return leftEndTick < rightEndTick;
            }

            return left.startTick < right.startTick;
        });
}

AppMidiNoteActionFeedback transposeLastMidiNotePitchInClip(
    AppProjectSession& session,
    const std::string& clipId,
    int semitoneDelta,
    const std::string& directionLabel)
{
    const auto targetClip = session.project().findClipById(clipId);
    if (!targetClip.has_value()) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::MissingClip,
            "无法调整 MIDI 音符音高：目标片段不存在。");
    }

    if (targetClip->type != ClipType::Midi) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::IncompatibleClipType,
            "无法调整 MIDI 音符音高：只能编辑 MIDI 片段里的音符。");
    }

    if (targetClip->midiNotes.empty()) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::EmptyClip,
            "无法调整 MIDI 音符音高：当前片段里还没有音符。");
    }

    const auto noteToEdit = lastNoteInTimelineOrder(*targetClip);
    const auto newNoteNumber = noteToEdit.noteNumber + semitoneDelta;
    if (newNoteNumber < 0 || newNoteNumber > 127) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::PitchFailed,
            "无法调整 MIDI 音符音高：目标音高超出 MIDI 0-127 范围。");
    }

    // 所有可预见失败都在命令执行前处理；命令成功后才会进入撤销历史并标脏工程。
    const auto result = session.executeProjectCommand(
        std::make_unique<SetMidiNotePitchCommand>(noteToEdit.id, newNoteNumber));
    if (!result.success) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::PitchFailed,
            "无法调整 MIDI 音符音高：工程模型拒绝了这次音高修改。");
    }

    return pitchSuccessFeedback(noteToEdit, directionLabel);
}

AppMidiNoteActionFeedback adjustLastMidiNoteVelocityInClip(
    AppProjectSession& session,
    const std::string& clipId,
    int velocityDelta,
    const std::string& directionLabel)
{
    const auto targetClip = session.project().findClipById(clipId);
    if (!targetClip.has_value()) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::MissingClip,
            "无法调整 MIDI 音符力度：目标片段不存在。");
    }

    if (targetClip->type != ClipType::Midi) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::IncompatibleClipType,
            "无法调整 MIDI 音符力度：只能编辑 MIDI 片段里的音符。");
    }

    if (targetClip->midiNotes.empty()) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::EmptyClip,
            "无法调整 MIDI 音符力度：当前片段里还没有音符。");
    }

    const auto noteToEdit = lastNoteInTimelineOrder(*targetClip);
    const auto newVelocity = noteToEdit.velocity + velocityDelta;
    if (newVelocity < 1 || newVelocity > 127) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::VelocityFailed,
            "无法调整 MIDI 音符力度：目标力度超出 MIDI 1-127 范围。");
    }

    // velocity 0 不保存为发声音符；边界通过后才执行命令，避免失败误标 dirty。
    const auto result = session.executeProjectCommand(
        std::make_unique<SetMidiNoteVelocityCommand>(noteToEdit.id, newVelocity));
    if (!result.success) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::VelocityFailed,
            "无法调整 MIDI 音符力度：工程模型拒绝了这次力度修改。");
    }

    return velocitySuccessFeedback(noteToEdit, directionLabel);
}

AppMidiNoteActionFeedback adjustLastMidiNoteLengthInClip(
    AppProjectSession& session,
    const std::string& clipId,
    std::int64_t lengthDeltaTick,
    const std::string& directionLabel)
{
    const auto targetClip = session.project().findClipById(clipId);
    if (!targetClip.has_value()) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::MissingClip,
            "无法调整 MIDI 音符长度：目标片段不存在。");
    }

    if (targetClip->type != ClipType::Midi) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::IncompatibleClipType,
            "无法调整 MIDI 音符长度：只能编辑 MIDI 片段里的音符。");
    }

    if (targetClip->midiNotes.empty()) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::EmptyClip,
            "无法调整 MIDI 音符长度：当前片段里还没有音符。");
    }

    const auto noteToEdit = lastNoteInTimelineOrder(*targetClip);
    const auto newLengthTick = noteToEdit.lengthTick + lengthDeltaTick;
    if (newLengthTick < defaultAppMidiNoteLengthStepTick) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::LengthFailed,
            "无法调整 MIDI 音符长度：音符不能短于十六分音符。");
    }

    if (noteToEdit.startTick > targetClip->lengthTick - newLengthTick) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::LengthFailed,
            "无法调整 MIDI 音符长度：音符右边界不能超出片段。");
    }

    // 长度微调不移动音符起点；命令记录旧时间，撤销时恢复原起点和长度。
    const auto result = session.executeProjectCommand(
        std::make_unique<SetMidiNoteTimingCommand>(noteToEdit.id, noteToEdit.startTick, newLengthTick));
    if (!result.success) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::LengthFailed,
            "无法调整 MIDI 音符长度：工程模型拒绝了这次长度修改。");
    }

    return lengthSuccessFeedback(noteToEdit, directionLabel);
}

AppMidiNoteActionFeedback moveLastMidiNoteStartInClip(
    AppProjectSession& session,
    const std::string& clipId,
    std::int64_t startDeltaTick,
    const std::string& directionLabel)
{
    const auto targetClip = session.project().findClipById(clipId);
    if (!targetClip.has_value()) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::MissingClip,
            "无法移动 MIDI 音符起点：目标片段不存在。");
    }

    if (targetClip->type != ClipType::Midi) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::IncompatibleClipType,
            "无法移动 MIDI 音符起点：只能编辑 MIDI 片段里的音符。");
    }

    if (targetClip->midiNotes.empty()) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::EmptyClip,
            "无法移动 MIDI 音符起点：当前片段里还没有音符。");
    }

    const auto noteToEdit = lastNoteInTimelineOrder(*targetClip);
    const auto newStartTick = noteToEdit.startTick + startDeltaTick;
    if (newStartTick < 0) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::TimingFailed,
            "无法移动 MIDI 音符起点：音符起点不能早于片段开头。");
    }

    if (noteToEdit.lengthTick > targetClip->lengthTick
        || newStartTick > targetClip->lengthTick - noteToEdit.lengthTick) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::TimingFailed,
            "无法移动 MIDI 音符起点：音符右边界不能超出片段。");
    }

    // 起点微调只改变音符在片段内的位置；命令会保留旧时间用于撤销。
    const auto result = session.executeProjectCommand(
        std::make_unique<SetMidiNoteTimingCommand>(noteToEdit.id, newStartTick, noteToEdit.lengthTick));
    if (!result.success) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::TimingFailed,
            "无法移动 MIDI 音符起点：工程模型拒绝了这次时间修改。");
    }

    return timingSuccessFeedback(noteToEdit, directionLabel);
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

    const auto previousNoteIds = currentMidiNoteIds(*targetClip);
    // 音符创建必须走命令历史；这样撤销/重做才能恢复同一个稳定 note id。
    const auto result = session.executeProjectCommand(
        std::make_unique<AddMidiNoteCommand>(
            clipId,
            startTick,
            defaultAppMidiNoteLengthTick,
            defaultAppMidiNoteNumber,
            defaultAppMidiNoteVelocity,
            defaultAppMidiNoteChannel));
    const auto createdNote = findNoteCreatedAfterCommand(session.project(), clipId, previousNoteIds);
    if (!result.success || !createdNote.has_value()) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::CreateFailed,
            "无法添加 MIDI 音符：工程模型拒绝了这次音符创建。");
    }

    return successFeedback(*createdNote);
}

AppMidiNoteActionFeedback deleteLastMidiNoteInClip(
    AppProjectSession& session,
    const std::string& clipId)
{
    const auto targetClip = session.project().findClipById(clipId);
    if (!targetClip.has_value()) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::MissingClip,
            "无法删除 MIDI 音符：目标片段不存在。");
    }

    if (targetClip->type != ClipType::Midi) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::IncompatibleClipType,
            "无法删除 MIDI 音符：只能从 MIDI 片段删除音符。");
    }

    if (targetClip->midiNotes.empty()) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::EmptyClip,
            "无法删除 MIDI 音符：当前片段里还没有音符。");
    }

    const auto noteToDelete = lastNoteInTimelineOrder(*targetClip);

    // 删除必须走命令历史；撤销才能恢复原 note id、时间和 MIDI 属性。
    const auto result = session.executeProjectCommand(
        std::make_unique<DeleteMidiNoteCommand>(noteToDelete.id));
    if (!result.success) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::DeleteFailed,
            "无法删除 MIDI 音符：工程模型拒绝了这次删除。");
    }

    return deleteSuccessFeedback(noteToDelete);
}

AppMidiNoteActionFeedback duplicateLastMidiNoteInClip(
    AppProjectSession& session,
    const std::string& clipId)
{
    const auto targetClip = session.project().findClipById(clipId);
    if (!targetClip.has_value()) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::MissingClip,
            "无法复制 MIDI 音符：目标片段不存在。");
    }

    if (targetClip->type != ClipType::Midi) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::IncompatibleClipType,
            "无法复制 MIDI 音符：只能从 MIDI 片段复制音符。");
    }

    if (targetClip->midiNotes.empty()) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::EmptyClip,
            "无法复制 MIDI 音符：当前片段里还没有音符。");
    }

    const auto noteToDuplicate = lastNoteInTimelineOrder(*targetClip);
    const auto newStartTick = noteToDuplicate.startTick + noteToDuplicate.lengthTick;
    if (noteToDuplicate.lengthTick > targetClip->lengthTick
        || newStartTick > targetClip->lengthTick - noteToDuplicate.lengthTick) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::ClipFull,
            "无法复制 MIDI 音符：源音符之后没有足够空间容纳副本。");
    }

    const auto previousNoteIds = currentMidiNoteIds(*targetClip);
    // 副本必须获得新 note id；命令历史会在重做时恢复同一个副本 id。
    const auto result = session.executeProjectCommand(
        std::make_unique<AddMidiNoteCommand>(
            clipId,
            newStartTick,
            noteToDuplicate.lengthTick,
            noteToDuplicate.noteNumber,
            noteToDuplicate.velocity,
            noteToDuplicate.channel));
    const auto copiedNote = findNoteCreatedAfterCommand(session.project(), clipId, previousNoteIds);
    if (!result.success || !copiedNote.has_value()) {
        return failureFeedback(
            AppMidiNoteActionFeedbackKind::CreateFailed,
            "无法复制 MIDI 音符：工程模型拒绝了这次音符创建。");
    }

    return duplicateSuccessFeedback(*copiedNote);
}

AppMidiNoteActionFeedback raiseLastMidiNotePitchInClip(
    AppProjectSession& session,
    const std::string& clipId)
{
    return transposeLastMidiNotePitchInClip(session, clipId, 1, "升高");
}

AppMidiNoteActionFeedback lowerLastMidiNotePitchInClip(
    AppProjectSession& session,
    const std::string& clipId)
{
    return transposeLastMidiNotePitchInClip(session, clipId, -1, "降低");
}

AppMidiNoteActionFeedback increaseLastMidiNoteVelocityInClip(
    AppProjectSession& session,
    const std::string& clipId)
{
    return adjustLastMidiNoteVelocityInClip(session, clipId, defaultAppMidiNoteVelocityStep, "增强");
}

AppMidiNoteActionFeedback decreaseLastMidiNoteVelocityInClip(
    AppProjectSession& session,
    const std::string& clipId)
{
    return adjustLastMidiNoteVelocityInClip(session, clipId, -defaultAppMidiNoteVelocityStep, "减弱");
}

AppMidiNoteActionFeedback lengthenLastMidiNoteInClip(
    AppProjectSession& session,
    const std::string& clipId)
{
    return adjustLastMidiNoteLengthInClip(session, clipId, defaultAppMidiNoteLengthStepTick, "延长");
}

AppMidiNoteActionFeedback shortenLastMidiNoteInClip(
    AppProjectSession& session,
    const std::string& clipId)
{
    return adjustLastMidiNoteLengthInClip(session, clipId, -defaultAppMidiNoteLengthStepTick, "缩短");
}

AppMidiNoteActionFeedback moveLastMidiNoteStartEarlierInClip(
    AppProjectSession& session,
    const std::string& clipId)
{
    return moveLastMidiNoteStartInClip(session, clipId, -defaultAppMidiNoteLengthStepTick, "左移");
}

AppMidiNoteActionFeedback moveLastMidiNoteStartLaterInClip(
    AppProjectSession& session,
    const std::string& clipId)
{
    return moveLastMidiNoteStartInClip(session, clipId, defaultAppMidiNoteLengthStepTick, "右移");
}

}
