#pragma once

#include "MidiDispatch.h"

#include <cstddef>
#include <string>

namespace trackloom {

// MidiOutputDeviceInfo 是真实 MIDI 输出端口的稳定身份信息。
// id 用于保存和重新绑定设备；name 用于界面显示，后续可以来自 JUCE 或 Windows MIDI API。
struct MidiOutputDeviceInfo {
    std::string id;
    std::string name;

    bool operator==(const MidiOutputDeviceInfo&) const = default;
};

// MidiOutputDeviceFailureReason 是设备发送层的稳定失败原因。
// 上层用它判断是否需要重新打开设备、提示用户重新选择设备，或保留当前播放状态。
enum class MidiOutputDeviceFailureReason {
    None,
    DeviceNotOpen,
    OpenRejected,
    SendRejected
};

// MidiOutputDevicePort 是平台 MIDI 端口的最小接口。
// 核心库只依赖这个接口；真正的 JUCE/Windows 设备实现后续放在平台适配层。
class MidiOutputDevicePort {
public:
    virtual ~MidiOutputDevicePort() = default;

    virtual const MidiOutputDeviceInfo& info() const = 0;
    virtual bool open() = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual bool sendMidiMessage(const MidiOutputMessage& message) = 0;
};

// MidiOutputDevice 把真实 MIDI 端口接入现有 MidiDispatch 流程。
// 它不做 MIDI 事件转换；转换仍由 MidiDispatch 统一负责，避免设备层重复解释事件。
class MidiOutputDevice final : public MidiEventReceiver {
public:
    explicit MidiOutputDevice(MidiOutputDevicePort& port);

    const MidiOutputDeviceInfo& info() const;
    bool open();
    void close();
    bool isOpen() const;

    MidiOutputDeviceFailureReason lastFailureReason() const;
    std::size_t sentMessageCount() const;

    bool receiveMidiEvent(
        const ScheduledMidiPlaybackEvent& event,
        const MidiOutputMessage& message) override;

private:
    MidiOutputDevicePort& port_;
    MidiOutputDeviceFailureReason lastFailureReason_ = MidiOutputDeviceFailureReason::None;
    std::size_t sentMessageCount_ = 0;
};

}
