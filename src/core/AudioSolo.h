#pragma once

#include "AudioEngine.h"

namespace trackloom {

// SoloAudioSource 表示“独奏监听”状态，不表示“静音”或“禁用”状态。
// 它不拥有被包裹音源；调用方必须保证 source 在使用期间有效。
class SoloAudioSource final : public AudioSource {
public:
    bool setSource(AudioSource* source);

    void setSoloed(bool soloed);
    bool isSoloed() const;

    void setSoloModeActive(bool active);
    bool isSoloModeActive() const;

    bool render(AudioBlock block, double sampleRate) override;

private:
    AudioSource* source_ = nullptr;
    bool soloed_ = false;
    bool soloModeActive_ = false;
};

}
