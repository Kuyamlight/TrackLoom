#include "PreparedMidiPlaybackRuntime.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <tuple>
#include <vector>

namespace trackloom {
namespace {

static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::int64_t>::is_always_lock_free);

constexpr std::size_t maximumTotalEvents = 1'000'000;
constexpr std::size_t maximumCallbackEvents = 4'096;

bool validOutputFormat(int channelCount, std::uint64_t channelMask) noexcept
{
    return channelCount >= 1 && channelCount <= 2
        && channelMask == ((std::uint64_t {1} << channelCount) - 1);
}

bool validEventPayload(const PreparedMidiEvent& event) noexcept
{
    if (event.channel < 1 || event.channel > 16 || event.noteNumber > 127) {
        return false;
    }
    switch (event.type) {
    case PreparedMidiEventType::NoteOn:
        return event.velocity >= 1 && event.velocity <= 127;
    case PreparedMidiEventType::NoteOff:
        return event.velocity == 0;
    }
    return false;
}

bool eventsStrictlyOrdered(const std::vector<PreparedMidiEvent>& events) noexcept
{
    for (std::size_t index = 1; index < events.size(); ++index) {
        const auto& previous = events[index - 1];
        const auto& current = events[index];
        if (previous.samplePosition > current.samplePosition
            || (previous.samplePosition == current.samplePosition
                && previous.eventOrdinal >= current.eventOrdinal)) {
            return false;
        }
    }
    return true;
}

bool addEventCount(std::size_t& total, std::size_t count) noexcept
{
    if (count > maximumTotalEvents - total) {
        return false;
    }
    total += count;
    return true;
}

PreparedMidiPlaybackPlanValidationResult validationFailure(
    PreparedMidiPlaybackPlanValidationFailureReason reason) noexcept
{
    return {false, reason};
}

void clearOutputs(float* const* outputs, int channels, int frames) noexcept
{
    if (outputs == nullptr || channels <= 0 || frames <= 0) {
        return;
    }
    for (int channel = 0; channel < channels; ++channel) {
        if (outputs[channel] != nullptr) {
            std::fill_n(outputs[channel], frames, 0.0f);
        }
    }
}

void updateMaximum(std::atomic<std::uint64_t>& destination, std::uint64_t value) noexcept
{
    auto observed = destination.load(std::memory_order_relaxed);
    while (observed < value
        && !destination.compare_exchange_weak(
            observed, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

}

PreparedMidiPlaybackPlanValidationResult validatePreparedMidiPlaybackPlan(
    const PreparedMidiPlaybackPlan& plan)
{
    if (!std::isfinite(plan.sampleRate) || plan.sampleRate <= 0.0
        || plan.maximumBlockFrames <= 0
        || !validOutputFormat(plan.outputChannelCount, plan.outputChannelMask)
        || plan.playbackStartSample < 0
        || plan.playbackStartSample
            > std::numeric_limits<std::int64_t>::max() - plan.maximumBlockFrames) {
        return validationFailure(
            PreparedMidiPlaybackPlanValidationFailureReason::InvalidFormat);
    }
    for (const auto& slot : plan.instrumentSlots) {
        if (slot.kind != PreparedMidiInstrumentKind::BuiltInSine
            || !std::isfinite(slot.gain) || slot.gain < 0.0f
            || !std::isfinite(slot.pan) || slot.pan < -1.0f || slot.pan > 1.0f
            || slot.outputBusIndex != 0) {
            return validationFailure(
                PreparedMidiPlaybackPlanValidationFailureReason::InvalidFormat);
        }
    }

    std::size_t totalEventCount = 0;
    if (!addEventCount(totalEventCount, plan.events.size())
        || !addEventCount(totalEventCount, plan.initialChaseNoteOnEvents.size())
        || (plan.loop
            && (!addEventCount(totalEventCount, plan.loop->boundaryNoteOffEvents.size())
                || !addEventCount(totalEventCount, plan.loop->startChaseNoteOnEvents.size())))) {
        return validationFailure(
            PreparedMidiPlaybackPlanValidationFailureReason::EventLimitExceeded);
    }

    std::int64_t loopEnd = 0;
    if (plan.loop) {
        if (plan.loop->loopStartSample < 0 || plan.loop->loopLengthSamples <= 0
            || plan.loop->loopStartSample
                > std::numeric_limits<std::int64_t>::max()
                    - plan.loop->loopLengthSamples) {
            return validationFailure(
                PreparedMidiPlaybackPlanValidationFailureReason::InvalidLoopTable);
        }
        loopEnd = plan.loop->loopStartSample + plan.loop->loopLengthSamples;
        if (plan.playbackStartSample < plan.loop->loopStartSample
            || plan.playbackStartSample >= loopEnd) {
            return validationFailure(
                PreparedMidiPlaybackPlanValidationFailureReason::InvalidLoopTable);
        }
    }

    const auto forEveryTable = [&plan](const auto& visitor) {
        for (const auto& event : plan.events) {
            if (!visitor(event)) {
                return false;
            }
        }
        for (const auto& event : plan.initialChaseNoteOnEvents) {
            if (!visitor(event)) {
                return false;
            }
        }
        if (plan.loop) {
            for (const auto& event : plan.loop->boundaryNoteOffEvents) {
                if (!visitor(event)) {
                    return false;
                }
            }
            for (const auto& event : plan.loop->startChaseNoteOnEvents) {
                if (!visitor(event)) {
                    return false;
                }
            }
        }
        return true;
    };
    if (!forEveryTable([&plan](const PreparedMidiEvent& event) {
            return event.instrumentSlotIndex < plan.instrumentSlots.size();
        })) {
        return validationFailure(
            PreparedMidiPlaybackPlanValidationFailureReason::InvalidInstrumentSlot);
    }

    for (const auto& event : plan.events) {
        const bool validCoordinate = plan.loop
            ? event.samplePosition >= 0
                && event.samplePosition < plan.loop->loopLengthSamples
            : event.samplePosition >= plan.playbackStartSample;
        if (!validCoordinate || !validEventPayload(event)) {
            return validationFailure(
                PreparedMidiPlaybackPlanValidationFailureReason::InvalidEventValue);
        }
    }
    const auto initialChaseSample = plan.loop
        ? plan.playbackStartSample - plan.loop->loopStartSample
        : plan.playbackStartSample;
    for (const auto& event : plan.initialChaseNoteOnEvents) {
        if (event.type != PreparedMidiEventType::NoteOn
            || event.samplePosition != initialChaseSample
            || !validEventPayload(event)) {
            return validationFailure(
                PreparedMidiPlaybackPlanValidationFailureReason::InvalidLoopTable);
        }
    }
    if (plan.loop) {
        for (const auto& event : plan.loop->boundaryNoteOffEvents) {
            if (event.type != PreparedMidiEventType::NoteOff
                || event.samplePosition != plan.loop->loopLengthSamples
                || !validEventPayload(event)) {
                return validationFailure(
                    PreparedMidiPlaybackPlanValidationFailureReason::InvalidLoopTable);
            }
        }
        for (const auto& event : plan.loop->startChaseNoteOnEvents) {
            if (event.type != PreparedMidiEventType::NoteOn
                || event.samplePosition != 0
                || !validEventPayload(event)) {
                return validationFailure(
                    PreparedMidiPlaybackPlanValidationFailureReason::InvalidLoopTable);
            }
        }
    }

    try {
        std::vector<std::uint8_t> ordinals(totalEventCount, 0);
        bool invalidOrdinal = false;
        forEveryTable([&](const PreparedMidiEvent& event) {
            if (event.eventOrdinal >= totalEventCount
                || ordinals[event.eventOrdinal] != 0) {
                invalidOrdinal = true;
                return false;
            }
            ordinals[event.eventOrdinal] = 1;
            return true;
        });
        if (invalidOrdinal
            || std::any_of(ordinals.begin(), ordinals.end(), [](std::uint8_t seen) {
                return seen == 0;
            })) {
            return validationFailure(
                PreparedMidiPlaybackPlanValidationFailureReason::InvalidEventOrdinal);
        }

        if (!eventsStrictlyOrdered(plan.events)
            || !eventsStrictlyOrdered(plan.initialChaseNoteOnEvents)
            || (plan.loop
                && (!eventsStrictlyOrdered(plan.loop->boundaryNoteOffEvents)
                    || !eventsStrictlyOrdered(plan.loop->startChaseNoteOnEvents)))) {
            return validationFailure(
                PreparedMidiPlaybackPlanValidationFailureReason::UnsortedEvents);
        }

        if (!plan.loop) {
            std::vector<std::int64_t> positions;
            positions.reserve(plan.events.size() + plan.initialChaseNoteOnEvents.size());
            for (const auto& event : plan.events) {
                positions.push_back(event.samplePosition);
            }
            for (const auto& event : plan.initialChaseNoteOnEvents) {
                positions.push_back(event.samplePosition);
            }
            std::sort(positions.begin(), positions.end());
            std::size_t windowEnd = 0;
            for (std::size_t windowStart = 0; windowStart < positions.size(); ++windowStart) {
                windowEnd = std::max(windowEnd, windowStart);
                while (windowEnd < positions.size()
                    && positions[windowEnd] - positions[windowStart]
                        < plan.maximumBlockFrames) {
                    ++windowEnd;
                }
                if (windowEnd - windowStart > maximumCallbackEvents) {
                    return validationFailure(
                        PreparedMidiPlaybackPlanValidationFailureReason::CallbackEventLimitExceeded);
                }
            }
        } else {
            if (plan.initialChaseNoteOnEvents.size() > maximumCallbackEvents) {
                return validationFailure(
                    PreparedMidiPlaybackPlanValidationFailureReason::CallbackEventLimitExceeded);
            }
            std::vector<std::uint64_t> periodicPositions;
            periodicPositions.reserve(plan.events.size()
                + plan.loop->boundaryNoteOffEvents.size()
                + plan.loop->startChaseNoteOnEvents.size());
            for (const auto& event : plan.events) {
                periodicPositions.push_back(static_cast<std::uint64_t>(event.samplePosition));
            }
            for (std::size_t index = 0;
                 index < plan.loop->boundaryNoteOffEvents.size(); ++index) {
                periodicPositions.push_back(0);
            }
            for (std::size_t index = 0;
                 index < plan.loop->startChaseNoteOnEvents.size(); ++index) {
                periodicPositions.push_back(0);
            }
            std::sort(periodicPositions.begin(), periodicPositions.end());
            const auto loopLength = static_cast<std::uint64_t>(
                plan.loop->loopLengthSamples);
            const auto blockLength = static_cast<std::uint64_t>(
                plan.maximumBlockFrames);
            const auto fullRoundCount = blockLength / loopLength;
            const auto remainderLength = blockLength % loopLength;
            const auto perRoundEventCount = periodicPositions.size();
            if (perRoundEventCount != 0
                && fullRoundCount > maximumCallbackEvents / perRoundEventCount) {
                return validationFailure(
                    PreparedMidiPlaybackPlanValidationFailureReason::CallbackEventLimitExceeded);
            }
            const auto fullRoundEventCount = static_cast<std::size_t>(fullRoundCount)
                * perRoundEventCount;
            std::size_t maximumRemainderEventCount = 0;
            if (remainderLength != 0 && !periodicPositions.empty()) {
                std::vector<std::uint64_t> doubledPositions;
                doubledPositions.reserve(periodicPositions.size() * 2);
                doubledPositions.insert(doubledPositions.end(),
                    periodicPositions.begin(), periodicPositions.end());
                for (const auto position : periodicPositions) {
                    doubledPositions.push_back(position + loopLength);
                }
                std::size_t windowEnd = 0;
                for (std::size_t windowStart = 0;
                     windowStart < periodicPositions.size(); ++windowStart) {
                    windowEnd = std::max(windowEnd, windowStart);
                    const auto maximumWindowEnd = windowStart + periodicPositions.size();
                    while (windowEnd < maximumWindowEnd
                        && doubledPositions[windowEnd] - doubledPositions[windowStart]
                            < remainderLength) {
                        ++windowEnd;
                    }
                    maximumRemainderEventCount = std::max(
                        maximumRemainderEventCount, windowEnd - windowStart);
                }
            }
            if (maximumRemainderEventCount
                > maximumCallbackEvents - fullRoundEventCount) {
                return validationFailure(
                    PreparedMidiPlaybackPlanValidationFailureReason::CallbackEventLimitExceeded);
            }

            auto firstCallbackEventCount = plan.initialChaseNoteOnEvents.size();
            const auto addFirstCallbackEvents = [&firstCallbackEventCount](std::size_t count) {
                if (count > maximumCallbackEvents - firstCallbackEventCount) {
                    return false;
                }
                firstCallbackEventCount += count;
                return true;
            };
            const auto relativePlaybackStart = static_cast<std::uint64_t>(
                plan.playbackStartSample - plan.loop->loopStartSample);
            const auto framesUntilFirstBoundary = loopLength - relativePlaybackStart;
            const auto firstSegmentEnd = relativePlaybackStart
                + std::min(blockLength, framesUntilFirstBoundary);
            std::size_t firstSegmentNormalEventCount = 0;
            for (const auto& event : plan.events) {
                const auto position = static_cast<std::uint64_t>(event.samplePosition);
                if (position >= relativePlaybackStart && position < firstSegmentEnd) {
                    ++firstSegmentNormalEventCount;
                }
            }
            if (!addFirstCallbackEvents(firstSegmentNormalEventCount)
                || (relativePlaybackStart == 0
                    && !addFirstCallbackEvents(
                        plan.loop->startChaseNoteOnEvents.size()))) {
                return validationFailure(
                    PreparedMidiPlaybackPlanValidationFailureReason::CallbackEventLimitExceeded);
            }
            if (blockLength > framesUntilFirstBoundary) {
                const auto framesAfterFirstBoundary = blockLength - framesUntilFirstBoundary;
                const auto completePeriodicRounds = framesAfterFirstBoundary / loopLength;
                const auto periodicRemainder = framesAfterFirstBoundary % loopLength;
                if (perRoundEventCount != 0
                    && completePeriodicRounds
                        > (maximumCallbackEvents - firstCallbackEventCount)
                            / perRoundEventCount) {
                    return validationFailure(
                        PreparedMidiPlaybackPlanValidationFailureReason::CallbackEventLimitExceeded);
                }
                if (!addFirstCallbackEvents(
                        static_cast<std::size_t>(completePeriodicRounds)
                            * perRoundEventCount)) {
                    return validationFailure(
                        PreparedMidiPlaybackPlanValidationFailureReason::CallbackEventLimitExceeded);
                }
                const auto remainderEventCount = static_cast<std::size_t>(
                    std::lower_bound(periodicPositions.begin(), periodicPositions.end(),
                        periodicRemainder)
                    - periodicPositions.begin());
                if (periodicRemainder != 0
                    && !addFirstCallbackEvents(remainderEventCount)) {
                    return validationFailure(
                        PreparedMidiPlaybackPlanValidationFailureReason::CallbackEventLimitExceeded);
                }
            }
        }
    } catch (const std::bad_alloc&) {
        return validationFailure(
            PreparedMidiPlaybackPlanValidationFailureReason::ValidationResourceUnavailable);
    }
    return {true, PreparedMidiPlaybackPlanValidationFailureReason::None};
}

bool PreparedMidiPlaybackRuntime::installPlan(const PreparedMidiPlaybackPlan* plan)
{
    if (plan == nullptr
        || state_.load(std::memory_order_acquire)
            != static_cast<std::uint32_t>(RealtimePlaybackState::Stopped)) {
        return false;
    }
    if (!validatePreparedMidiPlaybackPlan(*plan).valid) {
        return false;
    }
    BuiltInPolySynth preparedSynth;
    if (!preparedSynth.prepare(plan->sampleRate)) {
        return false;
    }
    plan_ = plan;
    synth_ = preparedSynth;
    const auto firstEventSample = plan_->loop
        ? plan_->playbackStartSample - plan_->loop->loopStartSample
        : plan_->playbackStartSample;
    eventIndex_ = static_cast<std::size_t>(std::lower_bound(
        plan_->events.begin(), plan_->events.end(), firstEventSample,
        [](const PreparedMidiEvent& event, std::int64_t samplePosition) {
            return event.samplePosition < samplePosition;
        }) - plan_->events.begin());
    initialChaseApplied_ = false;
    boundaryTransitionPending_ = false;
    loopHeadChasePending_ = plan_->loop
        && plan_->playbackStartSample == plan_->loop->loopStartSample;
    stopReleaseStarted_ = false;
    projectSamplePositionCursor_ = plan_->playbackStartSample;
    loopIterationCursor_ = 0;
    lastError_.store(static_cast<std::uint32_t>(RealtimeAudioError::None),
        std::memory_order_release);
    callbackCount_.store(0, std::memory_order_relaxed);
    callbackTimeoutCount_.store(0, std::memory_order_relaxed);
    callbackExceptionCount_.store(0, std::memory_order_relaxed);
    oversizedBlockCount_.store(0, std::memory_order_relaxed);
    voiceStealCount_.store(0, std::memory_order_relaxed);
    staleNoteOffCount_.store(0, std::memory_order_relaxed);
    largestObservedBlockFrames_.store(0, std::memory_order_relaxed);
    renderedSampleCount_.store(0, std::memory_order_relaxed);
    loopIteration_.store(0, std::memory_order_relaxed);
    projectSamplePosition_.store(plan_->playbackStartSample, std::memory_order_release);
    return true;
}

bool PreparedMidiPlaybackRuntime::start() noexcept
{
    if (plan_ == nullptr) {
        return false;
    }
    auto expected = static_cast<std::uint32_t>(RealtimePlaybackState::Stopped);
    return state_.compare_exchange_strong(expected,
        static_cast<std::uint32_t>(RealtimePlaybackState::Playing),
        std::memory_order_acq_rel, std::memory_order_acquire);
}

bool PreparedMidiPlaybackRuntime::requestStop() noexcept
{
    auto expected = static_cast<std::uint32_t>(RealtimePlaybackState::Playing);
    return state_.compare_exchange_strong(expected,
        static_cast<std::uint32_t>(RealtimePlaybackState::Stopping),
        std::memory_order_acq_rel, std::memory_order_acquire);
}

void PreparedMidiPlaybackRuntime::hardReset(RealtimeAudioError error) noexcept
{
    synth_.reset();
    plan_ = nullptr;
    eventIndex_ = 0;
    initialChaseApplied_ = false;
    boundaryTransitionPending_ = false;
    loopHeadChasePending_ = false;
    stopReleaseStarted_ = false;
    projectSamplePositionCursor_ = 0;
    loopIterationCursor_ = 0;
    projectSamplePosition_.store(0, std::memory_order_release);
    loopIteration_.store(0, std::memory_order_release);
    lastError_.store(static_cast<std::uint32_t>(error), std::memory_order_release);
    state_.store(static_cast<std::uint32_t>(error == RealtimeAudioError::None
            ? RealtimePlaybackState::Stopped
            : RealtimePlaybackState::Faulted),
        std::memory_order_release);
}

void PreparedMidiPlaybackRuntime::recordCallbackTimeout() noexcept
{
    callbackTimeoutCount_.fetch_add(1, std::memory_order_relaxed);
}

void PreparedMidiPlaybackRuntime::recordCallbackException() noexcept
{
    callbackExceptionCount_.fetch_add(1, std::memory_order_relaxed);
    synth_.reset();
    lastError_.store(static_cast<std::uint32_t>(RealtimeAudioError::CallbackException),
        std::memory_order_release);
    state_.store(static_cast<std::uint32_t>(RealtimePlaybackState::Faulted),
        std::memory_order_release);
}

void PreparedMidiPlaybackRuntime::processBlock(
    float* const* outputs,
    int channels,
    int frames)
{
    clearOutputs(outputs, channels, frames);
    callbackCount_.fetch_add(1, std::memory_order_relaxed);
    if (frames > 0) {
        updateMaximum(largestObservedBlockFrames_, static_cast<std::uint64_t>(frames));
    }
    const auto callbackState = static_cast<RealtimePlaybackState>(
        state_.load(std::memory_order_acquire));
    if ((callbackState != RealtimePlaybackState::Playing
            && callbackState != RealtimePlaybackState::Stopping)
        || plan_ == nullptr) {
        return;
    }
    const auto fault = [this](RealtimeAudioError error) noexcept {
        lastError_.store(static_cast<std::uint32_t>(error), std::memory_order_release);
        state_.store(static_cast<std::uint32_t>(RealtimePlaybackState::Faulted),
            std::memory_order_release);
    };
    if (frames <= 0 || channels != plan_->outputChannelCount
        || (channels != 1 && channels != 2)) {
        fault(RealtimeAudioError::InvalidFormat);
        return;
    }
    if (frames > plan_->maximumBlockFrames) {
        oversizedBlockCount_.fetch_add(1, std::memory_order_relaxed);
        fault(RealtimeAudioError::OversizedBlock);
        return;
    }
    if (!plan_->loop
        && projectSamplePositionCursor_
            > std::numeric_limits<std::int64_t>::max() - frames) {
        fault(RealtimeAudioError::InvalidFormat);
        return;
    }

    const auto renderSegment = [this, outputs, channels](int startFrame, int frameCount) noexcept {
        if (frameCount <= 0) {
            return;
        }
        bool allChannelsAvailable = outputs != nullptr;
        if (allChannelsAvailable) {
            for (int channel = 0; channel < channels; ++channel) {
                if (outputs[channel] == nullptr) {
                    allChannelsAvailable = false;
                    break;
                }
            }
        }
        if (allChannelsAvailable) {
            synth_.render(outputs, channels, startFrame, frameCount);
            return;
        }
        for (int frame = startFrame; frame < startFrame + frameCount; ++frame) {
            float scratch[2] {0.0f, 0.0f};
            float* frameOutputs[2] {scratch, scratch + 1};
            if (outputs != nullptr) {
                for (int channel = 0; channel < channels; ++channel) {
                    if (outputs[channel] != nullptr) {
                        frameOutputs[channel] = outputs[channel] + frame;
                    }
                }
            }
            synth_.render(frameOutputs, channels, 0, 1);
        }
    };

    const auto advancePlaybackPosition = [this](int renderedFrames) noexcept {
        if (renderedFrames <= 0) {
            return;
        }
        if (!plan_->loop) {
            projectSamplePositionCursor_ += renderedFrames;
        } else {
            const auto& loop = *plan_->loop;
            const auto relative = projectSamplePositionCursor_ - loop.loopStartSample;
            const auto advanced = relative + static_cast<std::int64_t>(renderedFrames);
            const auto iterations = static_cast<std::uint64_t>(
                advanced / loop.loopLengthSamples);
            projectSamplePositionCursor_ = loop.loopStartSample
                + advanced % loop.loopLengthSamples;
            loopIterationCursor_ += iterations;
            loopIteration_.store(loopIterationCursor_, std::memory_order_release);
        }
        projectSamplePosition_.store(projectSamplePositionCursor_, std::memory_order_release);
        renderedSampleCount_.fetch_add(static_cast<std::uint64_t>(renderedFrames),
            std::memory_order_relaxed);
    };

    if (callbackState == RealtimePlaybackState::Stopping) {
        if (!stopReleaseStarted_) {
            synth_.releaseAll();
            stopReleaseStarted_ = true;
        }
        int releaseFrames = 0;
        while (releaseFrames < frames && synth_.hasActiveVoices()) {
            renderSegment(releaseFrames, 1);
            ++releaseFrames;
        }
        advancePlaybackPosition(releaseFrames);
        if (!synth_.hasActiveVoices()) {
            state_.store(static_cast<std::uint32_t>(RealtimePlaybackState::Stopped),
                std::memory_order_release);
        }
        return;
    }

    std::size_t callbackEventCount = 0;
    const auto addCallbackEvents = [&callbackEventCount](std::size_t count) noexcept {
        if (count > maximumCallbackEvents - callbackEventCount) {
            return false;
        }
        callbackEventCount += count;
        return true;
    };
    bool densityValid = initialChaseApplied_
        || addCallbackEvents(plan_->initialChaseNoteOnEvents.size());
    if (densityValid && !plan_->loop) {
        const auto blockEnd = projectSamplePositionCursor_
            + static_cast<std::int64_t>(frames);
        auto index = eventIndex_;
        while (index < plan_->events.size()
            && plan_->events[index].samplePosition < blockEnd) {
            if (!addCallbackEvents(1)) {
                densityValid = false;
                break;
            }
            ++index;
        }
    } else if (densityValid) {
        const auto& loop = *plan_->loop;
        auto index = eventIndex_;
        auto position = projectSamplePositionCursor_;
        auto boundaryPending = boundaryTransitionPending_;
        auto headPending = loopHeadChasePending_;
        int remainingFrames = frames;
        while (remainingFrames > 0 && densityValid) {
            if (position == loop.loopStartSample) {
                if (boundaryPending) {
                    densityValid = addCallbackEvents(loop.boundaryNoteOffEvents.size());
                    boundaryPending = false;
                    headPending = true;
                }
                if (densityValid && headPending) {
                    densityValid = addCallbackEvents(loop.startChaseNoteOnEvents.size());
                    while (densityValid && index < plan_->events.size()
                        && plan_->events[index].samplePosition == 0) {
                        densityValid = addCallbackEvents(1);
                        ++index;
                    }
                    headPending = false;
                }
            }
            const auto relativeStart = position - loop.loopStartSample;
            const auto segmentFrames = static_cast<int>(std::min<std::int64_t>(
                remainingFrames, loop.loopLengthSamples - relativeStart));
            const auto relativeEnd = relativeStart + segmentFrames;
            while (densityValid && index < plan_->events.size()
                && plan_->events[index].samplePosition < relativeEnd) {
                densityValid = addCallbackEvents(1);
                ++index;
            }
            remainingFrames -= segmentFrames;
            position += segmentFrames;
            if (position == loop.loopStartSample + loop.loopLengthSamples) {
                position = loop.loopStartSample;
                index = 0;
                boundaryPending = true;
            }
        }
    }
    if (!densityValid) {
        fault(RealtimeAudioError::EventDensityExceeded);
        return;
    }

    const auto applyEvent = [this](
                                const PreparedMidiEvent& event,
                                std::uint64_t loopIteration) noexcept {
        const auto& instrument = plan_->instrumentSlots[event.instrumentSlotIndex];
        const auto outcome = event.type == PreparedMidiEventType::NoteOn
            ? synth_.noteOn(event, loopIteration, instrument)
            : synth_.noteOff(event, loopIteration);
        if (outcome == BuiltInPolySynthEventOutcome::VoiceStolen) {
            voiceStealCount_.fetch_add(1, std::memory_order_relaxed);
        } else if (outcome == BuiltInPolySynthEventOutcome::StaleNoteOff) {
            staleNoteOffCount_.fetch_add(1, std::memory_order_relaxed);
        }
    };

    if (!initialChaseApplied_) {
        for (const auto& event : plan_->initialChaseNoteOnEvents) {
            applyEvent(event, loopIterationCursor_);
        }
        initialChaseApplied_ = true;
    }

    if (plan_->loop) {
        const auto& loop = *plan_->loop;
        const auto loopEnd = loop.loopStartSample + loop.loopLengthSamples;
        const auto applyLoopHead = [&]() noexcept {
            if (boundaryTransitionPending_) {
                for (const auto& event : loop.boundaryNoteOffEvents) {
                    applyEvent(event, loopIterationCursor_);
                }
                ++loopIterationCursor_;
                loopIteration_.store(loopIterationCursor_, std::memory_order_release);
                boundaryTransitionPending_ = false;
                loopHeadChasePending_ = true;
            }
            if (!loopHeadChasePending_) {
                return;
            }

            std::size_t chaseIndex = 0;
            while (eventIndex_ < plan_->events.size()
                && plan_->events[eventIndex_].samplePosition == 0
                && chaseIndex < loop.startChaseNoteOnEvents.size()) {
                if (loop.startChaseNoteOnEvents[chaseIndex].eventOrdinal
                    < plan_->events[eventIndex_].eventOrdinal) {
                    applyEvent(loop.startChaseNoteOnEvents[chaseIndex], loopIterationCursor_);
                    ++chaseIndex;
                } else {
                    applyEvent(plan_->events[eventIndex_], loopIterationCursor_);
                    ++eventIndex_;
                }
            }
            while (chaseIndex < loop.startChaseNoteOnEvents.size()) {
                applyEvent(loop.startChaseNoteOnEvents[chaseIndex], loopIterationCursor_);
                ++chaseIndex;
            }
            while (eventIndex_ < plan_->events.size()
                && plan_->events[eventIndex_].samplePosition == 0) {
                applyEvent(plan_->events[eventIndex_], loopIterationCursor_);
                ++eventIndex_;
            }
            loopHeadChasePending_ = false;
        };

        int outputFrame = 0;
        while (outputFrame < frames) {
            if (projectSamplePositionCursor_ == loop.loopStartSample) {
                applyLoopHead();
            }
            const auto relativeStart = projectSamplePositionCursor_ - loop.loopStartSample;
            const auto framesUntilBoundary = loop.loopLengthSamples - relativeStart;
            const auto segmentFrames = static_cast<int>(std::min<std::int64_t>(
                frames - outputFrame, framesUntilBoundary));
            const auto relativeEnd = relativeStart + segmentFrames;
            int segmentRenderedFrames = 0;
            while (eventIndex_ < plan_->events.size()
                && plan_->events[eventIndex_].samplePosition < relativeEnd) {
                const auto eventSample = plan_->events[eventIndex_].samplePosition;
                const auto eventFrame = static_cast<int>(eventSample - relativeStart);
                renderSegment(outputFrame + segmentRenderedFrames,
                    eventFrame - segmentRenderedFrames);
                segmentRenderedFrames = eventFrame;
                while (eventIndex_ < plan_->events.size()
                    && plan_->events[eventIndex_].samplePosition == eventSample) {
                    applyEvent(plan_->events[eventIndex_], loopIterationCursor_);
                    ++eventIndex_;
                }
            }
            renderSegment(outputFrame + segmentRenderedFrames,
                segmentFrames - segmentRenderedFrames);
            outputFrame += segmentFrames;
            projectSamplePositionCursor_ += segmentFrames;
            if (projectSamplePositionCursor_ == loopEnd) {
                projectSamplePositionCursor_ = loop.loopStartSample;
                eventIndex_ = 0;
                boundaryTransitionPending_ = true;
                if (outputFrame < frames) {
                    applyLoopHead();
                }
            }
        }
        projectSamplePosition_.store(projectSamplePositionCursor_, std::memory_order_release);
        renderedSampleCount_.fetch_add(static_cast<std::uint64_t>(frames),
            std::memory_order_relaxed);
        return;
    }

    int renderedFrames = 0;
    const auto blockStart = projectSamplePositionCursor_;
    const auto blockEnd = blockStart + static_cast<std::int64_t>(frames);
    while (eventIndex_ < plan_->events.size()
        && plan_->events[eventIndex_].samplePosition < blockEnd) {
        const auto eventSample = plan_->events[eventIndex_].samplePosition;
        const auto eventFrame = static_cast<int>(eventSample - blockStart);
        renderSegment(renderedFrames, eventFrame - renderedFrames);
        renderedFrames = eventFrame;
        while (eventIndex_ < plan_->events.size()
            && plan_->events[eventIndex_].samplePosition == eventSample) {
            applyEvent(plan_->events[eventIndex_], loopIterationCursor_);
            ++eventIndex_;
        }
    }
    renderSegment(renderedFrames, frames - renderedFrames);
    projectSamplePositionCursor_ = blockEnd;
    projectSamplePosition_.store(projectSamplePositionCursor_, std::memory_order_release);
    renderedSampleCount_.fetch_add(static_cast<std::uint64_t>(frames),
        std::memory_order_relaxed);
    if (eventIndex_ == plan_->events.size() && !synth_.hasActiveVoices()) {
        state_.store(static_cast<std::uint32_t>(RealtimePlaybackState::Stopped),
            std::memory_order_release);
    }
}

RealtimeAudioDiagnosticsSnapshot PreparedMidiPlaybackRuntime::snapshot() const noexcept
{
    RealtimeAudioDiagnosticsSnapshot result;
    result.state = static_cast<RealtimePlaybackState>(
        state_.load(std::memory_order_acquire));
    result.lastError = static_cast<RealtimeAudioError>(
        lastError_.load(std::memory_order_acquire));
    result.callbackCount = callbackCount_.load(std::memory_order_relaxed);
    result.callbackTimeoutCount = callbackTimeoutCount_.load(std::memory_order_relaxed);
    result.callbackExceptionCount = callbackExceptionCount_.load(std::memory_order_relaxed);
    result.oversizedBlockCount = oversizedBlockCount_.load(std::memory_order_relaxed);
    result.voiceStealCount = voiceStealCount_.load(std::memory_order_relaxed);
    result.staleNoteOffCount = staleNoteOffCount_.load(std::memory_order_relaxed);
    result.largestObservedBlockFrames = largestObservedBlockFrames_.load(
        std::memory_order_relaxed);
    result.renderedSampleCount = renderedSampleCount_.load(std::memory_order_relaxed);
    result.loopIteration = loopIteration_.load(std::memory_order_relaxed);
    result.projectSamplePosition = projectSamplePosition_.load(std::memory_order_acquire);
    return result;
}

}
