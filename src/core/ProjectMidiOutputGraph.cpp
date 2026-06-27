#include "ProjectMidiOutputGraph.h"

#include "AudioEngineMidiBridge.h"

namespace trackloom {
namespace {

bool bindingTargetsInstrumentTrack(
    const Project& project,
    const MidiTrackReceiverBinding& binding)
{
    const auto track = project.findTrackById(binding.trackId);
    return track.has_value() && track->type == TrackType::Instrument;
}

}

bool ProjectMidiOutputGraph::rebuild(
    const Project& project,
    const std::vector<MidiTrackReceiverBinding>& bindings)
{
    for (const auto& binding : bindings) {
        if (!bindingTargetsInstrumentTrack(project, binding)) {
            return false;
        }
    }

    // MidiTrackRouter 继续负责空 ID、空 receiver 和重复 trackId 的一致性校验。
    return router_.rebuild(bindings);
}

std::size_t ProjectMidiOutputGraph::receiverCount() const
{
    return router_.receiverCount();
}

MidiDispatchResult ProjectMidiOutputGraph::dispatch(const AudioEngineRenderResult& result)
{
    // 复用 AudioEngineMidiBridge，避免项目级输出图重新实现调度事件发送规则。
    return dispatchAudioEngineMidiEvents(result, router_);
}

}
