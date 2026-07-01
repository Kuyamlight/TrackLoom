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
    const AppProjectSession&,
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

    status.groups.push_back(std::move(fileMenu));
    status.groups.push_back(std::move(playbackMenu));
    return status;
}

}
