#include "AppMidiClipActions.h"

#include "Command.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace trackloom {
namespace {

AppMidiClipActionFeedback successFeedback(const TimelineClip& clip)
{
    AppMidiClipActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppMidiClipActionFeedbackKind::Success;
    feedback.clipId = clip.id;
    feedback.message = "已创建 MIDI 片段：" + clip.name + "。";
    return feedback;
}

AppMidiClipActionFeedback deleteSuccessFeedback(const TimelineClip& clip)
{
    AppMidiClipActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppMidiClipActionFeedbackKind::Success;
    feedback.clipId = clip.id;
    feedback.message = "已删除 MIDI 片段：" + clip.name + "。";
    return feedback;
}

AppMidiClipActionFeedback duplicateSuccessFeedback(const TimelineClip& clip)
{
    AppMidiClipActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppMidiClipActionFeedbackKind::Success;
    feedback.clipId = clip.id;
    feedback.message = "已复制 MIDI 片段：" + clip.name + "。";
    return feedback;
}

AppMidiClipActionFeedback renameSuccessFeedback(const TimelineClip& clip, const std::string& newName)
{
    AppMidiClipActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppMidiClipActionFeedbackKind::Success;
    feedback.clipId = clip.id;
    feedback.message = "已重命名 MIDI 片段：" + clip.name + " -> " + newName + "。";
    return feedback;
}

AppMidiClipActionFeedback splitSuccessFeedback(const TimelineClip& rightClip)
{
    AppMidiClipActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppMidiClipActionFeedbackKind::Success;
    feedback.clipId = rightClip.id;
    feedback.message = "已拆分 MIDI 片段：" + rightClip.name + "。";
    return feedback;
}

AppMidiClipActionFeedback moveSuccessFeedback(const TimelineClip& clip, const std::string& directionLabel)
{
    AppMidiClipActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppMidiClipActionFeedbackKind::Success;
    feedback.clipId = clip.id;
    feedback.message = "已" + directionLabel + " MIDI 片段：" + clip.name + "。";
    return feedback;
}

AppMidiClipActionFeedback moveToTrackSuccessFeedback(const TimelineClip& clip, const Track& targetTrack)
{
    AppMidiClipActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppMidiClipActionFeedbackKind::Success;
    feedback.clipId = clip.id;
    feedback.message = "已移动 MIDI 片段到轨道：" + clip.name + " -> " + targetTrack.name + "。";
    return feedback;
}

AppMidiClipActionFeedback trimEndSuccessFeedback(const TimelineClip& clip)
{
    AppMidiClipActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppMidiClipActionFeedbackKind::Success;
    feedback.clipId = clip.id;
    feedback.message = "已缩短 MIDI 片段片尾：" + clip.name + "。";
    return feedback;
}

AppMidiClipActionFeedback extendEndSuccessFeedback(const TimelineClip& clip)
{
    AppMidiClipActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppMidiClipActionFeedbackKind::Success;
    feedback.clipId = clip.id;
    feedback.message = "已延长 MIDI 片段片尾：" + clip.name + "。";
    return feedback;
}

AppMidiClipActionFeedback trimStartSuccessFeedback(const TimelineClip& clip)
{
    AppMidiClipActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppMidiClipActionFeedbackKind::Success;
    feedback.clipId = clip.id;
    feedback.message = "已缩短 MIDI 片段片头：" + clip.name + "。";
    return feedback;
}

AppMidiClipActionFeedback extendStartSuccessFeedback(const TimelineClip& clip)
{
    AppMidiClipActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppMidiClipActionFeedbackKind::Success;
    feedback.clipId = clip.id;
    feedback.message = "已延长 MIDI 片段片头：" + clip.name + "。";
    return feedback;
}

AppMidiClipActionFeedback failureFeedback(
    AppMidiClipActionFeedbackKind kind,
    std::string message)
{
    AppMidiClipActionFeedback feedback;
    feedback.success = false;
    feedback.kind = kind;
    feedback.message = std::move(message);
    return feedback;
}

std::int64_t nextClipStartTickForTrack(const Project& project, const std::string& trackId)
{
    std::int64_t nextStartTick = 0;

    for (const auto& clip : project.clips()) {
        if (clip.trackId == trackId) {
            nextStartTick = std::max(nextStartTick, clip.startTick + clip.lengthTick);
        }
    }

    return nextStartTick;
}

std::size_t clipCountForTrack(const Project& project, const std::string& trackId)
{
    std::size_t count = 0;

    for (const auto& clip : project.clips()) {
        if (clip.trackId == trackId) {
            ++count;
        }
    }

    return count;
}

std::vector<std::string> currentClipIds(const Project& project)
{
    std::vector<std::string> ids;
    ids.reserve(project.clips().size());
    for (const auto& clip : project.clips()) {
        ids.push_back(clip.id);
    }

    return ids;
}

bool containsClipId(const std::vector<std::string>& ids, const std::string& clipId)
{
    for (const auto& id : ids) {
        if (id == clipId) {
            return true;
        }
    }

    return false;
}

std::optional<TimelineClip> findClipCreatedAfterCommand(
    const Project& project,
    const std::vector<std::string>& previousClipIds)
{
    for (const auto& clip : project.clips()) {
        if (!containsClipId(previousClipIds, clip.id)) {
            return clip;
        }
    }

    return std::nullopt;
}

std::string trimClipName(std::string name)
{
    // 文本框输入可能带入首尾空白；保存前统一清理，避免出现肉眼难以分辨的片段名。
    const auto first = name.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }

