#include "PreparedMidiPlaybackPlan.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace trackloom {
namespace {

bool trackShouldEmitMidi(const Track& track, bool soloModeActive)
{
    return track.type == TrackType::Instrument && !track.playback.muted
        && !track.playback.disabled && (!soloModeActive || track.playback.soloed);
}

bool projectHasSoloedTrack(const Project& project)
{
    return std::any_of(project.tracks().begin(), project.tracks().end(), [](const auto& track) {
        return track.playback.soloed;
    });
}

PreparedMidiPlaybackPlanBuildResult failure(PreparedMidiPlaybackPlanBuildFailureReason reason)
{
    return {reason, nullptr};
}

struct SourceOrder {
    std::size_t track = 0;
    std::size_t clip = 0;
    std::size_t note = 0;
};

struct EventWithSource {
    PreparedMidiEvent event;
    SourceOrder source;
};

bool eventSortsBefore(const EventWithSource& left, const EventWithSource& right)
{
    return std::tuple(left.event.samplePosition, left.event.type == PreparedMidiEventType::NoteOn,
               left.source.track, left.source.clip, left.source.note)
        < std::tuple(right.event.samplePosition, right.event.type == PreparedMidiEventType::NoteOn,
               right.source.track, right.source.clip, right.source.note);
}

bool sourceSortsBefore(const EventWithSource& left, const EventWithSource& right)
{
    return std::tuple(left.source.track, left.source.clip, left.source.note,
               left.event.type == PreparedMidiEventType::NoteOn)
        < std::tuple(right.source.track, right.source.clip, right.source.note,
               right.event.type == PreparedMidiEventType::NoteOn);
}

bool tryConvertTickToSample(
    const Project& project,
    std::int64_t tick,
    double sampleRate,
    std::int64_t& samplePosition)
{
    const auto seconds = project.tickToSeconds(tick);
    const auto scaled = static_cast<double>(seconds * sampleRate);
    const auto safeMinimum = static_cast<double>(std::numeric_limits<std::int64_t>::min());
    const auto exclusiveMaximum = -safeMinimum;
    if (!std::isfinite(seconds) || !std::isfinite(scaled)
        || scaled < safeMinimum || scaled >= exclusiveMaximum) {
        return false;
    }

    samplePosition = static_cast<std::int64_t>(std::llround(scaled));
    return true;
}

bool tryAddTicks(std::int64_t left, std::int64_t right, std::int64_t& sum)
{
    if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right)
        || (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
        return false;
    }
    sum = left + right;
    return true;
}

bool isValidOutputFormat(int channelCount, std::uint64_t channelMask)
{
    return channelCount >= 1 && channelCount <= 2
        && channelMask == ((std::uint64_t{1} << channelCount) - 1);
}

}

PreparedMidiPlaybackPlanBuildResult buildPreparedMidiPlaybackPlan(
    PreparedMidiPlaybackPlanBuildRequest request,
    std::stop_token stopToken)
{
    return detail::buildPreparedMidiPlaybackPlanWithLimits(
        std::move(request), stopToken, {});
}

