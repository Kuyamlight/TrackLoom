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

    std::shared_ptr<FakeMidiOutputPortState> latestStateForDevice(const std::string& deviceId) const
    {
        for (auto state = states_.rbegin(); state != states_.rend(); ++state) {
            if ((*state)->info.id == deviceId) {
                return *state;
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

void juceMidiOutputBindingRefreshPlansUnavailableDeviceRemoval()
{
    const std::vector<trackloom::MidiOutputDeviceTrackBinding> bindings {
        { "track-a", deviceInfo("device-a", "Old Device A") },
        { "track-b", deviceInfo("device-b", "Missing Device B") },
        { "track-c", deviceInfo("device-c", "Device C") }
    };
    const std::vector<trackloom::MidiOutputDeviceInfo> visibleDevices {
        deviceInfo("device-a", "Renamed Device A"),
        deviceInfo("device-c", "Device C")
    };

    const auto plan = trackloom::planMidiOutputDeviceBindingRefresh(bindings, visibleDevices);

    require(plan.availableBindings.size() == 2, "binding refresh should keep routable device bindings");
    require(plan.availableBindings[0].trackId == "track-a",
        "binding refresh should keep available bindings in original binding order");
    require(plan.availableBindings[0].device.name == "Renamed Device A",
        "binding refresh should update available binding display name from visible device list");
    require(plan.availableBindings[1].trackId == "track-c",
        "binding refresh should keep later available bindings after an unavailable binding");
    require(plan.unavailableBindings.size() == 1,
        "binding refresh should report one binding whose selected device disappeared");
    require(plan.unavailableBindings[0].trackId == "track-b",
        "binding refresh should report affected track id for disappeared device");
    require(plan.unavailableBindings[0].device.name == "Missing Device B",
        "binding refresh should preserve unavailable binding device info for UI and reconnect");
    require(plan.requiresSafeRebuild,
        "binding refresh should request safe rebuild when any active binding device disappeared");
}

void juceMidiOutputBindingRefreshKeepsAllVisibleBindingsWithoutRebuild()
{
    const std::vector<trackloom::MidiOutputDeviceTrackBinding> bindings {
        { "track-a", deviceInfo("device-a", "Old Device A") },
        { "track-b", deviceInfo("device-b", "Old Device B") }
    };
    const std::vector<trackloom::MidiOutputDeviceInfo> visibleDevices {
        deviceInfo("device-b", "Device B Current"),
        deviceInfo("device-a", "Device A Current")
    };

    const auto plan = trackloom::planMidiOutputDeviceBindingRefresh(bindings, visibleDevices);

    require(plan.availableBindings.size() == 2,
        "binding refresh should keep all bindings when all selected devices are visible");
    require(plan.availableBindings[0].trackId == "track-a" && plan.availableBindings[1].trackId == "track-b",
        "binding refresh should preserve binding order instead of visible-device order");
    require(plan.availableBindings[0].device.name == "Device A Current",
        "binding refresh should update first binding device info from visible list");
    require(plan.availableBindings[1].device.name == "Device B Current",
        "binding refresh should update second binding device info from visible list");
    require(plan.unavailableBindings.empty(),
        "binding refresh should report no unavailable bindings when all devices are visible");
    require(!plan.requiresSafeRebuild,
        "binding refresh should not request safe rebuild when all selected devices remain visible");
}

void juceMidiOutputRoutingStateDescribesSelectedRouteStatuses()
{
    const trackloom::MidiOutputRoutingState state {
        {
            { "track-applied", deviceInfo("device-a", "Device A") },
            { "track-pending", deviceInfo("device-b", "Device B") },
            { "track-offline", deviceInfo("device-c", "Device C") },
            { "track-open-missing", deviceInfo("device-d", "Device D") }
        },
        {
            deviceInfo("device-a", "Device A"),
            deviceInfo("device-b", "Device B"),
            deviceInfo("device-d", "Device D")
        },
        {
            { "track-applied", deviceInfo("device-a", "Device A") },
            { "track-open-missing", deviceInfo("device-d", "Device D") }
        },
        {
            deviceInfo("device-a", "Device A")
        }
    };

    const auto statuses = trackloom::describeMidiOutputRoutingState(state);

    require(statuses.size() == 4,
        "routing state description should report one status per selected route");
    require(statuses[0].selectedBinding.trackId == "track-applied",
        "routing state description should preserve selected binding order");
    require(statuses[0].status == trackloom::MidiOutputRouteStatus::Applied,
        "routing state description should mark visible, applied, open routes as applied");
    require(statuses[1].status == trackloom::MidiOutputRouteStatus::PendingApply,
        "routing state description should mark visible but unapplied routes as pending");
    require(statuses[2].status == trackloom::MidiOutputRouteStatus::DeviceUnavailable,
        "routing state description should mark invisible selected devices as unavailable");
    require(statuses[3].status == trackloom::MidiOutputRouteStatus::OpenDeviceMissing,
        "routing state description should mark applied routes without open devices as inconsistent");
}

void juceMidiOutputRoutingStateReportsStaleAppliedRoutes()
{
    const trackloom::MidiOutputRoutingState state {
        {
            { "track-current", deviceInfo("device-b", "Device B") }
        },
        {
            deviceInfo("device-a", "Device A"),
            deviceInfo("device-b", "Device B")
        },
        {
            { "track-old", deviceInfo("device-a", "Device A") }
        },
        {
            deviceInfo("device-a", "Device A")
        }
    };

    const auto statuses = trackloom::describeMidiOutputRoutingState(state);

    require(statuses.size() == 2,
        "routing state description should include selected routes and stale applied routes");
    require(statuses[0].selectedBinding.trackId == "track-current",
        "routing state description should keep selected routes before stale applied routes");
    require(statuses[0].status == trackloom::MidiOutputRouteStatus::PendingApply,
        "routing state description should mark newly selected route as pending");
    require(statuses[1].selectedBinding.trackId == "track-old",
        "routing state description should report old applied route after selected routes");
    require(statuses[1].status == trackloom::MidiOutputRouteStatus::StaleAppliedRoute,
        "routing state description should mark unselected applied route as stale");
}

void juceMidiOutputRoutingStateReportsWhetherApplyIsNeeded()
{
    require(!trackloom::midiOutputRouteStatusRequiresApply(trackloom::MidiOutputRouteStatus::Applied),
        "applied midi output route status should not require applying changes");
    require(!trackloom::midiOutputRouteStatusRequiresApply(trackloom::MidiOutputRouteStatus::DeviceUnavailable),
        "unavailable-only midi output route status should wait for reconnect instead of applying changes");
    require(trackloom::midiOutputRouteStatusRequiresApply(trackloom::MidiOutputRouteStatus::PendingApply),
        "pending midi output route status should require applying changes");
    require(trackloom::midiOutputRouteStatusRequiresApply(trackloom::MidiOutputRouteStatus::OpenDeviceMissing),
        "missing-open-device midi output route status should require applying changes");
    require(trackloom::midiOutputRouteStatusRequiresApply(trackloom::MidiOutputRouteStatus::StaleAppliedRoute),
        "stale applied midi output route status should require applying changes");

    const trackloom::MidiOutputRoutingState appliedState {
        {
            { "track-a", deviceInfo("device-a", "Device A") }
        },
        {
            deviceInfo("device-a", "Device A")
        },
        {
            { "track-a", deviceInfo("device-a", "Device A") }
        },
        {
            deviceInfo("device-a", "Device A")
        }
    };
    require(!trackloom::midiOutputRoutingStateRequiresApply(appliedState),
        "fully applied midi output routing state should not require applying changes");

    const trackloom::MidiOutputRoutingState pendingState {
        {
            { "track-a", deviceInfo("device-a", "Device A") }
        },
        {
            deviceInfo("device-a", "Device A")
        },
        {},
        {}
    };
    require(trackloom::midiOutputRoutingStateRequiresApply(pendingState),
        "pending midi output routing state should require applying changes");

    const trackloom::MidiOutputRoutingState unavailableOnlyState {
        {
            { "track-a", deviceInfo("device-a", "Device A") }
        },
        {},
        {},
        {}
    };
    require(!trackloom::midiOutputRoutingStateRequiresApply(unavailableOnlyState),
        "offline-only midi output routing state should not require applying changes until the device returns");
}

void juceMidiOutputDeviceManagerRefreshRemovesMissingDeviceViaSafeRebuild()
{
    trackloom::Project project("JUCE MIDI device manager refresh");
    const auto leadTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto bassTrack = project.createTrack("Bass", trackloom::TrackType::Instrument);

    trackloom::ProjectPlaybackSession session;
    FakeMidiOutputPortFactory factory;
    trackloom::JuceMidiOutputDeviceManager manager(
        [&](trackloom::MidiOutputDeviceInfo info) {
            return factory.create(std::move(info));
        });

    const std::vector<trackloom::MidiOutputDeviceTrackBinding> bindings {
        { leadTrack.id, deviceInfo("device-a", "Device A") },
        { bassTrack.id, deviceInfo("device-b", "Device B") }
    };

    require(session.prepare(1920.0, 2, 480), "refresh removal test session should prepare");
    require(manager.rebuildProjectMidiOutputSafely(session, project, bindings, 0).success,
        "refresh removal test should establish two output devices");

    const auto deviceB = factory.stateForDevice("device-b");
    const auto refresh = manager.refreshProjectMidiOutputForVisibleDevicesSafely(
        session,
        project,
        bindings,
        { deviceInfo("device-a", "Device A Current") },
        24);
    const auto openDevices = manager.openDeviceInfos();

    require(refresh.success, "device manager refresh should remove missing device through safe rebuild");
    require(refresh.safeRebuildAttempted,
        "device manager refresh should attempt safe rebuild when a bound device disappeared");
    require(refresh.bindingPlan.availableBindings.size() == 1,
        "device manager refresh should keep one available binding");
    require(refresh.bindingPlan.unavailableBindings.size() == 1,
        "device manager refresh should report one unavailable binding");
    require(refresh.bindingPlan.unavailableBindings[0].trackId == bassTrack.id,
        "device manager refresh should report the track whose device disappeared");
    require(openDevices.size() == 1 && openDevices[0].id == "device-a",
        "device manager refresh should retain only still-visible device routes");
    require(openDevices[0].name == "Device A Current",
        "device manager refresh should reopen retained routes with current device info");
    require(session.midiReceiverCount() == 1,
        "device manager refresh should remove missing device receiver from session");
    require(deviceB != nullptr && deviceB->closeCallCount == 1,
        "device manager refresh should close the disappeared device after successful rebuild");
}

void juceMidiOutputDeviceManagerRefreshKeepsOldRouteWhenSafeRebuildFails()
{
    trackloom::Project project("JUCE MIDI device manager refresh");
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

    const std::vector<trackloom::MidiOutputDeviceTrackBinding> bindings {
        { track.id, deviceInfo("device-a", "Device A") }
    };

    require(session.prepare(1920.0, 2, 480), "refresh rollback test session should prepare");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }),
        "refresh rollback test should rebuild audio graph");
    require(manager.rebuildProjectMidiOutputSafely(session, project, bindings, 0).success,
        "refresh rollback test should establish an initial output device");

    transport.play();
    const auto blockResult = renderPlaybackBlock(session, transport, project);
    const auto deviceA = factory.stateForDevice("device-a");
    require(blockResult.midiDispatch.success, "refresh rollback test should deliver initial note on");
    require(deviceA != nullptr, "refresh rollback test should record fake device A");
    require(session.activeMidiNoteCount() == 1,
        "refresh rollback test should have one active note before safe rebuild");

    deviceA->sendShouldSucceed = false;
    const auto refresh = manager.refreshProjectMidiOutputForVisibleDevicesSafely(
        session,
        project,
        bindings,
        {},
        32);
    const auto openDevices = manager.openDeviceInfos();

    require(!refresh.success, "device manager refresh should fail when old active notes cannot release");
    require(refresh.failureReason == trackloom::MidiOutputDeviceManagerFailureReason::MidiReleaseFailed,
        "device manager refresh should expose midi release failure");
    require(refresh.safeRebuildAttempted,
        "device manager refresh should attempt safe rebuild before reporting release failure");
    require(refresh.bindingPlan.unavailableBindings.size() == 1,
        "failed device manager refresh should still report unavailable binding for UI");
    require(openDevices.size() == 1 && openDevices[0].id == "device-a",
        "failed device manager refresh should keep old open device identity");
    require(deviceA->open, "failed device manager refresh should keep old device open");
    require(session.midiReceiverCount() == 1,
        "failed device manager refresh should keep old session midi route");
    require(session.activeMidiNoteCount() == 1,
        "failed device manager refresh should keep active note state for another release attempt");
}

