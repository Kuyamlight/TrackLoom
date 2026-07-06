#pragma once

#include "AppCommandShortcuts.h"
#include "AppMainMenu.h"

#include <cstddef>
#include <string>
#include <vector>

namespace trackloom {

struct AppShortcutStatusRow {
    int commandId = 0;
    bool enabled = false;
    bool customized = false;
    std::string groupName;
    std::string label;
    std::string shortcutLabel;
};

struct AppShortcutConflictStatusRow {
    AppShortcutChord chord;
    std::string shortcutLabel;
    int existingCommandId = 0;
    std::string existingCommandLabel;
    int requestedCommandId = 0;
    std::string requestedCommandLabel;
};

struct AppShortcutStatus {
    std::vector<AppShortcutStatusRow> rows;
    std::vector<AppShortcutConflictStatusRow> conflicts;
    std::size_t customBindingCount = 0;
    std::size_t activeCustomBindingCount = 0;
    std::string summary;
};

// describeAppShortcutStatus 把菜单命令、活动快捷键和用户覆盖项合成设置页可直接展示的快照。
// 它不读取设置文件，也不注册快捷键；调用方先决定用哪份活动表，再把结果传进来。
AppShortcutStatus describeAppShortcutStatus(
    const AppMainMenuStatus& menu,
    const AppShortcutCustomizationResult& customization,
    const std::vector<AppShortcutBinding>& customBindings);

}
