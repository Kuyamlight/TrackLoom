#include "AudioEngine.h"

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

}
