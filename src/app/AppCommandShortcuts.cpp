#include "AppCommandShortcuts.h"

#include "AppMainMenu.h"

#include <cctype>
#include <sstream>

namespace trackloom {
namespace {

char normalizedShortcutKey(char key)
{
    // 快捷键字母不区分大小写。先转成 unsigned char，可避免 char 为负值时 std::tolower 行为不确定。
    return static_cast<char>(std::tolower(static_cast<unsigned char>(key)));
}

char displayShortcutKey(char key)
{
    return static_cast<char>(std::toupper(static_cast<unsigned char>(normalizedShortcutKey(key))));
}

bool sameShortcutChord(const AppShortcutChord& left, const AppShortcutChord& right)
{
    return normalizedShortcutKey(left.key) == normalizedShortcutKey(right.key)
        && left.primaryModifier == right.primaryModifier
        && left.shift == right.shift
        && left.alt == right.alt;
}

}

std::vector<AppShortcutBinding> defaultAppShortcutBindings()
{
    // 这张表是首期窗口内快捷键的唯一来源。
    // command id 仍来自主菜单枚举，确保菜单、快捷键和命令面板指向同一命令。
    return {
        {{ 'n', true, false, false }, appMainMenuCommandId(AppMainMenuCommand::NewProject)},
        {{ 'o', true, false, false }, appMainMenuCommandId(AppMainMenuCommand::OpenProject)},
        {{ 's', true, false, false }, appMainMenuCommandId(AppMainMenuCommand::SaveProject)},
        {{ 's', true, true, false }, appMainMenuCommandId(AppMainMenuCommand::SaveProjectAs)},
        {{ 'z', true, false, false }, appMainMenuCommandId(AppMainMenuCommand::UndoProject)},
        {{ 'y', true, false, false }, appMainMenuCommandId(AppMainMenuCommand::RedoProject)},
        {{ 'z', true, true, false }, appMainMenuCommandId(AppMainMenuCommand::RedoProject)}
    };
}

std::string describeAppShortcutChord(const AppShortcutChord& chord)
{
    if (chord.key == '\0') {
        return {};
    }

    std::ostringstream label;
    bool needsSeparator = false;
    const auto appendPart = [&](const std::string& part) {
        if (needsSeparator) {
            label << '+';
        }
        label << part;
        needsSeparator = true;
    };

    if (chord.primaryModifier) {
        appendPart("Ctrl");
    }
    if (chord.shift) {
        appendPart("Shift");
    }
    if (chord.alt) {
        appendPart("Alt");
    }
    appendPart(std::string(1, displayShortcutKey(chord.key)));

    return label.str();
}

std::optional<int> appCommandIdForShortcut(const AppShortcutChord& chord)
{
    // 第一批快捷键只接受“主修饰键 + 字母”，Alt 组合保留给系统菜单或未来明确设计。
    if (!chord.primaryModifier || chord.alt || chord.key == '\0') {
        return std::nullopt;
    }

    for (const auto& binding : defaultAppShortcutBindings()) {
        if (sameShortcutChord(chord, binding.chord)) {
            return binding.commandId;
        }
    }

    return std::nullopt;
}

}
