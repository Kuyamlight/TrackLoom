#include "JuceMidiOutput.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace trackloom {

MidiOutputDeviceInfo midiOutputDeviceInfoFromJuce(const juce::MidiDeviceInfo& info)
{
    // identifier 是系统给端口的稳定标识，name 只用于显示。
    return MidiOutputDeviceInfo {
        info.identifier.toStdString(),
        info.name.toStdString()
    };
}

juce::MidiMessage juceMidiMessageFromOutputMessage(const MidiOutputMessage& message)
{
    // MidiDispatch 已经验证并生成三字节消息；这里不重新解释音乐语义。
    return juce::MidiMessage(
        static_cast<int>(message.statusByte),
        static_cast<int>(message.data1),
        static_cast<int>(message.data2));
}

std::vector<MidiOutputDeviceInfo> availableJuceMidiOutputDevices()
{
    std::vector<MidiOutputDeviceInfo> devices;

    // 没有外接 MIDI 设备时 JUCE 会返回空列表，调用方应把它当作正常状态。
    for (const auto& device : juce::MidiOutput::getAvailableDevices()) {
        devices.push_back(midiOutputDeviceInfoFromJuce(device));
    }

    return devices;
}

JuceMidiOutputPort::JuceMidiOutputPort(MidiOutputDeviceInfo info)
    : info_(std::move(info))
{
}

const MidiOutputDeviceInfo& JuceMidiOutputPort::info() const
{
    return info_;
}

bool JuceMidiOutputPort::open()
{
    // 重复打开同一端口不重新创建句柄，避免打断已经建立的设备连接。
    if (output_ != nullptr) {
        return true;
    }

    output_ = juce::MidiOutput::openDevice(juce::String(info_.id));
    return output_ != nullptr;
}

void JuceMidiOutputPort::close()
{
    output_.reset();
}

bool JuceMidiOutputPort::isOpen() const
{
    return output_ != nullptr;
}

bool JuceMidiOutputPort::sendMidiMessage(const MidiOutputMessage& message)
{
    if (output_ == nullptr) {
        return false;
    }

    // JUCE 的 sendMessageNow 没有失败返回值；这里的 true 只代表已把消息交给 JUCE。
    output_->sendMessageNow(juceMidiMessageFromOutputMessage(message));
    return true;
}

std::unique_ptr<MidiOutputDevicePort> createJuceMidiOutputPort(MidiOutputDeviceInfo info)
{
    return std::make_unique<JuceMidiOutputPort>(std::move(info));
}

}
