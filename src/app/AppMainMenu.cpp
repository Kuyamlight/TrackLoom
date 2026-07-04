#include "AppMainMenu.h"

#include <limits>
#include <string>
#include <utility>

namespace trackloom {
namespace {

constexpr int recentProjectCommandBase = 3000;

AppMainMenuItem commandItem(AppMainMenuCommand command, std::string label, bool enabled)
{
    return { appMainMenuCommandId(command), false, enabled, std::move(label) };
}

AppMainMenuItem recentProjectItem(const AppRecentProjectRow& row)
{
    return {
        appMainMenuRecentProjectCommandId(row.number),
        false,
        true,
        std::to_string(row.number) + ". " + row.displayName
    };
}

AppMainMenuItem separatorItem()
{
    AppMainMenuItem item;
    item.separator = true;
    return item;
}

AppMainMenuItem disabledInfoItem(std::string label)
{
    AppMainMenuItem item;
    item.enabled = false;
    item.label = std::move(label);
    return item;
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

bool selectedTrackHasType(
    const Project& project,
    const std::string& trackId,
    TrackType type)
{
    if (trackId.empty()) {
        return false;
    }

    const auto track = project.findTrackById(trackId);
    return track.has_value() && track->type == type;
}

bool selectedClipHasType(
    const Project& project,
    const std::string& clipId,
    ClipType type)
{
    if (clipId.empty()) {
        return false;
    }

    const auto clip = project.findClipById(clipId);
    return clip.has_value() && clip->type == type;
}

bool canMoveSelectedClipToTargetTrackOfType(
    const Project& project,
    const std::string& clipId,
    ClipType clipType,
    const std::string& targetTrackId,
    TrackType trackType)
{
    if (clipId.empty() || targetTrackId.empty()) {
        return false;
    }

    const auto clip = project.findClipById(clipId);
    if (!clip.has_value() || clip->type != clipType) {
        return false;
    }

    if (clip->trackId == targetTrackId) {
        return false;
    }

    const auto ownerTrack = project.findTrackById(clip->trackId);
    if (!ownerTrack.has_value() || ownerTrack->type != trackType) {
        return false;
    }

    return selectedTrackHasType(project, targetTrackId, trackType);
}

bool canMoveSelectedTrackOfType(
    const Project& project,
    const std::string& trackId,
    TrackType type,
    int offset)
{
    if (!selectedTrackHasType(project, trackId, type)) {
        return false;
    }

    const auto index = trackIndexById(project, trackId);
    if (!index.has_value()) {
        return false;
    }

    if (offset < 0) {
        return *index > 0;
    }

    return *index + 1 < project.tracks().size();
}

}

int appMainMenuCommandId(AppMainMenuCommand command)
{
    return static_cast<int>(command);
}

int appMainMenuRecentProjectCommandId(std::size_t number)
{
    if (number == 0
        || number > static_cast<std::size_t>(std::numeric_limits<int>::max() - recentProjectCommandBase)) {
        return 0;
    }

    return recentProjectCommandBase + static_cast<int>(number);
}

std::optional<std::size_t> appMainMenuRecentProjectNumberFromCommandId(int commandId)
{
    if (commandId <= recentProjectCommandBase) {
        return std::nullopt;
    }

    return static_cast<std::size_t>(commandId - recentProjectCommandBase);
}

AppMainMenuStatus describeAppMainMenu(
    const AppProjectSession& session,
    const AppPlaybackController& playback,
    const AppRecentProjects& recentProjects)
{
    AppMainMenuStatus status;

    AppMainMenuGroup fileMenu;
    fileMenu.name = "文件";
    fileMenu.items.push_back(commandItem(AppMainMenuCommand::NewProject, "新建工程", true));
    fileMenu.items.push_back(commandItem(AppMainMenuCommand::OpenProject, "打开工程...", true));
    fileMenu.items.push_back(commandItem(AppMainMenuCommand::SaveProject, "保存", true));
    fileMenu.items.push_back(commandItem(AppMainMenuCommand::SaveProjectAs, "另存为...", true));
    fileMenu.items.push_back(separatorItem());

    const auto recentStatus = describeAppRecentProjects(recentProjects);
    if (recentStatus.rows.empty()) {
        fileMenu.items.push_back(disabledInfoItem("暂无最近工程"));
    } else {
        for (const auto& row : recentStatus.rows) {
            fileMenu.items.push_back(recentProjectItem(row));
        }
    }

    AppMainMenuGroup editMenu;
    editMenu.name = "编辑";
    // 菜单层只读取是否可撤销/重做；真正修改工程的动作交给命令分发器和会话层。
    editMenu.items.push_back(commandItem(
        AppMainMenuCommand::UndoProject,
        "撤销",
        session.canUndoProjectEdit()));
    editMenu.items.push_back(commandItem(
        AppMainMenuCommand::RedoProject,
        "重做",
        session.canRedoProjectEdit()));

    AppMainMenuGroup trackMenu;
    trackMenu.name = "轨道";
    // 这三个命令不依赖当前选择；依赖目标轨道或片段的编辑命令后续单独接入，避免菜单层读取 UI 私有状态。
    trackMenu.items.push_back(commandItem(
        AppMainMenuCommand::AddInstrumentTrack,
        "添加乐器轨",
        true));
    trackMenu.items.push_back(commandItem(
        AppMainMenuCommand::AddAudioTrack,
        "添加音频轨",
        true));
    trackMenu.items.push_back(commandItem(
        AppMainMenuCommand::AddFolderTrack,
        "添加文件夹轨",
        true));

    AppMainMenuGroup clipMenu;
    clipMenu.name = "片段";
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::RenameSelectedMidiClip,
        "重命名所选 MIDI 片段",
        false));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::DeleteSelectedMidiClip,
        "删除所选 MIDI 片段",
        false));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::DuplicateSelectedMidiClip,
        "复制所选 MIDI 片段",
        false));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::SplitSelectedMidiClip,
        "拆分所选 MIDI 片段",
        false));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::MoveSelectedMidiClipToTargetTrack,
        "移动所选 MIDI 片段到目标乐器轨",
        false));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::MoveSelectedMidiClipLeft,
        "左移所选 MIDI 片段",
        false));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::MoveSelectedMidiClipRight,
        "右移所选 MIDI 片段",
        false));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::TrimSelectedMidiClipEnd,
        "缩短所选 MIDI 片尾",
        false));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::ExtendSelectedMidiClipEnd,
        "延长所选 MIDI 片尾",
        false));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::TrimSelectedMidiClipStart,
        "缩短所选 MIDI 片头",
        false));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::ExtendSelectedMidiClipStart,
        "延长所选 MIDI 片头",
        false));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::RenameSelectedAudioClip,
        "重命名所选音频片段",
        false));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::DeleteSelectedAudioClip,
        "删除所选音频片段",
        false));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::DuplicateSelectedAudioClip,
        "复制所选音频片段",
        false));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::SplitSelectedAudioClip,
        "拆分所选音频片段",
        false));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::MoveSelectedAudioClipToTargetTrack,
        "移动所选音频片段到目标音频轨",
        false));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::MoveSelectedAudioClipLeft,
        "左移所选音频片段",
        false));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::MoveSelectedAudioClipRight,
        "右移所选音频片段",
        false));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::TrimSelectedAudioClipEnd,
        "缩短所选音频片尾",
        false));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::ExtendSelectedAudioClipEnd,
        "延长所选音频片尾",
        false));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::TrimSelectedAudioClipStart,
        "缩短所选音频片头",
        false));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::ExtendSelectedAudioClipStart,
        "延长所选音频片头",
        false));

    AppMainMenuGroup playbackMenu;
    playbackMenu.name = "播放";
    playbackMenu.items.push_back(commandItem(
        AppMainMenuCommand::PlayProject,
        "播放",
        !playback.isPlaying()));
    playbackMenu.items.push_back(commandItem(
        AppMainMenuCommand::StopProject,
        "停止",
        playback.isPlaying()));
    playbackMenu.items.push_back(commandItem(
        AppMainMenuCommand::RewindProject,
        "回到开头",
        playback.currentSample() > 0));

    AppMainMenuGroup toolsMenu;
    toolsMenu.name = "工具";
    // 命令面板是本地临时 UI 状态，不依赖工程是否可保存或播放。
    toolsMenu.items.push_back(commandItem(
        AppMainMenuCommand::OpenCommandPalette,
        "命令面板...",
        true));

    status.groups.push_back(std::move(fileMenu));
    status.groups.push_back(std::move(editMenu));
    status.groups.push_back(std::move(trackMenu));
    status.groups.push_back(std::move(clipMenu));
    status.groups.push_back(std::move(playbackMenu));
    status.groups.push_back(std::move(toolsMenu));
    return status;
}

