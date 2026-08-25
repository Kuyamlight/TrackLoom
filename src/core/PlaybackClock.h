#pragma once

#include "LoopRange.h"
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

// LoopedPlaybackTickWindow 是循环播放时，一个音频 block 内的子窗口。
// sampleOffset 和 frameCount 都相对原始 block，方便后续 MIDI 设备或插件按同一个 block 调度。
struct LoopedPlaybackTickWindow {
    PlaybackTickWindow window;
    int sampleOffset = 0;
    int frameCount = 0;

    bool operator==(const LoopedPlaybackTickWindow&) const = default;
};

// MidiChaseMode 控制播放窗口是否补发窗口起点已经按下的 MIDI 音符。
// 默认关闭，避免连续播放时每个 block 都重复触发长音符；需要起播或 seek chase 时显式开启。
enum class MidiChaseMode {
    Disabled,
    Enabled
};

// playbackTickWindowForBlock 只把 Transport 当前 sample 窗口换算为 tick 窗口。
// 它不推进播放头，也不收集事件，方便后续音频线程在明确边界内调用。
std::optional<PlaybackTickWindow> playbackTickWindowForBlock(
    const Project& project,
    const Transport& transport,
    int frameCount);

// collectMidiPlaybackEventsForBlock 是播放桥接便利函数。
// 只有 chaseMode 为 Enabled 时，才会为窗口起点已经按下的音符补发 chase Note On。
std::vector<MidiPlaybackEvent> collectMidiPlaybackEventsForBlock(
    const Project& project,
    const Transport& transport,
    int frameCount,
    MidiChaseMode chaseMode = MidiChaseMode::Disabled);

// collectScheduledMidiPlaybackEventsForBlock 为后续设备或插件桥接准备 block 内 sample offset。
// 它仍不发送事件，只把 MIDI 播放事件转换成当前 block 内的调度位置；chase 由调用方显式选择。
std::vector<ScheduledMidiPlaybackEvent> collectScheduledMidiPlaybackEventsForBlock(
    const Project& project,
    const Transport& transport,
    int frameCount,
    MidiChaseMode chaseMode = MidiChaseMode::Disabled);

// playbackTickWindowsForLoopedBlock 把一个 block 拆成按播放顺序排列的循环子窗口。
// 它只做确定性时间切分，不推进 Transport，也不修改工程或打开外部设备。
std::vector<LoopedPlaybackTickWindow> playbackTickWindowsForLoopedBlock(
    const Project& project,
    const Transport& transport,
    int frameCount,
    const PlaybackLoopRange& loopRange);

// collectScheduledMidiPlaybackEventsForLoopedBlock 复用循环子窗口收集 MIDI 事件。
// 返回的 sampleOffset 是原始 block 内的位置；显式 chase 时回绕后的 chase 事件也按原始 block 继续计数。
std::vector<ScheduledMidiPlaybackEvent> collectScheduledMidiPlaybackEventsForLoopedBlock(
    const Project& project,
    const Transport& transport,
    int frameCount,
    const PlaybackLoopRange& loopRange,
    MidiChaseMode chaseMode = MidiChaseMode::Disabled);

}
