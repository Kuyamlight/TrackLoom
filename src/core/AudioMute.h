#pragma once

#include "AudioEngine.h"

namespace trackloom {

// MuteAudioSource 表示“静音”状态，不表示“禁用”状态。
// 它不拥有被包裹音源；调用方必须保证 source 在使用期间有效。
class MuteAudioSource final : public AudioSource {
public:
    bool setSource(AudioSource* source);

    void setMuted(bool muted);
    bool isMuted() const;

    bool render(AudioBlock block, double sampleRate) override;

private:
    AudioSource* source_ = nullptr;
    bool muted_ = false;
};

}
