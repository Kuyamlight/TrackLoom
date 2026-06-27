#include "MidiTrackRouter.h"

namespace trackloom {
namespace {

bool containsTrackId(
    const std::vector<MidiTrackReceiverBinding>& bindings,
    const std::string& trackId)
{
    for (const auto& binding : bindings) {
        if (binding.trackId == trackId) {
            return true;
        }
    }

    return false;
}

}

bool MidiTrackRouter::rebuild(const std::vector<MidiTrackReceiverBinding>& bindings)
{
    std::vector<MidiTrackReceiverBinding> nextBindings;
    nextBindings.reserve(bindings.size());

    for (const auto& binding : bindings) {
        if (binding.trackId.empty() || binding.receiver == nullptr) {
            return false;
        }
        if (containsTrackId(nextBindings, binding.trackId)) {
            return false;
        }

        nextBindings.push_back(binding);
    }

    bindings_ = nextBindings;
    return true;
}

std::size_t MidiTrackRouter::receiverCount() const
{
    return bindings_.size();
}

bool MidiTrackRouter::receiveMidiEvent(
    const ScheduledMidiPlaybackEvent& event,
    const MidiOutputMessage& message)
{
    // 只做只读查找和转发；缺失路由交给 MidiDispatchResult 记录失败索引。
    for (const auto& binding : bindings_) {
        if (binding.trackId == event.event.trackId) {
            return binding.receiver->receiveMidiEvent(event, message);
        }
    }

    return false;
}

}
