#pragma once

#include "AudioMixer.h"
#include "AudioTrackPlayback.h"
#include "Project.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace trackloom {

struct TrackAudioSourceBinding {
    std::string trackId;
    AudioSource* source = nullptr;
};

// ProjectPlaybackGraph 把工程轨道状态应用到绑定音源，并输出一个可渲染的混音源。
// 它不拥有输入音源；只拥有内部 TrackPlaybackAudioSource 包装器。
class ProjectPlaybackGraph final : public AudioSource {
public:
    bool prepare(int channelCount, int maxBlockFrames);
    bool rebuild(const Project& project, const std::vector<TrackAudioSourceBinding>& bindings);

    std::size_t sourceCount() const;
    bool isSoloModeActive() const;

    bool render(AudioBlock block, double sampleRate) override;

private:
    bool prepared_ = false;
    int channelCount_ = 0;
    int maxBlockFrames_ = 0;
    bool soloModeActive_ = false;
    SourceMixer mixer_;
    std::vector<std::unique_ptr<TrackPlaybackAudioSource>> sources_;
};

}
