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

}
