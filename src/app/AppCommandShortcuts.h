#pragma once

#include <optional>

namespace trackloom {

// AppShortcutChord 是平台无关的快捷键描述。
// JUCE、未来命令面板或测试都先转换成这个结构，再映射到稳定命令 id。
struct AppShortcutChord {
    char key = '\0';
    bool primaryModifier = false;
    bool shift = false;
    bool alt = false;
};

// appCommandIdForShortcut 只负责快捷键到命令 id 的映射，不执行命令。
// 返回的 id 与 AppMainMenu 使用同一套编号，后续可统一交给 AppCommandDispatcher。
std::optional<int> appCommandIdForShortcut(const AppShortcutChord& chord);

}
