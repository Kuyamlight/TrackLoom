#pragma once

#include "AppCommandPalette.h"

#include <cstddef>
#include <optional>
#include <string>

namespace trackloom {

struct AppCommandPaletteSessionStatus {
    bool open = false;
    std::string query;
    AppCommandPaletteStatus filteredPalette;
    std::optional<std::size_t> highlightedIndex;
};

// AppCommandPaletteSession 保存命令面板弹出期间的临时 UI 状态。
// 它不执行命令、不依赖 JUCE，也不写入 Project 工程文件。
class AppCommandPaletteSession final {
public:
    const AppCommandPaletteSessionStatus& status() const;

    void open(AppCommandPaletteStatus palette);
    void close();
    void updateQuery(std::string query);
    void moveHighlightDown();
    void moveHighlightUp();

private:
    void refreshFilteredPalette();
    void moveHighlight(bool forward);

    AppCommandPaletteStatus sourcePalette_;
    AppCommandPaletteSessionStatus status_;
};

// activateHighlightedAppCommandPaletteCommand 是命令面板按 Enter 的应用层边界。
// 它只执行当前高亮的 enabled 命令，不根据 query 重新选择第一条结果。
AppCommandPaletteActivationResult activateHighlightedAppCommandPaletteCommand(
    const AppCommandPaletteSessionStatus& status,
    const AppCommandHandlers& handlers);

}
