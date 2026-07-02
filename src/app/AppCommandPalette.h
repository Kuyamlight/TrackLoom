#pragma once

#include "AppCommandShortcuts.h"
#include "AppMainMenu.h"

#include <optional>
#include <string>
#include <vector>

namespace trackloom {

struct AppCommandPaletteItem {
    int commandId = 0;
    bool enabled = false;
    std::string groupName;
    std::string label;
    std::string shortcutLabel;
};

struct AppCommandPaletteStatus {
    std::vector<AppCommandPaletteItem> items;
};

// describeAppCommandPalette 把主菜单快照展开成命令面板可显示的扁平列表。
// 它只复制命令数据，不执行命令；分隔线和提示行不会进入命令面板。
AppCommandPaletteStatus describeAppCommandPalette(const AppMainMenuStatus& menu);

// filterAppCommandPalette 在已展开的命令列表里做轻量搜索。
// 当前只做标签和菜单组名匹配，后续真正 UI 可以在这个稳定结果上渲染。
AppCommandPaletteStatus filterAppCommandPalette(
    const AppCommandPaletteStatus& palette,
    const std::string& query);

// addAppCommandPaletteShortcutLabels 返回带快捷键显示文本的新快照。
// 它不注册快捷键，也不执行命令；只是把已有默认快捷键表映射到 command id。
AppCommandPaletteStatus addAppCommandPaletteShortcutLabels(
    const AppCommandPaletteStatus& palette,
    const std::vector<AppShortcutBinding>& shortcutBindings);

// selectFirstExecutableAppCommand 只选择命令，不执行命令。
// 它按当前过滤顺序返回第一个 enabled 项；执行仍由 AppCommandDispatcher 负责。
std::optional<AppCommandPaletteItem> selectFirstExecutableAppCommand(
    const AppCommandPaletteStatus& palette,
    const std::string& query);

}