    const auto last = name.find_last_not_of(" \t\r\n");
    return name.substr(first, last - first + 1);
}

bool midiNotesCanSplitAtOffset(const TimelineClip& clip, std::int64_t splitOffset)
{
    for (const auto& note : clip.midiNotes) {
        const auto noteEndTick = note.startTick + note.lengthTick;
        // splitOffset 是片段内部的相对 tick；完全落在左侧或右侧的音符可以原样保留或移动。
        if (noteEndTick <= splitOffset || note.startTick >= splitOffset) {
            continue;
        }

        // 跨过切点的音符需要后续明确“切断、延长或保持”的规则；当前先拒绝。
        return false;
    }

    return true;
}

bool midiNotesFitClipLength(const TimelineClip& clip, std::int64_t lengthTick)
{
    for (const auto& note : clip.midiNotes) {
        // MIDI 音符保存为片段内相对 tick；片尾缩短后，任何超出新长度的音符都会被保留策略阻止。
        if (note.startTick + note.lengthTick > lengthTick) {
            return false;
        }
    }

    return true;
}

struct MidiNoteTimingUpdate {
    std::string noteId;
    std::int64_t startTick = 0;
    std::int64_t lengthTick = 0;
};

bool noteTimingFitsLength(std::int64_t startTick, std::int64_t lengthTick, std::int64_t clipLengthTick)
{
    return startTick >= 0
        && lengthTick > 0
        && clipLengthTick > 0
        && startTick <= clipLengthTick - lengthTick;
}

bool canAddTickOffset(std::int64_t startTick, std::int64_t offsetTick)
{
    if (offsetTick > 0) {
        return startTick <= std::numeric_limits<std::int64_t>::max() - offsetTick;
    }

    if (offsetTick < 0) {
        return startTick >= std::numeric_limits<std::int64_t>::min() - offsetTick;
    }

    return true;
}

AppMidiClipActionFeedback moveMidiClipByTickOffset(
    AppProjectSession& session,
    const std::string& clipId,
    std::int64_t offsetTick,
    const std::string& directionLabel)
{
    // 先用只读快照完成校验；移动失败时不能把未修改工程误标为 dirty。
    const auto targetClip = session.project().findClipById(clipId);
    if (!targetClip.has_value()) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::MissingClip,
            "无法移动 MIDI 片段：目标片段不存在。");
    }

    if (targetClip->type != ClipType::Midi) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::IncompatibleClipType,
            "无法移动 MIDI 片段：只能移动 MIDI 片段。");
    }

    const auto targetTrack = session.project().findTrackById(targetClip->trackId);
    if (!targetTrack.has_value()) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::MissingTrack,
            "无法移动 MIDI 片段：片段所属轨道不存在。");
    }

    if (targetTrack->type != TrackType::Instrument) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::IncompatibleTrackType,
            "无法移动 MIDI 片段：MIDI 片段只能停留在乐器轨。");
    }

    if (!canAddTickOffset(targetClip->startTick, offsetTick)) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::MoveFailed,
            "无法移动 MIDI 片段：目标位置超出时间线范围。");
    }

    const auto newStartTick = targetClip->startTick + offsetTick;
    if (newStartTick < 0) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::MoveFailed,
            "无法左移 MIDI 片段：片段不能移动到时间线起点之前。");
    }

    // 当前移动只改变片段外壳起点，不改片段长度，也不改 MIDI 音符在片段内的相对 tick。
    if (!session.editProject().setClipTiming(clipId, newStartTick, targetClip->lengthTick)) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::MoveFailed,
            "无法移动 MIDI 片段：工程模型拒绝了这次移动。");
    }

    return moveSuccessFeedback(*targetClip, directionLabel);
}

