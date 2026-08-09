#include "BuiltInPolySynth.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace trackloom {

bool BuiltInPolySynth::prepare(double sampleRate) noexcept
{
    prepared_ = std::isfinite(sampleRate) && sampleRate > 0.0;
    if (prepared_) {
        sampleRate_ = sampleRate;
        attackFrames_ = static_cast<std::size_t>(std::ceil(sampleRate * 0.005));
        decayFrames_ = static_cast<std::size_t>(std::ceil(sampleRate * 0.020));
        releaseFrames_ = static_cast<std::size_t>(std::ceil(sampleRate * 0.030));
        reset();
    }
    return prepared_;
}

void BuiltInPolySynth::reset() noexcept
{
    voices_ = {};
    nextVoiceStartSerial_ = 0;
}

BuiltInPolySynthEventOutcome BuiltInPolySynth::noteOn(
    const PreparedMidiEvent& event,
    std::uint64_t loopIteration,
    const PreparedMidiInstrumentSlot& instrument) noexcept
{
    std::size_t selected = voiceCount;
    for (std::size_t index = 0; index < voices_.size(); ++index) {
        if (!voices_[index].active) {
            selected = index;
            break;
        }
    }
    const bool stolen = selected == voiceCount;
    if (stolen) {
        std::array<detail::BuiltInPolySynthVoiceSelectionState, voiceCount> states {};
        for (std::size_t index = 0; index < voices_.size(); ++index) {
            states[index] = {
                voices_[index].envelopeStage != Voice::EnvelopeStage::Release,
                voices_[index].voiceStartSerial,
            };
        }
        selected = detail::selectBuiltInPolySynthVoiceToSteal(states);
    }

    auto& voice = voices_[selected];
    voice.active = false;
    voice.active = true;
    voice.phase = 0.0;
    const double frequency = 440.0 * std::exp2((static_cast<int>(event.noteNumber) - 69) / 12.0);
    voice.phaseIncrement = 2.0 * std::acos(-1.0) * frequency / sampleRate_;
    voice.amplitude = std::min(0.045f,
        0.045f * static_cast<float>(event.velocity) / 127.0f * std::max(instrument.gain, 0.0f));
    voice.leftScale = 1.0f - std::max(instrument.pan, 0.0f);
    voice.rightScale = 1.0f + std::min(instrument.pan, 0.0f);
    voice.envelope = 0.0f;
    voice.releaseStep = 0.0f;
    voice.envelopeFrame = 0;
    voice.envelopeStage = Voice::EnvelopeStage::Attack;
    voice.key = {event.noteInstanceId, loopIteration};
    voice.voiceStartSerial = nextVoiceStartSerial_++;
    voice.eventOrdinal = event.eventOrdinal;
    return stolen ? BuiltInPolySynthEventOutcome::VoiceStolen : BuiltInPolySynthEventOutcome::Applied;
}

BuiltInPolySynthEventOutcome BuiltInPolySynth::noteOff(
    const PreparedMidiEvent& event,
    std::uint64_t loopIteration) noexcept
{
    const BuiltInPolySynthVoiceKey key {event.noteInstanceId, loopIteration};
    for (auto& voice : voices_) {
        if (voice.active
            && voice.envelopeStage != Voice::EnvelopeStage::Release
            && voice.key == key) {
            voice.envelopeStage = Voice::EnvelopeStage::Release;
            voice.envelopeFrame = 0;
            voice.releaseStep = voice.envelope / static_cast<float>(releaseFrames_);
            return BuiltInPolySynthEventOutcome::Applied;
        }
    }
    return BuiltInPolySynthEventOutcome::StaleNoteOff;
}

void BuiltInPolySynth::releaseAll() noexcept
{
    for (auto& voice : voices_) {
        if (voice.active) {
            voice.envelopeStage = Voice::EnvelopeStage::Release;
            voice.envelopeFrame = 0;
            voice.releaseStep = voice.envelope / static_cast<float>(releaseFrames_);
        }
    }
}