AppMainMenuStatus describeAppMainMenu(
    const AppProjectSession& session,
    const AppPlaybackController& playback,
    const AppRecentProjects& recentProjects,
    const AppMainMenuSelection& selection)
{
    auto status = describeAppMainMenu(session, playback, recentProjects);
    if (status.groups.size() < 3) {
        return status;
    }

    auto& trackMenu = status.groups[2];
    trackMenu.items.push_back(separatorItem());

    const auto hasSelectedInstrumentTrack = selectedTrackHasType(
        session.project(),
        selection.selectedInstrumentTrackId,
        TrackType::Instrument);
    const auto hasSelectedAudioTrack = selectedTrackHasType(
        session.project(),
        selection.selectedAudioTrackId,
        TrackType::Audio);
    trackMenu.items.push_back(commandItem(
        AppMainMenuCommand::RenameSelectedInstrumentTrack,
        "重命名所选乐器轨",
        hasSelectedInstrumentTrack));
    trackMenu.items.push_back(commandItem(
        AppMainMenuCommand::DeleteSelectedInstrumentTrack,
        "删除所选乐器轨",
        hasSelectedInstrumentTrack));
    trackMenu.items.push_back(commandItem(
        AppMainMenuCommand::MoveSelectedInstrumentTrackUp,
        "上移所选乐器轨",
        canMoveSelectedTrackOfType(session.project(), selection.selectedInstrumentTrackId, TrackType::Instrument, -1)));
    trackMenu.items.push_back(commandItem(
        AppMainMenuCommand::MoveSelectedInstrumentTrackDown,
        "下移所选乐器轨",
        canMoveSelectedTrackOfType(session.project(), selection.selectedInstrumentTrackId, TrackType::Instrument, 1)));
    trackMenu.items.push_back(commandItem(
        AppMainMenuCommand::ToggleSelectedInstrumentTrackMute,
        "切换所选乐器轨静音",
        hasSelectedInstrumentTrack));
    trackMenu.items.push_back(commandItem(
        AppMainMenuCommand::ToggleSelectedInstrumentTrackSolo,
        "切换所选乐器轨独奏",
        hasSelectedInstrumentTrack));
    trackMenu.items.push_back(commandItem(
        AppMainMenuCommand::ToggleSelectedInstrumentTrackDisabled,
        "切换所选乐器轨禁用",
        hasSelectedInstrumentTrack));
    trackMenu.items.push_back(commandItem(
        AppMainMenuCommand::ToggleSelectedInstrumentTrackHidden,
        "切换所选乐器轨隐藏",
        hasSelectedInstrumentTrack));
    trackMenu.items.push_back(separatorItem());
    trackMenu.items.push_back(commandItem(
        AppMainMenuCommand::DeleteSelectedAudioTrack,
        "删除所选音频轨",
        hasSelectedAudioTrack));
    trackMenu.items.push_back(commandItem(
        AppMainMenuCommand::MoveSelectedAudioTrackUp,
        "上移所选音频轨",
        canMoveSelectedTrackOfType(session.project(), selection.selectedAudioTrackId, TrackType::Audio, -1)));
    trackMenu.items.push_back(commandItem(
        AppMainMenuCommand::MoveSelectedAudioTrackDown,
        "下移所选音频轨",
        canMoveSelectedTrackOfType(session.project(), selection.selectedAudioTrackId, TrackType::Audio, 1)));
    trackMenu.items.push_back(commandItem(
        AppMainMenuCommand::ToggleSelectedAudioTrackMute,
        "切换所选音频轨静音",
        hasSelectedAudioTrack));
    trackMenu.items.push_back(commandItem(
        AppMainMenuCommand::ToggleSelectedAudioTrackSolo,
        "切换所选音频轨独奏",
        hasSelectedAudioTrack));
    trackMenu.items.push_back(commandItem(
        AppMainMenuCommand::ToggleSelectedAudioTrackDisabled,
        "切换所选音频轨禁用",
        hasSelectedAudioTrack));
    trackMenu.items.push_back(commandItem(
        AppMainMenuCommand::ToggleSelectedAudioTrackHidden,
        "切换所选音频轨隐藏",
        hasSelectedAudioTrack));

    if (status.groups.size() < 4) {
        return status;
    }

    auto& clipMenu = status.groups[3];
    const auto hasSelectedMidiClip = selectedClipHasType(
        session.project(),
        selection.selectedMidiClipId,
        ClipType::Midi);
    const auto hasSelectedAudioClip = selectedClipHasType(
        session.project(),
        selection.selectedAudioClipId,
        ClipType::Audio);
    const auto canMoveSelectedMidiClipToTargetTrack = canMoveSelectedClipToTargetTrackOfType(
        session.project(),
        selection.selectedMidiClipId,
        ClipType::Midi,
        selection.selectedInstrumentTrackId,
        TrackType::Instrument);
    const auto canMoveSelectedAudioClipToTargetTrack = canMoveSelectedClipToTargetTrackOfType(
        session.project(),
        selection.selectedAudioClipId,
        ClipType::Audio,
        selection.selectedAudioTrackId,
        TrackType::Audio);
    clipMenu.items.clear();
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::RenameSelectedMidiClip,
        "重命名所选 MIDI 片段",
        hasSelectedMidiClip));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::DeleteSelectedMidiClip,
        "删除所选 MIDI 片段",
        hasSelectedMidiClip));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::DuplicateSelectedMidiClip,
        "复制所选 MIDI 片段",
        hasSelectedMidiClip));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::SplitSelectedMidiClip,
        "拆分所选 MIDI 片段",
        hasSelectedMidiClip));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::MoveSelectedMidiClipToTargetTrack,
        "移动所选 MIDI 片段到目标乐器轨",
        canMoveSelectedMidiClipToTargetTrack));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::MoveSelectedMidiClipLeft,
        "左移所选 MIDI 片段",
        hasSelectedMidiClip));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::MoveSelectedMidiClipRight,
        "右移所选 MIDI 片段",
        hasSelectedMidiClip));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::TrimSelectedMidiClipEnd,
        "缩短所选 MIDI 片尾",
        hasSelectedMidiClip));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::ExtendSelectedMidiClipEnd,
        "延长所选 MIDI 片尾",
        hasSelectedMidiClip));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::TrimSelectedMidiClipStart,
        "缩短所选 MIDI 片头",
        hasSelectedMidiClip));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::ExtendSelectedMidiClipStart,
        "延长所选 MIDI 片头",
        hasSelectedMidiClip));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::RenameSelectedAudioClip,
        "重命名所选音频片段",
        hasSelectedAudioClip));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::DeleteSelectedAudioClip,
        "删除所选音频片段",
        hasSelectedAudioClip));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::DuplicateSelectedAudioClip,
        "复制所选音频片段",
        hasSelectedAudioClip));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::SplitSelectedAudioClip,
        "拆分所选音频片段",
        hasSelectedAudioClip));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::MoveSelectedAudioClipToTargetTrack,
        "移动所选音频片段到目标音频轨",
        canMoveSelectedAudioClipToTargetTrack));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::MoveSelectedAudioClipLeft,
        "左移所选音频片段",
        hasSelectedAudioClip));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::MoveSelectedAudioClipRight,
        "右移所选音频片段",
        hasSelectedAudioClip));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::TrimSelectedAudioClipEnd,
        "缩短所选音频片尾",
        hasSelectedAudioClip));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::ExtendSelectedAudioClipEnd,
        "延长所选音频片尾",
        hasSelectedAudioClip));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::TrimSelectedAudioClipStart,
        "缩短所选音频片头",
        hasSelectedAudioClip));
    clipMenu.items.push_back(commandItem(
        AppMainMenuCommand::ExtendSelectedAudioClipStart,
        "延长所选音频片头",
        hasSelectedAudioClip));
    return status;
}

}
