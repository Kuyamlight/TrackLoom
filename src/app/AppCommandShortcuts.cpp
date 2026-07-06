#include "AppCommandShortcuts.h"

#include "AppMainMenu.h"

#include <algorithm>
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

std::optional<int> commandIdForShortcutInBindings(
    const AppShortcutChord& chord,
    const std::vector<AppShortcutBinding>& bindings)
{
    if (!isSupportedAppShortcutChord(chord)) {
        return std::nullopt;
    }

    for (const auto& binding : bindings) {
        if (sameShortcutChord(chord, binding.chord)) {
            return binding.commandId;
        }
    }

    return std::nullopt;
}

void removeBindingsForCommand(std::vector<AppShortcutBinding>& bindings, int commandId)
{
    // 一个命令可能有多个默认快捷键，例如命令面板同时支持 Ctrl+K 和 Ctrl+Shift+P。
    // 用户覆盖该命令时，先移除旧绑定，避免同一命令留下多份来源不清的快捷键。
    bindings.erase(
        std::remove_if(
            bindings.begin(),
            bindings.end(),
            [commandId](const AppShortcutBinding& binding) {
                return binding.commandId == commandId;
            }),
        bindings.end());
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
        {{ 'z', true, true, false }, appMainMenuCommandId(AppMainMenuCommand::RedoProject)},
        {{ 'k', true, false, false }, appMainMenuCommandId(AppMainMenuCommand::OpenCommandPalette)},
        {{ 'p', true, true, false }, appMainMenuCommandId(AppMainMenuCommand::OpenCommandPalette)}
    };
}

bool isSupportedAppShortcutChord(const AppShortcutChord& chord)
{
    // 首期只接受“主修饰键 + 字母”的应用内快捷键。
    // Alt 组合常被系统菜单占用，先显式拒绝，避免以后出现平台行为差异。
    return chord.primaryModifier && !chord.alt && chord.key != '\0';
}

AppShortcutCustomizationResult customizeAppShortcutBindings(
    const std::vector<AppShortcutBinding>& customBindings)
{
    AppShortcutCustomizationResult result;
    result.bindings = defaultAppShortcutBindings();

    for (const auto& customBinding : customBindings) {
        if (customBinding.commandId <= 0 || !isSupportedAppShortcutChord(customBinding.chord)) {
            continue;
        }

        const auto existingCommandId = commandIdForShortcutInBindings(customBinding.chord, result.bindings);
        if (existingCommandId.has_value() && existingCommandId.value() != customBinding.commandId) {
            result.conflicts.push_back({
                customBinding.chord,
                existingCommandId.value(),
                customBinding.commandId
            });
            continue;
        }

        removeBindingsForCommand(result.bindings, customBinding.commandId);
        result.bindings.push_back(customBinding);
    }

    return result;
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
    return appCommandIdForShortcut(chord, defaultAppShortcutBindings());
}

std::optional<int> appCommandIdForShortcut(
    const AppShortcutChord& chord,
    const std::vector<AppShortcutBinding>& bindings)
{
    return commandIdForShortcutInBindings(chord, bindings);
}

std::optional<int> appCommandIdForShortcut(
    const AppShortcutChord& chord,
    AppShortcutContext context)
{
    if (context == AppShortcutContext::CommandPaletteOpen) {
        return std::nullopt;
    }

    return appCommandIdForShortcut(chord);
}

}
