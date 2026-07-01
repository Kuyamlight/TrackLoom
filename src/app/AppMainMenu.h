#pragma once

#include "AppPlaybackActions.h"
#include "AppProjectSession.h"
#include "AppRecentProjects.h"

#include <optional>
#include <string>
#include <vector>

namespace trackloom {

// AppMainMenuCommand 是桌面主菜单里的稳定命令标识。
// JUCE 菜单、快捷键和后续 AI 工具可以共享这些 id，而不是各自发明编号。
enum class AppMainMenuCommand {
    NewProject = 1001,
    OpenProject = 1002,
    SaveProject = 1003,
    SaveProjectAs = 1004,
    PlayProject = 1101,
    StopProject = 1102,
    RewindProject = 1103
};

struct AppMainMenuItem {
    int commandId = 0;
    bool separator = false;
    bool enabled = false;
    std::string label;
};

struct AppMainMenuGroup {
    std::string name;
    std::vector<AppMainMenuItem> items;
};

struct AppMainMenuStatus {
    std::vector<AppMainMenuGroup> groups;
};

// appMainMenuCommandId 把强类型命令转换成 JUCE PopupMenu 需要的正整数 id。
int appMainMenuCommandId(AppMainMenuCommand command);

// 最近工程是动态菜单项，编号来自用户可见的 1-based 最近工程行号。
int appMainMenuRecentProjectCommandId(std::size_t number);
std::optional<std::size_t> appMainMenuRecentProjectNumberFromCommandId(int commandId);

// describeAppMainMenu 只生成菜单快照，不执行命令。
// 菜单启用状态读取应用运行态；真正的新建、打开、播放仍由现有动作函数执行。
AppMainMenuStatus describeAppMainMenu(
    const AppProjectSession& session,
    const AppPlaybackController& playback,
    const AppRecentProjects& recentProjects);

}
