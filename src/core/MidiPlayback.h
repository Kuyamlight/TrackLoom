#pragma once

#include "Project.h"

#include <cstdint>
#include <string>
#include <vector>

namespace trackloom {

// MidiPlaybackEventType 描述调度器输出的最小 MIDI 播放事件类型。
// 当前只支持音符开和音符关；CC、弯音、触后等事件会在后续 MIDI 表达能力扩展时加入。
enum class MidiPlaybackEventType {
    NoteOn,
    NoteOff
};

// MidiPlaybackEvent 是从工程数据计算出的播放事件，不是持久化工程数据。
// absoluteTick 使用工程时间线坐标，方便后续接入插件、MIDI 输出或离线渲染。
struct MidiPlaybackEvent {
    MidiPlaybackEventType type = MidiPlaybackEventType::NoteOn;
    std::string trackId;
    std::string clipId;
    std::string noteId;
    std::int64_t absoluteTick = 0;
    int noteNumber = 60;
    int velocity = 0;
    int channel = 1;

    bool operator==(const MidiPlaybackEvent&) const = default;
};

// collectMidiPlaybackEvents 收集半开 tick 窗口 [startTick, endTick) 内的 MIDI 播放事件。
// 这是纯调度函数：不修改工程，不访问设备，不分配实时音频资源，也不产生声音。
std::vector<MidiPlaybackEvent> collectMidiPlaybackEvents(
    const Project& project,
    std::int64_t startTick,
    std::int64_t endTick);

}
