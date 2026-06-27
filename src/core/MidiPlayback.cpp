#include "MidiPlayback.h"

#include <algorithm>

namespace trackloom {
namespace {

bool projectHasSoloedTrack(const Project& project)
{
    for (const auto& track : project.tracks()) {
        if (track.playback.soloed) {
            return true;
        }
    }

    return false;
}

bool trackShouldEmitMidi(const Track& track, bool soloModeActive)
{
    if (track.type != TrackType::Instrument) {
        return false;
    }
    if (track.playback.muted || track.playback.disabled) {
        return false;
    }

    // 只要工程中存在 solo 轨，未 solo 的轨道就不参与播放调度。
    return !soloModeActive || track.playback.soloed;
}

bool tickIsInsideWindow(std::int64_t tick, std::int64_t startTick, std::int64_t endTick)
{
    return tick >= startTick && tick < endTick;
}

int eventTypeSortKey(MidiPlaybackEventType type)
{
    // 同一 tick 上先关旧音符再开新音符，避免后续发给乐器时出现不必要的重叠歧义。
    return type == MidiPlaybackEventType::NoteOff ? 0 : 1;
}

bool eventSortsBefore(const MidiPlaybackEvent& left, const MidiPlaybackEvent& right)
{
    if (left.absoluteTick != right.absoluteTick) {
        return left.absoluteTick < right.absoluteTick;
    }
    if (left.type != right.type) {
        return eventTypeSortKey(left.type) < eventTypeSortKey(right.type);
    }
    if (left.trackId != right.trackId) {
        return left.trackId < right.trackId;
    }
    if (left.clipId != right.clipId) {
        return left.clipId < right.clipId;
    }
    if (left.noteId != right.noteId) {
        return left.noteId < right.noteId;
    }
    if (left.noteNumber != right.noteNumber) {
        return left.noteNumber < right.noteNumber;
    }

    return left.channel < right.channel;
}

}

std::vector<MidiPlaybackEvent> collectMidiPlaybackEvents(
    const Project& project,
    std::int64_t startTick,
    std::int64_t endTick)
{
    std::vector<MidiPlaybackEvent> events;
    if (startTick < 0 || endTick <= startTick) {
        return events;
    }

    const bool soloModeActive = projectHasSoloedTrack(project);
    for (const auto& clip : project.clips()) {
        if (clip.type != ClipType::Midi) {
            continue;
        }

        const auto track = project.findTrackById(clip.trackId);
        if (!track.has_value() || !trackShouldEmitMidi(*track, soloModeActive)) {
            continue;
        }

        for (const auto& note : clip.midiNotes) {
            const auto noteOnTick = clip.startTick + note.startTick;
            const auto noteOffTick = noteOnTick + note.lengthTick;

            if (tickIsInsideWindow(noteOnTick, startTick, endTick)) {
                events.push_back({
                    MidiPlaybackEventType::NoteOn,
                    clip.trackId,
                    clip.id,
                    note.id,
                    noteOnTick,
                    note.noteNumber,
                    note.velocity,
                    note.channel
                });
            }

            if (tickIsInsideWindow(noteOffTick, startTick, endTick)) {
                events.push_back({
                    MidiPlaybackEventType::NoteOff,
                    clip.trackId,
                    clip.id,
                    note.id,
                    noteOffTick,
                    note.noteNumber,
                    0,
                    note.channel
                });
            }
        }
    }

    std::sort(events.begin(), events.end(), eventSortsBefore);
    return events;
}

}
