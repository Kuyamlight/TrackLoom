#pragma once

#include "AudioDisable.h"
#include "AudioMute.h"
#include "AudioSolo.h"
#include "Project.h"

namespace trackloom {

// TrackPlaybackAudioSource 把轨道播放状态应用到一条音频源链。
// 它不拥有原始输入音源，但拥有内部 wrapper，避免调用方手工拼接状态节点。
class TrackPlaybackAudioSource final : public AudioSource {
public:
    bool setSource(AudioSource* source);

    void setPlaybackState(TrackPlaybackState state);
    TrackPlaybackState playbackState() const;

    void setSoloModeActive(bool active);
    bool isSoloModeActive() const;

    bool render(AudioBlock block, double sampleRate) override;

private:
    AudioSource* source_ = nullptr;
    TrackPlaybackState playbackState_;
    bool soloModeActive_ = false;

    DisabledAudioSource disabled_;
    MuteAudioSource muted_;
    SoloAudioSource solo_;
};

}
