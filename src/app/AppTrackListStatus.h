#pragma once

#include "Project.h"

#include <cstddef>
#include <string>
#include <vector>

namespace trackloom {

// AppTrackListRow 是桌面 UI 的轨道列表行快照。
// 它只复制显示需要的稳定信息，不把 UI 选择状态写回 Project。
struct AppTrackListRow {
    std::size_t number = 0;
    std::string trackId;
    std::string name;
    std::string typeLabel;
    std::size_t clipCount = 0;
    std::vector<std::string> stateLabels;
    std::string summary;
};

// AppTrackListStatus 是首屏轨道列表可以直接消费的值类型状态。
// 空列表和真实行分开表达，便于 UI 在没有轨道时给出明确下一步。
struct AppTrackListStatus {
    std::vector<AppTrackListRow> rows;
    std::string emptyMessage;
};

// describeAppTrackList 把核心 Project 中的轨道转换成可展示列表。
// 后续轨道选择、重命名、MIDI 片段入口应复用这里的行语义，避免 UI 自己解释 TrackType 和状态标记。
AppTrackListStatus describeAppTrackList(const Project& project);

}
