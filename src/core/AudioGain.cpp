#include "AudioGain.h"

#include <cmath>

namespace trackloom {
namespace {

bool isValidGain(float gain)
{
    return std::isfinite(gain) && gain >= 0.0f;
}

}

bool GainAudioSource::setSource(AudioSource* source)
{
    if (source == nullptr) {
        return false;
    }

    source_ = source;
    return true;
}

bool GainAudioSource::setGain(float gain)
{
    if (!isValidGain(gain)) {
        return false;
    }

    gain_ = gain;
    return true;
}

bool GainAudioSource::render(AudioBlock block, double sampleRate)
{
    if (source_ == nullptr || !block.isValid()) {
        return false;
    }

    if (!source_->render(block, sampleRate)) {
        return false;
    }

    // 这里直接原地缩放样本，不分配内存，适合后续放入实时音频路径。
    for (int channel = 0; channel < block.channelCount(); ++channel) {
        for (int frame = 0; frame < block.frameCount(); ++frame) {
            block.sampleAt(channel, frame) *= gain_;
        }
    }

    return true;
}

}
