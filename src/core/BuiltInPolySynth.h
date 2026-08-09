#pragma once

#include "PreparedMidiPlaybackPlan.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace trackloom {

struct BuiltInPolySynthVoiceKey {
    std::uint32_t noteInstanceId = 0;
    std::uint64_t loopIteration = 0;
    bool operator==(const BuiltInPolySynthVoiceKey&) const = default;
};

enum class BuiltInPolySynthEventOutcome {
    Applied,
    VoiceStolen,
    StaleNoteOff
};

struct BuiltInPolySynthVoiceSnapshot {
    bool active = false;
    BuiltInPolySynthVoiceKey key;
    std::uint64_t voiceStartSerial = 0;
    std::uint32_t eventOrdinal = 0;
};

class BuiltInPolySynth final {
public:
    static constexpr std::size_t voiceCount = 16;

    bool prepare(double sampleRate) noexcept;
    void reset() noexcept;
    BuiltInPolySynthEventOutcome noteOn(
        const PreparedMidiEvent& event,
        std::uint64_t loopIteration,
        const PreparedMidiInstrumentSlot& instrument) noexcept;
    BuiltInPolySynthEventOutcome noteOff(
        const PreparedMidiEvent& event,
        std::uint64_t loopIteration) noexcept;
    void releaseAll() noexcept;
    void render(
        float* const* outputChannels,
        int channelCount,
        int startFrame,
        int frameCount) noexcept;
    bool hasActiveVoices() const noexcept;
    BuiltInPolySynthVoiceSnapshot voiceSnapshot(std::size_t index) const noexcept;

private:
    struct Voice {
        enum class EnvelopeStage {
            Attack,
            Decay,
            Sustain,
            Release
        };

        bool active = false;
        double phase = 0.0;
        double phaseIncrement = 0.0;
        float amplitude = 0.0f;
        float leftScale = 1.0f;
        float rightScale = 1.0f;
        float envelope = 0.0f;
        float releaseStep = 0.0f;
        std::size_t envelopeFrame = 0;
        EnvelopeStage envelopeStage = EnvelopeStage::Attack;
        BuiltInPolySynthVoiceKey key;
        std::uint64_t voiceStartSerial = 0;
        std::uint32_t eventOrdinal = 0;
    };

    std::array<Voice, voiceCount> voices_ {};
    double sampleRate_ = 0.0;
    std::size_t attackFrames_ = 0;
    std::size_t decayFrames_ = 0;
    std::size_t releaseFrames_ = 0;
    std::uint64_t nextVoiceStartSerial_ = 0;
    bool prepared_ = false;
};

namespace detail {
struct BuiltInPolySynthVoiceSelectionState {
    bool active = false;
    std::uint64_t voiceStartSerial = 0;
};

std::size_t selectBuiltInPolySynthVoiceToSteal(
    std::span<const BuiltInPolySynthVoiceSelectionState> voices) noexcept;
}

}