void BuiltInPolySynth::render(
    float* const* outputChannels,
    int channelCount,
    int startFrame,
    int frameCount) noexcept
{
    if (outputChannels == nullptr
        || (channelCount != 1 && channelCount != 2)
        || startFrame < 0
        || frameCount <= 0
        || startFrame > std::numeric_limits<int>::max() - frameCount) {
        return;
    }
    for (int channel = 0; channel < channelCount; ++channel) {
        if (outputChannels[channel] == nullptr) {
            return;
        }
    }
    for (int frame = startFrame; frame < startFrame + frameCount; ++frame) {
        for (auto& voice : voices_) {
            if (!voice.active) {
                continue;
            }
            const float sample = static_cast<float>(std::sin(voice.phase))
                * voice.amplitude * voice.envelope;
            voice.phase += voice.phaseIncrement;
            if (outputChannels[0] != nullptr) {
                outputChannels[0][frame] += channelCount == 1 ? sample : sample * voice.leftScale;
            }
            if (channelCount >= 2 && outputChannels[1] != nullptr) {
                outputChannels[1][frame] += sample * voice.rightScale;
            }

            switch (voice.envelopeStage) {
            case Voice::EnvelopeStage::Attack:
                ++voice.envelopeFrame;
                voice.envelope = std::min(1.0f,
                    static_cast<float>(voice.envelopeFrame) / static_cast<float>(attackFrames_));
                if (voice.envelopeFrame >= attackFrames_) {
                    voice.envelopeStage = Voice::EnvelopeStage::Decay;
                    voice.envelopeFrame = 0;
                }
                break;
            case Voice::EnvelopeStage::Decay:
                ++voice.envelopeFrame;
                voice.envelope = 1.0f - 0.2f
                    * static_cast<float>(voice.envelopeFrame) / static_cast<float>(decayFrames_);
                if (voice.envelopeFrame >= decayFrames_) {
                    voice.envelope = 0.8f;
                    voice.envelopeStage = Voice::EnvelopeStage::Sustain;
                    voice.envelopeFrame = 0;
                }
                break;
            case Voice::EnvelopeStage::Sustain:
                break;
            case Voice::EnvelopeStage::Release:
                ++voice.envelopeFrame;
                voice.envelope = std::max(0.0f, voice.envelope - voice.releaseStep);
                if (voice.envelopeFrame >= releaseFrames_) {
                    voice = {};
                }
                break;
            }
        }
    }
}

bool BuiltInPolySynth::hasActiveVoices() const noexcept
{
    return std::any_of(voices_.begin(), voices_.end(), [](const Voice& voice) {
        return voice.active;
    });
}

BuiltInPolySynthVoiceSnapshot BuiltInPolySynth::voiceSnapshot(std::size_t index) const noexcept
{
    if (index >= voices_.size()) {
        return {};
    }
    const auto& voice = voices_[index];
    return {voice.active, voice.key, voice.voiceStartSerial, voice.eventOrdinal};
}

namespace detail {

std::size_t selectBuiltInPolySynthVoiceToSteal(
    std::span<const BuiltInPolySynthVoiceSelectionState> voices) noexcept
{
    std::size_t selected = voices.size();
    std::uint64_t oldestSerial = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t index = 0; index < voices.size(); ++index) {
        if (!voices[index].active
            && (selected == voices.size() || voices[index].voiceStartSerial < oldestSerial)) {
            selected = index;
            oldestSerial = voices[index].voiceStartSerial;
        }
    }
    if (selected != voices.size()) {
        return selected;
    }

    oldestSerial = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t index = 0; index < voices.size(); ++index) {
        if (selected == voices.size() || voices[index].voiceStartSerial < oldestSerial) {
            selected = index;
            oldestSerial = voices[index].voiceStartSerial;
        }
    }
    return selected;
}

}
}
