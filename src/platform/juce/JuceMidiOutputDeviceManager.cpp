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

bool hasDeviceId(
    const std::vector<MidiOutputDeviceInfo>& devices,
    const std::string& deviceId)
{
    for (const auto& device : devices) {
        if (device.id == deviceId) {
            return true;
        }
    }

    return false;
}

bool hasTrackDeviceBinding(
    const std::vector<MidiOutputDeviceTrackBinding>& bindings,
    const MidiOutputDeviceTrackBinding& expected)
{
    for (const auto& binding : bindings) {
        if (binding.trackId == expected.trackId && binding.device.id == expected.device.id) {
            return true;
        }
    }

    return false;
}

std::optional<MidiOutputDeviceInfo> findVisibleDeviceById(
    const std::vector<MidiOutputDeviceInfo>& devices,
    const std::string& deviceId)
{
    if (deviceId.empty()) {
        return std::nullopt;
    }

    for (const auto& device : devices) {
        if (device.id == deviceId) {
            return device;
        }
    }

    return std::nullopt;
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

bool appliedBindingsMatchAvailableBindings(
    const std::vector<MidiOutputDeviceTrackBinding>& appliedBindings,
    const std::vector<MidiOutputDeviceTrackBinding>& bindings)
{
    if (appliedBindings.size() != bindings.size()) {
        return false;
    }

    for (std::size_t index = 0; index < bindings.size(); ++index) {
        if (appliedBindings[index].trackId != bindings[index].trackId) {
            return false;
        }
        if (appliedBindings[index].device.id != bindings[index].device.id) {
            return false;
        }
    }

    return true;
}

}

MidiOutputDeviceBindingRefreshPlan planMidiOutputDeviceBindingRefresh(
    const std::vector<MidiOutputDeviceTrackBinding>& bindings,
    const std::vector<MidiOutputDeviceInfo>& visibleDevices)
{
    MidiOutputDeviceBindingRefreshPlan plan;
    plan.availableBindings.reserve(bindings.size());
    plan.unavailableBindings.reserve(bindings.size());

    for (const auto& binding : bindings) {
        const auto visibleDevice = findVisibleDeviceById(visibleDevices, binding.device.id);
        if (visibleDevice.has_value()) {
            // 可见设备使用最新快照，避免 UI 后续继续显示旧设备名称。
            plan.availableBindings.push_back({ binding.trackId, *visibleDevice });
        } else {
            // 不可见设备保留原绑定信息，给 UI 提示和未来重连使用。
            plan.unavailableBindings.push_back(binding);
        }
    }

    plan.requiresSafeRebuild = !plan.unavailableBindings.empty();
    return plan;
}

