#include "AppMidiClipActions.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

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

    // editProject 会立刻标记 dirty；因此上面的校验必须先完成，失败路径不得触碰可编辑工程。
    auto createdClip = session.editProject().createClip(
        trackId,
        clipName,
        ClipType::Midi,
        startTick,
        defaultAppMidiClipLengthTick);

    if (!createdClip.has_value()) {
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

    // 删除前所有校验都已完成；只有真实删除才允许把会话标记为 dirty。
    if (!session.editProject().removeClipById(clipId)) {
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

    // 重命名前所有校验都已完成；只有真实修改才允许把会话标记为 dirty。
    if (!session.editProject().renameClipById(clipId, trimmedName)) {
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

}
