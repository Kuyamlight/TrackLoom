#include "AppTrackActions.h"

#include <cstddef>
#include <optional>
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

AppTrackActionFeedback createAudioSuccessFeedback(const Track& track)
{
    AppTrackActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppTrackActionFeedbackKind::Success;
    feedback.trackId = track.id;
    feedback.message = "已添加音频轨：" + track.name + "。";
    return feedback;
}

AppTrackActionFeedback createFolderSuccessFeedback(const Track& track)
{
    AppTrackActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppTrackActionFeedbackKind::Success;
    feedback.trackId = track.id;
    feedback.message = "已添加文件夹轨：" + track.name + "。";
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

AppTrackActionFeedback moveSuccessFeedback(const Track& track, bool movedUp)
{
    AppTrackActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppTrackActionFeedbackKind::Success;
    feedback.trackId = track.id;
    feedback.message = std::string(movedUp ? "已上移乐器轨：" : "已下移乐器轨：") + track.name + "。";
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

std::string nextDefaultAudioTrackName(const Project& project)
{
    // 音频轨采用与乐器轨一致的项目顺序编号；后续轨道模板稳定后再按类型计数或用户偏好扩展。
    return "Audio " + std::to_string(project.tracks().size() + 1);
}

std::string nextDefaultFolderTrackName(const Project& project)
{
    // 文件夹轨同样按当前工程顺序编号；层级命名和模板命名留给正式轨道编辑器。
    return "Folder " + std::to_string(project.tracks().size() + 1);
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

std::optional<std::size_t> trackIndexById(const Project& project, const std::string& trackId)
{
    const auto& tracks = project.tracks();
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        if (tracks[index].id == trackId) {
            return index;
        }
    }

    return std::nullopt;
}

AppTrackActionFeedback moveInstrumentTrackByOffset(
    AppProjectSession& session,
    const std::string& trackId,
    int offset)
{
    const auto targetTrack = session.project().findTrackById(trackId);
    if (!targetTrack.has_value()) {
        return failureFeedback(
            AppTrackActionFeedbackKind::MissingTrack,
            "无法移动乐器轨：目标轨道不存在。");
    }

    if (targetTrack->type != TrackType::Instrument) {
        return failureFeedback(
            AppTrackActionFeedbackKind::IncompatibleTrackType,
            "无法移动乐器轨：当前入口只能移动乐器轨。");
    }

    const auto currentIndex = trackIndexById(session.project(), trackId);
    if (!currentIndex.has_value()) {
        return failureFeedback(
            AppTrackActionFeedbackKind::MissingTrack,
            "无法移动乐器轨：目标轨道不存在。");
    }

    if ((offset < 0 && *currentIndex == 0)
        || (offset > 0 && *currentIndex + 1 >= session.project().tracks().size())) {
        return failureFeedback(
            AppTrackActionFeedbackKind::AlreadyAtBoundary,
            offset < 0
                ? "无法上移乐器轨：目标轨道已经在列表顶部。"
                : "无法下移乐器轨：目标轨道已经在列表底部。");
    }

    const auto targetIndex = offset < 0
        ? *currentIndex - 1
        : *currentIndex + 1;

    // 所有边界都在 editProject() 前完成；真正移动时才把会话标记为 dirty。
    if (!session.editProject().moveTrackToIndex(trackId, targetIndex)) {
        return failureFeedback(
            AppTrackActionFeedbackKind::MoveFailed,
            "无法移动乐器轨：工程模型拒绝了这次顺序调整。");
    }

    return moveSuccessFeedback(*targetTrack, offset < 0);
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

AppTrackActionFeedback createDefaultAudioTrack(AppProjectSession& session)
{
    const auto name = nextDefaultAudioTrackName(session.project());

    // 当前只创建空音频轨；音频文件导入、波形和音频片段会在后续阶段单独接入。
    const auto createdTrack = session.editProject().createTrack(name, TrackType::Audio);
    if (createdTrack.id.empty()) {
        return failureFeedback(
            AppTrackActionFeedbackKind::CreateFailed,
            "无法添加音频轨：工程模型没有返回有效轨道 ID。");
    }

    return createAudioSuccessFeedback(createdTrack);
}

AppTrackActionFeedback createDefaultFolderTrack(AppProjectSession& session)
{
    const auto name = nextDefaultFolderTrackName(session.project());

    // 当前只创建空文件夹轨；层级归组、折叠显示和批量移动后续单独接入。
    const auto createdTrack = session.editProject().createTrack(name, TrackType::Folder);
    if (createdTrack.id.empty()) {
        return failureFeedback(
            AppTrackActionFeedbackKind::CreateFailed,
            "无法添加文件夹轨：工程模型没有返回有效轨道 ID。");
    }

    return createFolderSuccessFeedback(createdTrack);
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

AppTrackActionFeedback moveInstrumentTrackUp(
    AppProjectSession& session,
    const std::string& trackId)
{
    return moveInstrumentTrackByOffset(session, trackId, -1);
}

AppTrackActionFeedback moveInstrumentTrackDown(
    AppProjectSession& session,
    const std::string& trackId)
{
    return moveInstrumentTrackByOffset(session, trackId, 1);
}

}
