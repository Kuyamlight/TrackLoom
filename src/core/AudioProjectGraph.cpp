#include "AudioProjectGraph.h"

#include <utility>

namespace trackloom {
namespace {

bool projectHasSoloedTrack(const Project& project)
{
    for (const auto& track : project.tracks()) {
        if (track.playback.soloed) {
            return true;
        }
    }

    return false;
}

}

bool ProjectPlaybackGraph::prepare(int channelCount, int maxBlockFrames)
{
    if (channelCount <= 0 || maxBlockFrames <= 0) {
        return false;
    }

    SourceMixer newMixer;
    if (!newMixer.prepare(channelCount, maxBlockFrames)) {
        return false;
    }

    channelCount_ = channelCount;
    maxBlockFrames_ = maxBlockFrames;
    mixer_ = std::move(newMixer);
    sources_.clear();
    prepared_ = true;
    return true;
}

bool ProjectPlaybackGraph::rebuild(const Project& project, const std::vector<TrackAudioSourceBinding>& bindings)
{
    if (!prepared_) {
        return false;
    }

    const bool nextSoloModeActive = projectHasSoloedTrack(project);
    SourceMixer nextMixer;
    if (!nextMixer.prepare(channelCount_, maxBlockFrames_)) {
        return false;
    }

    std::vector<std::unique_ptr<TrackPlaybackAudioSource>> nextSources;
    nextSources.reserve(bindings.size());

    for (const auto& binding : bindings) {
        if (binding.source == nullptr) {
            return false;
        }

        const auto track = project.findTrackById(binding.trackId);
        if (!track.has_value()) {
            return false;
        }

        auto playbackSource = std::make_unique<TrackPlaybackAudioSource>();
        if (!playbackSource->setSource(binding.source)) {
            return false;
        }

        playbackSource->setPlaybackState(track->playback);
        playbackSource->setSoloModeActive(nextSoloModeActive);

        if (!nextMixer.addSource(playbackSource.get())) {
            return false;
        }

        nextSources.push_back(std::move(playbackSource));
    }

    soloModeActive_ = nextSoloModeActive;
    mixer_ = std::move(nextMixer);
    sources_ = std::move(nextSources);
    return true;
}

std::size_t ProjectPlaybackGraph::sourceCount() const
{
    return sources_.size();
}

bool ProjectPlaybackGraph::isSoloModeActive() const
{
    return soloModeActive_;
}

bool ProjectPlaybackGraph::render(AudioBlock block, double sampleRate)
{
    if (!prepared_) {
        return false;
    }

    return mixer_.render(block, sampleRate);
}

}
