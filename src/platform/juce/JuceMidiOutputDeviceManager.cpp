#include "JuceMidiOutputDeviceManager.h"

#include <utility>

namespace trackloom {
namespace {

bool hasTrackBinding(
    const std::vector<MidiOutputDeviceTrackBinding>& bindings,
    const std::string& trackId)
{
    for (const auto& binding : bindings) {
        if (binding.trackId == trackId) {
            return true;
        }
    }

    return false;
}

bool isValidDeviceTrackBinding(
    const Project& project,
    const std::vector<MidiOutputDeviceTrackBinding>& acceptedBindings,
    const MidiOutputDeviceTrackBinding& binding)
{
    if (binding.trackId.empty() || binding.device.id.empty()) {
        return false;
    }
    if (hasTrackBinding(acceptedBindings, binding.trackId)) {
        return false;
    }

    const auto track = project.findTrackById(binding.trackId);
    return track.has_value() && track->type == TrackType::Instrument;
}

MidiOutputDeviceManagerFailureReason failureReasonFromSessionRebuild(
    ProjectPlaybackMidiOutputRebuildFailureReason reason)
{
    switch (reason) {
    case ProjectPlaybackMidiOutputRebuildFailureReason::None:
        return MidiOutputDeviceManagerFailureReason::None;
    case ProjectPlaybackMidiOutputRebuildFailureReason::SessionNotPrepared:
        return MidiOutputDeviceManagerFailureReason::SessionNotPrepared;
    case ProjectPlaybackMidiOutputRebuildFailureReason::MidiReleaseFailed:
        return MidiOutputDeviceManagerFailureReason::MidiReleaseFailed;
    case ProjectPlaybackMidiOutputRebuildFailureReason::OutputBindingRejected:
        return MidiOutputDeviceManagerFailureReason::OutputBindingRejected;
    }

    return MidiOutputDeviceManagerFailureReason::OutputBindingRejected;
}

}

JuceMidiOutputDeviceManager::JuceMidiOutputDeviceManager(MidiOutputDevicePortFactory portFactory)
    : portFactory_(std::move(portFactory))
{
}

JuceMidiOutputDeviceManager::~JuceMidiOutputDeviceManager()
{
    closeActiveDevices();
}

MidiOutputDeviceManagerRebuildResult JuceMidiOutputDeviceManager::rebuildProjectMidiOutputSafely(
    ProjectPlaybackSession& session,
    const Project& project,
    const std::vector<MidiOutputDeviceTrackBinding>& bindings,
    int releaseSampleOffset)
{
    MidiOutputDeviceManagerRebuildResult result;
    result.openedDeviceCount = activeDevices_.size();

    // 没有 prepare 的播放会话无法安全持有路由，先拒绝，避免打开真实设备后又无法接入。
    if (!session.isPrepared()) {
        result.failureReason = MidiOutputDeviceManagerFailureReason::SessionNotPrepared;
        return result;
    }

    std::vector<MidiOutputDeviceTrackBinding> acceptedBindings;
    acceptedBindings.reserve(bindings.size());
    for (const auto& binding : bindings) {
        // 先做纯工程校验，再打开设备；非法轨道不能触发外部 I/O。
        if (!isValidDeviceTrackBinding(project, acceptedBindings, binding)) {
            result.failureReason = MidiOutputDeviceManagerFailureReason::InvalidBinding;
            result.failedTrackId = binding.trackId;
            result.failedDevice = binding.device;
            return result;
        }

        acceptedBindings.push_back(binding);
    }

    std::vector<ActiveDevice> nextDevices;
    std::vector<MidiTrackReceiverBinding> receiverBindings;
    nextDevices.reserve(bindings.size());
    receiverBindings.reserve(bindings.size());

    for (const auto& binding : bindings) {
        if (!portFactory_) {
            closeDeviceSet(nextDevices);
            result.failureReason = MidiOutputDeviceManagerFailureReason::DeviceFactoryRejected;
            result.failedTrackId = binding.trackId;
            result.failedDevice = binding.device;
            return result;
        }

        auto port = portFactory_(binding.device);
        if (port == nullptr) {
            closeDeviceSet(nextDevices);
            result.failureReason = MidiOutputDeviceManagerFailureReason::DeviceFactoryRejected;
            result.failedTrackId = binding.trackId;
            result.failedDevice = binding.device;
            return result;
        }

        auto receiver = std::make_unique<MidiOutputDevice>(*port);
        if (!receiver->open()) {
            receiver->close();
            closeDeviceSet(nextDevices);
            result.failureReason = MidiOutputDeviceManagerFailureReason::DeviceOpenRejected;
            result.failedTrackId = binding.trackId;
            result.failedDevice = binding.device;
            return result;
        }

        // receiver 由 nextDevices 拥有；播放会话只保存非拥有指针。
        receiverBindings.push_back({ binding.trackId, receiver.get() });
        nextDevices.push_back({ std::move(port), std::move(receiver) });
    }

    result.sessionRebuild = session.rebuildMidiOutputSafely(project, receiverBindings, releaseSampleOffset);
    if (!result.sessionRebuild.success) {
        // 安全重建失败时保留旧设备集合；只关闭本次临时打开的新设备。
        closeDeviceSet(nextDevices);
        result.failureReason = failureReasonFromSessionRebuild(result.sessionRebuild.failureReason);
        result.openedDeviceCount = activeDevices_.size();
        return result;
    }

    closeActiveDevices();
    activeDevices_ = std::move(nextDevices);

    result.success = true;
    result.failureReason = MidiOutputDeviceManagerFailureReason::None;
    result.openedDeviceCount = activeDevices_.size();
    return result;
}

std::size_t JuceMidiOutputDeviceManager::openDeviceCount() const
{
    return activeDevices_.size();
}

std::vector<MidiOutputDeviceInfo> JuceMidiOutputDeviceManager::openDeviceInfos() const
{
    std::vector<MidiOutputDeviceInfo> infos;
    infos.reserve(activeDevices_.size());

    for (const auto& device : activeDevices_) {
        infos.push_back(device.receiver->info());
    }

    return infos;
}

void JuceMidiOutputDeviceManager::closeActiveDevices()
{
    closeDeviceSet(activeDevices_);
}

void JuceMidiOutputDeviceManager::closeDeviceSet(std::vector<ActiveDevice>& devices)
{
    for (auto& device : devices) {
        if (device.receiver != nullptr) {
            device.receiver->close();
        } else if (device.port != nullptr) {
            device.port->close();
        }
    }

    devices.clear();
}

}
