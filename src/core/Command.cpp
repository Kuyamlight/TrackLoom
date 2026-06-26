#include "Command.h"

#include <utility>

namespace trackloom {
namespace {

bool trackTypeAcceptsClip(TrackType trackType, ClipType clipType)
{
    // 命令层提前做同样的类型检查，让错误在进入执行阶段前就能报告清楚。
    if (trackType == TrackType::Instrument) {
        return clipType == ClipType::Midi;
    }
    if (trackType == TrackType::Audio) {
        return clipType == ClipType::Audio;
    }

    return false;
}

}

CommandResult CommandResult::ok()
{
    return { true, "" };
}

CommandResult CommandResult::fail(std::string message)
{
    return { false, std::move(message) };
}

CommandResult CommandStack::execute(Project& project, std::unique_ptr<Command> command)
{
    const auto validation = command->validate(project);
    if (!validation.success) {
        return validation;
    }

    const auto result = command->execute(project);
    if (!result.success) {
        return result;
    }

    undoStack_.push_back(std::move(command));
    redoStack_.clear();
    return CommandResult::ok();
}

bool CommandStack::undo(Project& project)
{
    if (undoStack_.empty()) {
        return false;
    }

    auto command = std::move(undoStack_.back());
    undoStack_.pop_back();
    command->undo(project);
    redoStack_.push_back(std::move(command));
    return true;
}

bool CommandStack::redo(Project& project)
{
    if (redoStack_.empty()) {
        return false;
    }

    auto command = std::move(redoStack_.back());
    redoStack_.pop_back();

    const auto result = command->execute(project);
    if (!result.success) {
        return false;
    }

    undoStack_.push_back(std::move(command));
    return true;
}

bool CommandStack::canUndo() const
{
    return !undoStack_.empty();
}

bool CommandStack::canRedo() const
{
    return !redoStack_.empty();
}

AddTrackCommand::AddTrackCommand(std::string trackName, TrackType trackType)
    : trackName_(std::move(trackName))
    , trackType_(trackType)
{
}

std::string AddTrackCommand::name() const
{
    return "AddTrack";
}

CommandResult AddTrackCommand::validate(const Project&) const
{
    if (trackName_.empty()) {
        return CommandResult::fail("Track name must not be empty.");
    }

    return CommandResult::ok();
}

CommandResult AddTrackCommand::execute(Project& project)
{
    if (createdTrack_.has_value()) {
        if (!project.insertExistingTrack(*createdTrack_)) {
            return CommandResult::fail("Track already exists.");
        }
        return CommandResult::ok();
    }

    createdTrack_ = project.createTrack(trackName_, trackType_);
    return CommandResult::ok();
}

void AddTrackCommand::undo(Project& project)
{
    if (createdTrack_.has_value()) {
        project.removeTrackById(createdTrack_->id);
    }
}

AddClipCommand::AddClipCommand(
    std::string trackId,
    std::string clipName,
    ClipType clipType,
    std::int64_t startTick,
    std::int64_t lengthTick)
    : trackId_(std::move(trackId))
    , clipName_(std::move(clipName))
    , clipType_(clipType)
    , startTick_(startTick)
    , lengthTick_(lengthTick)
{
}

std::string AddClipCommand::name() const
{
    return "AddClip";
}

CommandResult AddClipCommand::validate(const Project& project) const
{
    if (clipName_.empty()) {
        return CommandResult::fail("Clip name must not be empty.");
    }

    if (!isValidClipTiming(startTick_, lengthTick_)) {
        return CommandResult::fail("Clip timing is invalid.");
    }

    const auto track = project.findTrackById(trackId_);
    if (!track.has_value()) {
        return CommandResult::fail("Track does not exist.");
    }

    if (!trackTypeAcceptsClip(track->type, clipType_)) {
        return CommandResult::fail("Clip type is not compatible with track type.");
    }

    return CommandResult::ok();
}

CommandResult AddClipCommand::execute(Project& project)
{
    if (createdClip_.has_value()) {
        if (!project.insertExistingClip(*createdClip_)) {
            return CommandResult::fail("Clip already exists or is no longer valid.");
        }
        return CommandResult::ok();
    }

    createdClip_ = project.createClip(trackId_, clipName_, clipType_, startTick_, lengthTick_);
    if (!createdClip_.has_value()) {
        return CommandResult::fail("Clip could not be created.");
    }

    return CommandResult::ok();
}

void AddClipCommand::undo(Project& project)
{
    if (createdClip_.has_value()) {
        project.removeClipById(createdClip_->id);
    }
}

SetTrackPlaybackStateCommand::SetTrackPlaybackStateCommand(std::string trackId, TrackPlaybackState newState)
    : trackId_(std::move(trackId))
    , newState_(newState)
{
}

std::string SetTrackPlaybackStateCommand::name() const
{
    return "SetTrackPlaybackState";
}

CommandResult SetTrackPlaybackStateCommand::validate(const Project& project) const
{
    if (!project.findTrackById(trackId_).has_value()) {
        return CommandResult::fail("Track does not exist.");
    }

    return CommandResult::ok();
}

CommandResult SetTrackPlaybackStateCommand::execute(Project& project)
{
    const auto track = project.findTrackById(trackId_);
    if (!track.has_value()) {
        return CommandResult::fail("Track does not exist.");
    }

    if (!oldState_.has_value()) {
        oldState_ = track->playback;
    }

    if (!project.setTrackPlaybackState(trackId_, newState_)) {
        return CommandResult::fail("Track does not exist.");
    }

    return CommandResult::ok();
}

void SetTrackPlaybackStateCommand::undo(Project& project)
{
    if (oldState_.has_value()) {
        project.setTrackPlaybackState(trackId_, *oldState_);
    }
}

SetTrackMixStateCommand::SetTrackMixStateCommand(std::string trackId, TrackMixState newState)
    : trackId_(std::move(trackId))
    , newState_(newState)
{
}

std::string SetTrackMixStateCommand::name() const
{
    return "SetTrackMixState";
}

CommandResult SetTrackMixStateCommand::validate(const Project& project) const
{
    if (!project.findTrackById(trackId_).has_value()) {
        return CommandResult::fail("Track does not exist.");
    }

    if (!isValidTrackMixState(newState_)) {
        return CommandResult::fail("Track mix state is invalid.");
    }

    return CommandResult::ok();
}

CommandResult SetTrackMixStateCommand::execute(Project& project)
{
    const auto track = project.findTrackById(trackId_);
    if (!track.has_value()) {
        return CommandResult::fail("Track does not exist.");
    }

    if (!isValidTrackMixState(newState_)) {
        return CommandResult::fail("Track mix state is invalid.");
    }

    if (!oldState_.has_value()) {
        oldState_ = track->mix;
    }

    if (!project.setTrackMixState(trackId_, newState_)) {
        return CommandResult::fail("Track mix state is invalid.");
    }

    return CommandResult::ok();
}

void SetTrackMixStateCommand::undo(Project& project)
{
    if (oldState_.has_value()) {
        project.setTrackMixState(trackId_, *oldState_);
    }
}

}
