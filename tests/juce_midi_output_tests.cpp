#include "JuceMidiOutput.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void juceMidiOutputMapsDeviceInfo()
{
    // 设备 id 用于保存和重连，显示名只给 UI 使用，两者都不能在转换时丢失。
    const juce::MidiDeviceInfo juceInfo("USB MIDI Out", "windows-midi-output-1");

    const auto mapped = trackloom::midiOutputDeviceInfoFromJuce(juceInfo);

    require(mapped.id == "windows-midi-output-1", "juce midi output adapter should preserve device id");
    require(mapped.name == "USB MIDI Out", "juce midi output adapter should preserve display name");
}

void juceMidiOutputConvertsCoreMessageBytes()
{
    // 平台适配层只传递字节，不重新判断 Note On、Note Off 或调度时间。
    const trackloom::MidiOutputMessage message {
        7,
        static_cast<std::uint8_t>(0x91),
        64,
        96
    };

    const auto juceMessage = trackloom::juceMidiMessageFromOutputMessage(message);
    const auto* rawData = juceMessage.getRawData();

    require(juceMessage.getRawDataSize() == 3, "juce midi output message should keep three MIDI bytes");
    require(rawData[0] == 0x91, "juce midi output message should preserve status byte");
    require(rawData[1] == 64, "juce midi output message should preserve first data byte");
    require(rawData[2] == 96, "juce midi output message should preserve second data byte");
    require(std::fabs(juceMessage.getTimeStamp()) <= 0.000001, "juce midi output message should send immediately");
}

void juceMidiOutputEnumeratesWithoutHardwareAssumptions()
{
    // 测试允许没有外接设备；如果系统返回设备，就检查最基本的 id/name 质量。
    const auto devices = trackloom::availableJuceMidiOutputDevices();

    for (const auto& device : devices) {
        require(!device.id.empty(), "enumerated juce midi output device should have a stable id");
        require(!device.name.empty(), "enumerated juce midi output device should have a display name");
    }
}

void juceMidiOutputPortRejectsUnknownDevice()
{
    // 明确不存在的 id 应该打开失败，用它验证失败路径而不依赖真实硬件。
    const trackloom::MidiOutputDeviceInfo missingDevice {
        "trackloom-missing-midi-output-device",
        "Missing MIDI Output"
    };
    trackloom::JuceMidiOutputPort port(missingDevice);

    require(port.info() == missingDevice, "juce midi output port should expose its configured device info");
    require(!port.isOpen(), "juce midi output port should start closed");
    require(!port.open(), "juce midi output port should reject unknown device ids");
    require(!port.isOpen(), "failed juce midi output open should keep the port closed");

    port.close();
    require(!port.isOpen(), "closing a failed juce midi output port should remain safe");
}

void juceMidiOutputFactoryPreservesDeviceInfo()
{
    // 设备管理器后续会通过接口指针保存端口，工厂必须保留原始设备身份。
    const trackloom::MidiOutputDeviceInfo info {
        "factory-midi-output",
        "Factory MIDI Output"
    };

    const auto port = trackloom::createJuceMidiOutputPort(info);

    require(port != nullptr, "juce midi output factory should create a port");
    require(port->info() == info, "juce midi output factory should preserve device info");
    require(!port->isOpen(), "factory-created juce midi output port should start closed");
}

}

int main()
{
    try {
        juceMidiOutputMapsDeviceInfo();
        juceMidiOutputConvertsCoreMessageBytes();
        juceMidiOutputEnumeratesWithoutHardwareAssumptions();
        juceMidiOutputPortRejectsUnknownDevice();
        juceMidiOutputFactoryPreservesDeviceInfo();
    } catch (const std::exception& error) {
        std::cerr << "JUCE MIDI output test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "JUCE MIDI output tests passed\n";
    return 0;
}