void juceMidiOutputDeviceManagerRefreshSkipsRebuildWhenAllBindingsVisible()
{
    trackloom::Project project("JUCE MIDI device manager refresh");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);

    trackloom::ProjectPlaybackSession session;
    FakeMidiOutputPortFactory factory;
    trackloom::JuceMidiOutputDeviceManager manager(
        [&](trackloom::MidiOutputDeviceInfo info) {
            return factory.create(std::move(info));
        });

    const std::vector<trackloom::MidiOutputDeviceTrackBinding> bindings {
        { track.id, deviceInfo("device-a", "Device A") }
    };

    require(session.prepare(1920.0, 2, 480), "refresh no-op test session should prepare");
    require(manager.rebuildProjectMidiOutputSafely(session, project, bindings, 0).success,
        "refresh no-op test should establish an initial output device");

    const auto refresh = manager.refreshProjectMidiOutputForVisibleDevicesSafely(
        session,
        project,
        bindings,
        { deviceInfo("device-a", "Device A Current") },
        16);
    const auto openDevices = manager.openDeviceInfos();

    require(refresh.success, "device manager refresh should succeed when all bindings remain visible");
    require(!refresh.safeRebuildAttempted,
        "device manager refresh should skip safe rebuild when no active device disappeared");
    require(refresh.bindingPlan.availableBindings.size() == 1,
        "device manager refresh should still return current available binding info");
    require(refresh.bindingPlan.availableBindings[0].device.name == "Device A Current",
        "device manager refresh should expose latest display name even without rebuilding");
    require(factory.createdPortCount() == 1,
        "device manager refresh should not open new ports when rebuild is unnecessary");
    require(openDevices.size() == 1 && openDevices[0].name == "Device A",
        "device manager refresh should not churn the existing open route for a display-name-only change");
    require(session.midiReceiverCount() == 1,
        "device manager refresh should keep existing session route when rebuild is unnecessary");
}