namespace detail {

PreparedMidiPlaybackPlanBuildResult buildPreparedMidiPlaybackPlanWithLimits(
    PreparedMidiPlaybackPlanBuildRequest request,
    std::stop_token,
    PreparedMidiPlaybackPlanLimits)
{
    if (!std::isfinite(request.sampleRate) || request.sampleRate <= 0.0) {
        return failure(PreparedMidiPlaybackPlanBuildFailureReason::InvalidSampleRate);
    }
    if (request.maximumBlockFrames <= 0) {
        return failure(PreparedMidiPlaybackPlanBuildFailureReason::InvalidMaximumBlockFrames);
    }
    if (!isValidOutputFormat(request.outputChannelCount, request.outputChannelMask)) {
        return failure(PreparedMidiPlaybackPlanBuildFailureReason::InvalidOutputFormat);
    }
    if (request.playbackStartSample < 0) {
        return failure(PreparedMidiPlaybackPlanBuildFailureReason::InvalidPlaybackStart);
    }
    if (request.loopRange) {
        return failure(PreparedMidiPlaybackPlanBuildFailureReason::InvalidLoopRange);
    }

    auto plan = std::make_unique<PreparedMidiPlaybackPlan>();
    plan->sampleRate = request.sampleRate;
    plan->maximumBlockFrames = request.maximumBlockFrames;
    plan->outputChannelCount = request.outputChannelCount;
    plan->outputChannelMask = request.outputChannelMask;
    plan->playbackStartSample = request.playbackStartSample;

    const bool soloModeActive = projectHasSoloedTrack(request.projectSnapshot);
    std::uint32_t slotIndex = 0;
    std::uint32_t noteInstanceId = 0;
    std::vector<EventWithSource> normalEvents;
    std::vector<EventWithSource> initialChaseEvents;
    for (std::size_t trackOrder = 0; trackOrder < request.projectSnapshot.tracks().size(); ++trackOrder) {
        const auto& track = request.projectSnapshot.tracks()[trackOrder];
        if (!std::isfinite(track.mix.gain) || track.mix.gain < 0.0f
            || !std::isfinite(track.mix.pan) || track.mix.pan < -1.0f || track.mix.pan > 1.0f) {
            return failure(PreparedMidiPlaybackPlanBuildFailureReason::InvalidTrackMix);
        }
        if (track.type == TrackType::Instrument) {
            plan->instrumentSlots.push_back({PreparedMidiInstrumentKind::BuiltInSine, track.mix.gain, track.mix.pan, 0});
            ++slotIndex;
        }
        if (!trackShouldEmitMidi(track, soloModeActive)) {
            continue;
        }

        const auto trackSlotIndex = slotIndex - 1;
        for (std::size_t clipOrder = 0; clipOrder < request.projectSnapshot.clips().size(); ++clipOrder) {
            const auto& clip = request.projectSnapshot.clips()[clipOrder];
            if (clip.trackId != track.id || clip.type != ClipType::Midi) {
                continue;
            }
            for (std::size_t noteOrder = 0; noteOrder < clip.midiNotes.size(); ++noteOrder) {
                const auto& note = clip.midiNotes[noteOrder];
                std::int64_t noteStartTick = 0;
                std::int64_t noteEndTick = 0;
                if (!tryAddTicks(clip.startTick, note.startTick, noteStartTick)
                    || !tryAddTicks(noteStartTick, note.lengthTick, noteEndTick)) {
                    return failure(PreparedMidiPlaybackPlanBuildFailureReason::SamplePositionOverflow);
                }
                std::int64_t onSample = 0;
                std::int64_t offSample = 0;
                if (!tryConvertTickToSample(request.projectSnapshot, noteStartTick, request.sampleRate, onSample)
                    || !tryConvertTickToSample(request.projectSnapshot, noteEndTick, request.sampleRate, offSample)) {
                    return failure(PreparedMidiPlaybackPlanBuildFailureReason::SamplePositionOverflow);
                }
                if (offSample < request.playbackStartSample) {
                    continue;
                }

                const SourceOrder source{trackOrder, clipOrder, noteOrder};
                const PreparedMidiEvent noteOn{onSample, noteInstanceId, 0, trackSlotIndex,
                    static_cast<std::uint8_t>(note.channel), static_cast<std::uint8_t>(note.noteNumber),
                    static_cast<std::uint8_t>(note.velocity), PreparedMidiEventType::NoteOn};
                const PreparedMidiEvent noteOff{offSample, noteInstanceId, 0, trackSlotIndex,
                    static_cast<std::uint8_t>(note.channel), static_cast<std::uint8_t>(note.noteNumber), 0,
                    PreparedMidiEventType::NoteOff};
                if (onSample < request.playbackStartSample && offSample > request.playbackStartSample) {
                    auto chase = noteOn;
                    chase.samplePosition = request.playbackStartSample;
                    initialChaseEvents.push_back({chase, source});
                }
                if (onSample >= request.playbackStartSample) {
                    normalEvents.push_back({noteOn, source});
                }
                if (offSample >= request.playbackStartSample) {
                    normalEvents.push_back({noteOff, source});
                }
                ++noteInstanceId;
            }
        }
    }

    std::stable_sort(normalEvents.begin(), normalEvents.end(), eventSortsBefore);
    std::stable_sort(initialChaseEvents.begin(), initialChaseEvents.end(), sourceSortsBefore);
    std::vector<EventWithSource*> allEvents;
    allEvents.reserve(normalEvents.size() + initialChaseEvents.size());
    const auto appendPointers = [&allEvents](auto& events) {
        for (auto& event : events) {
            allEvents.push_back(&event);
        }
    };
    appendPointers(normalEvents);
    appendPointers(initialChaseEvents);
    std::stable_sort(allEvents.begin(), allEvents.end(), [](const auto* left, const auto* right) {
        return sourceSortsBefore(*left, *right);
    });
    for (std::size_t ordinal = 0; ordinal < allEvents.size(); ++ordinal) {
        allEvents[ordinal]->event.eventOrdinal = static_cast<std::uint32_t>(ordinal);
    }

    for (const auto& event : normalEvents) {
        plan->events.push_back(event.event);
    }
    for (const auto& event : initialChaseEvents) {
        plan->initialChaseNoteOnEvents.push_back(event.event);
    }
    return {PreparedMidiPlaybackPlanBuildFailureReason::None, std::move(plan)};
}

}

}
