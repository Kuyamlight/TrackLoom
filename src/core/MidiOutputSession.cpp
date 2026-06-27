#include "MidiOutputSession.h"

#include <algorithm>

namespace trackloom {
namespace {

std::size_t deliveredEventCount(
    const MidiDispatchResult& result,
    std::size_t availableEventCount)
{
    if (result.deliveredEventCount <= 0) {
        return 0;
    }

    return std::min(static_cast<std::size_t>(result.deliveredEventCount), availableEventCount);
}

}

bool MidiOutputSession::rebuild(
    const Project& project,
    const std::vector<MidiTrackReceiverBinding>& bindings)
{
    if (!activeNotes_.empty()) {
        return false;
    }

    return graph_.rebuild(project, bindings);
}

std::size_t MidiOutputSession::receiverCount() const
{
    return graph_.receiverCount();
}

std::size_t MidiOutputSession::activeNoteCount() const
{
    return activeNotes_.size();
}

bool MidiOutputSession::sameMidiKey(const ActiveMidiNote& note, const MidiPlaybackEvent& event)
{
    return note.trackId == event.trackId
        && note.channel == event.channel
        && note.noteNumber == event.noteNumber;
}

ScheduledMidiPlaybackEvent MidiOutputSession::releaseEventForActiveNote(
    const ActiveMidiNote& note,
    int sampleOffset)
{
    // releaseAllActiveNotes 不是时间线调度事件，absoluteTick 只保留 0 作为“立即释放”的占位值。
    return ScheduledMidiPlaybackEvent {
        MidiPlaybackEvent {
            MidiPlaybackEventType::NoteOff,
            note.trackId,
            note.clipId,
            note.noteId,
            0,
            note.noteNumber,
            0,
            note.channel
        },
        sampleOffset
    };
}

void MidiOutputSession::observeDeliveredEvent(const MidiPlaybackEvent& event)
{
    if (event.type == MidiPlaybackEventType::NoteOn) {
        auto activeNote = std::find_if(
            activeNotes_.begin(),
            activeNotes_.end(),
            [&event](const ActiveMidiNote& note) {
                return sameMidiKey(note, event);
            });

        if (activeNote == activeNotes_.end()) {
            activeNotes_.push_back(ActiveMidiNote {
                event.trackId,
                event.clipId,
                event.noteId,
                event.noteNumber,
                event.channel,
                1
            });
        } else {
            ++activeNote->holdCount;
            activeNote->clipId = event.clipId;
            activeNote->noteId = event.noteId;
        }
    } else if (event.type == MidiPlaybackEventType::NoteOff) {
        auto activeNote = std::find_if(
            activeNotes_.begin(),
            activeNotes_.end(),
            [&event](const ActiveMidiNote& note) {
                return sameMidiKey(note, event);
            });

        if (activeNote != activeNotes_.end()) {
            --activeNote->holdCount;
            if (activeNote->holdCount <= 0) {
                activeNotes_.erase(activeNote);
            }
        }
    }
}

MidiDispatchResult MidiOutputSession::dispatch(const AudioEngineRenderResult& result)
{
    const auto dispatchResult = graph_.dispatch(result);
    const auto deliveredCount = deliveredEventCount(
        dispatchResult,
        result.scheduledMidiEvents.size());

    for (std::size_t index = 0; index < deliveredCount; ++index) {
        observeDeliveredEvent(result.scheduledMidiEvents[index].event);
    }

    return dispatchResult;
}

MidiDispatchResult MidiOutputSession::releaseAllActiveNotes(int sampleOffset)
{
    AudioEngineRenderResult releaseResult;
    releaseResult.scheduledMidiEvents.reserve(activeNotes_.size());

    for (const auto& activeNote : activeNotes_) {
        releaseResult.scheduledMidiEvents.push_back(
            MidiOutputSession::releaseEventForActiveNote(activeNote, sampleOffset));
    }

    const auto dispatchResult = graph_.dispatch(releaseResult);
    const auto deliveredCount = deliveredEventCount(
        dispatchResult,
        releaseResult.scheduledMidiEvents.size());

    for (std::size_t index = 0; index < deliveredCount; ++index) {
        const auto& event = releaseResult.scheduledMidiEvents[index].event;
        auto activeNote = std::find_if(
            activeNotes_.begin(),
            activeNotes_.end(),
            [&event](const ActiveMidiNote& note) {
                return sameMidiKey(note, event);
            });

        // releaseAllActiveNotes 是停止播放时的“全部释放”语义，成功送达一次 Note Off 后清掉整个键。
        if (activeNote != activeNotes_.end()) {
            activeNotes_.erase(activeNote);
        }
    }

    return dispatchResult;
}

}
