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

// 设备列表刷新后，当前轨道绑定的可用性计划。
// availableBindings 可直接用于安全重建路由；unavailableBindings 保留给 UI 提示和未来重连。
struct MidiOutputDeviceBindingRefreshPlan {
    std::vector<MidiOutputDeviceTrackBinding> availableBindings;
    std::vector<MidiOutputDeviceTrackBinding> unavailableBindings;
    bool requiresSafeRebuild = false;
};

// 根据最新可见设备列表规划轨道绑定变化，不打开设备，也不修改播放会话。
// 设备不可见时从可路由绑定中移除，但保留原绑定信息，方便用户重新连接后恢复。
MidiOutputDeviceBindingRefreshPlan planMidiOutputDeviceBindingRefresh(
    const std::vector<MidiOutputDeviceTrackBinding>& bindings,
    const std::vector<MidiOutputDeviceInfo>& visibleDevices);

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

// MidiOutputDeviceManagerRefreshResult 连接“设备刷新计划”和“安全重建结果”。
// safeRebuildAttempted 为 false 时表示没有设备消失，旧路由保持不动，rebuild 字段不代表失败。
struct MidiOutputDeviceManagerRefreshResult {
    bool success = false;
    MidiOutputDeviceManagerFailureReason failureReason = MidiOutputDeviceManagerFailureReason::None;
    MidiOutputDeviceBindingRefreshPlan bindingPlan;
    bool safeRebuildAttempted = false;
    MidiOutputDeviceManagerRebuildResult rebuild;
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

