#include "AppShortcutStatus.h"

#include <map>
#include <set>
#include <sstream>
#include <string>

namespace trackloom {
namespace {

bool isCommandItem(const AppMainMenuItem& item)
{
    return !item.separator && item.commandId > 0;
}

void appendShortcutLabel(std::string& label, const AppShortcutChord& chord)
{
    const auto nextLabel = describeAppShortcutChord(chord);
    if (nextLabel.empty()) {
        return;
    }

    if (!label.empty()) {
        label += ", ";
    }
    label += nextLabel;
}

std::map<int, std::string> shortcutLabelsByCommandId(
    const std::vector<AppShortcutBinding>& bindings)
{
    std::map<int, std::string> labels;
    for (const auto& binding : bindings) {
        appendShortcutLabel(labels[binding.commandId], binding.chord);
    }

    return labels;
}

std::map<int, std::string> commandLabelsById(const AppMainMenuStatus& menu)
{
    std::map<int, std::string> labels;
    for (const auto& group : menu.groups) {
        for (const auto& item : group.items) {
            if (isCommandItem(item)) {
                labels[item.commandId] = item.label;
            }
        }
    }

    return labels;
}

std::string commandLabelOrFallback(
    const std::map<int, std::string>& labels,
    int commandId)
{
    if (const auto found = labels.find(commandId); found != labels.end()) {
        return found->second;
    }

    return "未知命令 " + std::to_string(commandId);
}

std::set<int> activeCustomCommandIds(
    const AppShortcutCustomizationResult& customization,
    const std::vector<AppShortcutBinding>& customBindings)
{
    std::set<int> commandIds;
    for (const auto& customBinding : customBindings) {
        if (customBinding.commandId <= 0 || !isSupportedAppShortcutChord(customBinding.chord)) {
            continue;
        }

        const auto activeCommandId = appCommandIdForShortcut(
            customBinding.chord,
            customization.bindings);
        if (activeCommandId.has_value() && activeCommandId.value() == customBinding.commandId) {
            commandIds.insert(customBinding.commandId);
        }
    }

    return commandIds;
}

std::string shortcutSummary(
    std::size_t commandCount,
    std::size_t activeCustomBindingCount,
    std::size_t conflictCount)
{
    std::ostringstream summary;
    summary << "快捷键："
            << commandCount << " 个命令，"
            << activeCustomBindingCount << " 个自定义快捷键，"
            << conflictCount << " 个冲突。";
    return summary.str();
}

}

AppShortcutStatus describeAppShortcutStatus(
    const AppMainMenuStatus& menu,
    const AppShortcutCustomizationResult& customization,
    const std::vector<AppShortcutBinding>& customBindings)
{
    AppShortcutStatus status;
    status.customBindingCount = customBindings.size();

    const auto labelsByCommandId = commandLabelsById(menu);
    const auto shortcutLabels = shortcutLabelsByCommandId(customization.bindings);
    const auto customizedCommandIds = activeCustomCommandIds(customization, customBindings);
    status.activeCustomBindingCount = customizedCommandIds.size();

    for (const auto& group : menu.groups) {
        for (const auto& item : group.items) {
            if (!isCommandItem(item)) {
                continue;
            }

            AppShortcutStatusRow row;
            row.commandId = item.commandId;
            row.enabled = item.enabled;
            row.customized = customizedCommandIds.find(item.commandId) != customizedCommandIds.end();
            row.groupName = group.name;
            row.label = item.label;
            if (const auto shortcutLabel = shortcutLabels.find(item.commandId);
                shortcutLabel != shortcutLabels.end()) {
                row.shortcutLabel = shortcutLabel->second;
            }
            status.rows.push_back(std::move(row));
        }
    }

    for (const auto& conflict : customization.conflicts) {
        status.conflicts.push_back({
            conflict.chord,
            describeAppShortcutChord(conflict.chord),
            conflict.existingCommandId,
            commandLabelOrFallback(labelsByCommandId, conflict.existingCommandId),
            conflict.requestedCommandId,
            commandLabelOrFallback(labelsByCommandId, conflict.requestedCommandId)
        });
    }

    status.summary = shortcutSummary(
        status.rows.size(),
        status.activeCustomBindingCount,
        status.conflicts.size());
    return status;
}

}