void juceMidiOutputRoutingControllerRefreshKeepsSelectionWhenDeviceDisappears()
{
    trackloom::Project project("JUCE MIDI routing controller");
    const auto leadTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto bassTrack = project.createTrack("Bass", trackloom::TrackType::Instrument);

    std::vector<std::vector<trackloom::MidiOutputDeviceInfo>> snapshots {
        {
            deviceInfo("device-a", "Device A"),
            deviceInfo("device-b", "Device B")
        },
        {
            deviceInfo("device-a", "Device A Current")
        }
    };
    std::size_t nextSnapshot = 0;
    FakeMidiOutputPortFactory factory;
    trackloom::JuceMidiOutputRoutingController controller(
        [&]() {
            require(nextSnapshot < snapshots.size(), "routing controller test should not over-enumerate");
            return snapshots[nextSnapshot++];
        },
        [&](trackloom::MidiOutputDeviceInfo info) {
            return factory.create(std::move(info));
        });
    trackloom::ProjectPlaybackSession session;

    require(session.prepare(1920.0, 2, 480), "routing controller removal test session should prepare");
    controller.refreshDevices();
    require(controller.setTrackOutputDevice(leadTrack.id, deviceInfo("device-a", "Device A")),
        "routing controller should accept first selected device");
    require(controller.setTrackOutputDevice(bassTrack.id, deviceInfo("device-b", "Device B")),
        "routing controller should accept second selected device");
    require(controller.rebuildSelectedProjectMidiOutputSafely(session, project, 0).success,
        "routing controller should establish initial selected routes");

    const auto deviceB = factory.stateForDevice("device-b");
    const auto refresh = controller.refreshDevicesAndRebuildSelectedProjectMidiOutputSafely(
        session,
        project,
        24);
    const auto selected = controller.selectedBindings();
    const auto openDevices = controller.openDeviceInfos();

    require(refresh.outputRefresh.success,
        "routing controller refresh should apply available bindings after a device disappears");
    require(refresh.outputRefresh.safeRebuildAttempted,
        "routing controller refresh should safely rebuild when selected device disappeared");
    require(refresh.outputRefresh.bindingPlan.unavailableBindings.size() == 1,
        "routing controller refresh should expose unavailable selected binding");
    require(selected.size() == 2, "routing controller should keep user selections after device disappearance");
    require(selected[0].trackId == leadTrack.id && selected[0].device.name == "Device A Current",
        "routing controller should update still-visible selected device info");
    require(selected[1].trackId == bassTrack.id && selected[1].device.name == "Device B",
        "routing controller should preserve disappeared selected device info for reconnect");
    require(openDevices.size() == 1 && openDevices[0].id == "device-a",
        "routing controller should keep only visible device in runtime route");
    require(deviceB != nullptr && deviceB->closeCallCount == 1,
        "routing controller should close disappeared runtime device after safe rebuild");
}

