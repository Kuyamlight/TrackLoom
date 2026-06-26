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

RenameClipCommand::RenameClipCommand(std::string clipId, std::string newName)
    : clipId_(std::move(clipId))
    , newName_(std::move(newName))
{
}

std::string RenameClipCommand::name() const
{
    return "RenameClip";
}

CommandResult RenameClipCommand::validate(const Project& project) const
{
    if (newName_.empty()) {
        return CommandResult::fail("Clip name must not be empty.");
    }

    if (!project.findClipById(clipId_).has_value()) {
        return CommandResult::fail("Clip does not exist.");
    }

    return CommandResult::ok();
}

CommandResult RenameClipCommand::execute(Project& project)
{
    const auto clip = project.findClipById(clipId_);
    if (!clip.has_value()) {
        return CommandResult::fail("Clip does not exist.");
    }

    if (newName_.empty()) {
        return CommandResult::fail("Clip name must not be empty.");
    }

    if (!oldName_.has_value()) {
        oldName_ = clip->name;
    }

    if (!project.renameClipById(clipId_, newName_)) {
        return CommandResult::fail("Clip could not be renamed.");
    }

    return CommandResult::ok();
}

void RenameClipCommand::undo(Project& project)
{
    if (oldName_.has_value()) {
        project.renameClipById(clipId_, *oldName_);
    }
}

SetClipTimingCommand::SetClipTimingCommand(std::string clipId, std::int64_t startTick, std::int64_t lengthTick)
    : clipId_(std::move(clipId))
    , startTick_(startTick)
    , lengthTick_(lengthTick)
{
}

std::string SetClipTimingCommand::name() const
{
    return "SetClipTiming";
}

CommandResult SetClipTimingCommand::validate(const Project& project) const
{
    if (!isValidClipTiming(startTick_, lengthTick_)) {
        return CommandResult::fail("Clip timing is invalid.");
    }

    if (!project.findClipById(clipId_).has_value()) {
        return CommandResult::fail("Clip does not exist.");
    }

    return CommandResult::ok();
}

CommandResult SetClipTimingCommand::execute(Project& project)
{
    const auto clip = project.findClipById(clipId_);
    if (!clip.has_value()) {
        return CommandResult::fail("Clip does not exist.");
    }

    if (!isValidClipTiming(startTick_, lengthTick_)) {
        return CommandResult::fail("Clip timing is invalid.");
    }

    if (!oldStartTick_.has_value() || !oldLengthTick_.has_value()) {
        oldStartTick_ = clip->startTick;
        oldLengthTick_ = clip->lengthTick;
    }

    if (!project.setClipTiming(clipId_, startTick_, lengthTick_)) {
        return CommandResult::fail("Clip timing is invalid.");
    }

    return CommandResult::ok();
}

void SetClipTimingCommand::undo(Project& project)
{
    if (oldStartTick_.has_value() && oldLengthTick_.has_value()) {
        project.setClipTiming(clipId_, *oldStartTick_, *oldLengthTick_);
    }
}

DeleteClipCommand::DeleteClipCommand(std::string clipId)
    : clipId_(std::move(clipId))
{
}

std::string DeleteClipCommand::name() const
{
    return "DeleteClip";
}

CommandResult DeleteClipCommand::validate(const Project& project) const
{
    if (!project.findClipById(clipId_).has_value()) {
        return CommandResult::fail("Clip does not exist.");
    }

    return CommandResult::ok();
}

CommandResult DeleteClipCommand::execute(Project& project)
{
    if (!deletedClip_.has_value()) {
        const auto clip = project.findClipById(clipId_);
        if (!clip.has_value()) {
            return CommandResult::fail("Clip does not exist.");
        }
        deletedClip_ = *clip;
    }

    if (!project.removeClipById(deletedClip_->id)) {
        return CommandResult::fail("Clip does not exist.");
    }

    return CommandResult::ok();
}

void DeleteClipCommand::undo(Project& project)
{
    if (deletedClip_.has_value()) {
        project.insertExistingClip(*deletedClip_);
    }
}

MoveClipToTrackCommand::MoveClipToTrackCommand(std::string clipId, std::string targetTrackId)
    : clipId_(std::move(clipId))
    , targetTrackId_(std::move(targetTrackId))
{
}

std::string MoveClipToTrackCommand::name() const
{
    return "MoveClipToTrack";
}

CommandResult MoveClipToTrackCommand::validate(const Project& project) const
{
    const auto clip = project.findClipById(clipId_);
    if (!clip.has_value()) {
        return CommandResult::fail("Clip does not exist.");
    }

    const auto targetTrack = project.findTrackById(targetTrackId_);
    if (!targetTrack.has_value()) {
        return CommandResult::fail("Target track does not exist.");
    }

    if (!trackTypeAcceptsClip(targetTrack->type, clip->type)) {
        return CommandResult::fail("Clip type is not compatible with target track type.");
    }

    return CommandResult::ok();
}

