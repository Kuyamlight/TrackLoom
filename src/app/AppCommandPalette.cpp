#include "AppCommandPalette.h"

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
        const auto searchableText = item.groupName + " " + item.label;
        if (containsSearchText(searchableText, query)) {
            status.items.push_back(item);
        }
    }

    return status;
}

}
