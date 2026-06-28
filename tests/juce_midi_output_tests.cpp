#include "JuceMidiOutput.h"
#include "JuceMidiOutputDeviceManager.h"

#include "ProjectPlaybackSession.h"

#include <functional>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

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

struct FakeMidiOutputPortState {
    trackloom::MidiOutputDeviceInfo info;
    bool open = false;
    bool openShouldSucceed = true;
    bool sendShouldSucceed = true;
    int openCallCount = 0;
    int closeCallCount = 0;
    std::vector<trackloom::MidiOutputMessage> sentMessages;
};

class FakeMidiOutputDevicePort final : public trackloom::MidiOutputDevicePort {
public:
    explicit FakeMidiOutputDevicePort(std::shared_ptr<FakeMidiOutputPortState> state)
        : state_(std::move(state))
    {
    }

    const trackloom::MidiOutputDeviceInfo& info() const override
    {
        return state_->info;
    }

    bool open() override
    {
        ++state_->openCallCount;
        state_->open = state_->openShouldSucceed;
        return state_->open;
    }

    void close() override
    {
        ++state_->closeCallCount;
        state_->open = false;
    }

    bool isOpen() const override
    {
        return state_->open;
    }

    bool sendMidiMessage(const trackloom::MidiOutputMessage& message) override
    {
        if (!state_->open || !state_->sendShouldSucceed) {
            return false;
        }

        state_->sentMessages.push_back(message);
        return true;
    }

private:
    std::shared_ptr<FakeMidiOutputPortState> state_;
};

class FakeMidiOutputPortFactory {
public:
    void rejectOpenForDevice(std::string deviceId)
    {
        openResults_[std::move(deviceId)] = false;
    }

    std::unique_ptr<trackloom::MidiOutputDevicePort> create(trackloom::MidiOutputDeviceInfo info)
    {
        auto state = std::make_shared<FakeMidiOutputPortState>();
        state->info = std::move(info);

        const auto openResult = openResults_.find(state->info.id);
        if (openResult != openResults_.end()) {
            state->openShouldSucceed = openResult->second;
        }

        states_.push_back(state);
        return std::make_unique<FakeMidiOutputDevicePort>(state);
    }

    std::shared_ptr<FakeMidiOutputPortState> stateForDevice(const std::string& deviceId) const
    {
        for (const auto& state : states_) {
            if (state->info.id == deviceId) {
                return state;
            }
        }

        return nullptr;
    }

    std::size_t createdPortCount() const
    {
        return states_.size();
    }

private:
    std::map<std::string, bool> openResults_;
    std::vector<std::shared_ptr<FakeMidiOutputPortState>> states_;
};

trackloom::MidiOutputDeviceInfo deviceInfo(std::string id, std::string name)
{
    return { std::move(id), std::move(name) };
}

void addLongMidiNote(trackloom::Project& project, const trackloom::Track& track)
{
    const auto clip = project.createClip(track.id, "Lead Clip", trackloom::ClipType::Midi, 0, 3840);
    require(clip.has_value(), "test project should create midi clip");

    const auto note = project.createMidiNote(clip->id, 0, 3840, 60, 100, 1);
    require(note.has_value(), "test project should create midi note");
}

