#pragma once

#include "AudioEngine.h"

namespace trackloom {

// GainAudioSource 给任意 AudioSource 套一层线性增益。
// 它不拥有被包裹音源；调用方必须保证 source 的生命周期长于本对象的使用期。
class GainAudioSource final : public AudioSource {
public:
    bool setSource(AudioSource* source);

    // gain 是线性倍数：1.0 表示原音量，0.0 表示静音，2.0 表示放大一倍。
    bool setGain(float gain);

    bool render(AudioBlock block, double sampleRate) override;

private:
    AudioSource* source_ = nullptr;
    float gain_ = 1.0f;
};

}
