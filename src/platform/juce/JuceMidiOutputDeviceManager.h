#pragma once

#include "JuceMidiOutput.h"
#include "ProjectPlaybackSession.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace trackloom {

// MidiOutputDeviceTrackBinding 是“工程轨道 -> 真实 MIDI 输出设备”的运行态配置。
// 它不写入工程文件；后续 UI 或设置层可以把用户选择转换成这个结构。
struct MidiOutputDeviceTrackBinding {
    std::string trackId;
    MidiOutputDeviceInfo device;
};

// MidiOutputDeviceManagerFailureReason 给 UI、设备层和 AI 工具提供稳定失败原因。
// 文案可以本地化，但逻辑判断应读取这个枚举，而不是解析错误字符串。
enum class MidiOutputDeviceManagerFailureReason {
    None,
    SessionNotPrepared,
    InvalidBinding,
    DeviceFactoryRejected,
    DeviceOpenRejected,
    MidiReleaseFailed,
    OutputBindingRejected
};

// MidiOutputDeviceManagerRebuildResult 记录一次设备配置重建的完整结果。
// sessionRebuild 保留底层播放会话的安全释放和路由切换细节。
struct MidiOutputDeviceManagerRebuildResult {
    bool success = false;
    MidiOutputDeviceManagerFailureReason failureReason = MidiOutputDeviceManagerFailureReason::None;
    ProjectPlaybackMidiOutputRebuildResult sessionRebuild;
    std::string failedTrackId;
    MidiOutputDeviceInfo failedDevice;
    std::size_t openedDeviceCount = 0;
};

using MidiOutputDevicePortFactory =
    std::function<std::unique_ptr<MidiOutputDevicePort>(MidiOutputDeviceInfo info)>;

// JuceMidiOutputDeviceManager 把 JUCE MIDI 端口接入项目播放会话。
// 它拥有真实端口和 MidiOutputDevice 适配器；ProjectPlaybackSession 只保存非拥有指针。
class JuceMidiOutputDeviceManager final {
public:
    explicit JuceMidiOutputDeviceManager(
        MidiOutputDevicePortFactory portFactory = createJuceMidiOutputPort);
    ~JuceMidiOutputDeviceManager();

    JuceMidiOutputDeviceManager(const JuceMidiOutputDeviceManager&) = delete;
    JuceMidiOutputDeviceManager& operator=(const JuceMidiOutputDeviceManager&) = delete;

    JuceMidiOutputDeviceManager(JuceMidiOutputDeviceManager&&) noexcept = delete;
    JuceMidiOutputDeviceManager& operator=(JuceMidiOutputDeviceManager&&) noexcept = delete;

    MidiOutputDeviceManagerRebuildResult rebuildProjectMidiOutputSafely(
        ProjectPlaybackSession& session,
        const Project& project,
        const std::vector<MidiOutputDeviceTrackBinding>& bindings,
        int releaseSampleOffset);

    std::size_t openDeviceCount() const;
    std::vector<MidiOutputDeviceInfo> openDeviceInfos() const;

private:
    struct ActiveDevice {
        std::unique_ptr<MidiOutputDevicePort> port;
        std::unique_ptr<MidiOutputDevice> receiver;
    };

    void closeActiveDevices();
    static void closeDeviceSet(std::vector<ActiveDevice>& devices);

    MidiOutputDevicePortFactory portFactory_;
    std::vector<ActiveDevice> activeDevices_;
};

}
