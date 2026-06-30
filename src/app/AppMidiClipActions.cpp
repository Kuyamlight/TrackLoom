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

}
