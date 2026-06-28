#include "PlaybackControl.h"

#include <utility>

namespace trackloom {
namespace {

PlaybackControlResult failedPlaybackControl(std::string message)
{
    PlaybackControlResult result;
    result.message = std::move(message);
    return result;
}

}

StopPlaybackCommand::StopPlaybackCommand(int releaseSampleOffset)
    : releaseSampleOffset_(releaseSampleOffset)
{
}

std::string StopPlaybackCommand::name() const
{
    return "StopPlayback";
}

PlaybackControlResult StopPlaybackCommand::execute(
    ProjectPlaybackSession& session,
    Transport& transport,
    const Project&) const
{
    if (!session.isPrepared()) {
        return failedPlaybackControl("Playback session is not prepared.");
    }

    PlaybackControlResult result;
    result.transportControl = session.stopPlayback(transport, releaseSampleOffset_);
    result.success = result.transportControl.success;
    if (!result.success) {
        result.message = "Stop playback failed.";
    }
    return result;
}

SeekPlaybackCommand::SeekPlaybackCommand(std::int64_t targetSample, int releaseSampleOffset)
    : targetSample_(targetSample)
    , releaseSampleOffset_(releaseSampleOffset)
{
}

std::string SeekPlaybackCommand::name() const
{
    return "SeekPlayback";
}

PlaybackControlResult SeekPlaybackCommand::execute(
    ProjectPlaybackSession& session,
    Transport& transport,
    const Project&) const
{
    if (targetSample_ < 0) {
        return failedPlaybackControl("Target sample must not be negative.");
    }
    if (!session.isPrepared()) {
        return failedPlaybackControl("Playback session is not prepared.");
    }

    PlaybackControlResult result;
    result.transportControl = session.seekPlaybackToSample(transport, targetSample_, releaseSampleOffset_);
    result.success = result.transportControl.success;
    if (!result.success) {
        result.message = "Seek playback failed.";
    }
    return result;
}

RebuildMidiOutputCommand::RebuildMidiOutputCommand(
    std::vector<MidiTrackReceiverBinding> bindings,
    int releaseSampleOffset)
    : bindings_(std::move(bindings))
    , releaseSampleOffset_(releaseSampleOffset)
{
}

std::string RebuildMidiOutputCommand::name() const
{
    return "RebuildMidiOutput";
}

PlaybackControlResult RebuildMidiOutputCommand::execute(
    ProjectPlaybackSession& session,
    Transport&,
    const Project& project) const
{
    if (!session.isPrepared()) {
        return failedPlaybackControl("Playback session is not prepared.");
    }

    PlaybackControlResult result;
    result.midiOutputRebuild = session.rebuildMidiOutputSafely(project, bindings_, releaseSampleOffset_);
    result.success = result.midiOutputRebuild.success;
    if (!result.success) {
        result.message = "Rebuild MIDI output failed.";
    }
    return result;
}

}
