#include "AudioSolo.h"

namespace trackloom {

bool SoloAudioSource::setSource(AudioSource* source)
{
    if (source == nullptr) {
        return false;
    }

    source_ = source;
    return true;
}

void SoloAudioSource::setSoloed(bool soloed)
{
    soloed_ = soloed;
}

bool SoloAudioSource::isSoloed() const
{
    return soloed_;
}

void SoloAudioSource::setSoloModeActive(bool active)
{
    soloModeActive_ = active;
}

bool SoloAudioSource::isSoloModeActive() const
{
    return soloModeActive_;
}

bool SoloAudioSource::render(AudioBlock block, double sampleRate)
{
    if (source_ == nullptr || !block.isValid()) {
        return false;
    }

    if (!source_->render(block, sampleRate)) {
        return false;
    }

    if (soloModeActive_ && !soloed_) {
        // 非 solo 轨在 solo 模式下不可听，但仍处理音源，避免监听状态暂停内部时间。
        block.clear();
    }

    return true;
}

}
