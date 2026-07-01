#include "AppCommandShortcuts.h"

#include "AppMainMenu.h"

#include <cctype>

namespace trackloom {
namespace {

char normalizedShortcutKey(char key)
{
    // 快捷键字母不区分大小写。先转成 unsigned char，可避免 char 为负值时 std::tolower 行为不确定。
    return static_cast<char>(std::tolower(static_cast<unsigned char>(key)));
}

std::optional<int> fileCommandShortcut(char key, bool shift)
{
    // 这里只返回菜单命令 id，不直接调用保存或打开函数。
    // 这样菜单、快捷键和未来命令面板都能走同一套 AppCommandDispatcher 执行规则。
    switch (key) {
    case 'n':
        if (!shift) {
            return appMainMenuCommandId(AppMainMenuCommand::NewProject);
        }
        break;
    case 'o':
        if (!shift) {
            return appMainMenuCommandId(AppMainMenuCommand::OpenProject);
        }
        break;
    case 's':
        return appMainMenuCommandId(shift
            ? AppMainMenuCommand::SaveProjectAs
            : AppMainMenuCommand::SaveProject);
    }

    return std::nullopt;
}

}

std::optional<int> appCommandIdForShortcut(const AppShortcutChord& chord)
{
    // 第一批快捷键只接受“主修饰键 + 字母”，Alt 组合保留给系统菜单或未来明确设计。
    if (!chord.primaryModifier || chord.alt || chord.key == '\0') {
        return std::nullopt;
    }

    return fileCommandShortcut(normalizedShortcutKey(chord.key), chord.shift);
}

}
