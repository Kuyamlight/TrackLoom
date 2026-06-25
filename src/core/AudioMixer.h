#pragma once

#include "AudioEngine.h"

#include <cstddef>
#include <vector>

namespace trackloom {

// SourceMixer 把多个 AudioSource 的输出相加到一个 AudioBlock。
// 它不拥有音源对象；调用方必须保证传入的音源在混音器使用期间仍然有效。
class SourceMixer final : public AudioSource {
public:
    // prepare 会分配 scratch buffer。后续 render 复用这块内存，避免实时路径临时分配。
    bool prepare(int channelCount, int maxBlockFrames);

    // 添加音源只保存指针，不复制、不接管生命周期。
    bool addSource(AudioSource* source);
    std::size_t sourceCount() const;

    bool render(AudioBlock block, double sampleRate) override;

private:
    bool acceptsBlock(AudioBlock block) const;

    bool prepared_ = false;
    int channelCount_ = 0;
    int maxBlockFrames_ = 0;
    std::vector<AudioSource*> sources_;
    std::vector<float> scratchSamples_;
};

}
