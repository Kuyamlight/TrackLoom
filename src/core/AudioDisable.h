#pragma once

#include "AudioEngine.h"

namespace trackloom {

// DisabledAudioSource 表示“禁用”状态，不表示“静音”状态。
// 禁用会跳过被包裹音源处理；需要继续处理但听不到时应使用 MuteAudioSource。
class DisabledAudioSource final : public AudioSource {
public:
    bool setSource(AudioSource* source);

    void setDisabled(bool disabled);
    bool isDisabled() const;

    bool render(AudioBlock block, double sampleRate) override;

private:
    AudioSource* source_ = nullptr;
    bool disabled_ = false;
};

}
