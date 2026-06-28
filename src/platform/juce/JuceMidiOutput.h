#pragma once

#include "MidiOutputDevice.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <memory>
#include <optional>
#include <string>
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

// 按稳定设备 id 查找当前可见的 JUCE MIDI 输出设备。
// 设备不在线或 id 为空时返回空结果，调用方据此决定跳过、提示或失败。
std::optional<MidiOutputDeviceInfo> findJuceMidiOutputDeviceById(const std::string& deviceId);

// 两次 MIDI 输出设备枚举之间的差异。
// added/retained 使用当前列表的设备信息，removed 使用旧列表的设备信息。
struct MidiOutputDeviceListDiff {
    std::vector<MidiOutputDeviceInfo> added;
    std::vector<MidiOutputDeviceInfo> removed;
    std::vector<MidiOutputDeviceInfo> retained;
};

// 比较两次设备快照，只按稳定 id 判断设备身份。
// 这个函数不访问系统设备，便于 UI、热插拔提示和测试复用同一套规则。
MidiOutputDeviceListDiff diffMidiOutputDeviceLists(
    const std::vector<MidiOutputDeviceInfo>& previous,
    const std::vector<MidiOutputDeviceInfo>& current);

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
