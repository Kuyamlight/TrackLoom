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

struct AppCommandPaletteVisibleRowsView {
    std::size_t firstRowIndex = 0;
    std::size_t totalRowCount = 0;
    bool hasPreviousRows = false;
    bool hasNextRows = false;
    std::vector<AppCommandPaletteSessionRow> rows;
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
    // Home/End 只把高亮移动到边界 enabled 命令；真正执行仍由 Enter 或点击激活函数负责。
    void moveHighlightToFirst();
    void moveHighlightToLast();
    // PageUp/PageDown 使用当前 UI 可见行数作为跳转步长。
    // 这里只移动高亮，不滚动 JUCE 组件；可见窗口由只读 view helper 计算。
    void moveHighlightPageDown(std::size_t visibleRowCount);
    void moveHighlightPageUp(std::size_t visibleRowCount);
    // 鼠标滚轮使用夹紧语义：正数向下、负数向上，滚到边界后停住，不像上下键那样循环。
    void moveHighlightByWheelSteps(int stepCount);

private:
    void refreshFilteredPalette();
    void moveHighlight(bool forward);
    void moveHighlightByPage(bool forward, std::size_t visibleRowCount);

    AppCommandPaletteStatus sourcePalette_;
    AppCommandPaletteSessionStatus status_;
};

// activateHighlightedAppCommandPaletteCommand 是命令面板按 Enter 的应用层边界。
// 它只执行当前高亮的 enabled 命令，不根据 query 重新选择第一条结果。
AppCommandPaletteActivationResult activateHighlightedAppCommandPaletteCommand(
    const AppCommandPaletteSessionStatus& status,
    const AppCommandHandlers& handlers);

// activateAppCommandPaletteSessionRow 是命令面板鼠标点击行的应用层边界。
// UI 只传当前渲染行对应的 commandId；这里重新确认会话打开、命令仍在过滤结果中且可用。
AppCommandPaletteActivationResult activateAppCommandPaletteSessionRow(
    const AppCommandPaletteSessionStatus& status,
    int commandId,
    const AppCommandHandlers& handlers);

// describeAppCommandPaletteSession 把会话状态转换成 UI 可直接渲染的只读快照。
// UI 不需要自己解释 highlightedIndex、disabled 项或空查询结果。
AppCommandPaletteSessionView describeAppCommandPaletteSession(
    const AppCommandPaletteSessionStatus& status);

// describeAppCommandPaletteSessionRow 生成单行可见文本。
// JUCE、测试或后续诊断面板复用它，避免各处重复拼接菜单组、快捷键和禁用提示。
std::string describeAppCommandPaletteSessionRow(
    const AppCommandPaletteSessionRow& row);

// firstVisibleAppCommandPaletteSessionRowIndex 计算 UI 应显示的第一条结果。
// 它只读取 view，不依赖 JUCE；高亮项超出可见范围时，让窗口刚好滚到能看到高亮行。
std::size_t firstVisibleAppCommandPaletteSessionRowIndex(
    const AppCommandPaletteSessionView& view,
    std::size_t visibleRowCount);

// describeVisibleAppCommandPaletteSessionRows 把完整命令面板结果裁剪成当前 UI 可显示的窗口。
// JUCE 只负责渲染 rows；滚动起点、总数和上下是否还有隐藏行都由应用层统一计算。
AppCommandPaletteVisibleRowsView describeVisibleAppCommandPaletteSessionRows(
    const AppCommandPaletteSessionView& view,
    std::size_t visibleRowCount);

// describeAppCommandPaletteVisibleRowsRange 生成当前可见结果范围，例如 "3-8 / 12"。
// 空可见窗口返回空字符串，避免 UI 在无结果或零行布局时显示误导性的范围。
std::string describeAppCommandPaletteVisibleRowsRange(
    const AppCommandPaletteVisibleRowsView& visibleRows);

}
