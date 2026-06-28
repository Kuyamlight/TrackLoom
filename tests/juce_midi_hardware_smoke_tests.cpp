#include "JuceMidiOutput.h"
#include "JuceMidiOutputDeviceManager.h"

#include "ProjectPlaybackSession.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

// CTest 约定 77 为“跳过”，这样没有外接 MIDI 设备的机器不会被判定为失败。
constexpr int skipCode = 77;

// 测试程序不用完整测试框架，失败时直接抛出带上下文的错误。
void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 集中读取环境变量，便于后续把运行规则改成配置对象。
const char* envValue(const char* name)
{
    return std::getenv(name);
}

// 默认不碰真实硬件，只有显式设置为 1 时才会发送 MIDI。
bool hardwareSmokeEnabled()
{
    const auto* enabled = envValue("TRACKLOOM_MIDI_HARDWARE_SMOKE");
    return enabled != nullptr && std::string(enabled) == "1";
}

// 真实发送必须指定设备 id，避免误把音符发到用户当前系统默认设备。
std::string selectedDeviceId()
{
    const auto* deviceId = envValue("TRACKLOOM_MIDI_OUTPUT_ID");
    return deviceId == nullptr ? std::string {} : std::string { deviceId };
}

// 冒烟测试只短暂保持 Note On，允许人工听见声音，同时限制最长时间避免悬挂。
int holdMilliseconds()
{
    const auto* hold = envValue("TRACKLOOM_MIDI_SMOKE_HOLD_MS");
    if (hold == nullptr) {
        return 50;
    }

    const auto parsed = std::atoi(hold);
    if (parsed < 0) {
        return 0;
    }
    if (parsed > 2000) {
        return 2000;
    }

    return parsed;
}

// 跳过或找不到指定设备时打印设备列表，方便用户复制稳定设备 id。
void printAvailableDevices()
{
    const auto devices = trackloom::availableJuceMidiOutputDevices();
    if (devices.empty()) {
        std::cout << "No JUCE MIDI output devices are currently visible.\n";
        return;
    }

    std::cout << "Available JUCE MIDI output devices:\n";
    for (const auto& device : devices) {
        std::cout << "  id=\"" << device.id << "\" name=\"" << device.name << "\"\n";
    }
}

// 这里的音频源只负责让播放会话能渲染一个 block，不参与真实音频输出。
class SilentAudioSource final : public trackloom::AudioSource {
public:
    bool render(trackloom::AudioBlock block, double sampleRate) override
    {
        if (!block.isValid() || sampleRate <= 0.0) {
            return false;
        }

        block.clear();
        return true;
    }
};

// 构造一个很短的中音 C，用来验证项目播放路径能把 MIDI 送到真实设备。
void addSmokeNote(trackloom::Project& project, const trackloom::Track& track)
{
    const auto clip = project.createClip(track.id, "Hardware Smoke Clip", trackloom::ClipType::Midi, 0, 3840);
    require(clip.has_value(), "hardware smoke test should create midi clip");

    const auto note = project.createMidiNote(clip->id, 0, 3840, 60, 80, 1);
    require(note.has_value(), "hardware smoke test should create midi note");
}

// 通过真实项目播放会话发送 Note On，再清空设备路由触发安全 Note Off 释放。
void sendShortMidiSmokeNote(const trackloom::MidiOutputDeviceInfo& device)
{
    trackloom::Project project("MIDI Hardware Smoke");
    const auto track = project.createTrack("Smoke Instrument", trackloom::TrackType::Instrument);
    addSmokeNote(project, track);

    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    trackloom::JuceMidiOutputDeviceManager manager;
    SilentAudioSource source;

    require(session.prepare(48000.0, 2, 480), "hardware smoke session should prepare");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }),
        "hardware smoke session should rebuild audio graph");

    const auto rebuild = manager.rebuildProjectMidiOutputSafely(
        session,
        project,
        { { track.id, device } },
        0);
    require(rebuild.success, "hardware smoke test should bind selected midi output device");

    std::vector<float> samples(2 * 480, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 480);

    transport.play();
    const auto blockResult = session.renderNextBlock(transport, block, project);
    require(blockResult.renderSucceeded, "hardware smoke test should render first block");
    require(blockResult.midiDispatch.success, "hardware smoke test should send note on");

    std::this_thread::sleep_for(std::chrono::milliseconds(holdMilliseconds()));

    const auto clear = manager.rebuildProjectMidiOutputSafely(session, project, {}, 0);
    require(clear.success, "hardware smoke test should release note and clear device route");
}

}

int main()
{
    try {
        if (!hardwareSmokeEnabled()) {
            std::cout
                << "Skipping hardware MIDI smoke test. Set TRACKLOOM_MIDI_HARDWARE_SMOKE=1 "
                << "and TRACKLOOM_MIDI_OUTPUT_ID to enable it.\n";
            printAvailableDevices();
            return skipCode;
        }

        const auto deviceId = selectedDeviceId();
        if (deviceId.empty()) {
            std::cout << "Skipping hardware MIDI smoke test because TRACKLOOM_MIDI_OUTPUT_ID is not set.\n";
            printAvailableDevices();
            return skipCode;
        }

        const auto device = trackloom::findJuceMidiOutputDeviceById(deviceId);
        if (!device.has_value()) {
            std::cerr << "Requested MIDI output device id was not found: " << deviceId << '\n';
            printAvailableDevices();
            return 1;
        }

        sendShortMidiSmokeNote(*device);
    } catch (const std::exception& error) {
        std::cerr << "Hardware MIDI smoke test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "Hardware MIDI smoke test passed\n";
    return 0;
}
