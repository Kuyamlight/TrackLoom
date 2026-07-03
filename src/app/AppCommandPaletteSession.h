#pragma once

#include "AppCommandPalette.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace trackloom {

struct AppCommandPaletteSessionStatus {
    bool open = false;
    std::string query;
    AppCommandPaletteStatus filteredPalette;
    std::optional<std::size_t> highlightedIndex;
};

struct AppCommandPaletteSessionRow {
    int commandId = 0;
    bool enabled = false;
    bool highlighted = false;
    std::string groupName;
    std::string label;
    std::string shortcutLabel;
};

struct AppCommandPaletteSessionView {
    bool open = false;
    std::string query;
    std::vector<AppCommandPaletteSessionRow> rows;
    std::string emptyMessage;
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

// describeAppCommandPaletteSession 把会话状态转换成 UI 可直接渲染的只读快照。
// UI 不需要自己解释 highlightedIndex、disabled 项或空查询结果。
AppCommandPaletteSessionView describeAppCommandPaletteSession(
    const AppCommandPaletteSessionStatus& status);

// describeAppCommandPaletteSessionRow 生成单行可见文本。
// JUCE、测试或后续诊断面板复用它，避免各处重复拼接菜单组、快捷键和禁用提示。
std::string describeAppCommandPaletteSessionRow(
    const AppCommandPaletteSessionRow& row);

}
