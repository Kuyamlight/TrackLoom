#include "AudioMixer.h"

#include <cmath>

namespace trackloom {
namespace {

bool isValidSampleRate(double sampleRate)
{
    return std::isfinite(sampleRate) && sampleRate > 0.0;
}

int sampleCountFor(AudioBlock block)
{
    return block.channelCount() * block.frameCount();
}

}

bool SourceMixer::prepare(int channelCount, int maxBlockFrames)
{
    if (channelCount <= 0 || maxBlockFrames <= 0) {
        return false;
    }

    channelCount_ = channelCount;
    maxBlockFrames_ = maxBlockFrames;
    scratchSamples_.assign(channelCount_ * maxBlockFrames_, 0.0f);
    prepared_ = true;
    return true;
}

bool SourceMixer::addSource(AudioSource* source)
{
    if (source == nullptr) {
        return false;
    }

    sources_.push_back(source);
    return true;
}

std::size_t SourceMixer::sourceCount() const
{
    return sources_.size();
}

bool SourceMixer::render(AudioBlock block, double sampleRate)
{
    if (!acceptsBlock(block) || !isValidSampleRate(sampleRate)) {
        return false;
    }

    block.clear();
    const auto activeSampleCount = sampleCountFor(block);
    for (auto* source : sources_) {
        for (int index = 0; index < activeSampleCount; ++index) {
            scratchSamples_[index] = 0.0f;
        }

        AudioBlock scratchBlock(scratchSamples_.data(), block.channelCount(), block.frameCount());
        if (!source->render(scratchBlock, sampleRate)) {
            return false;
        }

        // 子音源先写入 scratch，再累加到输出，避免后一个音源覆盖前一个音源。
        for (int channel = 0; channel < block.channelCount(); ++channel) {
            for (int frame = 0; frame < block.frameCount(); ++frame) {
                block.sampleAt(channel, frame) += scratchBlock.sampleAt(channel, frame);
            }
        }
    }

    return true;
}

bool SourceMixer::acceptsBlock(AudioBlock block) const
{
    if (!prepared_ || !block.isValid()) {
        return false;
    }

    return block.channelCount() == channelCount_ && block.frameCount() <= maxBlockFrames_;
}

}