CommandResult MoveClipToTrackCommand::execute(Project& project)
{
    const auto clip = project.findClipById(clipId_);
    if (!clip.has_value()) {
        return CommandResult::fail("Clip does not exist.");
    }

    if (!oldTrackId_.has_value()) {
        oldTrackId_ = clip->trackId;
    }

    if (!project.moveClipToTrack(clipId_, targetTrackId_)) {
        return CommandResult::fail("Clip could not be moved to target track.");
    }

    return CommandResult::ok();
}

void MoveClipToTrackCommand::undo(Project& project)
{
    if (oldTrackId_.has_value()) {
        project.moveClipToTrack(clipId_, *oldTrackId_);
    }
}

SplitClipCommand::SplitClipCommand(std::string clipId, std::int64_t splitTick)
    : clipId_(std::move(clipId))
    , splitTick_(splitTick)
{
}

std::string SplitClipCommand::name() const
{
    return "SplitClip";
}

CommandResult SplitClipCommand::validate(const Project& project) const
{
    const auto clip = project.findClipById(clipId_);
    if (!clip.has_value()) {
        return CommandResult::fail("Clip does not exist.");
    }

    const auto clipEndTick = clip->startTick + clip->lengthTick;
    if (splitTick_ <= clip->startTick || splitTick_ >= clipEndTick) {
        return CommandResult::fail("Split tick must be inside the clip.");
    }

    return CommandResult::ok();
}

CommandResult SplitClipCommand::execute(Project& project)
{
    const auto clip = project.findClipById(clipId_);
    if (!clip.has_value()) {
        return CommandResult::fail("Clip does not exist.");
    }

    if (!originalClip_.has_value()) {
        originalClip_ = *clip;
        rightClip_ = project.splitClipAtTick(clipId_, splitTick_);
        if (!rightClip_.has_value()) {
            originalClip_.reset();
            return CommandResult::fail("Clip could not be split.");
        }
        return CommandResult::ok();
    }

    if (!rightClip_.has_value() || project.findClipById(rightClip_->id).has_value()) {
        return CommandResult::fail("Right split clip already exists.");
    }

    const auto leftLength = splitTick_ - originalClip_->startTick;
    if (!project.setClipTiming(clipId_, originalClip_->startTick, leftLength)) {
        return CommandResult::fail("Clip could not be restored for split redo.");
    }

    if (!project.insertExistingClip(*rightClip_)) {
        project.setClipTiming(clipId_, originalClip_->startTick, originalClip_->lengthTick);
        return CommandResult::fail("Right split clip could not be restored.");
    }

    return CommandResult::ok();
}

void SplitClipCommand::undo(Project& project)
{
    if (!originalClip_.has_value() || !rightClip_.has_value()) {
        return;
    }

    project.removeClipById(rightClip_->id);
    project.setClipTiming(clipId_, originalClip_->startTick, originalClip_->lengthTick);
}

DuplicateClipCommand::DuplicateClipCommand(std::string clipId, std::string targetTrackId, std::int64_t startTick)
    : clipId_(std::move(clipId))
    , targetTrackId_(std::move(targetTrackId))
    , startTick_(startTick)
{
}

std::string DuplicateClipCommand::name() const
{
    return "DuplicateClip";
}

CommandResult DuplicateClipCommand::validate(const Project& project) const
{
    const auto clip = project.findClipById(clipId_);
    if (!clip.has_value()) {
        return CommandResult::fail("Clip does not exist.");
    }

    if (startTick_ < 0) {
        return CommandResult::fail("Duplicate start tick is invalid.");
    }

    const auto targetTrack = project.findTrackById(targetTrackId_);
    if (!targetTrack.has_value()) {
        return CommandResult::fail("Target track does not exist.");
    }

    if (!trackTypeAcceptsClip(targetTrack->type, clip->type)) {
        return CommandResult::fail("Clip type is not compatible with target track type.");
    }

    return CommandResult::ok();
}

CommandResult DuplicateClipCommand::execute(Project& project)
{
    if (createdClip_.has_value()) {
        if (!project.insertExistingClip(*createdClip_)) {
            return CommandResult::fail("Duplicate clip could not be restored.");
        }
        return CommandResult::ok();
    }

    createdClip_ = project.duplicateClipToTrackAtTick(clipId_, targetTrackId_, startTick_);
    if (!createdClip_.has_value()) {
        return CommandResult::fail("Clip could not be duplicated.");
    }

    return CommandResult::ok();
}

void DuplicateClipCommand::undo(Project& project)
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
