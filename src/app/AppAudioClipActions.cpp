#include "AppAudioClipActions.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
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

AppAudioClipActionFeedback duplicateSuccessFeedback(const TimelineClip& clip)
{
    AppAudioClipActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppAudioClipActionFeedbackKind::Success;
    feedback.clipId = clip.id;
    feedback.message = "已复制音频片段：" + clip.name + "。";
    return feedback;
}

AppAudioClipActionFeedback renameSuccessFeedback(const TimelineClip& clip)
{
    AppAudioClipActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppAudioClipActionFeedbackKind::Success;
    feedback.clipId = clip.id;
    feedback.message = "已重命名音频片段：" + clip.name + "。";
    return feedback;
}

AppAudioClipActionFeedback moveSuccessFeedback(const TimelineClip& clip, const std::string& directionLabel)
{
    AppAudioClipActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppAudioClipActionFeedbackKind::Success;
    feedback.clipId = clip.id;
    feedback.message = "已" + directionLabel + "音频片段：" + clip.name + "。";
    return feedback;
}

AppAudioClipActionFeedback trimEndSuccessFeedback(const TimelineClip& clip)
{
    AppAudioClipActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppAudioClipActionFeedbackKind::Success;
    feedback.clipId = clip.id;
    feedback.message = "已缩短音频片段片尾：" + clip.name + "。";
    return feedback;
}

AppAudioClipActionFeedback extendEndSuccessFeedback(const TimelineClip& clip)
{
    AppAudioClipActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppAudioClipActionFeedbackKind::Success;
    feedback.clipId = clip.id;
    feedback.message = "已延长音频片段片尾：" + clip.name + "。";
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

std::string trimClipName(std::string name)
{
    const auto isNotSpace = [](unsigned char value) {
        return std::isspace(value) == 0;
    };

    const auto begin = std::find_if(name.begin(), name.end(), isNotSpace);
    const auto end = std::find_if(name.rbegin(), name.rend(), isNotSpace).base();

    if (begin >= end) {
        return {};
    }

    return std::string(begin, end);
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

AppAudioClipActionFeedback moveAudioClipByTickOffset(
    AppProjectSession& session,
    const std::string& clipId,
    std::int64_t offsetTick,
    const std::string& directionLabel)
{
    // 先用只读快照完成校验；移动失败时不能把未修改工程误标为 dirty。
    const auto targetClip = session.project().findClipById(clipId);
    if (!targetClip.has_value()) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::MissingClip,
            "无法移动音频片段：目标片段不存在。");
    }

    if (targetClip->type != ClipType::Audio) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::IncompatibleClipType,
            "无法移动音频片段：只能移动音频片段。");
    }

    const auto targetTrack = session.project().findTrackById(targetClip->trackId);
    if (!targetTrack.has_value()) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::MissingTrack,
            "无法移动音频片段：片段所属轨道不存在。");
    }

    if (targetTrack->type != TrackType::Audio) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::IncompatibleTrackType,
            "无法移动音频片段：音频片段只能停留在音频轨。");
    }

    if (!canAddTickOffset(targetClip->startTick, offsetTick)) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::MoveFailed,
            "无法移动音频片段：目标位置超出时间线范围。");
    }

    const auto newStartTick = targetClip->startTick + offsetTick;
    if (newStartTick < 0) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::MoveFailed,
            "无法左移音频片段：片段不能移动到时间线起点之前。");
    }

    // 当前移动只改变空音频片段外壳起点；素材偏移和波形规则等音频导入后再单独实现。
    if (!session.editProject().setClipTiming(clipId, newStartTick, targetClip->lengthTick)) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::MoveFailed,
            "无法移动音频片段：工程模型拒绝了这次移动。");
    }

    const auto movedClip = session.project().findClipById(clipId);
    return moveSuccessFeedback(movedClip.value_or(*targetClip), directionLabel);
}

AppAudioClipActionFeedback trimAudioClipEndByTickOffset(
    AppProjectSession& session,
    const std::string& clipId,
    std::int64_t offsetTick)
{
    // 片尾缩短只改空音频片段外壳长度；失败时不能把未修改工程误标为 dirty。
    const auto targetClip = session.project().findClipById(clipId);
    if (!targetClip.has_value()) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::MissingClip,
            "无法缩短音频片段片尾：目标片段不存在。");
    }

    if (targetClip->type != ClipType::Audio) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::IncompatibleClipType,
            "无法缩短音频片段片尾：只能修剪音频片段。");
    }

    const auto targetTrack = session.project().findTrackById(targetClip->trackId);
    if (!targetTrack.has_value()) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::MissingTrack,
            "无法缩短音频片段片尾：片段所属轨道不存在。");
    }

    if (targetTrack->type != TrackType::Audio) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::IncompatibleTrackType,
            "无法缩短音频片段片尾：音频片段只能停留在音频轨。");
    }

    if (targetClip->lengthTick <= offsetTick) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::TrimFailed,
            "无法缩短音频片段片尾：片段长度不足一拍。");
    }

    const auto newLengthTick = targetClip->lengthTick - offsetTick;
    if (!canAddTickOffset(targetClip->startTick, newLengthTick)) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::TrimFailed,
            "无法缩短音频片段片尾：目标片尾超出时间线范围。");
    }

    const auto newEndTick = targetClip->startTick + newLengthTick;
    if (!session.editProject().trimClipEndToTick(clipId, newEndTick)) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::TrimFailed,
            "无法缩短音频片段片尾：工程模型拒绝了这次修剪。");
    }

    const auto trimmedClip = session.project().findClipById(clipId);
    return trimEndSuccessFeedback(trimmedClip.value_or(*targetClip));
}

