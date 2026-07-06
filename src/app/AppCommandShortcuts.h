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

// AppShortcutConflict 记录被拒绝的用户覆盖。
// existingCommandId 是当前已经占用该组合键的命令，requestedCommandId 是用户想绑定的新命令。
struct AppShortcutConflict {
    AppShortcutChord chord;
    int existingCommandId = 0;
    int requestedCommandId = 0;
};

// AppShortcutCustomizationResult 是一次快捷键合并的完整结果。
// bindings 是可直接用于查询的活动表，conflicts 保留需要 UI 提示用户处理的冲突。
struct AppShortcutCustomizationResult {
    std::vector<AppShortcutBinding> bindings;
    std::vector<AppShortcutConflict> conflicts;
};

enum class AppShortcutContext {
    MainWindow,
    CommandPaletteOpen
};

// defaultAppShortcutBindings 暴露当前注册的默认快捷键表。
// 它只描述快捷键和 command id 的关系，不执行命令，也不读取菜单启用状态。
std::vector<AppShortcutBinding> defaultAppShortcutBindings();

// isSupportedAppShortcutChord 只判断当前应用层愿意接管的快捷键形态。
// 设置加载、键盘触发和命令面板标签应共用它，避免本地设置接受运行时无法触发的组合键。
bool isSupportedAppShortcutChord(const AppShortcutChord& chord);

// customizeAppShortcutBindings 从默认表生成活动表，再应用用户覆盖。
// 合法且不冲突的覆盖会移除同一命令的旧绑定；冲突覆盖会被记录并保留原绑定。
AppShortcutCustomizationResult customizeAppShortcutBindings(
    const std::vector<AppShortcutBinding>& customBindings);

// describeAppShortcutChord 把平台无关 chord 转成 Windows 首期可读标签。
// 命令面板、菜单提示和测试都可复用这个函数，避免多处手写 Ctrl+S 文案。
std::string describeAppShortcutChord(const AppShortcutChord& chord);

// appCommandIdForShortcut 只负责快捷键到命令 id 的映射，不执行命令。
// 返回的 id 与 AppMainMenu 使用同一套编号，后续可统一交给 AppCommandDispatcher。
std::optional<int> appCommandIdForShortcut(const AppShortcutChord& chord);

// 这个重载查询指定活动表，供未来设置界面、命令面板标签和测试共用同一份映射。
std::optional<int> appCommandIdForShortcut(
    const AppShortcutChord& chord,
    const std::vector<AppShortcutBinding>& bindings);

// 带上下文的快捷键映射用于隔离浮层输入焦点。
// 命令面板打开时，未被命令面板专门处理的组合键不再触发全局工程命令。
std::optional<int> appCommandIdForShortcut(
    const AppShortcutChord& chord,
    AppShortcutContext context);

}
