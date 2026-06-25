#include "AudioTrackPlayback.h"

namespace trackloom {

bool TrackPlaybackAudioSource::setSource(AudioSource* source)
{
    if (source == nullptr) {
        return false;
    }

    if (!solo_.setSource(source)) {
        return false;
    }
    if (!muted_.setSource(&solo_)) {
        return false;
    }
    if (!disabled_.setSource(&muted_)) {
        return false;
    }

    source_ = source;
    return true;
}

void TrackPlaybackAudioSource::setPlaybackState(TrackPlaybackState state)
{
    playbackState_ = state;
}

TrackPlaybackState TrackPlaybackAudioSource::playbackState() const
{
    return playbackState_;
}

void TrackPlaybackAudioSource::setSoloModeActive(bool active)
{
    soloModeActive_ = active;
}

bool TrackPlaybackAudioSource::isSoloModeActive() const
{
    return soloModeActive_;
}

bool TrackPlaybackAudioSource::render(AudioBlock block, double sampleRate)
{
    if (source_ == nullptr) {
        return false;
    }

    disabled_.setDisabled(playbackState_.disabled);
    muted_.setMuted(playbackState_.muted);
    solo_.setSoloed(playbackState_.soloed);
    solo_.setSoloModeActive(soloModeActive_);

    return disabled_.render(block, sampleRate);
}

}
