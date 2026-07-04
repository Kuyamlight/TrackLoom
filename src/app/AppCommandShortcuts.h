#pragma once

#include <optional>
#include <string>
#include <vector>

namespace trackloom {

// AppShortcutChord 是平台无关的快捷键描述。
// JUCE、未来命令面板或测试都先转换成这个结构，再映射到稳定命令 id。
struct AppShortcutChord {
    char key = '\0';
    bool primaryModifier = false;
    bool shift = false;
    bool alt = false;
};

struct AppShortcutBinding {
    AppShortcutChord chord;
    int commandId = 0;
};

enum class AppShortcutContext {
    MainWindow,
    CommandPaletteOpen
};

// defaultAppShortcutBindings 暴露当前注册的默认快捷键表。
// 它只描述快捷键和 command id 的关系，不执行命令，也不读取菜单启用状态。
std::vector<AppShortcutBinding> defaultAppShortcutBindings();

// describeAppShortcutChord 把平台无关 chord 转成 Windows 首期可读标签。
// 命令面板、菜单提示和测试都可复用这个函数，避免多处手写 Ctrl+S 文案。
std::string describeAppShortcutChord(const AppShortcutChord& chord);

// appCommandIdForShortcut 只负责快捷键到命令 id 的映射，不执行命令。
// 返回的 id 与 AppMainMenu 使用同一套编号，后续可统一交给 AppCommandDispatcher。
std::optional<int> appCommandIdForShortcut(const AppShortcutChord& chord);

// 带上下文的快捷键映射用于隔离浮层输入焦点。
// 命令面板打开时，未被命令面板专门处理的组合键不再触发全局工程命令。
std::optional<int> appCommandIdForShortcut(
    const AppShortcutChord& chord,
    AppShortcutContext context);

}
