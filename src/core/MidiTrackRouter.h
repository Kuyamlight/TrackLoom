#pragma once

#include "MidiDispatch.h"

#include <cstddef>
#include <string>
#include <vector>

namespace trackloom {

// MidiTrackReceiverBinding 说明一条工程轨道当前应把 MIDI 发往哪个接收器。
// receiver 不拥有对象；插件、采样器或测试接收器的生命周期由调用方管理。
struct MidiTrackReceiverBinding {
    std::string trackId;
    MidiEventReceiver* receiver = nullptr;
};

// MidiTrackRouter 是按轨道分发 MIDI 的轻量接收器。
// 它本身不转换 MIDI 消息，只把 MidiDispatch 已转换好的消息转交给对应轨道。
class MidiTrackRouter final : public MidiEventReceiver {
public:
    // rebuild 在播放前更新路由表；非法输入会返回 false，并保留之前的有效路由。
    bool rebuild(const std::vector<MidiTrackReceiverBinding>& bindings);

    std::size_t receiverCount() const;

    bool receiveMidiEvent(
        const ScheduledMidiPlaybackEvent& event,
        const MidiOutputMessage& message) override;

private:
    std::vector<MidiTrackReceiverBinding> bindings_;
};

}