AppMidiClipActionFeedback moveMidiClipToTrackImpl(
    AppProjectSession& session,
    const std::string& clipId,
    const std::string& targetTrackId)
{
    // 跨轨移动只改片段归属；失败路径必须先读快照，避免同轨或无效目标把工程误标为 dirty。
    const auto targetClip = session.project().findClipById(clipId);
    if (!targetClip.has_value()) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::MissingClip,
            "无法移动 MIDI 片段到目标轨：目标片段不存在。");
    }

    if (targetClip->type != ClipType::Midi) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::IncompatibleClipType,
            "无法移动 MIDI 片段到目标轨：只能移动 MIDI 片段。");
    }

    const auto sourceTrack = session.project().findTrackById(targetClip->trackId);
    if (!sourceTrack.has_value()) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::MissingTrack,
            "无法移动 MIDI 片段到目标轨：片段所属轨道不存在。");
    }

    if (sourceTrack->type != TrackType::Instrument) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::IncompatibleTrackType,
            "无法移动 MIDI 片段到目标轨：源轨道不是乐器轨。");
    }

    const auto targetTrack = session.project().findTrackById(targetTrackId);
    if (!targetTrack.has_value()) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::MissingTrack,
            "无法移动 MIDI 片段到目标轨：目标轨道不存在。");
    }

    if (targetTrack->type != TrackType::Instrument) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::IncompatibleTrackType,
            "无法移动 MIDI 片段到目标轨：目标轨道必须是乐器轨。");
    }

    if (targetClip->trackId == targetTrackId) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::MoveFailed,
            "无法移动 MIDI 片段到目标轨：目标轨道与当前轨道相同。");
    }

    // 当前只做轨道归属切换；重叠处理、跨类型转换和批量移动属于后续时间线编辑器。
    if (!session.editProject().moveClipToTrack(clipId, targetTrackId)) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::MoveFailed,
            "无法移动 MIDI 片段到目标轨：工程模型拒绝了这次移动。");
    }

    return moveToTrackSuccessFeedback(*targetClip, *targetTrack);
}

AppMidiClipActionFeedback trimMidiClipEndByTickOffset(
    AppProjectSession& session,
    const std::string& clipId,
    std::int64_t offsetTick)
{
    // 只读快照先完成所有可预见失败校验；这样失败路径不会触碰 editProject()。
    const auto targetClip = session.project().findClipById(clipId);
    if (!targetClip.has_value()) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::MissingClip,
            "无法缩短 MIDI 片段片尾：目标片段不存在。");
    }

    if (targetClip->type != ClipType::Midi) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::IncompatibleClipType,
            "无法缩短 MIDI 片段片尾：只能修剪 MIDI 片段。");
    }

    const auto targetTrack = session.project().findTrackById(targetClip->trackId);
    if (!targetTrack.has_value()) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::MissingTrack,
            "无法缩短 MIDI 片段片尾：片段所属轨道不存在。");
    }

    if (targetTrack->type != TrackType::Instrument) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::IncompatibleTrackType,
            "无法缩短 MIDI 片段片尾：MIDI 片段只能停留在乐器轨。");
    }

    if (targetClip->lengthTick <= offsetTick) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::TrimFailed,
            "无法缩短 MIDI 片段片尾：片段长度不足一拍。");
    }

    const auto newLengthTick = targetClip->lengthTick - offsetTick;
    if (!midiNotesFitClipLength(*targetClip, newLengthTick)) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::TrimFailed,
            "无法缩短 MIDI 片段片尾：缩短后会截掉已有音符。");
    }

    if (!canAddTickOffset(targetClip->startTick, newLengthTick)) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::TrimFailed,
            "无法缩短 MIDI 片段片尾：目标片尾超出时间线范围。");
    }

    const auto newEndTick = targetClip->startTick + newLengthTick;
    // 当前只做右边界向内修剪；不创建素材偏移，也不改变 MIDI 音符相对 tick。
    if (!session.editProject().trimClipEndToTick(clipId, newEndTick)) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::TrimFailed,
            "无法缩短 MIDI 片段片尾：工程模型拒绝了这次修剪。");
    }

    return trimEndSuccessFeedback(*targetClip);
}