trackloom::ProjectPlaybackBlockResult renderPlaybackBlock(
    trackloom::ProjectPlaybackSession& session,
    trackloom::Transport& transport,
    const trackloom::Project& project)
{
    std::vector<float> samples(2 * 480, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 480);

    return session.renderNextBlock(transport, block, project);
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

void juceMidiOutputFindsNoMissingDeviceId()
{
    // 这个 id 故意使用 TrackLoom 专属前缀，避免和真实系统设备撞名。
    const auto missingDevice = trackloom::findJuceMidiOutputDeviceById(
        "trackloom-missing-midi-output-device");

    require(!missingDevice.has_value(), "juce midi output lookup should return empty for missing device id");
}

void juceMidiOutputDiffsDeviceListSnapshots()
{
    const std::vector<trackloom::MidiOutputDeviceInfo> previous {
        deviceInfo("device-a", "Old Device A"),
        deviceInfo("device-b", "Old Device B")
    };
    const std::vector<trackloom::MidiOutputDeviceInfo> current {
        deviceInfo("device-b", "Renamed Device B"),
        deviceInfo("device-c", "New Device C")
    };

    const auto diff = trackloom::diffMidiOutputDeviceLists(previous, current);

    require(diff.retained.size() == 1, "midi output device diff should retain devices with matching ids");
    require(diff.retained[0].id == "device-b", "midi output device diff should retain by stable id");
    require(diff.retained[0].name == "Renamed Device B",
        "midi output device diff should keep current display name for retained devices");
    require(diff.added.size() == 1 && diff.added[0].id == "device-c",
        "midi output device diff should report newly visible devices");
    require(diff.removed.size() == 1 && diff.removed[0].id == "device-a",
        "midi output device diff should report disappeared devices");
}

void juceMidiOutputDiffKeepsPredictableOrder()
{
    const std::vector<trackloom::MidiOutputDeviceInfo> previous {
        deviceInfo("old-1", "Old 1"),
        deviceInfo("kept-2", "Kept 2"),
        deviceInfo("old-3", "Old 3"),
        deviceInfo("kept-4", "Kept 4")
    };
    const std::vector<trackloom::MidiOutputDeviceInfo> current {
        deviceInfo("new-5", "New 5"),
        deviceInfo("kept-4", "Kept 4 Current"),
        deviceInfo("new-6", "New 6"),
        deviceInfo("kept-2", "Kept 2 Current")
    };

    const auto diff = trackloom::diffMidiOutputDeviceLists(previous, current);

    require(diff.added.size() == 2, "midi output device diff should report two added devices");
    require(diff.added[0].id == "new-5" && diff.added[1].id == "new-6",
        "midi output device diff should keep added devices in current-list order");
    require(diff.retained.size() == 2, "midi output device diff should report two retained devices");
    require(diff.retained[0].id == "kept-4" && diff.retained[1].id == "kept-2",
        "midi output device diff should keep retained devices in current-list order");
    require(diff.removed.size() == 2, "midi output device diff should report two removed devices");
    require(diff.removed[0].id == "old-1" && diff.removed[1].id == "old-3",
        "midi output device diff should keep removed devices in previous-list order");
}

void juceMidiOutputDeviceListRefreshesFromInjectedProvider()
{
    std::vector<std::vector<trackloom::MidiOutputDeviceInfo>> snapshots {
        {
            deviceInfo("device-a", "Device A"),
            deviceInfo("device-b", "Device B")
        },
        {
            deviceInfo("device-b", "Device B Current"),
            deviceInfo("device-c", "Device C")
        }
    };
    std::size_t nextSnapshot = 0;
    trackloom::JuceMidiOutputDeviceList deviceList(
        [&]() {
            require(nextSnapshot < snapshots.size(), "device list test should not over-enumerate");
            return snapshots[nextSnapshot++];
        });

    const auto initial = deviceList.refresh();

    require(initial.added.size() == 2, "initial midi device refresh should report visible devices as added");
    require(initial.removed.empty(), "initial midi device refresh should not report removed devices");
    require(initial.retained.empty(), "initial midi device refresh should not report retained devices");
    require(deviceList.devices().size() == 2, "device list should cache initial visible devices");
    require(deviceList.findById("device-a").has_value(), "device list should find cached device by id");

    const auto updated = deviceList.refresh();

    require(updated.retained.size() == 1 && updated.retained[0].id == "device-b",
        "second midi device refresh should retain matching device ids");
    require(updated.retained[0].name == "Device B Current",
        "second midi device refresh should use current display name for retained devices");
    require(updated.added.size() == 1 && updated.added[0].id == "device-c",
        "second midi device refresh should report newly visible device");
    require(updated.removed.size() == 1 && updated.removed[0].id == "device-a",
        "second midi device refresh should report disappeared device");
    require(deviceList.devices().size() == 2 && deviceList.devices()[0].id == "device-b",
        "device list should replace cache with latest snapshot");
    require(!deviceList.findById("device-a").has_value(), "device list should not find removed cached device");
    require(nextSnapshot == 2, "device list refresh should enumerate exactly once per refresh");
}

void juceMidiOutputDeviceListFindsCachedDevicesWithoutEnumerating()
{
    std::size_t enumerateCount = 0;
    trackloom::JuceMidiOutputDeviceList deviceList(
        [&]() {
            ++enumerateCount;
            return std::vector<trackloom::MidiOutputDeviceInfo> {
                deviceInfo("device-a", "Device A")
            };
        });

    require(!deviceList.findById("device-a").has_value(),
        "fresh midi device list should not enumerate during cached lookup");
    require(enumerateCount == 0, "cached lookup before refresh should not call provider");

    deviceList.refresh();
    const auto found = deviceList.findById("device-a");

    require(found.has_value(), "cached lookup should find device after refresh");
    require(found->name == "Device A", "cached lookup should return cached device info");
    require(enumerateCount == 1, "cached lookup after refresh should not enumerate again");
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

void juceMidiOutputDeviceManagerConnectsDeviceToPlaybackSession()
{
    trackloom::Project project("JUCE MIDI device manager");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    addLongMidiNote(project, track);

    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    SilentAudioSource source;
    FakeMidiOutputPortFactory factory;
    trackloom::JuceMidiOutputDeviceManager manager(
        [&](trackloom::MidiOutputDeviceInfo info) {
            return factory.create(std::move(info));
        });

    require(session.prepare(1920.0, 2, 480), "device manager test session should prepare");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }), "device manager test should rebuild audio graph");

    const auto rebuild = manager.rebuildProjectMidiOutputSafely(
        session,
        project,
        { { track.id, deviceInfo("device-a", "Device A") } },
        0);

    require(rebuild.success, "device manager should rebuild playback midi output");
    require(rebuild.failureReason == trackloom::MidiOutputDeviceManagerFailureReason::None,
        "successful device manager rebuild should expose no failure reason");
    require(session.midiReceiverCount() == 1, "device manager should bind one midi receiver to the session");
    require(manager.openDeviceCount() == 1, "device manager should retain one open device");

    transport.play();
    const auto blockResult = renderPlaybackBlock(session, transport, project);
    const auto deviceA = factory.stateForDevice("device-a");

    require(blockResult.renderSucceeded, "device manager playback block should render");
    require(blockResult.midiDispatch.success, "device manager playback block should dispatch midi");
    require(deviceA != nullptr, "device manager test should record fake device A");
    require(deviceA->sentMessages.size() == 1, "device manager should send note on to fake device");
    require(deviceA->sentMessages[0].statusByte == 0x90, "device manager should send note on status byte");
}

