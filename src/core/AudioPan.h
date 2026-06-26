#pragma once

#include "AudioEngine.h"

namespace trackloom {

// PanAudioSource 给任意 AudioSource 套一层立体声左右平衡。
// 它不拥有被包裹音源；调用方必须保证 source 的生命周期长于本对象的使用期。
class PanAudioSource final : public AudioSource {
public:
    bool setSource(AudioSource* source);

    // pan 使用 [-1.0, 1.0]：-1 全左，0 居中，1 全右。
    // 当前实现是可测试的 stereo balance，不声称是最终专业声像律。
    bool setPan(float pan);
    float pan() const;

    bool render(AudioBlock block, double sampleRate) override;

private:
    AudioSource* source_ = nullptr;
    float pan_ = 0.0f;
};

}
