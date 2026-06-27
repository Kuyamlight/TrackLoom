#include "ProjectPlaybackSession.h"

#include <utility>

namespace trackloom {

bool ProjectPlaybackSession::prepare(double sampleRate, int channelCount, int maxBlockFrames)
{
    if (midiOutput_.activeNoteCount() > 0) {
        return false;
    }

    AudioEngine nextAudioEngine;
    ProjectPlaybackGraph nextAudioGraph;

    if (!nextAudioEngine.prepare(sampleRate, channelCount, maxBlockFrames)) {
        return false;
    }
    if (!nextAudioGraph.prepare(channelCount, maxBlockFrames)) {
        return false;
    }

    audioEngine_ = nextAudioEngine;
    audioGraph_ = std::move(nextAudioGraph);
    midiOutput_ = MidiOutputSession {};
    chaseNextMidiBlock_ = true;
    prepared_ = true;
    return true;
}

bool ProjectPlaybackSession::isPrepared() const
{
    return prepared_;
}

double ProjectPlaybackSession::sampleRate() const
{
    return audioEngine_.sampleRate();
}

int ProjectPlaybackSession::channelCount() const
{
    return audioEngine_.channelCount();
}

int ProjectPlaybackSession::maxBlockFrames() const
{
    return audioEngine_.maxBlockFrames();
}

bool ProjectPlaybackSession::rebuildAudioGraph(
    const Project& project,
    const std::vector<TrackAudioSourceBinding>& bindings)
{
    if (!prepared_) {
        return false;
    }

    return audioGraph_.rebuild(project, bindings);
}

bool ProjectPlaybackSession::rebuildMidiOutput(
    const Project& project,
    const std::vector<MidiTrackReceiverBinding>& bindings)
{
    if (!prepared_) {
        return false;
    }

    const auto rebuilt = midiOutput_.rebuild(project, bindings);
    if (rebuilt) {
        chaseNextMidiBlock_ = true;
    }

    return rebuilt;
}

std::size_t ProjectPlaybackSession::audioSourceCount() const
{
    return audioGraph_.sourceCount();
}

std::size_t ProjectPlaybackSession::midiReceiverCount() const
{
    return midiOutput_.receiverCount();
}

std::size_t ProjectPlaybackSession::activeMidiNoteCount() const
{
    return midiOutput_.activeNoteCount();
}

bool ProjectPlaybackSession::requestMidiChaseOnNextBlock()
{
    if (!prepared_) {
        return false;
    }

    chaseNextMidiBlock_ = true;
    return true;
}

ProjectPlaybackBlockResult ProjectPlaybackSession::renderNextBlock(
    Transport& transport,
    AudioBlock block,
    const Project& project)
{
    ProjectPlaybackBlockResult result;

    if (!prepared_) {
        return result;
    }

    const auto shouldChase = chaseNextMidiBlock_ && transport.isPlaying();
    result.renderSucceeded = audioEngine_.renderNextBlockWithMidi(
        transport,
        block,
        &audioGraph_,
        project,
        shouldChase ? MidiChaseMode::Enabled : MidiChaseMode::Disabled,
        result.renderResult);

    if (!result.renderSucceeded) {
        return result;
    }

    if (shouldChase) {
        chaseNextMidiBlock_ = false;
    }

    result.midiDispatch = midiOutput_.dispatch(result.renderResult);
    return result;
}

ProjectPlaybackBlockResult ProjectPlaybackSession::renderNextLoopedBlock(
    Transport& transport,
    AudioBlock block,
    const Project& project,
    const PlaybackLoopRange& loopRange)
{
    ProjectPlaybackBlockResult result;

    if (!prepared_) {
        return result;
    }

    const auto shouldChase = chaseNextMidiBlock_ && transport.isPlaying();
    result.renderSucceeded = audioEngine_.renderNextBlockWithLoopedMidi(
        transport,
        block,
        &audioGraph_,
        project,
        loopRange,
        shouldChase ? MidiChaseMode::Enabled : MidiChaseMode::Disabled,
        result.renderResult);

    if (!result.renderSucceeded) {
        return result;
    }

    if (shouldChase) {
        chaseNextMidiBlock_ = false;
    }

    result.midiDispatch = midiOutput_.dispatch(result.renderResult);
    return result;
}

MidiDispatchResult ProjectPlaybackSession::releaseActiveMidiNotes(int sampleOffset)
{
    if (!prepared_) {
        MidiDispatchResult result;
        result.success = false;
        return result;
    }

    auto result = midiOutput_.releaseAllActiveNotes(sampleOffset);
    if (result.success) {
        chaseNextMidiBlock_ = true;
    }

    return result;
}

}
