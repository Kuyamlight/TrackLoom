#include "AppTrackStateActions.h"

#include <string>
#include <utility>

namespace trackloom {
namespace {

AppTrackStateActionFeedback successFeedback(
    AppTrackStateActionTarget target,
    const Track& track,
    bool enabled,
    std::string enabledText,
    std::string disabledText)
{
    AppTrackStateActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppTrackStateActionFeedbackKind::Success;
    feedback.target = target;
    feedback.trackId = track.id;
    feedback.enabled = enabled;
    feedback.message = (enabled ? enabledText : disabledText) + "：" + track.name + "。";
    return feedback;
}

AppTrackStateActionFeedback failureFeedback(
    AppTrackStateActionTarget target,
    AppTrackStateActionFeedbackKind kind,
    std::string message)
{
    AppTrackStateActionFeedback feedback;
    feedback.success = false;
    feedback.kind = kind;
    feedback.target = target;
    feedback.message = std::move(message);
    return feedback;
}

AppTrackStateActionFeedback togglePlaybackState(
    AppProjectSession& session,
    const std::string& trackId,
    AppTrackStateActionTarget target,
    bool TrackPlaybackState::*field,
    std::string enabledText,
    std::string disabledText)
{
    const auto targetTrack = session.project().findTrackById(trackId);
    if (!targetTrack.has_value()) {
        return failureFeedback(
            target,
            AppTrackStateActionFeedbackKind::MissingTrack,
            "无法切换轨道状态：目标轨道不存在。");
    }

    // 先从只读快照复制当前播放状态，再只翻转一个字段。
    // 这样静音、独奏和禁用互不覆盖，后续新增状态也不会被清空。
    auto nextPlayback = targetTrack->playback;
    nextPlayback.*field = !(nextPlayback.*field);

    if (!session.editProject().setTrackPlaybackState(trackId, nextPlayback)) {
        return failureFeedback(
            target,
            AppTrackStateActionFeedbackKind::UpdateFailed,
            "无法切换轨道状态：工程模型拒绝了这次播放状态更新。");
    }

    return successFeedback(
        target,
        *targetTrack,
        nextPlayback.*field,
        std::move(enabledText),
        std::move(disabledText));
}

}

AppTrackStateActionFeedback toggleTrackMuted(AppProjectSession& session, const std::string& trackId)
{
    return togglePlaybackState(
        session,
        trackId,
        AppTrackStateActionTarget::Mute,
        &TrackPlaybackState::muted,
        "已静音轨道",
        "已取消轨道静音");
}

AppTrackStateActionFeedback toggleTrackSoloed(AppProjectSession& session, const std::string& trackId)
{
    return togglePlaybackState(
        session,
        trackId,
        AppTrackStateActionTarget::Solo,
        &TrackPlaybackState::soloed,
        "已独奏轨道",
        "已取消轨道独奏");
}

AppTrackStateActionFeedback toggleTrackDisabled(AppProjectSession& session, const std::string& trackId)
{
    return togglePlaybackState(
        session,
        trackId,
        AppTrackStateActionTarget::Disable,
        &TrackPlaybackState::disabled,
        "已禁用轨道",
        "已启用轨道");
}

AppTrackStateActionFeedback toggleTrackHidden(AppProjectSession& session, const std::string& trackId)
{
    const auto targetTrack = session.project().findTrackById(trackId);
    if (!targetTrack.has_value()) {
        return failureFeedback(
            AppTrackStateActionTarget::Hide,
            AppTrackStateActionFeedbackKind::MissingTrack,
            "无法切换轨道显示状态：目标轨道不存在。");
    }

    // hidden 只影响显示，不影响播放；复制现有 view 可保留文件夹折叠等独立状态。
    auto nextView = targetTrack->view;
    nextView.hidden = !nextView.hidden;

    if (!isValidTrackViewState(targetTrack->type, nextView)) {
        return failureFeedback(
            AppTrackStateActionTarget::Hide,
            AppTrackStateActionFeedbackKind::UpdateFailed,
            "无法切换轨道显示状态：新的显示状态不符合轨道类型约束。");
    }

    if (!session.editProject().setTrackViewState(trackId, nextView)) {
        return failureFeedback(
            AppTrackStateActionTarget::Hide,
            AppTrackStateActionFeedbackKind::UpdateFailed,
            "无法切换轨道显示状态：工程模型拒绝了这次显示状态更新。");
    }

    return successFeedback(
        AppTrackStateActionTarget::Hide,
        *targetTrack,
        nextView.hidden,
        "已隐藏轨道",
        "已显示轨道");
}

}
