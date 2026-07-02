#pragma once

#include "AppMainMenu.h"

#include <string>
#include <vector>

namespace trackloom {

struct AppCommandPaletteItem {
    int commandId = 0;
    bool enabled = false;
    std::string groupName;
    std::string label;
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

}