void juceMidiOutputDeviceManagerKeepsOldDeviceWhenNewDeviceOpenFails()
{
    trackloom::Project project("JUCE MIDI device manager");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);

    trackloom::ProjectPlaybackSession session;
    FakeMidiOutputPortFactory factory;
    trackloom::JuceMidiOutputDeviceManager manager(
        [&](trackloom::MidiOutputDeviceInfo info) {
            return factory.create(std::move(info));
        });

    require(session.prepare(1920.0, 2, 480), "open failure test session should prepare");
    require(manager.rebuildProjectMidiOutputSafely(
        session,
        project,
        { { track.id, deviceInfo("device-a", "Device A") } },
        0).success,
        "open failure test should establish initial device");

    factory.rejectOpenForDevice("device-b");
    const auto rebuild = manager.rebuildProjectMidiOutputSafely(
        session,
        project,
        { { track.id, deviceInfo("device-b", "Device B") } },
        0);
    const auto deviceA = factory.stateForDevice("device-a");
    const auto openDevices = manager.openDeviceInfos();

    require(!rebuild.success, "device manager should reject a device that cannot open");
    require(rebuild.failureReason == trackloom::MidiOutputDeviceManagerFailureReason::DeviceOpenRejected,
        "device manager should expose device-open failure reason");
    require(deviceA != nullptr && deviceA->open, "device manager should keep old device open after failed rebuild");
    require(openDevices.size() == 1 && openDevices[0].id == "device-a",
        "device manager should keep old open device identity after failed rebuild");
    require(session.midiReceiverCount() == 1, "failed device rebuild should keep old session route");
}

void juceMidiOutputDeviceManagerReleasesOldDeviceBeforeSwitching()
{
    trackloom::Project project("JUCE MIDI device manager");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    addLongMidiNote(project, track);

    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    SilentAudioSource source;
    FakeMidiOutputPortFactory factory;
    trackloom::JuceMidiOutputDeviceManager manager(
        [&](trackloom::MidiOutputDeviceInfo info) {
            return factory.create(std::move(info));
        });

    require(session.prepare(1920.0, 2, 480), "safe switch test session should prepare");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }), "safe switch test should rebuild audio graph");
    require(manager.rebuildProjectMidiOutputSafely(
        session,
        project,
        { { track.id, deviceInfo("device-a", "Device A") } },
        0).success,
        "safe switch test should establish old device");

    transport.play();
    const auto blockResult = renderPlaybackBlock(session, transport, project);
    require(blockResult.midiDispatch.success, "safe switch test should deliver initial note on");
    require(session.activeMidiNoteCount() == 1, "safe switch test should have one active midi note");

    const auto rebuild = manager.rebuildProjectMidiOutputSafely(
        session,
        project,
        { { track.id, deviceInfo("device-b", "Device B") } },
        32);
    const auto deviceA = factory.stateForDevice("device-a");
    const auto openDevices = manager.openDeviceInfos();

    require(rebuild.success, "device manager should switch devices after releasing active notes");
    require(rebuild.sessionRebuild.midiRelease.success, "device manager should expose successful midi release");
    require(deviceA != nullptr, "safe switch test should record old fake device");
    require(deviceA->sentMessages.size() == 2, "device manager should send note off before switching devices");
    require(deviceA->sentMessages[1].statusByte == 0x80, "device manager should release old note with note off");
    require(deviceA->sentMessages[1].sampleOffset == 32, "device manager should preserve release sample offset");
    require(deviceA->closeCallCount == 1, "device manager should close old device after successful switch");
    require(openDevices.size() == 1 && openDevices[0].id == "device-b",
        "device manager should retain new device after successful switch");
    require(session.activeMidiNoteCount() == 0, "device manager switch should clear released active notes");
}

