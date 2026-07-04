#include "AppTrackActions.h"

#include "Command.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

AppTrackActionFeedback deleteAudioSuccessFeedback(const Track& track)
{
    AppTrackActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppTrackActionFeedbackKind::Success;
    feedback.trackId = track.id;
    feedback.message = "已删除音频轨：" + track.name + "。";
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

AppTrackActionFeedback moveSuccessFeedback(const Track& track, bool movedUp, const std::string& trackTypeLabel)
{
    AppTrackActionFeedback feedback;
    feedback.success = true;
    feedback.kind = AppTrackActionFeedbackKind::Success;
    feedback.trackId = track.id;
    feedback.message = std::string(movedUp ? "已上移" : "已下移") + trackTypeLabel + "：" + track.name + "。";
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

std::vector<std::string> currentTrackIds(const Project& project)
{
    std::vector<std::string> ids;
    ids.reserve(project.tracks().size());
    for (const auto& track : project.tracks()) {
        ids.push_back(track.id);
    }

    return ids;
}

bool containsTrackId(const std::vector<std::string>& ids, const std::string& trackId)
{
    for (const auto& id : ids) {
        if (id == trackId) {
            return true;
        }
    }

    return false;
}

std::optional<Track> findTrackCreatedAfterCommand(
    const Project& project,
    const std::vector<std::string>& previousTrackIds)
{
    for (const auto& track : project.tracks()) {
        if (!containsTrackId(previousTrackIds, track.id)) {
            return track;
        }
    }

    return std::nullopt;
}

AppTrackActionFeedback moveTrackByOffset(
    AppProjectSession& session,
    const std::string& trackId,
    TrackType expectedType,
    const std::string& trackTypeLabel,
    int offset)
{
    const auto targetTrack = session.project().findTrackById(trackId);
    if (!targetTrack.has_value()) {
        return failureFeedback(
            AppTrackActionFeedbackKind::MissingTrack,
            "无法移动" + trackTypeLabel + "：目标轨道不存在。");
    }

    if (targetTrack->type != expectedType) {
        return failureFeedback(
            AppTrackActionFeedbackKind::IncompatibleTrackType,
            "无法移动" + trackTypeLabel + "：当前入口只能移动" + trackTypeLabel + "。");
    }

    const auto currentIndex = trackIndexById(session.project(), trackId);
    if (!currentIndex.has_value()) {
        return failureFeedback(
            AppTrackActionFeedbackKind::MissingTrack,
            "无法移动" + trackTypeLabel + "：目标轨道不存在。");
    }

    if ((offset < 0 && *currentIndex == 0)
        || (offset > 0 && *currentIndex + 1 >= session.project().tracks().size())) {
        return failureFeedback(
            AppTrackActionFeedbackKind::AlreadyAtBoundary,
            offset < 0
                ? "无法上移" + trackTypeLabel + "：目标轨道已经在列表顶部。"
                : "无法下移" + trackTypeLabel + "：目标轨道已经在列表底部。");
    }

    const auto targetIndex = offset < 0
        ? *currentIndex - 1
        : *currentIndex + 1;

    // 所有边界都在命令执行前完成；真正移动时才进入撤销历史并标记 dirty。
    const auto result = session.executeProjectCommand(
        std::make_unique<MoveTrackCommand>(trackId, targetIndex));
    if (!result.success) {
        return failureFeedback(
            AppTrackActionFeedbackKind::MoveFailed,
            "无法移动" + trackTypeLabel + "：工程模型拒绝了这次顺序调整。");
    }

    return moveSuccessFeedback(*targetTrack, offset < 0, trackTypeLabel);
}

}

AppTrackActionFeedback createDefaultInstrumentTrack(AppProjectSession& session)
{
    const auto name = nextDefaultInstrumentTrackName(session.project());
    const auto previousTrackIds = currentTrackIds(session.project());

    // 创建轨道本身就是修改；通过核心命令执行，后续撤销/重做才能恢复同一个稳定 ID。
    const auto result = session.executeProjectCommand(
        std::make_unique<AddTrackCommand>(name, TrackType::Instrument));
    const auto createdTrack = findTrackCreatedAfterCommand(session.project(), previousTrackIds);
    if (!result.success || !createdTrack.has_value()) {
        return failureFeedback(
            AppTrackActionFeedbackKind::CreateFailed,
            "无法添加乐器轨：工程模型没有返回有效轨道 ID。");
    }

    return createSuccessFeedback(*createdTrack);
}

AppTrackActionFeedback createDefaultAudioTrack(AppProjectSession& session)
{
    const auto name = nextDefaultAudioTrackName(session.project());
    const auto previousTrackIds = currentTrackIds(session.project());

    // 当前只创建空音频轨；音频文件导入、波形和音频片段会在后续阶段单独接入。
    const auto result = session.executeProjectCommand(
        std::make_unique<AddTrackCommand>(name, TrackType::Audio));
    const auto createdTrack = findTrackCreatedAfterCommand(session.project(), previousTrackIds);
    if (!result.success || !createdTrack.has_value()) {
        return failureFeedback(
            AppTrackActionFeedbackKind::CreateFailed,
            "无法添加音频轨：工程模型没有返回有效轨道 ID。");
    }

    return createAudioSuccessFeedback(*createdTrack);
}

AppTrackActionFeedback createDefaultFolderTrack(AppProjectSession& session)
{
    const auto name = nextDefaultFolderTrackName(session.project());
    const auto previousTrackIds = currentTrackIds(session.project());

    // 当前只创建空文件夹轨；层级归组、折叠显示和批量移动后续单独接入。
    const auto result = session.executeProjectCommand(
        std::make_unique<AddTrackCommand>(name, TrackType::Folder));
    const auto createdTrack = findTrackCreatedAfterCommand(session.project(), previousTrackIds);
    if (!result.success || !createdTrack.has_value()) {
        return failureFeedback(
            AppTrackActionFeedbackKind::CreateFailed,
            "无法添加文件夹轨：工程模型没有返回有效轨道 ID。");
    }

    return createFolderSuccessFeedback(*createdTrack);
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

    // 删除前所有校验都已完成；只有真实删除才进入撤销历史并标记 dirty。
    const auto result = session.executeProjectCommand(
        std::make_unique<DeleteTrackCommand>(trackId));
    if (!result.success) {
        return failureFeedback(
            AppTrackActionFeedbackKind::DeleteFailed,
            "无法删除乐器轨：工程模型拒绝了这次删除。");
    }

    return deleteSuccessFeedback(*targetTrack);
}

AppTrackActionFeedback deleteAudioTrackById(
    AppProjectSession& session,
    const std::string& trackId)
{
    const auto targetTrack = session.project().findTrackById(trackId);
    if (!targetTrack.has_value()) {
        return failureFeedback(
            AppTrackActionFeedbackKind::MissingTrack,
            "无法删除音频轨：目标轨道不存在。");
    }

    if (targetTrack->type != TrackType::Audio) {
        return failureFeedback(
            AppTrackActionFeedbackKind::IncompatibleTrackType,
            "无法删除音频轨：当前入口只能删除音频轨。");
    }

    // 删除前所有校验都已完成；只有真实删除才进入撤销历史并标记 dirty。
    const auto result = session.executeProjectCommand(
        std::make_unique<DeleteTrackCommand>(trackId));
    if (!result.success) {
        return failureFeedback(
            AppTrackActionFeedbackKind::DeleteFailed,
            "无法删除音频轨：工程模型拒绝了这次删除。");
    }

    return deleteAudioSuccessFeedback(*targetTrack);
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

    // 重命名前所有校验都已完成；只有真实修改才进入撤销历史并标记 dirty。
    const auto result = session.executeProjectCommand(
        std::make_unique<RenameTrackCommand>(trackId, trimmedName));
    if (!result.success) {
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
    return moveTrackByOffset(session, trackId, TrackType::Instrument, "乐器轨", -1);
}

AppTrackActionFeedback moveInstrumentTrackDown(
    AppProjectSession& session,
    const std::string& trackId)
{
    return moveTrackByOffset(session, trackId, TrackType::Instrument, "乐器轨", 1);
}

AppTrackActionFeedback moveAudioTrackUp(
    AppProjectSession& session,
    const std::string& trackId)
{
    return moveTrackByOffset(session, trackId, TrackType::Audio, "音频轨", -1);
}

AppTrackActionFeedback moveAudioTrackDown(
    AppProjectSession& session,
    const std::string& trackId)
{
    return moveTrackByOffset(session, trackId, TrackType::Audio, "音频轨", 1);
}

}