void juceMidiOutputRoutingControllerRestoresRouteWhenDeviceReappears()
{
    trackloom::Project project("JUCE MIDI routing controller");
    const auto leadTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto bassTrack = project.createTrack("Bass", trackloom::TrackType::Instrument);

    std::vector<std::vector<trackloom::MidiOutputDeviceInfo>> snapshots {
        {
            deviceInfo("device-a", "Device A"),
            deviceInfo("device-b", "Device B")
        },
        {
            deviceInfo("device-a", "Device A Current")
        },
        {
            deviceInfo("device-a", "Device A Current"),
            deviceInfo("device-b", "Device B Reconnected")
        }
    };
    std::size_t nextSnapshot = 0;
    FakeMidiOutputPortFactory factory;
    trackloom::JuceMidiOutputRoutingController controller(
        [&]() {
            require(nextSnapshot < snapshots.size(), "routing controller reconnect test should not over-enumerate");
            return snapshots[nextSnapshot++];
        },
        [&](trackloom::MidiOutputDeviceInfo info) {
            return factory.create(std::move(info));
        });
    trackloom::ProjectPlaybackSession session;

    require(session.prepare(1920.0, 2, 480), "routing controller reconnect test session should prepare");
    controller.refreshDevices();
    require(controller.setTrackOutputDevice(leadTrack.id, deviceInfo("device-a", "Device A")),
        "routing controller reconnect test should select device A");
    require(controller.setTrackOutputDevice(bassTrack.id, deviceInfo("device-b", "Device B")),
        "routing controller reconnect test should select device B");
    require(controller.rebuildSelectedProjectMidiOutputSafely(session, project, 0).success,
        "routing controller reconnect test should establish initial route");

    const auto removed = controller.refreshDevicesAndRebuildSelectedProjectMidiOutputSafely(
        session,
        project,
        16);
    require(removed.outputRefresh.success,
        "routing controller reconnect test should safely apply device removal");
    require(controller.openDeviceInfos().size() == 1,
        "routing controller reconnect test should shrink runtime route after device removal");

    const auto reconnected = controller.refreshDevicesAndRebuildSelectedProjectMidiOutputSafely(
        session,
        project,
        24);
    const auto selected = controller.selectedBindings();
    const auto openDevices = controller.openDeviceInfos();

    require(reconnected.deviceDiff.added.size() == 1 && reconnected.deviceDiff.added[0].id == "device-b",
        "routing controller refresh should report reconnected device as added");
    require(reconnected.outputRefresh.success,
        "routing controller should rebuild routes when a selected device reappears");
    require(reconnected.outputRefresh.safeRebuildAttempted,
        "routing controller should mark route restoration as a safe rebuild attempt");
    require(selected.size() == 2 && selected[1].device.name == "Device B Reconnected",
        "routing controller should update reconnected selected device info");
    require(openDevices.size() == 2,
        "routing controller should restore runtime route count after selected device reappears");
    require(openDevices[0].id == "device-a" && openDevices[1].id == "device-b",
        "routing controller should restore both selected device routes after reconnect");
}

