#include "Command.h"

#include <utility>

namespace trackloom {

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

}
