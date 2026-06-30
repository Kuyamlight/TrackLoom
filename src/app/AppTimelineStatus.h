#pragma once

#include "Project.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace trackloom {

// AppTimelineClipRow 是桌面首屏时间线的只读片段行。
// 它复制 UI 展示需要的信息，不把选择、拖拽或编辑状态写回 Project。
struct AppTimelineClipRow {
    std::size_t number = 0;
    std::string clipId;
    std::string trackId;
    std::string trackName;
    std::string name;
    ClipType type = ClipType::Midi;
    std::string typeLabel;
    std::int64_t startTick = 0;
    std::int64_t lengthTick = 0;
    std::size_t noteCount = 0;
    // 空 MIDI 片段没有“末尾音符”，UI 必须先看这个开关再显示下面的字段。
    bool hasLastMidiNote = false;
    // 下面四个字段描述当前按钮式音符编辑入口会操作的那个末尾音符。
    std::int64_t lastMidiNoteStartTick = 0;
    std::int64_t lastMidiNoteLengthTick = 0;
    int lastMidiNoteNumber = 0;
    int lastMidiNoteVelocity = 0;
    std::string summary;
};

// AppTimelineStatus 是首屏时间线可以直接渲染的值类型快照。
// 当前只展示已有片段；真正的拖拽、裁剪和钢琴卷帘编辑会由后续命令入口处理。
struct AppTimelineStatus {
    std::vector<AppTimelineClipRow> rows;
    std::string emptyMessage;
};

// describeAppTimeline 把 Project 里的时间线片段转换成中文摘要。
// UI 只消费这里的结果，避免界面层重复解释 ClipType、tick 和轨道归属。
AppTimelineStatus describeAppTimeline(const Project& project);

}