AppMidiClipActionFeedback extendMidiClipEndByTickOffset(
    AppProjectSession& session,
    const std::string& clipId,
    std::int64_t offsetTick)
{
    // 片尾延长只增加空白音乐时间；所有校验先读快照，失败时不污染 dirty 状态。
    const auto targetClip = session.project().findClipById(clipId);
    if (!targetClip.has_value()) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::MissingClip,
            "无法延长 MIDI 片段片尾：目标片段不存在。");
    }

    if (targetClip->type != ClipType::Midi) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::IncompatibleClipType,
            "无法延长 MIDI 片段片尾：只能延长 MIDI 片段。");
    }

    const auto targetTrack = session.project().findTrackById(targetClip->trackId);
    if (!targetTrack.has_value()) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::MissingTrack,
            "无法延长 MIDI 片段片尾：片段所属轨道不存在。");
    }

    if (targetTrack->type != TrackType::Instrument) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::IncompatibleTrackType,
            "无法延长 MIDI 片段片尾：MIDI 片段只能停留在乐器轨。");
    }

    if (offsetTick <= 0 || !canAddTickOffset(targetClip->lengthTick, offsetTick)) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::ExtendFailed,
            "无法延长 MIDI 片段片尾：目标长度超出时间线范围。");
    }

    const auto newLengthTick = targetClip->lengthTick + offsetTick;
    if (!canAddTickOffset(targetClip->startTick, newLengthTick)) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::ExtendFailed,
            "无法延长 MIDI 片段片尾：目标片尾超出时间线范围。");
    }

    // 只移动右边界；MIDI 音符仍然使用原来的片段内相对 tick，声音内容不被平移。
    if (!session.editProject().setClipTiming(clipId, targetClip->startTick, newLengthTick)) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::ExtendFailed,
            "无法延长 MIDI 片段片尾：工程模型拒绝了这次延长。");
    }

    return extendEndSuccessFeedback(*targetClip);
}

AppMidiClipActionFeedback trimMidiClipStartByTickOffset(
    AppProjectSession& session,
    const std::string& clipId,
    std::int64_t offsetTick)
{
    // 片头缩短会改变片段内坐标系；这里先完整预演，避免失败路径留下半截音符移动。
    const auto targetClip = session.project().findClipById(clipId);
    if (!targetClip.has_value()) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::MissingClip,
            "无法缩短 MIDI 片段片头：目标片段不存在。");
    }

    if (targetClip->type != ClipType::Midi) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::IncompatibleClipType,
            "无法缩短 MIDI 片段片头：只能修剪 MIDI 片段。");
    }

    const auto targetTrack = session.project().findTrackById(targetClip->trackId);
    if (!targetTrack.has_value()) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::MissingTrack,
            "无法缩短 MIDI 片段片头：片段所属轨道不存在。");
    }

    if (targetTrack->type != TrackType::Instrument) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::IncompatibleTrackType,
            "无法缩短 MIDI 片段片头：MIDI 片段只能停留在乐器轨。");
    }

    if (offsetTick <= 0 || targetClip->lengthTick <= offsetTick || !canAddTickOffset(targetClip->startTick, offsetTick)) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::TrimFailed,
            "无法缩短 MIDI 片段片头：片段长度不足一拍。");
    }

    const auto newStartTick = targetClip->startTick + offsetTick;
    const auto newLengthTick = targetClip->lengthTick - offsetTick;
    std::vector<MidiNoteTimingUpdate> noteUpdates;
    noteUpdates.reserve(targetClip->midiNotes.size());

    for (const auto& note : targetClip->midiNotes) {
        if (note.startTick < offsetTick) {
            return failureFeedback(
                AppMidiClipActionFeedbackKind::TrimFailed,
                "无法缩短 MIDI 片段片头：缩短后会截掉已有音符。");
        }

        const auto shiftedStartTick = note.startTick - offsetTick;
        if (!noteTimingFitsLength(shiftedStartTick, note.lengthTick, newLengthTick)) {
            return failureFeedback(
                AppMidiClipActionFeedbackKind::TrimFailed,
                "无法缩短 MIDI 片段片头：音符移动后超出片段范围。");
        }

        noteUpdates.push_back({ note.id, shiftedStartTick, note.lengthTick });
    }

    auto& project = session.editProject();
    for (const auto& update : noteUpdates) {
        if (!project.setMidiNoteTiming(update.noteId, update.startTick, update.lengthTick)) {
            return failureFeedback(
                AppMidiClipActionFeedbackKind::TrimFailed,
                "无法缩短 MIDI 片段片头：工程模型拒绝了音符时间调整。");
        }
    }

    if (!project.setClipTiming(clipId, newStartTick, newLengthTick)) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::TrimFailed,
            "无法缩短 MIDI 片段片头：工程模型拒绝了这次修剪。");
    }

    return trimStartSuccessFeedback(*targetClip);
}

