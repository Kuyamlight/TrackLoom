#include "AppAudioClipActions.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace trackloom {
namespace {

AppAudioClipActionFeedback successFeedback(const TimelineClip& clip)
{
    AppAudioClipActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppAudioClipActionFeedbackKind::Success;
    feedback.clipId = clip.id;
    feedback.message = "已创建音频片段：" + clip.name + "。";
    return feedback;
}

AppAudioClipActionFeedback deleteSuccessFeedback(const TimelineClip& clip)
{
    AppAudioClipActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppAudioClipActionFeedbackKind::Success;
    feedback.clipId = clip.id;
    feedback.message = "已删除音频片段：" + clip.name + "。";
    return feedback;
}

AppAudioClipActionFeedback failureFeedback(
    AppAudioClipActionFeedbackKind kind,
    std::string message)
{
    AppAudioClipActionFeedback feedback;
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

}

AppAudioClipActionFeedback createDefaultAudioClipOnTrack(
    AppProjectSession& session,
    const std::string& trackId)
{
    const auto targetTrack = session.project().findTrackById(trackId);
    if (!targetTrack.has_value()) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::MissingTrack,
            "无法创建音频片段：目标轨道不存在。");
    }

    if (targetTrack->type != TrackType::Audio) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::IncompatibleTrackType,
            "无法创建音频片段：只能在音频轨上创建音频片段。");
    }

    const auto startTick = nextClipStartTickForTrack(session.project(), trackId);
    const auto clipNumber = clipCountForTrack(session.project(), trackId) + 1;
    const auto clipName = targetTrack->name + " Audio " + std::to_string(clipNumber);

    // editProject 会把会话标脏；因此必须先完成所有可预见失败校验。
    const auto createdClip = session.editProject().createClip(
        trackId,
        clipName,
        ClipType::Audio,
        startTick,
        defaultAppAudioClipLengthTick);

    if (!createdClip.has_value()) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::CreateFailed,
            "无法创建音频片段：工程模型拒绝了这次片段创建。");
    }

    return successFeedback(*createdClip);
}

AppAudioClipActionFeedback deleteAudioClipById(
    AppProjectSession& session,
    const std::string& clipId)
{
    // 先用只读工程快照校验目标；删除失败时不能把工程误标为 dirty。
    const auto targetClip = session.project().findClipById(clipId);
    if (!targetClip.has_value()) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::MissingClip,
            "无法删除音频片段：目标片段不存在。");
    }

    if (targetClip->type != ClipType::Audio) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::IncompatibleClipType,
            "无法删除音频片段：只能删除音频片段。");
    }

    // 所有可预见校验都已完成；只有真实删除才允许把会话标记为 dirty。
    if (!session.editProject().removeClipById(clipId)) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::DeleteFailed,
            "无法删除音频片段：工程模型拒绝了这次删除。");
    }

    return deleteSuccessFeedback(*targetClip);
}

}
