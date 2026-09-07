#pragma once

#include "BuiltInPolySynth.h"
#include "PreparedMidiPlaybackPlan.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace trackloom {

enum class RealtimePlaybackState : std::uint32_t {
    Stopped,
    Playing,
    Stopping,
    Faulted
};

enum class RealtimeAudioError : std::uint32_t {
    None,
    InvalidFormat,
    OversizedBlock,
    EventDensityExceeded,
    CallbackException,
    DeviceError
};

struct RealtimeAudioDiagnosticsSnapshot {
    RealtimePlaybackState state = RealtimePlaybackState::Stopped;
    RealtimeAudioError lastError = RealtimeAudioError::None;
    std::uint64_t callbackCount = 0;
    std::uint64_t callbackTimeoutCount = 0;
    std::uint64_t callbackExceptionCount = 0;
    std::uint64_t oversizedBlockCount = 0;
    std::uint64_t voiceStealCount = 0;
    std::uint64_t staleNoteOffCount = 0;
    std::uint64_t largestObservedBlockFrames = 0;
    std::uint64_t renderedSampleCount = 0;
    std::uint64_t loopIteration = 0;
    std::int64_t projectSamplePosition = 0;
};

enum class PreparedMidiPlaybackPlanValidationFailureReason {
    None,
    InvalidFormat,
    InvalidInstrumentSlot,
    InvalidEventValue,
    UnsortedEvents,
    InvalidEventOrdinal,
    InvalidLoopTable,
    EventLimitExceeded,
    CallbackEventLimitExceeded,
    ValidationResourceUnavailable
};

struct PreparedMidiPlaybackPlanValidationResult {
    bool valid = false;
    PreparedMidiPlaybackPlanValidationFailureReason failureReason =
        PreparedMidiPlaybackPlanValidationFailureReason::None;
};

PreparedMidiPlaybackPlanValidationResult validatePreparedMidiPlaybackPlan(
    const PreparedMidiPlaybackPlan& plan);

namespace detail {
struct PreparedMidiPlaybackRuntimeTestAccess;
}

class PreparedMidiPlaybackRuntime final {
public:
    bool installPlan(const PreparedMidiPlaybackPlan* plan);
    bool start() noexcept;
    bool requestStop() noexcept;
    void hardReset(RealtimeAudioError error = RealtimeAudioError::None) noexcept;
    void processBlock(float* const* outputs, int channels, int frames);
    void recordCallbackTimeout() noexcept;
    void recordCallbackException() noexcept;
    RealtimeAudioDiagnosticsSnapshot snapshot() const noexcept;

private:
    friend struct detail::PreparedMidiPlaybackRuntimeTestAccess;

    const PreparedMidiPlaybackPlan* plan_ = nullptr;
    BuiltInPolySynth synth_;
    std::size_t eventIndex_ = 0;
    bool initialChaseApplied_ = false;
    bool loopHeadChasePending_ = false;
    bool stopReleaseStarted_ = false;
    std::int64_t projectSamplePositionCursor_ = 0;
    std::uint64_t loopIterationCursor_ = 0;
    std::atomic<std::uint32_t> state_ {
        static_cast<std::uint32_t>(RealtimePlaybackState::Stopped)};
    std::atomic<std::uint32_t> lastError_ {
        static_cast<std::uint32_t>(RealtimeAudioError::None)};
    std::atomic<std::uint64_t> callbackCount_ {0};
    std::atomic<std::uint64_t> callbackTimeoutCount_ {0};
    std::atomic<std::uint64_t> callbackExceptionCount_ {0};
    std::atomic<std::uint64_t> oversizedBlockCount_ {0};
    std::atomic<std::uint64_t> voiceStealCount_ {0};
    std::atomic<std::uint64_t> staleNoteOffCount_ {0};
    std::atomic<std::uint64_t> largestObservedBlockFrames_ {0};
    std::atomic<std::uint64_t> renderedSampleCount_ {0};
    std::atomic<std::uint64_t> loopIteration_ {0};
    std::atomic<std::int64_t> projectSamplePosition_ {0};
};

}
