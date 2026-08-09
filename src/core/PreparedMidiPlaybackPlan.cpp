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
    const auto scaled = static_cast<long double>(seconds) * static_cast<long double>(sampleRate);
    if (!std::isfinite(seconds) || !std::isfinite(scaled)
        || scaled < static_cast<long double>(std::numeric_limits<std::int64_t>::min())
        || scaled > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
        return false;
    }

    samplePosition = static_cast<std::int64_t>(std::llround(static_cast<double>(scaled)));
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
    std::stop_token stopToken,
    PreparedMidiPlaybackPlanLimits limits)
{
    if (stopToken.stop_requested()) {
        return failure(PreparedMidiPlaybackPlanBuildFailureReason::Cancelled);
    }
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
    if (request.loopRange && (request.loopRange->startTick < 0
            || request.loopRange->endTick <= request.loopRange->startTick)) {
        return failure(PreparedMidiPlaybackPlanBuildFailureReason::InvalidLoopRange);
    }

    auto plan = std::make_unique<PreparedMidiPlaybackPlan>();
    plan->sampleRate = request.sampleRate;
    plan->maximumBlockFrames = request.maximumBlockFrames;
    plan->outputChannelCount = request.outputChannelCount;
    plan->outputChannelMask = request.outputChannelMask;
    plan->playbackStartSample = request.playbackStartSample;

    std::optional<PreparedMidiLoop> preparedLoop;
    std::int64_t loopEndSample = 0;
    if (request.loopRange) {
        std::int64_t loopStartSample = 0;
        if (!tryConvertTickToSample(request.projectSnapshot, request.loopRange->startTick,
                request.sampleRate, loopStartSample)
            || !tryConvertTickToSample(request.projectSnapshot, request.loopRange->endTick,
                request.sampleRate, loopEndSample)) {
            return failure(PreparedMidiPlaybackPlanBuildFailureReason::SamplePositionOverflow);
        }
        if (loopEndSample <= loopStartSample) {
            return failure(PreparedMidiPlaybackPlanBuildFailureReason::InvalidLoopRange);
        }
        preparedLoop = {loopStartSample, loopEndSample - loopStartSample, {}, {}};
    }

    const bool soloModeActive = projectHasSoloedTrack(request.projectSnapshot);
    std::uint32_t slotIndex = 0;
    std::uint32_t noteInstanceId = 0;
    std::vector<EventWithSource> normalEvents;
    std::vector<EventWithSource> initialChaseEvents;
    std::vector<EventWithSource> loopBoundaryEvents;
    std::vector<EventWithSource> loopStartChaseEvents;
    for (std::size_t trackOrder = 0; trackOrder < request.projectSnapshot.tracks().size(); ++trackOrder) {
        if (stopToken.stop_requested()) {
            return failure(PreparedMidiPlaybackPlanBuildFailureReason::Cancelled);
        }
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
            if (stopToken.stop_requested()) {
                return failure(PreparedMidiPlaybackPlanBuildFailureReason::Cancelled);
            }
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
                const bool crossesLoopStart = preparedLoop && onSample < preparedLoop->loopStartSample
                    && offSample > preparedLoop->loopStartSample;
                const bool crossesLoopEnd = preparedLoop && onSample < loopEndSample && offSample > loopEndSample;
                if (offSample < request.playbackStartSample && !crossesLoopStart && !crossesLoopEnd) {
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
                if (crossesLoopEnd) {
                    auto boundaryNoteOff = noteOff;
                    boundaryNoteOff.samplePosition = loopEndSample;
                    loopBoundaryEvents.push_back({boundaryNoteOff, source});
                }
                if (crossesLoopStart) {
                    auto loopChase = noteOn;
                    loopChase.samplePosition = preparedLoop->loopStartSample;
                    loopStartChaseEvents.push_back({loopChase, source});
                }
                ++noteInstanceId;
            }
        }
    }

    if (normalEvents.size() + initialChaseEvents.size() + loopBoundaryEvents.size()
        + loopStartChaseEvents.size() > limits.maximumTotalEvents) {
        return failure(PreparedMidiPlaybackPlanBuildFailureReason::EventLimitExceeded);
    }

    std::stable_sort(normalEvents.begin(), normalEvents.end(), eventSortsBefore);
    std::stable_sort(initialChaseEvents.begin(), initialChaseEvents.end(), sourceSortsBefore);
    std::stable_sort(loopBoundaryEvents.begin(), loopBoundaryEvents.end(), eventSortsBefore);
    std::stable_sort(loopStartChaseEvents.begin(), loopStartChaseEvents.end(), sourceSortsBefore);
    std::vector<EventWithSource*> allEvents;
    allEvents.reserve(normalEvents.size() + initialChaseEvents.size() + loopBoundaryEvents.size()
        + loopStartChaseEvents.size());
    const auto appendPointers = [&allEvents](auto& events) {
        for (auto& event : events) {
            allEvents.push_back(&event);
        }
    };
    appendPointers(normalEvents);
    appendPointers(initialChaseEvents);
    appendPointers(loopBoundaryEvents);
    appendPointers(loopStartChaseEvents);
    std::stable_sort(allEvents.begin(), allEvents.end(), [](const auto* left, const auto* right) {
        return sourceSortsBefore(*left, *right);
    });
    if (allEvents.size() > std::numeric_limits<std::uint32_t>::max()) {
        return failure(PreparedMidiPlaybackPlanBuildFailureReason::EventLimitExceeded);
    }
    for (std::uint32_t ordinal = 0; ordinal < allEvents.size(); ++ordinal) {
        allEvents[ordinal]->event.eventOrdinal = ordinal;
    }

    for (const auto& event : normalEvents) {
        plan->events.push_back(event.event);
    }
    for (const auto& event : initialChaseEvents) {
        plan->initialChaseNoteOnEvents.push_back(event.event);
    }
    if (preparedLoop) {
        for (const auto& event : loopBoundaryEvents) {
            preparedLoop->boundaryNoteOffEvents.push_back(event.event);
        }
        for (const auto& event : loopStartChaseEvents) {
            preparedLoop->startChaseNoteOnEvents.push_back(event.event);
        }
        plan->loop = std::move(preparedLoop);
    }

    std::vector<std::int64_t> callbackPositions;
    callbackPositions.reserve(plan->events.size() + plan->initialChaseNoteOnEvents.size()
        + loopBoundaryEvents.size() + loopStartChaseEvents.size());
    for (const auto& event : plan->events) {
        callbackPositions.push_back(event.samplePosition);
    }
    for (const auto& event : plan->initialChaseNoteOnEvents) {
        callbackPositions.push_back(event.samplePosition);
    }
    if (plan->loop) {
        for (const auto& event : plan->loop->boundaryNoteOffEvents) {
            callbackPositions.push_back(event.samplePosition);
        }
        for (const auto& event : plan->loop->startChaseNoteOnEvents) {
            callbackPositions.push_back(event.samplePosition);
        }
    }
    std::sort(callbackPositions.begin(), callbackPositions.end());
    std::size_t callbackEndIndex = 0;
    for (std::size_t index = 0; index < callbackPositions.size(); ++index) {
        callbackEndIndex = std::max(callbackEndIndex, index);
        const auto callbackEnd = static_cast<long double>(callbackPositions[index])
            + static_cast<long double>(request.maximumBlockFrames);
        while (callbackEndIndex < callbackPositions.size()
            && static_cast<long double>(callbackPositions[callbackEndIndex]) < callbackEnd) {
            ++callbackEndIndex;
        }
        if (callbackEndIndex - index > limits.maximumCallbackEvents) {
            return failure(PreparedMidiPlaybackPlanBuildFailureReason::CallbackEventLimitExceeded);
        }
    }
    return {PreparedMidiPlaybackPlanBuildFailureReason::None, std::move(plan)};
}

}

}
