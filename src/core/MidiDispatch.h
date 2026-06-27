#pragma once

#include "PlaybackClock.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace trackloom {

// MidiOutputMessage 是发送层最小 MIDI 1.0 三字节消息。
// sampleOffset 表示消息应在当前音频 block 的第几个 sample 生效。
struct MidiOutputMessage {
    int sampleOffset = 0;
    std::uint8_t statusByte = 0;
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;

    bool operator==(const MidiOutputMessage&) const = default;
};

// MidiDispatchResult 记录一次批量发送的结果，便于设备或插件失败时上层做降级处理。
// failedEventIndex 为 -1 表示没有失败；失败时使用输入事件数组的 0 基索引。
struct MidiDispatchResult {
    bool success = true;
    int attemptedEventCount = 0;
    int deliveredEventCount = 0;
    int failedEventIndex = -1;
};

// MidiEventReceiver 是 MIDI 输出的抽象接收器。
// 测试使用假接收器；真实设备、插件或采样器桥接器后续实现这个接口。
class MidiEventReceiver {
public:
    virtual ~MidiEventReceiver() = default;

    virtual bool receiveMidiEvent(
        const ScheduledMidiPlaybackEvent& event,
        const MidiOutputMessage& message) = 0;
};

// midiOutputMessageForEvent 把高层 scheduled 事件转换为 MIDI 1.0 输出消息。
// 输入非法时返回空值，避免把坏工程数据或坏调用传给真实设备。
std::optional<MidiOutputMessage> midiOutputMessageForEvent(
    const ScheduledMidiPlaybackEvent& event);

// dispatchScheduledMidiEvents 按输入顺序发送事件，遇到转换失败或接收器失败立即停止。
// 它不打开设备、不访问插件、不分配后台线程，只调用传入的 receiver。
MidiDispatchResult dispatchScheduledMidiEvents(
    const std::vector<ScheduledMidiPlaybackEvent>& events,
    MidiEventReceiver& receiver);

}