AppMidiClipActionFeedback extendMidiClipStartByTickOffset(
    AppProjectSession& session,
    const std::string& clipId,
    std::int64_t offsetTick)
{
    // 片头延长会在左侧增加空白时间；已有音符相对 tick 右移，绝对播放时间保持不变。
    const auto targetClip = session.project().findClipById(clipId);
    if (!targetClip.has_value()) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::MissingClip,
            "无法延长 MIDI 片段片头：目标片段不存在。");
    }

    if (targetClip->type != ClipType::Midi) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::IncompatibleClipType,
            "无法延长 MIDI 片段片头：只能延长 MIDI 片段。");
    }

    const auto targetTrack = session.project().findTrackById(targetClip->trackId);
    if (!targetTrack.has_value()) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::MissingTrack,
            "无法延长 MIDI 片段片头：片段所属轨道不存在。");
    }

    if (targetTrack->type != TrackType::Instrument) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::IncompatibleTrackType,
            "无法延长 MIDI 片段片头：MIDI 片段只能停留在乐器轨。");
    }

    if (offsetTick <= 0
        || targetClip->startTick < offsetTick
        || !canAddTickOffset(targetClip->lengthTick, offsetTick)) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::ExtendFailed,
            "无法延长 MIDI 片段片头：目标片头超出时间线范围。");
    }

    const auto newStartTick = targetClip->startTick - offsetTick;
    const auto newLengthTick = targetClip->lengthTick + offsetTick;
    std::vector<MidiNoteTimingUpdate> noteUpdates;
    noteUpdates.reserve(targetClip->midiNotes.size());

    for (const auto& note : targetClip->midiNotes) {
        if (!canAddTickOffset(note.startTick, offsetTick)) {
            return failureFeedback(
                AppMidiClipActionFeedbackKind::ExtendFailed,
                "无法延长 MIDI 片段片头：音符时间超出可表示范围。");
        }

        const auto shiftedStartTick = note.startTick + offsetTick;
        if (!noteTimingFitsLength(shiftedStartTick, note.lengthTick, newLengthTick)) {
            return failureFeedback(
                AppMidiClipActionFeedbackKind::ExtendFailed,
                "无法延长 MIDI 片段片头：音符移动后超出片段范围。");
        }

        noteUpdates.push_back({ note.id, shiftedStartTick, note.lengthTick });
    }

    auto& project = session.editProject();
    if (!project.setClipTiming(clipId, newStartTick, newLengthTick)) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::ExtendFailed,
            "无法延长 MIDI 片段片头：工程模型拒绝了这次延长。");
    }

    for (const auto& update : noteUpdates) {
        if (!project.setMidiNoteTiming(update.noteId, update.startTick, update.lengthTick)) {
            return failureFeedback(
                AppMidiClipActionFeedbackKind::ExtendFailed,
                "无法延长 MIDI 片段片头：工程模型拒绝了音符时间调整。");
        }
    }

    return extendStartSuccessFeedback(*targetClip);
}

}

