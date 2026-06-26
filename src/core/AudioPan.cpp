#include "AudioPan.h"

#include <algorithm>
#include <cmath>

namespace trackloom {
namespace {

bool isValidPan(float pan)
{
    return std::isfinite(pan) && pan >= -1.0f && pan <= 1.0f;
}

}

bool PanAudioSource::setSource(AudioSource* source)
{
    if (source == nullptr) {
        return false;
    }

    source_ = source;
    return true;
}

bool PanAudioSource::setPan(float pan)
{
    if (!isValidPan(pan)) {
        return false;
    }

    pan_ = pan;
    return true;
}

float PanAudioSource::pan() const
{
    return pan_;
}

bool PanAudioSource::render(AudioBlock block, double sampleRate)
{
    if (source_ == nullptr || !block.isValid() || sampleRate <= 0.0) {
        return false;
    }

    if (!source_->render(block, sampleRate)) {
        return false;
    }

    if (block.channelCount() < 2) {
        return true;
    }

    const float leftScale = 1.0f - std::max(pan_, 0.0f);
    const float rightScale = 1.0f + std::min(pan_, 0.0f);

    // 只处理前两个声道，后续多声道/双声像需要单独规格，避免现在暗含错误路由策略。
    for (int frame = 0; frame < block.frameCount(); ++frame) {
        block.sampleAt(0, frame) *= leftScale;
        block.sampleAt(1, frame) *= rightScale;
    }

    return true;
}

}