void juceMidiOutputRoutingControllerRebuildsWhenSameDeviceMovesToAnotherTrack()
{
    trackloom::Project project("JUCE MIDI routing controller");
    const auto leadTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto bassTrack = project.createTrack("Bass", trackloom::TrackType::Instrument);
    addLongMidiNote(project, bassTrack);

    FakeMidiOutputPortFactory factory;
    trackloom::JuceMidiOutputRoutingController controller(
        []() {
            return std::vector<trackloom::MidiOutputDeviceInfo> {
                deviceInfo("device-a", "Device A")
            };
        },
        [&](trackloom::MidiOutputDeviceInfo info) {
            return factory.create(std::move(info));
        });
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    SilentAudioSource source;

    require(session.prepare(1920.0, 2, 480), "routing controller reassignment test session should prepare");
    require(session.rebuildAudioGraph(project, { { bassTrack.id, &source } }),
        "routing controller reassignment test should rebuild audio graph");
    controller.refreshDevices();
    require(controller.setTrackOutputDevice(leadTrack.id, deviceInfo("device-a", "Device A")),
        "routing controller reassignment test should first select lead route");
    require(controller.rebuildSelectedProjectMidiOutputSafely(session, project, 0).success,
        "routing controller reassignment test should establish lead route");

    require(controller.clearTrackOutputDevice(leadTrack.id),
        "routing controller reassignment test should clear old lead route selection");
    require(controller.setTrackOutputDevice(bassTrack.id, deviceInfo("device-a", "Device A")),
        "routing controller reassignment test should select same device for bass route");
    const auto refresh = controller.refreshDevicesAndRebuildSelectedProjectMidiOutputSafely(
        session,
        project,
        12);

    require(refresh.outputRefresh.success,
        "routing controller should rebuild when same device is reassigned to another track");
    require(refresh.outputRefresh.safeRebuildAttempted,
        "routing controller should mark same-device track reassignment as a safe rebuild");

    transport.play();
    const auto blockResult = renderPlaybackBlock(session, transport, project);
    const auto deviceA = factory.latestStateForDevice("device-a");

    require(blockResult.renderSucceeded,
        "routing controller reassignment test should render midi block");
    require(blockResult.midiDispatch.success,
        "routing controller should dispatch midi after same-device track reassignment");
    require(deviceA != nullptr && deviceA->sentMessages.size() == 1,
        "routing controller should send bass note through reassigned device route");
    require(deviceA->sentMessages[0].statusByte == 0x90,
        "routing controller should send note on after reassigned device route");
}

