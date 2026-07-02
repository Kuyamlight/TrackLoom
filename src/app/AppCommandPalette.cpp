#include "AppCommandPalette.h"

#include <map>
#include <string>

namespace trackloom {
namespace {

bool isCommandItem(const AppMainMenuItem& item)
{
    return !item.separator && item.commandId > 0;
}

std::string asciiLowerCopy(std::string value)
{
    for (auto& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }

    return value;
}

bool containsSearchText(const std::string& text, const std::string& query)
{
    if (query.empty()) {
        return true;
    }

    return asciiLowerCopy(text).find(asciiLowerCopy(query)) != std::string::npos;
}

void appendShortcutLabel(std::string& existingLabel, const std::string& nextLabel)
{
    if (nextLabel.empty()) {
        return;
    }

    if (!existingLabel.empty()) {
        existingLabel += ", ";
    }
    existingLabel += nextLabel;
}

}

AppCommandPaletteStatus describeAppCommandPalette(const AppMainMenuStatus& menu)
{
    AppCommandPaletteStatus status;

    for (const auto& group : menu.groups) {
        for (const auto& item : group.items) {
            if (!isCommandItem(item)) {
                continue;
            }

            // 命令面板复用菜单的 command id；真正执行仍走 AppCommandDispatcher。
            status.items.push_back({
                item.commandId,
                item.enabled,
                group.name,
                item.label
            });
        }
    }

    return status;
}

AppCommandPaletteStatus filterAppCommandPalette(
    const AppCommandPaletteStatus& palette,
    const std::string& query)
{
    AppCommandPaletteStatus status;

    for (const auto& item : palette.items) {
        const auto searchableText = item.groupName + " " + item.label + " " + item.shortcutLabel;
        if (containsSearchText(searchableText, query)) {
            status.items.push_back(item);
        }
    }

    return status;
}

AppCommandPaletteStatus addAppCommandPaletteShortcutLabels(
    const AppCommandPaletteStatus& palette,
    const std::vector<AppShortcutBinding>& shortcutBindings)
{
    std::map<int, std::string> labelsByCommandId;

    for (const auto& binding : shortcutBindings) {
        appendShortcutLabel(labelsByCommandId[binding.commandId], describeAppShortcutChord(binding.chord));
    }

    auto status = palette;
    for (auto& item : status.items) {
        if (const auto label = labelsByCommandId.find(item.commandId);
            label != labelsByCommandId.end()) {
            item.shortcutLabel = label->second;
        } else {
            item.shortcutLabel.clear();
        }
    }

    return status;
}

AppCommandPaletteSelectionResult selectAppCommandPaletteItem(
    const AppCommandPaletteStatus& palette,
    const std::string& query)
{
    const auto filteredPalette = filterAppCommandPalette(palette, query);

    if (filteredPalette.items.empty()) {
        return {
            AppCommandPaletteSelectionResultKind::NoMatchingCommand,
            std::nullopt
        };
    }

    for (const auto& item : filteredPalette.items) {
        if (item.enabled) {
            return {
                AppCommandPaletteSelectionResultKind::Selected,
                item
            };
        }
    }

    return {
        AppCommandPaletteSelectionResultKind::OnlyDisabledMatches,
        std::nullopt
    };
}

AppCommandPaletteActivationResult activateAppCommandPaletteCommand(
    const AppCommandPaletteStatus& palette,
    const std::string& query,
    const AppCommandHandlers& handlers)
{
    AppCommandPaletteActivationResult result;
    result.selection = selectAppCommandPaletteItem(palette, query);

    if (result.selection.kind == AppCommandPaletteSelectionResultKind::NoMatchingCommand) {
        result.kind = AppCommandPaletteActivationResultKind::NoMatchingCommand;
        return result;
    }

    if (result.selection.kind == AppCommandPaletteSelectionResultKind::OnlyDisabledMatches) {
        result.kind = AppCommandPaletteActivationResultKind::OnlyDisabledMatches;
        return result;
    }

    if (!result.selection.item.has_value()) {
        result.kind = AppCommandPaletteActivationResultKind::DispatchFailed;
        return result;
    }

    result.dispatch = dispatchAppCommand(result.selection.item->commandId, handlers);
    if (!result.dispatch.executed) {
        result.kind = AppCommandPaletteActivationResultKind::DispatchFailed;
        return result;
    }

    result.executed = true;
    result.kind = AppCommandPaletteActivationResultKind::Executed;
    return result;
}

std::optional<AppCommandPaletteItem> selectFirstExecutableAppCommand(
    const AppCommandPaletteStatus& palette,
    const std::string& query)
{
    return selectAppCommandPaletteItem(palette, query).item;
}

}