void juceMidiOutputDeviceManagerClearsOutputsAndClosesDevices()
{
    trackloom::Project project("JUCE MIDI device manager");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);

    trackloom::ProjectPlaybackSession session;
    FakeMidiOutputPortFactory factory;
    trackloom::JuceMidiOutputDeviceManager manager(
        [&](trackloom::MidiOutputDeviceInfo info) {
            return factory.create(std::move(info));
        });

    require(session.prepare(1920.0, 2, 480), "clear output test session should prepare");
    require(manager.rebuildProjectMidiOutputSafely(
        session,
        project,
        { { track.id, deviceInfo("device-a", "Device A") } },
        0).success,
        "clear output test should establish initial device");

    const auto deviceA = factory.stateForDevice("device-a");
    const auto clear = manager.rebuildProjectMidiOutputSafely(session, project, {}, 0);

    require(clear.success, "device manager should clear midi output bindings");
    require(manager.openDeviceCount() == 0, "device manager should retain no devices after clear");
    require(session.midiReceiverCount() == 0, "clearing device manager should clear session midi routes");
    require(deviceA != nullptr && deviceA->closeCallCount == 1, "clearing device manager should close old device");
}

void juceMidiOutputDeviceManagerRejectsInvalidTrackBeforeOpeningDevice()
{
    trackloom::Project project("JUCE MIDI device manager");
    const auto audioTrack = project.createTrack("Audio", trackloom::TrackType::Audio);

    trackloom::ProjectPlaybackSession session;
    FakeMidiOutputPortFactory factory;
    trackloom::JuceMidiOutputDeviceManager manager(
        [&](trackloom::MidiOutputDeviceInfo info) {
            return factory.create(std::move(info));
        });

    require(session.prepare(1920.0, 2, 480), "invalid track test session should prepare");

    const auto rebuild = manager.rebuildProjectMidiOutputSafely(
        session,
        project,
        { { audioTrack.id, deviceInfo("device-a", "Device A") } },
        0);

    require(!rebuild.success, "device manager should reject non-instrument track binding");
    require(rebuild.failureReason == trackloom::MidiOutputDeviceManagerFailureReason::InvalidBinding,
        "device manager should expose invalid binding reason");
    require(factory.createdPortCount() == 0, "device manager should not open devices for invalid bindings");
    require(manager.openDeviceCount() == 0, "invalid binding should not change retained devices");
}

}

int main()
{
    try {
        juceMidiOutputMapsDeviceInfo();
        juceMidiOutputConvertsCoreMessageBytes();
        juceMidiOutputEnumeratesWithoutHardwareAssumptions();
        juceMidiOutputFindsNoMissingDeviceId();
        juceMidiOutputDiffsDeviceListSnapshots();
        juceMidiOutputDiffKeepsPredictableOrder();
        juceMidiOutputDeviceListRefreshesFromInjectedProvider();
        juceMidiOutputDeviceListFindsCachedDevicesWithoutEnumerating();
        juceMidiOutputPortRejectsUnknownDevice();
        juceMidiOutputFactoryPreservesDeviceInfo();
        juceMidiOutputDeviceManagerConnectsDeviceToPlaybackSession();
        juceMidiOutputDeviceManagerKeepsOldDeviceWhenNewDeviceOpenFails();
        juceMidiOutputDeviceManagerReleasesOldDeviceBeforeSwitching();
        juceMidiOutputDeviceManagerClearsOutputsAndClosesDevices();
        juceMidiOutputDeviceManagerRejectsInvalidTrackBeforeOpeningDevice();
    } catch (const std::exception& error) {
        std::cerr << "JUCE MIDI output test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "JUCE MIDI output tests passed\n";
    return 0;
}
