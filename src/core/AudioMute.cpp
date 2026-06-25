#include "AudioMute.h"

namespace trackloom {

bool MuteAudioSource::setSource(AudioSource* source)
{
    if (source == nullptr) {
        return false;
    }

    source_ = source;
    return true;
}

void MuteAudioSource::setMuted(bool muted)
{
    muted_ = muted;
}

bool MuteAudioSource::isMuted() const
{
    return muted_;
}

bool MuteAudioSource::render(AudioBlock block, double sampleRate)
{
    if (source_ == nullptr || !block.isValid()) {
        return false;
    }

    if (!source_->render(block, sampleRate)) {
        return false;
    }

    if (muted_) {
        // 静音仍先处理音源，保证相位、时间位置和未来插件状态不会因静音而暂停。
        block.clear();
    }

    return true;
}

}
