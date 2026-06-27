#pragma once

#include "AudioEngine.h"
#include "MidiDispatch.h"
#include "MidiTrackRouter.h"
#include "Project.h"

#include <cstddef>
#include <vector>

namespace trackloom {

// ProjectMidiOutputGraph 是工程级 MIDI 输出图。
// 它只验证轨道绑定并分发已渲染事件，不拥有真实设备、插件或采样器实例。
class ProjectMidiOutputGraph final {
public:
    // rebuild 在播放前更新绑定；只有工程中存在的乐器轨可以绑定 MIDI 接收器。
    bool rebuild(const Project& project, const std::vector<MidiTrackReceiverBinding>& bindings);

    std::size_t receiverCount() const;

    // dispatch 发送 AudioEngine 当前 block 已经产出的 scheduled MIDI 事件。
    MidiDispatchResult dispatch(const AudioEngineRenderResult& result);

private:
    MidiTrackRouter router_;
};

}
