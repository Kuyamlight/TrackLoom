#include "AudioDisable.h"

namespace trackloom {

bool DisabledAudioSource::setSource(AudioSource* source)
{
    if (source == nullptr) {
        return false;
    }

    source_ = source;
    return true;
}

void DisabledAudioSource::setDisabled(bool disabled)
{
    disabled_ = disabled;
}

bool DisabledAudioSource::isDisabled() const
{
    return disabled_;
}

bool DisabledAudioSource::render(AudioBlock block, double sampleRate)
{
    if (source_ == nullptr || !block.isValid()) {
        return false;
    }

    if (disabled_) {
        // 禁用表示跳过处理链；这和静音不同，静音仍会让音源继续处理。
        block.clear();
        return true;
    }

    return source_->render(block, sampleRate);
}

}