AppMidiClipActionFeedback createDefaultMidiClipOnTrack(
    AppProjectSession& session,
    const std::string& trackId)
{
    const auto targetTrack = session.project().findTrackById(trackId);
    if (!targetTrack.has_value()) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::MissingTrack,
            "无法创建 MIDI 片段：目标轨道不存在。");
    }

    if (targetTrack->type != TrackType::Instrument) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::IncompatibleTrackType,
            "无法创建 MIDI 片段：只能在乐器轨上创建 MIDI 片段。");
    }

    const auto startTick = nextClipStartTickForTrack(session.project(), trackId);
    const auto clipNumber = clipCountForTrack(session.project(), trackId) + 1;
    const auto clipName = targetTrack->name + " MIDI " + std::to_string(clipNumber);
    const auto previousClipIds = currentClipIds(session.project());

    // 片段创建是真正的工程编辑；通过核心命令执行，撤销/重做才能恢复同一个稳定 clip id。
    const auto result = session.executeProjectCommand(
        std::make_unique<AddClipCommand>(
            trackId,
            clipName,
            ClipType::Midi,
            startTick,
            defaultAppMidiClipLengthTick));
    const auto createdClip = findClipCreatedAfterCommand(session.project(), previousClipIds);
    if (!result.success || !createdClip.has_value()) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::CreateFailed,
            "无法创建 MIDI 片段：工程模型拒绝了这次片段创建。");
    }

    return successFeedback(*createdClip);
}

AppMidiClipActionFeedback deleteMidiClipById(
    AppProjectSession& session,
    const std::string& clipId)
{
    // 先读取只读快照做应用层校验，避免失败路径调用 editProject() 后误标 dirty。
    const auto targetClip = session.project().findClipById(clipId);
    if (!targetClip.has_value()) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::MissingClip,
            "无法删除 MIDI 片段：目标片段不存在。");
    }

    if (targetClip->type != ClipType::Midi) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::IncompatibleClipType,
            "无法删除 MIDI 片段：只能删除 MIDI 片段。");
    }

    // 删除前所有校验都已完成；真正删除时走核心命令，片段和内部音符才能被撤销恢复。
    const auto result = session.executeProjectCommand(
        std::make_unique<DeleteClipCommand>(clipId));
    if (!result.success) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::DeleteFailed,
            "无法删除 MIDI 片段：工程模型拒绝了这次删除。");
    }

    return deleteSuccessFeedback(*targetClip);
}

AppMidiClipActionFeedback duplicateMidiClipAfterItself(
    AppProjectSession& session,
    const std::string& clipId)
{
    const auto sourceClip = session.project().findClipById(clipId);
    if (!sourceClip.has_value()) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::MissingClip,
            "无法复制 MIDI 片段：目标片段不存在。");
    }

    if (sourceClip->type != ClipType::Midi) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::IncompatibleClipType,
            "无法复制 MIDI 片段：只能复制 MIDI 片段。");
    }

    const auto targetTrack = session.project().findTrackById(sourceClip->trackId);
    if (!targetTrack.has_value()) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::MissingTrack,
            "无法复制 MIDI 片段：片段所属轨道不存在。");
    }

    if (targetTrack->type != TrackType::Instrument) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::IncompatibleTrackType,
            "无法复制 MIDI 片段：MIDI 片段只能复制到乐器轨。");
    }

    const auto duplicateStartTick = sourceClip->startTick + sourceClip->lengthTick;

    // 复制前所有校验都已完成；只有真实复制才允许把会话标记为 dirty。
    const auto duplicate = session.editProject().duplicateClipToTrackAtTick(
        clipId,
        sourceClip->trackId,
        duplicateStartTick);
    if (!duplicate.has_value()) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::DuplicateFailed,
            "无法复制 MIDI 片段：工程模型拒绝了这次复制。");
    }

    return duplicateSuccessFeedback(*duplicate);
}

AppMidiClipActionFeedback renameMidiClipById(
    AppProjectSession& session,
    const std::string& clipId,
    std::string name)
{
    const auto trimmedName = trimClipName(std::move(name));
    if (trimmedName.empty()) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::EmptyName,
            "无法重命名 MIDI 片段：片段名称不能为空。");
    }

    const auto targetClip = session.project().findClipById(clipId);
    if (!targetClip.has_value()) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::MissingClip,
            "无法重命名 MIDI 片段：目标片段不存在。");
    }

    if (targetClip->type != ClipType::Midi) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::IncompatibleClipType,
            "无法重命名 MIDI 片段：只能重命名 MIDI 片段。");
    }

    // 重命名前所有校验都已完成；真正修改时走核心命令，撤销/重做才能恢复旧名称。
    const auto result = session.executeProjectCommand(
        std::make_unique<RenameClipCommand>(clipId, trimmedName));
    if (!result.success) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::RenameFailed,
            "无法重命名 MIDI 片段：工程模型拒绝了这次重命名。");
    }

    return renameSuccessFeedback(*targetClip, trimmedName);
}