    MidiOutputDeviceManagerRefreshResult refreshProjectMidiOutputForVisibleDevicesSafely(
        ProjectPlaybackSession& session,
        const Project& project,
        const std::vector<MidiOutputDeviceTrackBinding>& bindings,
        const std::vector<MidiOutputDeviceInfo>& visibleDevices,
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

// MidiOutputRoutingRefreshResult 是设备设置控制层的一次刷新结果。
// deviceDiff 给 UI 提示设备变化；outputRefresh 给播放层提示路由应用结果。
struct MidiOutputRoutingRefreshResult {
    MidiOutputDeviceListDiff deviceDiff;
    MidiOutputDeviceManagerRefreshResult outputRefresh;
};

// MidiOutputRoutingRefreshSummary 是给 UI/诊断面板使用的刷新摘要。
// 它把设备变化、离线选择和安全重建失败折叠成几个布尔值，避免界面层重复拆结果结构。
struct MidiOutputRoutingRefreshSummary {
    bool deviceListChanged = false;
    bool hasUnavailableSelections = false;
    bool safeRebuildAttempted = false;
    bool safeRebuildFailed = false;
    bool requiresUserAttention = false;
};

MidiOutputRoutingRefreshSummary summarizeMidiOutputRoutingRefreshResult(
    const MidiOutputRoutingRefreshResult& result);

// MidiOutputRoutingApplyResult 表示一次“应用当前选择”的结果。
// safeRebuildAttempted 为 false 时表示当前选择已经生效，调用方不需要提示设备被重开。
struct MidiOutputRoutingApplyResult {
    bool success = false;
    MidiOutputDeviceManagerFailureReason failureReason = MidiOutputDeviceManagerFailureReason::None;
    MidiOutputDeviceBindingRefreshPlan bindingPlan;
    bool safeRebuildAttempted = false;
    MidiOutputDeviceManagerRebuildResult rebuild;
    std::size_t openedDeviceCount = 0;
};

// MidiOutputRoutingApplySummary 是给 UI/诊断面板使用的“应用选择”摘要。
// 它说明本次应用是否成功、是否真的触碰运行态路由，以及是否需要提示用户。
struct MidiOutputRoutingApplySummary {
    bool success = false;
    bool hasUnavailableSelections = false;
    bool safeRebuildAttempted = false;
    bool safeRebuildFailed = false;
    bool didApplyRuntimeChanges = false;
    bool requiresUserAttention = false;
};

MidiOutputRoutingApplySummary summarizeMidiOutputRoutingApplyResult(
    const MidiOutputRoutingApplyResult& result);

// MidiOutputRoutingState 是控制层给 UI 或诊断工具读取的一次性状态快照。
// 它只描述当前内存状态，不写工程文件，也不触发设备枚举、端口打开或路由重建。
struct MidiOutputRoutingState {
    std::vector<MidiOutputDeviceTrackBinding> selectedBindings;
    std::vector<MidiOutputDeviceInfo> visibleDevices;
    std::vector<MidiOutputDeviceTrackBinding> appliedBindings;
    std::vector<MidiOutputDeviceInfo> openDevices;
};

// 单条用户选择在当前路由快照中的可展示状态。
// UI 应根据这个枚举显示提示，而不是自己比较多个底层列表。
enum class MidiOutputRouteStatus {
    Applied,
    PendingApply,
    DeviceUnavailable,
    OpenDeviceMissing,
    StaleAppliedRoute
};

struct MidiOutputRouteStatusDescription {
    MidiOutputDeviceTrackBinding selectedBinding;
    MidiOutputRouteStatus status = MidiOutputRouteStatus::PendingApply;
};

std::vector<MidiOutputRouteStatusDescription> describeMidiOutputRoutingState(
    const MidiOutputRoutingState& state);

// 将单条展示状态转换为“是否需要应用更改”，避免 UI 重复编写状态比较规则。
bool midiOutputRouteStatusRequiresApply(MidiOutputRouteStatus status);

// 检查整份路由快照是否还有未应用到运行态播放会话的差异。
bool midiOutputRoutingStateRequiresApply(const MidiOutputRoutingState& state);

// JuceMidiOutputRoutingController 保存用户当前选择，并把设备刷新应用到安全路由重建。
// 它不写工程文件、不启动后台轮询，也不直接处理 UI；后续界面和 AI 工具可复用这个边界。
class JuceMidiOutputRoutingController final {
public:
    explicit JuceMidiOutputRoutingController(
        MidiOutputDeviceListProvider deviceListProvider = availableJuceMidiOutputDevices,
        MidiOutputDevicePortFactory portFactory = createJuceMidiOutputPort);

    bool setTrackOutputDevice(std::string trackId, MidiOutputDeviceInfo device);

    // 从当前设备缓存按 id 选择输出设备；不会隐式重新枚举系统设备，也不会直接打开端口。
    bool setTrackOutputDeviceById(std::string trackId, const std::string& deviceId);

    bool clearTrackOutputDevice(const std::string& trackId);

    const std::vector<MidiOutputDeviceTrackBinding>& selectedBindings() const;
    const std::vector<MidiOutputDeviceInfo>& devices() const;
    MidiOutputRoutingState routingState() const;

    MidiOutputDeviceListDiff refreshDevices();

    MidiOutputDeviceManagerRebuildResult rebuildSelectedProjectMidiOutputSafely(
        ProjectPlaybackSession& session,
        const Project& project,
        int releaseSampleOffset);

    MidiOutputRoutingApplyResult applySelectedProjectMidiOutputIfNeeded(
        ProjectPlaybackSession& session,
        const Project& project,
        int releaseSampleOffset);

    MidiOutputRoutingRefreshResult refreshDevicesAndRebuildSelectedProjectMidiOutputSafely(
        ProjectPlaybackSession& session,
        const Project& project,
        int releaseSampleOffset);

    std::size_t openDeviceCount() const;
    std::vector<MidiOutputDeviceInfo> openDeviceInfos() const;

private:
    void updateSelectedDeviceInfosFromVisibleDevices();

    JuceMidiOutputDeviceList deviceList_;
    JuceMidiOutputDeviceManager deviceManager_;
    std::vector<MidiOutputDeviceTrackBinding> selectedBindings_;
    std::vector<MidiOutputDeviceTrackBinding> appliedBindings_;
};

}