void juceMidiOutputRoutingControllerSelectsVisibleDeviceByIdFromCache()
{
    std::size_t enumerateCount = 0;
    trackloom::JuceMidiOutputRoutingController controller(
        [&]() {
            ++enumerateCount;
            return std::vector<trackloom::MidiOutputDeviceInfo> {
                deviceInfo("device-a", "Device A Current")
            };
        },
        nullptr);

    controller.refreshDevices();

    require(controller.setTrackOutputDeviceById("track-a", "device-a"),
        "routing controller should select a visible cached device by id");

    const auto selected = controller.selectedBindings();
    require(selected.size() == 1,
        "routing controller should create one selected binding from cached device id");
    require(selected[0].trackId == "track-a",
        "routing controller should preserve track id when selecting by device id");
    require(selected[0].device.id == "device-a" && selected[0].device.name == "Device A Current",
        "routing controller should store current cached device info when selecting by id");
    require(enumerateCount == 1,
        "routing controller should not enumerate devices while selecting from cache");
}

void juceMidiOutputRoutingControllerRejectsMissingDeviceIdWithoutChangingSelection()
{
    std::size_t enumerateCount = 0;
    trackloom::JuceMidiOutputRoutingController controller(
        [&]() {
            ++enumerateCount;
            return std::vector<trackloom::MidiOutputDeviceInfo> {
                deviceInfo("device-a", "Device A")
            };
        },
        nullptr);

    controller.refreshDevices();
    require(controller.setTrackOutputDeviceById("track-a", "device-a"),
        "routing controller missing-id test should establish initial cached selection");

    require(!controller.setTrackOutputDeviceById("track-a", "missing-device"),
        "routing controller should reject a device id that is not in the current cache");

    const auto selected = controller.selectedBindings();
    require(selected.size() == 1,
        "routing controller should keep existing selection after missing device id rejection");
    require(selected[0].device.id == "device-a",
        "routing controller should not replace existing selection with missing device id");
    require(enumerateCount == 1,
        "routing controller should not enumerate devices while rejecting missing cached id");
}

void juceMidiOutputRoutingControllerReportsRoutingStateSnapshot()
{
    trackloom::Project project("JUCE MIDI routing controller");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);

    std::vector<std::vector<trackloom::MidiOutputDeviceInfo>> snapshots {
        {
            deviceInfo("device-a", "Device A")
        },
        {}
    };
    std::size_t nextSnapshot = 0;
    FakeMidiOutputPortFactory factory;
    trackloom::JuceMidiOutputRoutingController controller(
        [&]() {
            require(nextSnapshot < snapshots.size(), "routing state snapshot test should not over-enumerate");
            return snapshots[nextSnapshot++];
        },
        [&](trackloom::MidiOutputDeviceInfo info) {
            return factory.create(std::move(info));
        });
    trackloom::ProjectPlaybackSession session;

    require(session.prepare(1920.0, 2, 480), "routing state snapshot test session should prepare");
    controller.refreshDevices();
    require(controller.setTrackOutputDeviceById(track.id, "device-a"),
        "routing state snapshot test should select a cached visible device");

    const auto selectedOnly = controller.routingState();
    require(selectedOnly.selectedBindings.size() == 1,
        "routing state should report selected binding before runtime rebuild");
    require(selectedOnly.visibleDevices.size() == 1,
        "routing state should report currently visible cached devices");
    require(selectedOnly.appliedBindings.empty(),
        "routing state should report no applied bindings before runtime rebuild");
    require(selectedOnly.openDevices.empty(),
        "routing state should report no open devices before runtime rebuild");

    require(controller.rebuildSelectedProjectMidiOutputSafely(session, project, 0).success,
        "routing state snapshot test should apply selected route");

    const auto applied = controller.routingState();
    require(applied.appliedBindings.size() == 1 && applied.appliedBindings[0].trackId == track.id,
        "routing state should report the track binding applied to the playback session");
    require(applied.openDevices.size() == 1 && applied.openDevices[0].id == "device-a",
        "routing state should report the open runtime device after rebuild");

    const auto refresh = controller.refreshDevicesAndRebuildSelectedProjectMidiOutputSafely(
        session,
        project,
        12);
    require(refresh.outputRefresh.success,
        "routing state snapshot test should safely remove disappeared runtime route");

    const auto removed = controller.routingState();
    require(removed.selectedBindings.size() == 1 && removed.selectedBindings[0].device.id == "device-a",
        "routing state should preserve user selection after selected device disappears");
    require(removed.visibleDevices.empty(),
        "routing state should report no visible devices after removal refresh");
    require(removed.appliedBindings.empty(),
        "routing state should report no applied routes after disappeared device is removed");
    require(removed.openDevices.empty(),
        "routing state should report no open devices after disappeared device is removed");
}

