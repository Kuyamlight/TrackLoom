#include "AppTrackActions.h"

#include <string>
#include <utility>

namespace trackloom {
namespace {

AppTrackActionFeedback createSuccessFeedback(const Track& track)
{
    AppTrackActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppTrackActionFeedbackKind::Success;
    feedback.trackId = track.id;
    feedback.message = "已添加乐器轨：" + track.name + "。";
    return feedback;
}

AppTrackActionFeedback deleteSuccessFeedback(const Track& track)
{
    AppTrackActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppTrackActionFeedbackKind::Success;
    feedback.trackId = track.id;
    feedback.message = "已删除乐器轨：" + track.name + "。";
    return feedback;
}

AppTrackActionFeedback renameSuccessFeedback(const Track& track, const std::string& newName)
{
    AppTrackActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppTrackActionFeedbackKind::Success;
    feedback.trackId = track.id;
    feedback.message = "已重命名轨道：" + track.name + " -> " + newName + "。";
    return feedback;
}

AppTrackActionFeedback failureFeedback(
    AppTrackActionFeedbackKind kind,
    std::string message)
{
    AppTrackActionFeedback feedback;
    feedback.success = false;
    feedback.kind = kind;
    feedback.message = std::move(message);
    return feedback;
}

std::string nextDefaultInstrumentTrackName(const Project& project)
{
    // 沿用当前首屏行为：按工程已有轨道总数生成可读名称，避免 UI 自己决定命名规则。
    return "Instrument " + std::to_string(project.tracks().size() + 1);
}

std::string trimTrackName(std::string name)
{
    // 桌面输入框可能带入首尾空白；这里统一清理，避免保存肉眼看不出的名称差异。
    const auto first = name.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }

    const auto last = name.find_last_not_of(" \t\r\n");
    return name.substr(first, last - first + 1);
}

}

AppTrackActionFeedback createDefaultInstrumentTrack(AppProjectSession& session)
{
    const auto name = nextDefaultInstrumentTrackName(session.project());

    // 创建轨道本身就是修改；这里没有失败前置条件，因此直接进入可编辑工程。
    const auto createdTrack = session.editProject().createTrack(name, TrackType::Instrument);
    if (createdTrack.id.empty()) {
        return failureFeedback(
            AppTrackActionFeedbackKind::CreateFailed,
            "无法添加乐器轨：工程模型没有返回有效轨道 ID。");
    }

    return createSuccessFeedback(createdTrack);
}

AppTrackActionFeedback deleteInstrumentTrackById(
    AppProjectSession& session,
    const std::string& trackId)
{
    const auto targetTrack = session.project().findTrackById(trackId);
    if (!targetTrack.has_value()) {
        return failureFeedback(
            AppTrackActionFeedbackKind::MissingTrack,
            "无法删除乐器轨：目标轨道不存在。");
    }

    if (targetTrack->type != TrackType::Instrument) {
        return failureFeedback(
            AppTrackActionFeedbackKind::IncompatibleTrackType,
            "无法删除乐器轨：当前入口只能删除乐器轨。");
    }

    // 删除前所有校验都已完成；只有真实删除才允许把会话标记为 dirty。
    if (!session.editProject().removeTrackById(trackId)) {
        return failureFeedback(
            AppTrackActionFeedbackKind::DeleteFailed,
            "无法删除乐器轨：工程模型拒绝了这次删除。");
    }

    return deleteSuccessFeedback(*targetTrack);
}

AppTrackActionFeedback renameTrackById(
    AppProjectSession& session,
    const std::string& trackId,
    std::string name)
{
    const auto trimmedName = trimTrackName(std::move(name));
    if (trimmedName.empty()) {
        return failureFeedback(
            AppTrackActionFeedbackKind::EmptyName,
            "无法重命名轨道：轨道名称不能为空。");
    }

    const auto targetTrack = session.project().findTrackById(trackId);
    if (!targetTrack.has_value()) {
        return failureFeedback(
            AppTrackActionFeedbackKind::MissingTrack,
            "无法重命名轨道：目标轨道不存在。");
    }

    // 重命名前所有校验都已完成；只有真实修改才允许把会话标记为 dirty。
    if (!session.editProject().renameTrackById(trackId, trimmedName)) {
        return failureFeedback(
            AppTrackActionFeedbackKind::RenameFailed,
            "无法重命名轨道：工程模型拒绝了这次重命名。");
    }

    return renameSuccessFeedback(*targetTrack, trimmedName);
}

}
