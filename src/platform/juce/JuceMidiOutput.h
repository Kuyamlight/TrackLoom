#pragma once

#include "MidiOutputDevice.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <memory>
#include <vector>

namespace trackloom {

// 把 JUCE 的设备描述转换成核心层稳定描述。
// 这样工程保存和设备路由只依赖 id/name，不直接依赖 JUCE 类型。
MidiOutputDeviceInfo midiOutputDeviceInfoFromJuce(const juce::MidiDeviceInfo& info);

// 把核心层已经验证过的三字节 MIDI 消息转换为 JUCE 消息。
// sampleOffset 仍由上层调度使用；JUCE 端口只负责立即发送字节。
juce::MidiMessage juceMidiMessageFromOutputMessage(const MidiOutputMessage& message);

// 枚举当前系统可打开的 JUCE MIDI 输出设备。
// 没有外接设备时返回空列表，这是正常状态，不应视为错误。
std::vector<MidiOutputDeviceInfo> availableJuceMidiOutputDevices();

// JUCE MIDI 输出端口是核心 MidiOutputDevicePort 的平台实现。
// 它只管理真实设备句柄，不参与播放调度、活动音符追踪或工程数据修改。
class JuceMidiOutputPort final : public MidiOutputDevicePort {
public:
    explicit JuceMidiOutputPort(MidiOutputDeviceInfo info);

    const MidiOutputDeviceInfo& info() const override;
    bool open() override;
    void close() override;
    bool isOpen() const override;
    bool sendMidiMessage(const MidiOutputMessage& message) override;

private:
    MidiOutputDeviceInfo info_;
    std::unique_ptr<juce::MidiOutput> output_;
};

// 用工厂函数隐藏具体端口类型，方便后续设备管理器只保存接口指针。
std::unique_ptr<MidiOutputDevicePort> createJuceMidiOutputPort(MidiOutputDeviceInfo info);

}