void juceMidiOutputRoutingControllerRefreshUpdatesSelectionWithoutRouteChurn()
{
    trackloom::Project project("JUCE MIDI routing controller");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);

    std::vector<std::vector<trackloom::MidiOutputDeviceInfo>> snapshots {
        {
            deviceInfo("device-a", "Device A")
        },
        {
            deviceInfo("device-a", "Device A Current")
        }
    };
    std::size_t nextSnapshot = 0;
    FakeMidiOutputPortFactory factory;
    trackloom::JuceMidiOutputRoutingController controller(
        [&]() {
            require(nextSnapshot < snapshots.size(), "routing controller rename test should not over-enumerate");
            return snapshots[nextSnapshot++];
        },
        [&](trackloom::MidiOutputDeviceInfo info) {
            return factory.create(std::move(info));
        });
    trackloom::ProjectPlaybackSession session;

    require(session.prepare(1920.0, 2, 480), "routing controller rename test session should prepare");
    controller.refreshDevices();
    require(controller.setTrackOutputDevice(track.id, deviceInfo("device-a", "Device A")),
        "routing controller should accept selected device");
    require(controller.rebuildSelectedProjectMidiOutputSafely(session, project, 0).success,
        "routing controller should establish initial route");

    const auto refresh = controller.refreshDevicesAndRebuildSelectedProjectMidiOutputSafely(
        session,
        project,
        16);
    const auto selected = controller.selectedBindings();
    const auto openDevices = controller.openDeviceInfos();

    require(refresh.outputRefresh.success,
        "routing controller refresh should succeed when selected device remains visible");
    require(!refresh.outputRefresh.safeRebuildAttempted,
        "routing controller refresh should not rebuild for a display-name-only change");
    require(selected.size() == 1 && selected[0].device.name == "Device A Current",
        "routing controller should update selected device display name from latest snapshot");
    require(factory.createdPortCount() == 1,
        "routing controller should not reopen ports for a display-name-only change");
    require(openDevices.size() == 1 && openDevices[0].name == "Device A",
        "routing controller should keep existing runtime route when no rebuild is needed");
}

void juceMidiOutputRoutingControllerClearsSelectionAndRoute()
{
    trackloom::Project project("JUCE MIDI routing controller");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);

    FakeMidiOutputPortFactory factory;
    trackloom::JuceMidiOutputRoutingController controller(
        []() {
            return std::vector<trackloom::MidiOutputDeviceInfo> {
                deviceInfo("device-a", "Device A")
            };
        },
        [&](trackloom::MidiOutputDeviceInfo info) {
            return factory.create(std::move(info));
        });
    trackloom::ProjectPlaybackSession session;

    require(session.prepare(1920.0, 2, 480), "routing controller clear test session should prepare");
    require(controller.setTrackOutputDevice(track.id, deviceInfo("device-a", "Device A")),
        "routing controller should accept selected device before clear");
    require(controller.rebuildSelectedProjectMidiOutputSafely(session, project, 0).success,
        "routing controller should establish route before clear");

    const auto deviceA = factory.stateForDevice("device-a");
    require(controller.clearTrackOutputDevice(track.id),
        "routing controller should clear an existing track output selection");
    const auto rebuild = controller.rebuildSelectedProjectMidiOutputSafely(session, project, 0);

    require(rebuild.success, "routing controller should rebuild after clearing selected routes");
    require(controller.selectedBindings().empty(),
        "routing controller should remove cleared track from selected bindings");
    require(controller.openDeviceCount() == 0,
        "routing controller should keep no open devices after clearing selected route");
    require(session.midiReceiverCount() == 0,
        "routing controller should clear session midi route after selected route removal");
    require(deviceA != nullptr && deviceA->closeCallCount == 1,
        "routing controller should close old runtime device after selected route removal");
}

