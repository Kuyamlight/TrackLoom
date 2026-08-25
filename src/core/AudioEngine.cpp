#include "AudioEngine.h"

#include "PlaybackClock.h"

#include <cmath>

namespace trackloom {
namespace {

bool isValidSampleRate(double sampleRate)
{
    return std::isfinite(sampleRate) && sampleRate > 0.0;
}

bool isValidFrequency(double frequencyHz)
{
    return std::isfinite(frequencyHz) && frequencyHz > 0.0;
}

bool isValidGain(float gain)
{
    return std::isfinite(gain) && gain >= 0.0f;
}

void copyScheduledEventsToRawEvents(AudioEngineRenderResult& result)
{
    result.midiEvents.reserve(result.scheduledMidiEvents.size());
    for (const auto& scheduledEvent : result.scheduledMidiEvents) {
        // raw MIDI 事件保留给调试和旧调用方；真正给设备/插件使用的是带 sample offset 的 scheduled 事件。
        result.midiEvents.push_back(scheduledEvent.event);
    }
}

constexpr double pi = 3.14159265358979323846264338327950288;
constexpr double twoPi = 2.0 * pi;

}

AudioBlock::AudioBlock(float* samples, int channelCount, int frameCount)
    : samples_(samples)
    , channelCount_(channelCount)
    , frameCount_(frameCount)
{
}

bool AudioBlock::isValid() const
{
    return samples_ != nullptr && channelCount_ > 0 && frameCount_ > 0;
}

int AudioBlock::channelCount() const
{
    return channelCount_;
}

int AudioBlock::frameCount() const
{
    return frameCount_;
}

float& AudioBlock::sampleAt(int channel, int frame)
{
    return samples_[channel * frameCount_ + frame];
}

const float& AudioBlock::sampleAt(int channel, int frame) const
{
    return samples_[channel * frameCount_ + frame];
}

void AudioBlock::clear()
{
    if (!isValid()) {
        return;
    }

    const auto sampleCount = channelCount_ * frameCount_;
    for (int index = 0; index < sampleCount; ++index) {
        samples_[index] = 0.0f;
    }
}

bool SineToneSource::setFrequency(double frequencyHz)
{
    if (!isValidFrequency(frequencyHz)) {
        return false;
    }

    frequencyHz_ = frequencyHz;
    return true;
}

bool SineToneSource::setGain(float gain)
{
    if (!isValidGain(gain)) {
        return false;
    }

    gain_ = gain;
    return true;
}

bool SineToneSource::render(AudioBlock block, double sampleRate)
{
    if (!block.isValid() || !isValidSampleRate(sampleRate)) {
        return false;
    }

    // 相位保存在对象里，连续 block 会接着上一段生成，测试结果稳定且可复现。
    const auto phaseDelta = twoPi * frequencyHz_ / sampleRate;
    for (int frame = 0; frame < block.frameCount(); ++frame) {
        const auto sample = static_cast<float>(std::sin(phaseRadians_) * gain_);
        for (int channel = 0; channel < block.channelCount(); ++channel) {
            block.sampleAt(channel, frame) = sample;
        }

        phaseRadians_ += phaseDelta;
        if (phaseRadians_ >= twoPi) {
            phaseRadians_ = std::fmod(phaseRadians_, twoPi);
        }
    }

    return true;
}

bool AudioEngine::prepare(double sampleRate, int channelCount, int maxBlockFrames)
{
    if (!isValidSampleRate(sampleRate) || channelCount <= 0 || maxBlockFrames <= 0) {
        return false;
    }

    sampleRate_ = sampleRate;
    channelCount_ = channelCount;
    maxBlockFrames_ = maxBlockFrames;
    prepared_ = true;
    return true;
}

bool AudioEngine::isPrepared() const
{
    return prepared_;
}

double AudioEngine::sampleRate() const
{
    return sampleRate_;
}

int AudioEngine::channelCount() const
{
    return channelCount_;
}

int AudioEngine::maxBlockFrames() const
{
    return maxBlockFrames_;
}

bool AudioEngine::renderNextBlock(Transport& transport, AudioBlock block)
{
    return renderNextBlock(transport, block, nullptr);
}

bool AudioEngine::renderNextBlock(Transport& transport, AudioBlock block, AudioSource* source)
{
    if (!prepared_ || !block.isValid()) {
        return false;
    }
    if (block.channelCount() != channelCount_ || block.frameCount() > maxBlockFrames_) {
        return false;
    }

    block.clear();
    transport.setSampleRate(sampleRate_);
    if (source != nullptr && transport.isPlaying()) {
        // 音源只在播放中渲染；停止时保持静音，避免停止状态产生隐藏输出。
        if (!source->render(block, sampleRate_)) {
            return false;
        }
    }

    return transport.advanceBySamples(block.frameCount());
}

bool AudioEngine::renderNextBlockWithMidi(
    Transport& transport,
    AudioBlock block,
    const Project& project,
    AudioEngineRenderResult& result)
{
    return renderNextBlockWithMidi(transport, block, nullptr, project, MidiChaseMode::Disabled, result);
}

bool AudioEngine::renderNextBlockWithMidi(
    Transport& transport,
    AudioBlock block,
    const Project& project,
    MidiChaseMode chaseMode,
    AudioEngineRenderResult& result)
{
    return renderNextBlockWithMidi(transport, block, nullptr, project, chaseMode, result);
}

bool AudioEngine::renderNextBlockWithMidi(
    Transport& transport,
    AudioBlock block,
    AudioSource* source,
    const Project& project,
    AudioEngineRenderResult& result)
{
    return renderNextBlockWithMidi(transport, block, source, project, MidiChaseMode::Disabled, result);
}

bool AudioEngine::renderNextBlockWithMidi(
    Transport& transport,
    AudioBlock block,
    AudioSource* source,
    const Project& project,
    MidiChaseMode chaseMode,
    AudioEngineRenderResult& result)
{
    // 每次调用先清空旧结果，避免停止播放或失败返回时调用方读到上一帧事件。
    result.midiEvents.clear();
    result.scheduledMidiEvents.clear();

    if (!prepared_ || !block.isValid()) {
        return false;
    }
    if (block.channelCount() != channelCount_ || block.frameCount() > maxBlockFrames_) {
        return false;
    }

    block.clear();

    // PlaybackClock 使用 Transport 的 sample rate 换算 tick 窗口。
    // 因此必须先同步为 AudioEngine 已准备好的真实渲染采样率，再收集事件。
    if (!transport.setSampleRate(sampleRate_)) {
        return false;
    }

    result.scheduledMidiEvents = collectScheduledMidiPlaybackEventsForBlock(
        project,
        transport,
        block.frameCount(),
        chaseMode);
    copyScheduledEventsToRawEvents(result);

    if (source != nullptr && transport.isPlaying()) {
        // MIDI 事件只描述“本 block 需要触发什么”，音源渲染失败时不能把事件当成有效输出。
        if (!source->render(block, sampleRate_)) {
            result.midiEvents.clear();
            result.scheduledMidiEvents.clear();
            block.clear();
            return false;
        }
    }

    if (!transport.advanceBySamples(block.frameCount())) {
        result.midiEvents.clear();
        result.scheduledMidiEvents.clear();
        return false;
    }

    return true;
}

bool AudioEngine::renderNextBlockWithLoopedMidi(
    Transport& transport,
    AudioBlock block,
    const Project& project,
    const PlaybackLoopRange& loopRange,
    AudioEngineRenderResult& result)
{
    return renderNextBlockWithLoopedMidi(
        transport,
        block,
        nullptr,
        project,
        loopRange,
        MidiChaseMode::Disabled,
        result);
}

bool AudioEngine::renderNextBlockWithLoopedMidi(
    Transport& transport,
    AudioBlock block,
    const Project& project,
    const PlaybackLoopRange& loopRange,
    MidiChaseMode chaseMode,
    AudioEngineRenderResult& result)
{
    return renderNextBlockWithLoopedMidi(transport, block, nullptr, project, loopRange, chaseMode, result);
}

bool AudioEngine::renderNextBlockWithLoopedMidi(
    Transport& transport,
    AudioBlock block,
    AudioSource* source,
    const Project& project,
    const PlaybackLoopRange& loopRange,
    AudioEngineRenderResult& result)
{
    return renderNextBlockWithLoopedMidi(
        transport,
        block,
        source,
        project,
        loopRange,
        MidiChaseMode::Disabled,
        result);
}

bool AudioEngine::renderNextBlockWithLoopedMidi(
    Transport& transport,
    AudioBlock block,
    AudioSource* source,
    const Project& project,
    const PlaybackLoopRange& loopRange,
    MidiChaseMode chaseMode,
    AudioEngineRenderResult& result)
{
    result.midiEvents.clear();
    result.scheduledMidiEvents.clear();

    if (!prepared_ || !block.isValid()) {
        return false;
    }
    if (block.channelCount() != channelCount_ || block.frameCount() > maxBlockFrames_) {
        return false;
    }
    if (!isValidPlaybackLoopRange(loopRange)) {
        return false;
    }

    block.clear();

    // 循环 MIDI 仍按 AudioEngine 的真实采样率换算 sample offset，不能沿用 Transport 旧采样率。
    if (!transport.setSampleRate(sampleRate_)) {
        return false;
    }

    result.scheduledMidiEvents = collectScheduledMidiPlaybackEventsForLoopedBlock(
        project,
        transport,
        block.frameCount(),
        loopRange,
        chaseMode);
    copyScheduledEventsToRawEvents(result);

    if (source != nullptr && transport.isPlaying()) {
        if (!source->render(block, sampleRate_)) {
            result.midiEvents.clear();
            result.scheduledMidiEvents.clear();
            block.clear();
            return false;
        }
    }

    if (!transport.advanceBySamples(block.frameCount())) {
        result.midiEvents.clear();
        result.scheduledMidiEvents.clear();
        return false;
    }

    return true;
}

}