AppAudioClipActionFeedback extendAudioClipEndByTickOffset(
    AppProjectSession& session,
    const std::string& clipId,
    std::int64_t offsetTick)
{
    // 片尾延长只增加空白外壳长度；不创建素材引用、波形或可听音频内容。
    const auto targetClip = session.project().findClipById(clipId);
    if (!targetClip.has_value()) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::MissingClip,
            "无法延长音频片段片尾：目标片段不存在。");
    }

    if (targetClip->type != ClipType::Audio) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::IncompatibleClipType,
            "无法延长音频片段片尾：只能延长音频片段。");
    }

    const auto targetTrack = session.project().findTrackById(targetClip->trackId);
    if (!targetTrack.has_value()) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::MissingTrack,
            "无法延长音频片段片尾：片段所属轨道不存在。");
    }

    if (targetTrack->type != TrackType::Audio) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::IncompatibleTrackType,
            "无法延长音频片段片尾：音频片段只能停留在音频轨。");
    }

    if (offsetTick <= 0 || !canAddTickOffset(targetClip->lengthTick, offsetTick)) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::ExtendFailed,
            "无法延长音频片段片尾：目标长度超出时间线范围。");
    }

    const auto newLengthTick = targetClip->lengthTick + offsetTick;
    if (!canAddTickOffset(targetClip->startTick, newLengthTick)) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::ExtendFailed,
            "无法延长音频片段片尾：目标片尾超出时间线范围。");
    }

    if (!session.editProject().setClipTiming(clipId, targetClip->startTick, newLengthTick)) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::ExtendFailed,
            "无法延长音频片段片尾：工程模型拒绝了这次延长。");
    }

    const auto extendedClip = session.project().findClipById(clipId);
    return extendEndSuccessFeedback(extendedClip.value_or(*targetClip));
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

AppAudioClipActionFeedback duplicateAudioClipAfterItself(
    AppProjectSession& session,
    const std::string& clipId)
{
    const auto sourceClip = session.project().findClipById(clipId);
    if (!sourceClip.has_value()) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::MissingClip,
            "无法复制音频片段：目标片段不存在。");
    }

    if (sourceClip->type != ClipType::Audio) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::IncompatibleClipType,
            "无法复制音频片段：只能复制音频片段。");
    }

    const auto targetTrack = session.project().findTrackById(sourceClip->trackId);
    if (!targetTrack.has_value()) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::MissingTrack,
            "无法复制音频片段：片段所属轨道不存在。");
    }

    if (targetTrack->type != TrackType::Audio) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::IncompatibleTrackType,
            "无法复制音频片段：音频片段只能复制到音频轨。");
    }

    if (!canAddTickOffset(sourceClip->startTick, sourceClip->lengthTick)) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::DuplicateFailed,
            "无法复制音频片段：目标位置超出时间线范围。");
    }

    const auto duplicateStartTick = sourceClip->startTick + sourceClip->lengthTick;

    // 当前复制只复制空音频片段外壳；真实素材引用、波形和素材偏移会在音频导入阶段单独设计。
    const auto duplicate = session.editProject().duplicateClipToTrackAtTick(
        clipId,
        sourceClip->trackId,
        duplicateStartTick);

    if (!duplicate.has_value()) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::DuplicateFailed,
            "无法复制音频片段：工程模型拒绝了这次复制。");
    }

    return duplicateSuccessFeedback(*duplicate);
}

AppAudioClipActionFeedback renameAudioClipById(
    AppProjectSession& session,
    const std::string& clipId,
    std::string name)
{
    const auto trimmedName = trimClipName(std::move(name));
    if (trimmedName.empty()) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::EmptyName,
            "无法重命名音频片段：名称不能为空。");
    }

    // 先用只读工程快照校验目标；重命名失败时不能把工程误标为 dirty。
    const auto targetClip = session.project().findClipById(clipId);
    if (!targetClip.has_value()) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::MissingClip,
            "无法重命名音频片段：目标片段不存在。");
    }

    if (targetClip->type != ClipType::Audio) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::IncompatibleClipType,
            "无法重命名音频片段：只能重命名音频片段。");
    }

    // 所有可预见校验都已完成；只有真实重命名才允许把会话标记为 dirty。
    if (!session.editProject().renameClipById(clipId, trimmedName)) {
        return failureFeedback(
            AppAudioClipActionFeedbackKind::RenameFailed,
            "无法重命名音频片段：工程模型拒绝了这次重命名。");
    }

    const auto renamedClip = session.project().findClipById(clipId);
    return renameSuccessFeedback(renamedClip.value_or(*targetClip));
}

AppAudioClipActionFeedback moveAudioClipLeftOneBeat(
    AppProjectSession& session,
    const std::string& clipId)
{
    return moveAudioClipByTickOffset(
        session,
        clipId,
        -Project::ticksPerQuarterNote,
        "左移");
}

AppAudioClipActionFeedback moveAudioClipRightOneBeat(
    AppProjectSession& session,
    const std::string& clipId)
{
    return moveAudioClipByTickOffset(
        session,
        clipId,
        Project::ticksPerQuarterNote,
        "右移");
}

AppAudioClipActionFeedback trimAudioClipEndEarlierOneBeat(
    AppProjectSession& session,
    const std::string& clipId)
{
    return trimAudioClipEndByTickOffset(
        session,
        clipId,
        Project::ticksPerQuarterNote);
}

AppAudioClipActionFeedback extendAudioClipEndLaterOneBeat(
    AppProjectSession& session,
    const std::string& clipId)
{
    return extendAudioClipEndByTickOffset(
        session,
        clipId,
        Project::ticksPerQuarterNote);
}

}