void juceMidiOutputRoutingControllerApplySkipsAlreadyAppliedRoutes()
{
    trackloom::Project project("JUCE MIDI routing controller");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);

    FakeMidiOutputPortFactory factory;
    trackloom::JuceMidiOutputRoutingController controller(
        []() {
            return std::vector<trackloom::MidiOutputDeviceInfo> {
                deviceInfo("device-a", "Device A")
            };
        },
        [&](trackloom::MidiOutputDeviceInfo info) {
            return factory.create(std::move(info));
        });

    trackloom::ProjectPlaybackSession session;
    require(session.prepare(1920.0, 2, 480), "routing controller apply no-op test session should prepare");
    controller.refreshDevices();
    require(controller.setTrackOutputDeviceById(track.id, "device-a"),
        "routing controller apply no-op test should select cached device");

    const auto firstApply = controller.applySelectedProjectMidiOutputIfNeeded(session, project, 0);
    require(firstApply.success, "routing controller should apply pending selected route");
    require(firstApply.safeRebuildAttempted,
        "routing controller should rebuild while selected route is still pending");
    require(factory.createdPortCount() == 1,
        "routing controller should open one port for the first apply");

    const auto deviceA = factory.latestStateForDevice("device-a");
    const auto secondApply = controller.applySelectedProjectMidiOutputIfNeeded(session, project, 0);

    require(secondApply.success, "routing controller should treat already-applied routes as a successful no-op");
    require(!secondApply.safeRebuildAttempted,
        "routing controller should not rebuild when selected routes already match runtime state");
    require(secondApply.openedDeviceCount == 1,
        "routing controller no-op apply should report the existing open device count");
    require(factory.createdPortCount() == 1,
        "routing controller no-op apply should not create another port");
    require(deviceA != nullptr && deviceA->open,
        "routing controller no-op apply should keep the existing device open");
}

void juceMidiOutputRoutingControllerApplyClearsStaleRoutes()
{
    trackloom::Project project("JUCE MIDI routing controller");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);

    FakeMidiOutputPortFactory factory;
    trackloom::JuceMidiOutputRoutingController controller(
        []() {
            return std::vector<trackloom::MidiOutputDeviceInfo> {
                deviceInfo("device-a", "Device A")
            };
        },
        [&](trackloom::MidiOutputDeviceInfo info) {
            return factory.create(std::move(info));
        });

    trackloom::ProjectPlaybackSession session;
    require(session.prepare(1920.0, 2, 480), "routing controller stale apply test session should prepare");
    controller.refreshDevices();
    require(controller.setTrackOutputDeviceById(track.id, "device-a"),
        "routing controller stale apply test should select cached device");
    require(controller.applySelectedProjectMidiOutputIfNeeded(session, project, 0).success,
        "routing controller stale apply test should establish initial route");

    const auto deviceA = factory.latestStateForDevice("device-a");
    require(controller.clearTrackOutputDevice(track.id),
        "routing controller stale apply test should clear existing selection");
    const auto clearApply = controller.applySelectedProjectMidiOutputIfNeeded(session, project, 0);

    require(clearApply.success, "routing controller should safely apply a cleared selection");
    require(clearApply.safeRebuildAttempted,
        "routing controller should rebuild when a stale applied route must be removed");
    require(controller.openDeviceCount() == 0,
        "routing controller should close runtime devices after applying cleared selection");
    require(controller.routingState().appliedBindings.empty(),
        "routing controller should remove stale applied bindings after successful apply");
    require(deviceA != nullptr && deviceA->closeCallCount == 1,
        "routing controller should close the stale runtime device after apply");
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
        juceMidiOutputBindingRefreshPlansUnavailableDeviceRemoval();
        juceMidiOutputBindingRefreshKeepsAllVisibleBindingsWithoutRebuild();
        juceMidiOutputRoutingStateDescribesSelectedRouteStatuses();
        juceMidiOutputRoutingStateReportsStaleAppliedRoutes();
        juceMidiOutputRoutingStateReportsWhetherApplyIsNeeded();
        juceMidiOutputDeviceManagerRefreshRemovesMissingDeviceViaSafeRebuild();
        juceMidiOutputDeviceManagerRefreshKeepsOldRouteWhenSafeRebuildFails();
        juceMidiOutputDeviceManagerRefreshSkipsRebuildWhenAllBindingsVisible();
        juceMidiOutputRoutingControllerRefreshKeepsSelectionWhenDeviceDisappears();
        juceMidiOutputRoutingControllerRestoresRouteWhenDeviceReappears();
        juceMidiOutputRoutingControllerRebuildsWhenSameDeviceMovesToAnotherTrack();
        juceMidiOutputRoutingControllerSelectsVisibleDeviceByIdFromCache();
        juceMidiOutputRoutingControllerRejectsMissingDeviceIdWithoutChangingSelection();
        juceMidiOutputRoutingControllerReportsRoutingStateSnapshot();
        juceMidiOutputRoutingControllerRefreshUpdatesSelectionWithoutRouteChurn();
        juceMidiOutputRoutingControllerClearsSelectionAndRoute();
        juceMidiOutputRoutingControllerApplySkipsAlreadyAppliedRoutes();
        juceMidiOutputRoutingControllerApplyClearsStaleRoutes();
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
