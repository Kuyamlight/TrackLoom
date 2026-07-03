#include "AppCommandPaletteSession.h"

#include <utility>

namespace trackloom {
namespace {

std::optional<std::size_t> firstEnabledIndex(const AppCommandPaletteStatus& palette)
{
    for (std::size_t index = 0; index < palette.items.size(); ++index) {
        if (palette.items[index].enabled) {
            return index;
        }
    }

    return std::nullopt;
}

std::optional<std::size_t> lastEnabledIndex(const AppCommandPaletteStatus& palette)
{
    for (std::size_t index = palette.items.size(); index > 0; --index) {
        const auto candidate = index - 1;
        if (palette.items[candidate].enabled) {
            return candidate;
        }
    }

    return std::nullopt;
}

std::optional<std::size_t> firstEnabledIndexAtOrAfter(
    const AppCommandPaletteStatus& palette,
    std::size_t startIndex)
{
    for (std::size_t index = startIndex; index < palette.items.size(); ++index) {
        if (palette.items[index].enabled) {
            return index;
        }
    }

    return std::nullopt;
}

std::optional<std::size_t> lastEnabledIndexAtOrBefore(
    const AppCommandPaletteStatus& palette,
    std::size_t startIndex)
{
    if (palette.items.empty()) {
        return std::nullopt;
    }

    auto index = startIndex >= palette.items.size()
        ? palette.items.size() - 1
        : startIndex;

    for (;;) {
        if (palette.items[index].enabled) {
            return index;
        }

        if (index == 0) {
            break;
        }

        --index;
    }

    return std::nullopt;
}

AppCommandPaletteSelectionResult selectHighlightedItem(const AppCommandPaletteSessionStatus& status)
{
    if (!status.open || status.filteredPalette.items.empty()) {
        return {
            AppCommandPaletteSelectionResultKind::NoMatchingCommand,
            std::nullopt
        };
    }

    if (!status.highlightedIndex.has_value()
        || status.highlightedIndex.value() >= status.filteredPalette.items.size()) {
        return {
            AppCommandPaletteSelectionResultKind::OnlyDisabledMatches,
            std::nullopt
        };
    }

    const auto& item = status.filteredPalette.items[status.highlightedIndex.value()];
    if (!item.enabled) {
        return {
            AppCommandPaletteSelectionResultKind::OnlyDisabledMatches,
            std::nullopt
        };
    }

    return {
        AppCommandPaletteSelectionResultKind::Selected,
        item
    };
}

AppCommandPaletteSelectionResult selectVisibleItemByCommandId(
    const AppCommandPaletteSessionStatus& status,
    int commandId)
{
    if (!status.open || status.filteredPalette.items.empty()) {
        return {
            AppCommandPaletteSelectionResultKind::NoMatchingCommand,
            std::nullopt
        };
    }

    bool foundDisabledMatch = false;
    for (const auto& item : status.filteredPalette.items) {
        if (item.commandId != commandId) {
            continue;
        }

        if (item.enabled) {
            return {
                AppCommandPaletteSelectionResultKind::Selected,
                item
            };
        }

        foundDisabledMatch = true;
    }

    return {
        foundDisabledMatch
            ? AppCommandPaletteSelectionResultKind::OnlyDisabledMatches
            : AppCommandPaletteSelectionResultKind::NoMatchingCommand,
        std::nullopt
    };
}

AppCommandPaletteActivationResult activateSelectedPaletteItem(
    AppCommandPaletteSelectionResult selection,
    const AppCommandHandlers& handlers)
{
    AppCommandPaletteActivationResult result;
    result.selection = std::move(selection);

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

}

const AppCommandPaletteSessionStatus& AppCommandPaletteSession::status() const
{
    return status_;
}

void AppCommandPaletteSession::open(AppCommandPaletteStatus palette)
{
    sourcePalette_ = std::move(palette);
    status_.open = true;
    status_.query.clear();
    refreshFilteredPalette();
}

void AppCommandPaletteSession::close()
{
    sourcePalette_.items.clear();
    status_ = {};
}

void AppCommandPaletteSession::updateQuery(std::string query)
{
    if (!status_.open) {
        return;
    }

    status_.query = std::move(query);
    refreshFilteredPalette();
}

void AppCommandPaletteSession::moveHighlightDown()
{
    moveHighlight(true);
}

void AppCommandPaletteSession::moveHighlightUp()
{
    moveHighlight(false);
}

void AppCommandPaletteSession::moveHighlightToFirst()
{
    if (!status_.open || status_.filteredPalette.items.empty()) {
        status_.highlightedIndex = std::nullopt;
        return;
    }

    status_.highlightedIndex = firstEnabledIndex(status_.filteredPalette);
}

void AppCommandPaletteSession::moveHighlightToLast()
{
    if (!status_.open || status_.filteredPalette.items.empty()) {
        status_.highlightedIndex = std::nullopt;
        return;
    }

    status_.highlightedIndex = lastEnabledIndex(status_.filteredPalette);
}

void AppCommandPaletteSession::moveHighlightPageDown(std::size_t visibleRowCount)
{
    moveHighlightByPage(true, visibleRowCount);
}

void AppCommandPaletteSession::moveHighlightPageUp(std::size_t visibleRowCount)
{
    moveHighlightByPage(false, visibleRowCount);
}

void AppCommandPaletteSession::refreshFilteredPalette()
{
    status_.filteredPalette = filterAppCommandPalette(sourcePalette_, status_.query);
    status_.highlightedIndex = firstEnabledIndex(status_.filteredPalette);
}

void AppCommandPaletteSession::moveHighlight(bool forward)
{
    if (!status_.open || status_.filteredPalette.items.empty()) {
        status_.highlightedIndex = std::nullopt;
        return;
    }

    if (!status_.highlightedIndex.has_value()
        || status_.highlightedIndex.value() >= status_.filteredPalette.items.size()) {
        status_.highlightedIndex = forward
            ? firstEnabledIndex(status_.filteredPalette)
            : lastEnabledIndex(status_.filteredPalette);
        return;
    }

    auto index = status_.highlightedIndex.value();
    for (std::size_t attempts = 0; attempts < status_.filteredPalette.items.size(); ++attempts) {
        if (forward) {
            index = (index + 1) % status_.filteredPalette.items.size();
        } else if (index == 0) {
            index = status_.filteredPalette.items.size() - 1;
        } else {
            --index;
        }

        if (status_.filteredPalette.items[index].enabled) {
            status_.highlightedIndex = index;
            return;
        }
    }

    status_.highlightedIndex = std::nullopt;
}

void AppCommandPaletteSession::moveHighlightByPage(bool forward, std::size_t visibleRowCount)
{
    if (visibleRowCount == 0) {
        return;
    }

    if (!status_.open || status_.filteredPalette.items.empty()) {
        status_.highlightedIndex = std::nullopt;
        return;
    }

    if (!status_.highlightedIndex.has_value()
        || status_.highlightedIndex.value() >= status_.filteredPalette.items.size()) {
        status_.highlightedIndex = forward
            ? firstEnabledIndex(status_.filteredPalette)
            : lastEnabledIndex(status_.filteredPalette);
        return;
    }

    const auto currentIndex = status_.highlightedIndex.value();
    const auto lastIndex = status_.filteredPalette.items.size() - 1;
    const auto targetIndex = forward
        ? (visibleRowCount > lastIndex - currentIndex ? lastIndex : currentIndex + visibleRowCount)
        : (currentIndex < visibleRowCount ? std::size_t{0} : currentIndex - visibleRowCount);

    if (status_.filteredPalette.items[targetIndex].enabled) {
        status_.highlightedIndex = targetIndex;
        return;
    }

    // PageDown 遇到 disabled 目标行时向下找；PageUp 则向上找。
    // 如果目标行之后/之前没有 enabled 命令，就夹到列表底部/顶部的 enabled 命令。
    status_.highlightedIndex = forward
        ? firstEnabledIndexAtOrAfter(status_.filteredPalette, targetIndex)
            .value_or(lastEnabledIndex(status_.filteredPalette).value_or(currentIndex))
        : lastEnabledIndexAtOrBefore(status_.filteredPalette, targetIndex)
            .value_or(firstEnabledIndex(status_.filteredPalette).value_or(currentIndex));
}

AppCommandPaletteActivationResult activateHighlightedAppCommandPaletteCommand(
    const AppCommandPaletteSessionStatus& status,
    const AppCommandHandlers& handlers)
{
    return activateSelectedPaletteItem(selectHighlightedItem(status), handlers);
}

AppCommandPaletteActivationResult activateAppCommandPaletteSessionRow(
    const AppCommandPaletteSessionStatus& status,
    int commandId,
    const AppCommandHandlers& handlers)
{
    return activateSelectedPaletteItem(selectVisibleItemByCommandId(status, commandId), handlers);
}

AppCommandPaletteSessionView describeAppCommandPaletteSession(
    const AppCommandPaletteSessionStatus& status)
{
    AppCommandPaletteSessionView view;
    view.open = status.open;
    view.query = status.query;

    if (!status.open) {
        return view;
    }

    for (std::size_t index = 0; index < status.filteredPalette.items.size(); ++index) {
        const auto& item = status.filteredPalette.items[index];
        view.rows.push_back({
            item.commandId,
            item.enabled,
            status.highlightedIndex.has_value()
                && status.highlightedIndex.value() == index
                && item.enabled,
            item.groupName,
            item.label,
            item.shortcutLabel
        });
    }

    if (view.rows.empty()) {
        view.emptyMessage = "没有匹配的命令";
    }

    return view;
}

std::string describeAppCommandPaletteSessionRow(
    const AppCommandPaletteSessionRow& row)
{
    auto text = row.groupName + " / " + row.label;

    if (!row.shortcutLabel.empty()) {
        text += "    " + row.shortcutLabel;
    }

    if (!row.enabled) {
        text += "    不可用";
    }

    return text;
}

std::size_t firstVisibleAppCommandPaletteSessionRowIndex(
    const AppCommandPaletteSessionView& view,
    std::size_t visibleRowCount)
{
    if (!view.open || visibleRowCount == 0 || view.rows.size() <= visibleRowCount) {
        return 0;
    }

    for (std::size_t index = 0; index < view.rows.size(); ++index) {
        if (!view.rows[index].highlighted) {
            continue;
        }

        if (index < visibleRowCount) {
            return 0;
        }

        const auto firstVisible = index - visibleRowCount + 1;
        const auto lastPossibleFirstVisible = view.rows.size() - visibleRowCount;
        return firstVisible > lastPossibleFirstVisible
            ? lastPossibleFirstVisible
            : firstVisible;
    }

    return 0;
}

AppCommandPaletteVisibleRowsView describeVisibleAppCommandPaletteSessionRows(
    const AppCommandPaletteSessionView& view,
    std::size_t visibleRowCount)
{
    AppCommandPaletteVisibleRowsView visibleRows;
    visibleRows.totalRowCount = view.rows.size();

    if (!view.open || visibleRowCount == 0 || view.rows.empty()) {
        return visibleRows;
    }

    visibleRows.firstRowIndex = firstVisibleAppCommandPaletteSessionRowIndex(
        view,
        visibleRowCount);

    const auto lastRowIndexExclusive = visibleRows.firstRowIndex + visibleRowCount > view.rows.size()
        ? view.rows.size()
        : visibleRows.firstRowIndex + visibleRowCount;

    for (auto rowIndex = visibleRows.firstRowIndex; rowIndex < lastRowIndexExclusive; ++rowIndex) {
        visibleRows.rows.push_back(view.rows[rowIndex]);
    }

    visibleRows.hasPreviousRows = visibleRows.firstRowIndex > 0;
    visibleRows.hasNextRows = lastRowIndexExclusive < view.rows.size();
    return visibleRows;
}

std::string describeAppCommandPaletteVisibleRowsRange(
    const AppCommandPaletteVisibleRowsView& visibleRows)
{
    if (visibleRows.rows.empty() || visibleRows.totalRowCount == 0) {
        return {};
    }

    const auto firstVisibleNumber = visibleRows.firstRowIndex + 1;
    const auto lastVisibleNumber = visibleRows.firstRowIndex + visibleRows.rows.size();

    if (firstVisibleNumber == lastVisibleNumber) {
        return std::to_string(firstVisibleNumber)
            + " / "
            + std::to_string(visibleRows.totalRowCount);
    }

    return std::to_string(firstVisibleNumber)
        + "-"
        + std::to_string(lastVisibleNumber)
        + " / "
        + std::to_string(visibleRows.totalRowCount);
}

}
