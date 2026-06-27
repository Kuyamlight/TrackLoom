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

}
