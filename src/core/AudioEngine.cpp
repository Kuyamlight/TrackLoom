#include "AudioEngine.h"

#include <cmath>

namespace trackloom {
namespace {

bool isValidSampleRate(double sampleRate)
{
    return std::isfinite(sampleRate) && sampleRate > 0.0;
}

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
    if (!prepared_ || !block.isValid()) {
        return false;
    }
    if (block.channelCount() != channelCount_ || block.frameCount() > maxBlockFrames_) {
        return false;
    }

    block.clear();
    transport.setSampleRate(sampleRate_);
    return transport.advanceBySamples(block.frameCount());
}

}
