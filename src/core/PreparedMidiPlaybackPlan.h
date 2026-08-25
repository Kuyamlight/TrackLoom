#pragma once

#include "LoopRange.h"
#include "Project.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

namespace trackloom {

enum class PreparedMidiEventType : std::uint8_t { NoteOff, NoteOn };
enum class PreparedMidiInstrumentKind : std::uint8_t { BuiltInSine };

struct PreparedMidiEvent {
    std::int64_t samplePosition = 0;
    std::uint32_t noteInstanceId = 0;
    std::uint32_t eventOrdinal = 0;
    std::uint32_t instrumentSlotIndex = 0;
    std::uint8_t channel = 1;
    std::uint8_t noteNumber = 60;
    std::uint8_t velocity = 0;
    PreparedMidiEventType type = PreparedMidiEventType::NoteOn;
};

struct PreparedMidiInstrumentSlot {
    PreparedMidiInstrumentKind kind = PreparedMidiInstrumentKind::BuiltInSine;
    float gain = 1.0f;
    float pan = 0.0f;
    std::uint32_t outputBusIndex = 0;
};

struct PreparedMidiLoop {
    std::int64_t loopStartSample = 0;
    std::int64_t loopLengthSamples = 0;
    std::vector<PreparedMidiEvent> boundaryNoteOffEvents;
    std::vector<PreparedMidiEvent> startChaseNoteOnEvents;
};

struct PreparedMidiPlaybackPlan {
    double sampleRate = 0.0;
    int maximumBlockFrames = 0;
    int outputChannelCount = 0;
    std::uint64_t outputChannelMask = 0;
    std::int64_t playbackStartSample = 0;
    std::vector<PreparedMidiInstrumentSlot> instrumentSlots;
    std::vector<PreparedMidiEvent> events;
    std::vector<PreparedMidiEvent> initialChaseNoteOnEvents;
    std::optional<PreparedMidiLoop> loop;
};

struct PreparedMidiPlaybackPlanBuildRequest {
    Project projectSnapshot;
    double sampleRate = 0.0;
    int maximumBlockFrames = 0;
    int outputChannelCount = 0;
    std::uint64_t outputChannelMask = 0;
    std::int64_t playbackStartSample = 0;
    std::optional<PlaybackLoopRange> loopRange;
};

enum class PreparedMidiPlaybackPlanBuildFailureReason {
    None,
    Cancelled,
    InvalidSampleRate,
    InvalidMaximumBlockFrames,
    InvalidOutputFormat,
    InvalidPlaybackStart,
    InvalidLoopRange,
    InvalidTrackMix,
    SamplePositionOverflow,
    EventLimitExceeded,
    CallbackEventLimitExceeded
};

struct PreparedMidiPlaybackPlanBuildResult {
    PreparedMidiPlaybackPlanBuildFailureReason failureReason =
        PreparedMidiPlaybackPlanBuildFailureReason::None;
    std::unique_ptr<const PreparedMidiPlaybackPlan> plan;
};

PreparedMidiPlaybackPlanBuildResult buildPreparedMidiPlaybackPlan(
    PreparedMidiPlaybackPlanBuildRequest request,
    std::stop_token stopToken = {});

namespace detail {
struct PreparedMidiPlaybackPlanLimits {
    std::size_t maximumTotalEvents = 1'000'000;
    std::size_t maximumCallbackEvents = 4'096;
};
PreparedMidiPlaybackPlanBuildResult buildPreparedMidiPlaybackPlanWithLimits(
    PreparedMidiPlaybackPlanBuildRequest request,
    std::stop_token stopToken,
    PreparedMidiPlaybackPlanLimits limits);
}

}
