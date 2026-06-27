#pragma once

#include "MidiPlayback.h"
#include "Project.h"
#include "Transport.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace trackloom {

// PlaybackTickWindow 表示一个音频 block 对应的工程 tick 半开窗口。
// startTick 包含，endTick 不包含；这个规则与 MidiPlayback 的调度窗口保持一致。
struct PlaybackTickWindow {
    std::int64_t startTick = 0;
    std::int64_t endTick = 0;

    bool operator==(const PlaybackTickWindow&) const = default;
};

// ScheduledMidiPlaybackEvent 是一次音频 block 内真正可调度的 MIDI 事件。
// event 保留音乐语义；sampleOffset 表示它应在当前 block 的第几个 sample 触发。
struct ScheduledMidiPlaybackEvent {
    MidiPlaybackEvent event;
    int sampleOffset = 0;

    bool operator==(const ScheduledMidiPlaybackEvent&) const = default;
};

// playbackTickWindowForBlock 只把 Transport 当前 sample 窗口换算为 tick 窗口。
// 它不推进播放头，也不收集事件，方便后续音频线程在明确边界内调用。
std::optional<PlaybackTickWindow> playbackTickWindowForBlock(
    const Project& project,
    const Transport& transport,
    int frameCount);

// collectMidiPlaybackEventsForBlock 是播放桥接便利函数。
// 它先计算 block 的 tick 窗口，再复用已测试的 MIDI 调度器收集事件。
std::vector<MidiPlaybackEvent> collectMidiPlaybackEventsForBlock(
    const Project& project,
    const Transport& transport,
    int frameCount);

// collectScheduledMidiPlaybackEventsForBlock 为后续设备或插件桥接准备 block 内 sample offset。
// 它仍不发送事件，只把已验证的 MIDI 播放事件转换成当前 block 内的调度位置。
std::vector<ScheduledMidiPlaybackEvent> collectScheduledMidiPlaybackEventsForBlock(
    const Project& project,
    const Transport& transport,
    int frameCount);

}
