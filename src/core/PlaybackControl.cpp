#include "PlaybackControl.h"

#include <utility>

namespace trackloom {
namespace {

PlaybackControlResult failedPlaybackControl(
    PlaybackControlFailureReason failureReason,
    std::string message)
{
    PlaybackControlResult result;
    result.failureReason = failureReason;
    result.message = std::move(message);
    return result;
}

PlaybackControlFailureReason playbackControlFailureReasonFor(
    ProjectPlaybackControlFailureReason failureReason)
{
    switch (failureReason) {
    case ProjectPlaybackControlFailureReason::None:
        return PlaybackControlFailureReason::None;
    case ProjectPlaybackControlFailureReason::SessionNotPrepared:
        return PlaybackControlFailureReason::SessionNotPrepared;
    case ProjectPlaybackControlFailureReason::InvalidTargetSample:
        return PlaybackControlFailureReason::InvalidTargetSample;
    case ProjectPlaybackControlFailureReason::MidiReleaseFailed:
        return PlaybackControlFailureReason::MidiReleaseFailed;
    case ProjectPlaybackControlFailureReason::TransportRejected:
        return PlaybackControlFailureReason::TransportChangeRejected;
    }

    return PlaybackControlFailureReason::TransportChangeRejected;
}

PlaybackControlFailureReason playbackControlFailureReasonFor(
    ProjectPlaybackMidiOutputRebuildFailureReason failureReason)
{
    switch (failureReason) {
    case ProjectPlaybackMidiOutputRebuildFailureReason::None:
        return PlaybackControlFailureReason::None;
    case ProjectPlaybackMidiOutputRebuildFailureReason::SessionNotPrepared:
        return PlaybackControlFailureReason::SessionNotPrepared;
    case ProjectPlaybackMidiOutputRebuildFailureReason::MidiReleaseFailed:
        return PlaybackControlFailureReason::MidiReleaseFailed;
    case ProjectPlaybackMidiOutputRebuildFailureReason::OutputBindingRejected:
        return PlaybackControlFailureReason::MidiOutputRebuildRejected;
    }

    return PlaybackControlFailureReason::MidiOutputRebuildRejected;
}

std::string messageFor(PlaybackControlFailureReason failureReason)
{
    switch (failureReason) {
    case PlaybackControlFailureReason::None:
        return {};
    case PlaybackControlFailureReason::SessionNotPrepared:
        return "Playback session is not prepared.";
    case PlaybackControlFailureReason::InvalidTargetSample:
        return "Target sample must not be negative.";
    case PlaybackControlFailureReason::MidiReleaseFailed:
        return "Active MIDI notes could not be released.";
    case PlaybackControlFailureReason::TransportChangeRejected:
        return "Transport rejected the playback control change.";
    case PlaybackControlFailureReason::MidiOutputRebuildRejected:
        return "MIDI output rebuild was rejected.";
    }

    return "Playback control failed.";
}

void finishFailedPlaybackControlResult(PlaybackControlResult& result)
{
    result.success = false;
    result.message = messageFor(result.failureReason);
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
        return failedPlaybackControl(
            PlaybackControlFailureReason::SessionNotPrepared,
            messageFor(PlaybackControlFailureReason::SessionNotPrepared));
    }

    PlaybackControlResult result;
    result.transportControl = session.stopPlayback(transport, releaseSampleOffset_);
    result.success = result.transportControl.success;
    if (!result.success) {
        result.failureReason = playbackControlFailureReasonFor(result.transportControl.failureReason);
        finishFailedPlaybackControlResult(result);
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
        return failedPlaybackControl(
            PlaybackControlFailureReason::InvalidTargetSample,
            messageFor(PlaybackControlFailureReason::InvalidTargetSample));
    }
    if (!session.isPrepared()) {
        return failedPlaybackControl(
            PlaybackControlFailureReason::SessionNotPrepared,
            messageFor(PlaybackControlFailureReason::SessionNotPrepared));
    }

    PlaybackControlResult result;
    result.transportControl = session.seekPlaybackToSample(transport, targetSample_, releaseSampleOffset_);
    result.success = result.transportControl.success;
    if (!result.success) {
        result.failureReason = playbackControlFailureReasonFor(result.transportControl.failureReason);
        finishFailedPlaybackControlResult(result);
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
        return failedPlaybackControl(
            PlaybackControlFailureReason::SessionNotPrepared,
            messageFor(PlaybackControlFailureReason::SessionNotPrepared));
    }

    PlaybackControlResult result;
    result.midiOutputRebuild = session.rebuildMidiOutputSafely(project, bindings_, releaseSampleOffset_);
    result.success = result.midiOutputRebuild.success;
    if (!result.success) {
        result.failureReason = playbackControlFailureReasonFor(result.midiOutputRebuild.failureReason);
        finishFailedPlaybackControlResult(result);
    }
    return result;
}

}
