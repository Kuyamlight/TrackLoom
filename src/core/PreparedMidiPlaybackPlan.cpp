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
        std::move(request),
        stopToken,
        detail::PreparedMidiPlaybackPlanLimits {1'000'000, 4'096});
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

    auto plan = std::make_unique<PreparedMidiPlaybackPlan>();
    plan->sampleRate = request.sampleRate;
    plan->maximumBlockFrames = request.maximumBlockFrames;
    plan->outputChannelCount = request.outputChannelCount;
    plan->outputChannelMask = request.outputChannelMask;
    plan->playbackStartSample = request.playbackStartSample;
    if (request.loopRange) {
        if (request.loopRange->startTick < 0
            || request.loopRange->endTick <= request.loopRange->startTick) {
            return failure(PreparedMidiPlaybackPlanBuildFailureReason::InvalidLoopRange);
        }

        std::int64_t loopStartSample = 0;
        std::int64_t loopEndSample = 0;
        if (!tryConvertTickToSample(request.projectSnapshot, request.loopRange->startTick,
                request.sampleRate, loopStartSample)
            || !tryConvertTickToSample(request.projectSnapshot, request.loopRange->endTick,
                request.sampleRate, loopEndSample)) {
            return failure(PreparedMidiPlaybackPlanBuildFailureReason::SamplePositionOverflow);
        }
        if (loopEndSample <= loopStartSample) {
            return failure(PreparedMidiPlaybackPlanBuildFailureReason::InvalidLoopRange);
        }

        const auto loopLengthSamples = loopEndSample - loopStartSample;
        auto relativeStart = (request.playbackStartSample - loopStartSample) % loopLengthSamples;
        if (relativeStart < 0) {
            relativeStart += loopLengthSamples;
        }
        plan->playbackStartSample = loopStartSample + relativeStart;
        plan->loop = PreparedMidiLoop{loopStartSample, loopLengthSamples, {}, {}};
    }

    const bool soloModeActive = projectHasSoloedTrack(request.projectSnapshot);
    std::uint32_t slotIndex = 0;
    std::uint32_t noteInstanceId = 0;
    std::vector<EventWithSource> normalEvents;
    std::vector<EventWithSource> initialChaseEvents;
    std::vector<EventWithSource> boundaryNoteOffEvents;
    std::vector<EventWithSource> startChaseNoteOnEvents;
    std::size_t totalEventCount = 0;
    const auto appendEvent = [&totalEventCount, &limits](
                                 std::vector<EventWithSource>& destination,
                                 EventWithSource event) {
        if (totalEventCount >= limits.maximumTotalEvents) {
            return false;
        }
        destination.push_back(std::move(event));
        ++totalEventCount;
        return true;
    };
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
                if (stopToken.stop_requested()) {
                    return failure(PreparedMidiPlaybackPlanBuildFailureReason::Cancelled);
                }
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
                if (!plan->loop && offSample < request.playbackStartSample) {
                    continue;
                }

                const SourceOrder source{trackOrder, clipOrder, noteOrder};
                const PreparedMidiEvent noteOn{onSample, noteInstanceId, 0, trackSlotIndex,
                    static_cast<std::uint8_t>(note.channel), static_cast<std::uint8_t>(note.noteNumber),
                    static_cast<std::uint8_t>(note.velocity), PreparedMidiEventType::NoteOn};
                const PreparedMidiEvent noteOff{offSample, noteInstanceId, 0, trackSlotIndex,
                    static_cast<std::uint8_t>(note.channel), static_cast<std::uint8_t>(note.noteNumber), 0,
                    PreparedMidiEventType::NoteOff};
                if (plan->loop) {
                    const auto loopStartSample = plan->loop->loopStartSample;
                    const auto loopEndSample = loopStartSample + plan->loop->loopLengthSamples;
                    bool noteEmitted = false;
                    if (onSample >= loopStartSample && onSample < loopEndSample) {
                        auto relativeNoteOn = noteOn;
                        relativeNoteOn.samplePosition -= loopStartSample;
                        if (!appendEvent(normalEvents, {relativeNoteOn, source})) {
                            return failure(PreparedMidiPlaybackPlanBuildFailureReason::EventLimitExceeded);
                        }
                        noteEmitted = true;
                    }
                    if (offSample >= loopStartSample && offSample < loopEndSample) {
                        auto relativeNoteOff = noteOff;
                        relativeNoteOff.samplePosition -= loopStartSample;
                        if (!appendEvent(normalEvents, {relativeNoteOff, source})) {
                            return failure(PreparedMidiPlaybackPlanBuildFailureReason::EventLimitExceeded);
                        }
                        noteEmitted = true;
                    }
                    if (onSample < loopEndSample && offSample >= loopEndSample) {
                        auto boundaryNoteOff = noteOff;
                        boundaryNoteOff.samplePosition = plan->loop->loopLengthSamples;
                        if (!appendEvent(boundaryNoteOffEvents, {boundaryNoteOff, source})) {
                            return failure(PreparedMidiPlaybackPlanBuildFailureReason::EventLimitExceeded);
                        }
                        noteEmitted = true;
                    }
                    if (onSample < loopStartSample && offSample > loopStartSample) {
                        auto startChase = noteOn;
                        startChase.samplePosition = 0;
                        if (!appendEvent(startChaseNoteOnEvents, {startChase, source})) {
                            return failure(PreparedMidiPlaybackPlanBuildFailureReason::EventLimitExceeded);
                        }
                        noteEmitted = true;
                    }
                    if (plan->playbackStartSample > loopStartSample
                        && onSample < plan->playbackStartSample
                        && offSample > plan->playbackStartSample) {
                        auto initialChase = noteOn;
                        initialChase.samplePosition = plan->playbackStartSample - loopStartSample;
                        if (!appendEvent(initialChaseEvents, {initialChase, source})) {
                            return failure(PreparedMidiPlaybackPlanBuildFailureReason::EventLimitExceeded);
                        }
                        noteEmitted = true;
                    }
                    if (noteEmitted) {
                        ++noteInstanceId;
                    }
                    continue;
                }
                if (onSample < request.playbackStartSample && offSample > request.playbackStartSample) {
                    auto chase = noteOn;
                    chase.samplePosition = request.playbackStartSample;
                    if (!appendEvent(initialChaseEvents, {chase, source})) {
                        return failure(PreparedMidiPlaybackPlanBuildFailureReason::EventLimitExceeded);
                    }
                }
                if (onSample >= request.playbackStartSample) {
                    if (!appendEvent(normalEvents, {noteOn, source})) {
                        return failure(PreparedMidiPlaybackPlanBuildFailureReason::EventLimitExceeded);
                    }
                }
                if (offSample >= request.playbackStartSample) {
                    if (!appendEvent(normalEvents, {noteOff, source})) {
                        return failure(PreparedMidiPlaybackPlanBuildFailureReason::EventLimitExceeded);
                    }
                }
                ++noteInstanceId;
            }
        }
    }

    if (stopToken.stop_requested()) {
        return failure(PreparedMidiPlaybackPlanBuildFailureReason::Cancelled);
    }
    std::stable_sort(normalEvents.begin(), normalEvents.end(), eventSortsBefore);
    std::stable_sort(initialChaseEvents.begin(), initialChaseEvents.end(), sourceSortsBefore);
    std::stable_sort(boundaryNoteOffEvents.begin(), boundaryNoteOffEvents.end(), sourceSortsBefore);
    std::stable_sort(startChaseNoteOnEvents.begin(), startChaseNoteOnEvents.end(), sourceSortsBefore);
    if (!plan->loop) {
        if (stopToken.stop_requested()) {
            return failure(PreparedMidiPlaybackPlanBuildFailureReason::Cancelled);
        }
        std::vector<std::int64_t> callbackSamplePositions;
        callbackSamplePositions.reserve(normalEvents.size() + initialChaseEvents.size());
        for (const auto& event : normalEvents) {
            callbackSamplePositions.push_back(event.event.samplePosition);
        }
        for (const auto& event : initialChaseEvents) {
            callbackSamplePositions.push_back(event.event.samplePosition);
        }
        std::sort(callbackSamplePositions.begin(), callbackSamplePositions.end());

        std::size_t windowEnd = 0;
        for (std::size_t windowStart = 0; windowStart < callbackSamplePositions.size(); ++windowStart) {
            windowEnd = std::max(windowEnd, windowStart);
            while (windowEnd < callbackSamplePositions.size()
                && callbackSamplePositions[windowEnd] - callbackSamplePositions[windowStart]
                    < request.maximumBlockFrames) {
                ++windowEnd;
            }
            if (windowEnd - windowStart > limits.maximumCallbackEvents) {
                return failure(PreparedMidiPlaybackPlanBuildFailureReason::CallbackEventLimitExceeded);
            }
        }
    } else {
        if (stopToken.stop_requested()) {
            return failure(PreparedMidiPlaybackPlanBuildFailureReason::Cancelled);
        }
        if (initialChaseEvents.size() > limits.maximumCallbackEvents) {
            return failure(PreparedMidiPlaybackPlanBuildFailureReason::CallbackEventLimitExceeded);
        }
        std::vector<std::uint64_t> periodicSamplePositions;
        periodicSamplePositions.reserve(normalEvents.size()
            + boundaryNoteOffEvents.size() + startChaseNoteOnEvents.size());
        for (const auto& event : normalEvents) {
            periodicSamplePositions.push_back(static_cast<std::uint64_t>(event.event.samplePosition));
        }
        for (const auto& event : boundaryNoteOffEvents) {
            periodicSamplePositions.push_back(0);
        }
        for (const auto& event : startChaseNoteOnEvents) {
            periodicSamplePositions.push_back(0);
        }
        std::sort(periodicSamplePositions.begin(), periodicSamplePositions.end());

        const auto loopLength = static_cast<std::uint64_t>(plan->loop->loopLengthSamples);
        const auto blockLength = static_cast<std::uint64_t>(request.maximumBlockFrames);
        const auto fullRoundCount = blockLength / loopLength;
        const auto remainderLength = blockLength % loopLength;
        const auto perRoundEventCount = periodicSamplePositions.size();
        if (perRoundEventCount != 0
            && fullRoundCount > limits.maximumCallbackEvents / perRoundEventCount) {
            return failure(PreparedMidiPlaybackPlanBuildFailureReason::CallbackEventLimitExceeded);
        }
        const auto fullRoundEventCount = static_cast<std::size_t>(fullRoundCount) * perRoundEventCount;

        std::size_t maximumRemainderEventCount = 0;
        if (remainderLength != 0 && !periodicSamplePositions.empty()) {
            std::vector<std::uint64_t> doubledPositions;
            doubledPositions.reserve(periodicSamplePositions.size() * 2);
            doubledPositions.insert(doubledPositions.end(),
                periodicSamplePositions.begin(), periodicSamplePositions.end());
            for (const auto position : periodicSamplePositions) {
                doubledPositions.push_back(position + loopLength);
            }

            std::size_t windowEnd = 0;
            for (std::size_t windowStart = 0;
                 windowStart < periodicSamplePositions.size(); ++windowStart) {
                windowEnd = std::max(windowEnd, windowStart);
                const auto maximumWindowEnd = windowStart + periodicSamplePositions.size();
                while (windowEnd < maximumWindowEnd
                    && doubledPositions[windowEnd] - doubledPositions[windowStart] < remainderLength) {
                    ++windowEnd;
                }
                maximumRemainderEventCount = std::max(
                    maximumRemainderEventCount, windowEnd - windowStart);
            }
        }
        if (maximumRemainderEventCount
            > limits.maximumCallbackEvents - fullRoundEventCount) {
            return failure(PreparedMidiPlaybackPlanBuildFailureReason::CallbackEventLimitExceeded);
        }

        auto firstCallbackEventCount = initialChaseEvents.size();
        const auto addFirstCallbackEvents = [&firstCallbackEventCount, &limits](std::size_t count) {
            if (count > limits.maximumCallbackEvents - firstCallbackEventCount) {
                return false;
            }
            firstCallbackEventCount += count;
            return true;
        };
        const auto relativePlaybackStart = static_cast<std::uint64_t>(
            plan->playbackStartSample - plan->loop->loopStartSample);
        const auto framesUntilFirstBoundary = loopLength - relativePlaybackStart;
        std::size_t firstSegmentNormalEventCount = 0;
        const auto firstSegmentEnd = relativePlaybackStart
            + std::min(blockLength, framesUntilFirstBoundary);
        for (const auto& event : normalEvents) {
            const auto position = static_cast<std::uint64_t>(event.event.samplePosition);
            if (position >= relativePlaybackStart && position < firstSegmentEnd) {
                ++firstSegmentNormalEventCount;
            }
        }
        if (!addFirstCallbackEvents(firstSegmentNormalEventCount)) {
            return failure(PreparedMidiPlaybackPlanBuildFailureReason::CallbackEventLimitExceeded);
        }
        if (relativePlaybackStart == 0
            && !addFirstCallbackEvents(startChaseNoteOnEvents.size())) {
            return failure(PreparedMidiPlaybackPlanBuildFailureReason::CallbackEventLimitExceeded);
        }
        if (blockLength > framesUntilFirstBoundary) {
            const auto framesAfterFirstBoundary = blockLength - framesUntilFirstBoundary;
            const auto completePeriodicRounds = framesAfterFirstBoundary / loopLength;
            const auto periodicRemainder = framesAfterFirstBoundary % loopLength;
            if (perRoundEventCount != 0
                && completePeriodicRounds
                    > (limits.maximumCallbackEvents - firstCallbackEventCount) / perRoundEventCount) {
                return failure(PreparedMidiPlaybackPlanBuildFailureReason::CallbackEventLimitExceeded);
            }
            if (!addFirstCallbackEvents(
                    static_cast<std::size_t>(completePeriodicRounds) * perRoundEventCount)) {
                return failure(PreparedMidiPlaybackPlanBuildFailureReason::CallbackEventLimitExceeded);
            }
            const auto periodicRemainderEventCount = static_cast<std::size_t>(
                std::lower_bound(periodicSamplePositions.begin(), periodicSamplePositions.end(),
                    periodicRemainder)
                - periodicSamplePositions.begin());
            if (periodicRemainder != 0
                && !addFirstCallbackEvents(periodicRemainderEventCount)) {
                return failure(PreparedMidiPlaybackPlanBuildFailureReason::CallbackEventLimitExceeded);
            }
        }
    }
    if (stopToken.stop_requested()) {
        return failure(PreparedMidiPlaybackPlanBuildFailureReason::Cancelled);
    }
    std::vector<EventWithSource*> allEvents;
    allEvents.reserve(normalEvents.size() + initialChaseEvents.size()
        + boundaryNoteOffEvents.size() + startChaseNoteOnEvents.size());
    const auto appendPointers = [&allEvents](auto& events) {
        for (auto& event : events) {
            allEvents.push_back(&event);
        }
    };
    appendPointers(normalEvents);
    appendPointers(initialChaseEvents);
    appendPointers(boundaryNoteOffEvents);
    appendPointers(startChaseNoteOnEvents);
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
    if (plan->loop) {
        for (const auto& event : boundaryNoteOffEvents) {
            plan->loop->boundaryNoteOffEvents.push_back(event.event);
        }
        for (const auto& event : startChaseNoteOnEvents) {
            plan->loop->startChaseNoteOnEvents.push_back(event.event);
        }
    }
    return {PreparedMidiPlaybackPlanBuildFailureReason::None, std::move(plan)};
}

}

}