std::vector<MidiOutputRouteStatusDescription> describeMidiOutputRoutingState(
    const MidiOutputRoutingState& state)
{
    std::vector<MidiOutputRouteStatusDescription> descriptions;
    descriptions.reserve(state.selectedBindings.size() + state.appliedBindings.size());

    for (const auto& selectedBinding : state.selectedBindings) {
        MidiOutputRouteStatus status = MidiOutputRouteStatus::PendingApply;
        if (!hasDeviceId(state.visibleDevices, selectedBinding.device.id)) {
            status = MidiOutputRouteStatus::DeviceUnavailable;
        } else if (!hasTrackDeviceBinding(state.appliedBindings, selectedBinding)) {
            status = MidiOutputRouteStatus::PendingApply;
        } else if (!hasDeviceId(state.openDevices, selectedBinding.device.id)) {
            // 已应用绑定但设备不在打开列表中，表示控制层状态不一致，交给 UI 或诊断面板提示。
            status = MidiOutputRouteStatus::OpenDeviceMissing;
        } else {
            status = MidiOutputRouteStatus::Applied;
        }

        descriptions.push_back({ selectedBinding, status });
    }

    for (const auto& appliedBinding : state.appliedBindings) {
        if (!hasTrackDeviceBinding(state.selectedBindings, appliedBinding)) {
            // 用户已经清除或改选，但运行态路由还没安全重建完成；UI 应提示这条旧路由待移除。
            descriptions.push_back({ appliedBinding, MidiOutputRouteStatus::StaleAppliedRoute });
        }
    }

    return descriptions;
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

MidiOutputDeviceManagerRefreshResult
JuceMidiOutputDeviceManager::refreshProjectMidiOutputForVisibleDevicesSafely(
    ProjectPlaybackSession& session,
    const Project& project,
    const std::vector<MidiOutputDeviceTrackBinding>& bindings,
    const std::vector<MidiOutputDeviceInfo>& visibleDevices,
    int releaseSampleOffset)
{
    MidiOutputDeviceManagerRefreshResult result;
    result.openedDeviceCount = activeDevices_.size();
    result.bindingPlan = planMidiOutputDeviceBindingRefresh(bindings, visibleDevices);

    if (!result.bindingPlan.requiresSafeRebuild) {
        // 所有已绑定设备仍可见时不打断现有路由；显示名变化只返回给 UI 后续刷新。
        result.success = true;
        return result;
    }

    result.safeRebuildAttempted = true;
    result.rebuild = rebuildProjectMidiOutputSafely(
        session,
        project,
        result.bindingPlan.availableBindings,
        releaseSampleOffset);
    result.success = result.rebuild.success;
    result.failureReason = result.rebuild.failureReason;
    result.openedDeviceCount = result.rebuild.openedDeviceCount;
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

JuceMidiOutputRoutingController::JuceMidiOutputRoutingController(
    MidiOutputDeviceListProvider deviceListProvider,
    MidiOutputDevicePortFactory portFactory)
    : deviceList_(std::move(deviceListProvider))
    , deviceManager_(std::move(portFactory))
{
}

bool JuceMidiOutputRoutingController::setTrackOutputDevice(
    std::string trackId,
    MidiOutputDeviceInfo device)
{
    if (trackId.empty() || device.id.empty()) {
        return false;
    }

    for (auto& binding : selectedBindings_) {
        if (binding.trackId == trackId) {
            // 同一轨道只能有一个当前选择；重新选择设备时保留原位置，避免 UI 顺序跳动。
            binding.device = std::move(device);
            return true;
        }
    }

    selectedBindings_.push_back({ std::move(trackId), std::move(device) });
    return true;
}

bool JuceMidiOutputRoutingController::setTrackOutputDeviceById(
    std::string trackId,
    const std::string& deviceId)
{
    if (trackId.empty() || deviceId.empty()) {
        return false;
    }

    // 设备选择只读取 refreshDevices() 建立的缓存；调用方决定何时刷新，避免选择时隐式访问系统设备。
    const auto device = deviceList_.findById(deviceId);
    if (!device.has_value()) {
        return false;
    }

    return setTrackOutputDevice(std::move(trackId), *device);
}

bool JuceMidiOutputRoutingController::clearTrackOutputDevice(const std::string& trackId)
{
    if (trackId.empty()) {
        return false;
    }

    for (auto binding = selectedBindings_.begin(); binding != selectedBindings_.end(); ++binding) {
        if (binding->trackId == trackId) {
            selectedBindings_.erase(binding);
            return true;
        }
    }

    return false;
}

const std::vector<MidiOutputDeviceTrackBinding>&
JuceMidiOutputRoutingController::selectedBindings() const
{
    return selectedBindings_;
}

const std::vector<MidiOutputDeviceInfo>& JuceMidiOutputRoutingController::devices() const
{
    return deviceList_.devices();
}

MidiOutputRoutingState JuceMidiOutputRoutingController::routingState() const
{
    return {
        selectedBindings_,
        deviceList_.devices(),
        appliedBindings_,
        deviceManager_.openDeviceInfos()
    };
}

MidiOutputDeviceListDiff JuceMidiOutputRoutingController::refreshDevices()
{
    auto diff = deviceList_.refresh();
    updateSelectedDeviceInfosFromVisibleDevices();
    return diff;
}

MidiOutputDeviceManagerRebuildResult
JuceMidiOutputRoutingController::rebuildSelectedProjectMidiOutputSafely(
    ProjectPlaybackSession& session,
    const Project& project,
    int releaseSampleOffset)
{
    auto result = deviceManager_.rebuildProjectMidiOutputSafely(
        session,
        project,
        selectedBindings_,
        releaseSampleOffset);
    if (result.success) {
        appliedBindings_ = selectedBindings_;
    }

    return result;
}

MidiOutputRoutingRefreshResult
JuceMidiOutputRoutingController::refreshDevicesAndRebuildSelectedProjectMidiOutputSafely(
    ProjectPlaybackSession& session,
    const Project& project,
    int releaseSampleOffset)
{
    MidiOutputRoutingRefreshResult result;
    result.deviceDiff = refreshDevices();

    result.outputRefresh.bindingPlan = planMidiOutputDeviceBindingRefresh(
        selectedBindings_,
        deviceList_.devices());
    result.outputRefresh.openedDeviceCount = deviceManager_.openDeviceCount();

    const auto routeAlreadyMatchesSelection = appliedBindingsMatchAvailableBindings(
        appliedBindings_,
        result.outputRefresh.bindingPlan.availableBindings);
    if (!result.outputRefresh.bindingPlan.requiresSafeRebuild && routeAlreadyMatchesSelection) {
        // 设备仍可见且运行态路由已匹配时，刷新只更新选择信息，不打断当前输出。
        result.outputRefresh.success = true;
        return result;
    }

    result.outputRefresh.safeRebuildAttempted = true;
    result.outputRefresh.rebuild = deviceManager_.rebuildProjectMidiOutputSafely(
        session,
        project,
        result.outputRefresh.bindingPlan.availableBindings,
        releaseSampleOffset);
    result.outputRefresh.success = result.outputRefresh.rebuild.success;
    result.outputRefresh.failureReason = result.outputRefresh.rebuild.failureReason;
    result.outputRefresh.openedDeviceCount = result.outputRefresh.rebuild.openedDeviceCount;
    if (result.outputRefresh.success) {
        appliedBindings_ = result.outputRefresh.bindingPlan.availableBindings;
    }
    return result;
}

std::size_t JuceMidiOutputRoutingController::openDeviceCount() const
{
    return deviceManager_.openDeviceCount();
}

std::vector<MidiOutputDeviceInfo> JuceMidiOutputRoutingController::openDeviceInfos() const
{
    return deviceManager_.openDeviceInfos();
}

void JuceMidiOutputRoutingController::updateSelectedDeviceInfosFromVisibleDevices()
{
    for (auto& binding : selectedBindings_) {
        const auto visibleDevice = findVisibleDeviceById(deviceList_.devices(), binding.device.id);
        if (visibleDevice.has_value()) {
            // 用户选择的设备仍在线时，只刷新显示信息；设备消失时保留旧信息用于提示和重连。
            binding.device = *visibleDevice;
        }
    }
}

}