AppMidiClipActionFeedback splitMidiClipAtMidpoint(
    AppProjectSession& session,
    const std::string& clipId)
{
    // 先读取只读快照做应用层校验，避免失败路径调用 editProject() 后误标 dirty。
    const auto targetClip = session.project().findClipById(clipId);
    if (!targetClip.has_value()) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::MissingClip,
            "无法拆分 MIDI 片段：目标片段不存在。");
    }

    if (targetClip->type != ClipType::Midi) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::IncompatibleClipType,
            "无法拆分 MIDI 片段：只能拆分 MIDI 片段。");
    }

    // 长度为 0 或 1 tick 时没有合法内部中点，不能拆出两个有效片段。
    if (targetClip->lengthTick <= 1) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::SplitFailed,
            "无法拆分 MIDI 片段：片段太短，没有可用的中点。");
    }

    const auto splitOffset = targetClip->lengthTick / 2;
    const auto splitTick = targetClip->startTick + splitOffset;
    // 这里再次防御整数边界，确保传给核心模型的是片段内部切点。
    if (splitOffset <= 0 || splitTick <= targetClip->startTick) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::SplitFailed,
            "无法拆分 MIDI 片段：片段中点不在有效范围内。");
    }

    if (!midiNotesCanSplitAtOffset(*targetClip, splitOffset)) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::SplitFailed,
            "无法拆分 MIDI 片段：存在跨越中点的音符，当前版本不会自动切断音符。");
    }

    // 拆分前所有可预见校验都已完成；只有真实拆分才允许把会话标记为 dirty。
    const auto rightClip = session.editProject().splitClipAtTick(clipId, splitTick);
    if (!rightClip.has_value()) {
        return failureFeedback(
            AppMidiClipActionFeedbackKind::SplitFailed,
            "无法拆分 MIDI 片段：工程模型拒绝了这次拆分。");
    }

    return splitSuccessFeedback(*rightClip);
}

AppMidiClipActionFeedback moveMidiClipLeftOneBeat(
    AppProjectSession& session,
    const std::string& clipId)
{
    return moveMidiClipByTickOffset(
        session,
        clipId,
        -Project::ticksPerQuarterNote,
        "左移");
}

AppMidiClipActionFeedback moveMidiClipRightOneBeat(
    AppProjectSession& session,
    const std::string& clipId)
{
    return moveMidiClipByTickOffset(
        session,
        clipId,
        Project::ticksPerQuarterNote,
        "右移");
}

AppMidiClipActionFeedback moveMidiClipToTrack(
    AppProjectSession& session,
    const std::string& clipId,
    const std::string& targetTrackId)
{
    return moveMidiClipToTrackImpl(session, clipId, targetTrackId);
}

AppMidiClipActionFeedback trimMidiClipEndEarlierOneBeat(
    AppProjectSession& session,
    const std::string& clipId)
{
    return trimMidiClipEndByTickOffset(
        session,
        clipId,
        Project::ticksPerQuarterNote);
}

AppMidiClipActionFeedback extendMidiClipEndLaterOneBeat(
    AppProjectSession& session,
    const std::string& clipId)
{
    return extendMidiClipEndByTickOffset(
        session,
        clipId,
        Project::ticksPerQuarterNote);
}

AppMidiClipActionFeedback trimMidiClipStartLaterOneBeat(
    AppProjectSession& session,
    const std::string& clipId)
{
    return trimMidiClipStartByTickOffset(
        session,
        clipId,
        Project::ticksPerQuarterNote);
}

AppMidiClipActionFeedback extendMidiClipStartEarlierOneBeat(
    AppProjectSession& session,
    const std::string& clipId)
{
    return extendMidiClipStartByTickOffset(
        session,
        clipId,
        Project::ticksPerQuarterNote);
}

}
