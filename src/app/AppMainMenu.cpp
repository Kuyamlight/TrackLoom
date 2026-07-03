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
    status.groups.push_back(std::move(playbackMenu));
    status.groups.push_back(std::move(toolsMenu));
    return status;
}

}
