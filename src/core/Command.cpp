#include "Command.h"

#include <algorithm>
#include <limits>
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

std::optional<TimelineClip> clipContainingMidiNote(const Project& project, const std::string& noteId)
{
    for (const auto& clip : project.clips()) {
        const auto noteIt = std::find_if(clip.midiNotes.begin(), clip.midiNotes.end(), [&](const MidiNoteEvent& note) {
            return note.id == noteId;
        });

        if (noteIt != clip.midiNotes.end()) {
            return clip;
        }
    }

    return std::nullopt;
}

bool midiNoteTimingFitsClip(const TimelineClip& clip, std::int64_t startTick, std::int64_t lengthTick)
{
    if (clip.type != ClipType::Midi || startTick < 0 || lengthTick <= 0) {
        return false;
    }

    return lengthTick <= clip.lengthTick && startTick <= clip.lengthTick - lengthTick;
}

struct MidiNoteTimingChange {
    std::string noteId;
    std::int64_t startTick = 0;
    std::int64_t lengthTick = 0;
};

bool shiftedMidiNoteStart(
    std::int64_t noteStartTick,
    std::int64_t clipStartOffset,
    std::int64_t& shiftedStartTick)
{
    if (clipStartOffset > 0) {
        if (noteStartTick < clipStartOffset) {
            return false;
        }
        shiftedStartTick = noteStartTick - clipStartOffset;
        return true;
    }

    if (clipStartOffset < 0) {
        const auto addedLeftSpace = -clipStartOffset;
        if (noteStartTick > std::numeric_limits<std::int64_t>::max() - addedLeftSpace) {
            return false;
        }
        shiftedStartTick = noteStartTick + addedLeftSpace;
        return true;
    }

    shiftedStartTick = noteStartTick;
    return true;
}

std::vector<MidiNoteTimingChange> noteTimingChangesFromNotes(const std::vector<MidiNoteEvent>& notes)
{
    std::vector<MidiNoteTimingChange> changes;
    changes.reserve(notes.size());

    for (const auto& note : notes) {
        changes.push_back({ note.id, note.startTick, note.lengthTick });
    }

    return changes;
}

bool collectMidiClipStartTimingChanges(
    const TimelineClip& clip,
    std::int64_t startTick,
    std::int64_t lengthTick,
    std::vector<MidiNoteTimingChange>& changes)
{
    if (clip.type != ClipType::Midi || !isValidClipTiming(startTick, lengthTick)) {
        return false;
    }

    const auto clipStartOffset = startTick - clip.startTick;
    auto targetClip = clip;
    targetClip.startTick = startTick;
    targetClip.lengthTick = lengthTick;
    changes.clear();
    changes.reserve(clip.midiNotes.size());

    for (const auto& note : clip.midiNotes) {
        std::int64_t shiftedStartTick = 0;
        if (!shiftedMidiNoteStart(note.startTick, clipStartOffset, shiftedStartTick)
            || !midiNoteTimingFitsClip(targetClip, shiftedStartTick, note.lengthTick)) {
            return false;
        }

        changes.push_back({ note.id, shiftedStartTick, note.lengthTick });
    }

    return true;
}

bool applyMidiNoteTimingChanges(Project& project, const std::vector<MidiNoteTimingChange>& changes)
{
    for (const auto& change : changes) {
        if (!project.setMidiNoteTiming(change.noteId, change.startTick, change.lengthTick)) {
            return false;
        }
    }

    return true;
}

bool applyClipTimingAndMidiNoteChanges(
    Project& project,
    const std::string& clipId,
    std::int64_t startTick,
    std::int64_t lengthTick,
    const std::vector<MidiNoteTimingChange>& changes)
{
    const auto clip = project.findClipById(clipId);
    if (!clip.has_value()) {
        return false;
    }

    // 缩短左边界时要先移动音符，否则旧音符可能暂时超出新的较短片段。
    // 延长左边界时要先扩大片段，否则右移后的音符可能暂时超出旧片段。
    if (lengthTick > clip->lengthTick) {
        return project.setClipTiming(clipId, startTick, lengthTick)
            && applyMidiNoteTimingChanges(project, changes);
    }

    return applyMidiNoteTimingChanges(project, changes)
        && project.setClipTiming(clipId, startTick, lengthTick);
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

SetProjectPlaybackLoopCommand::SetProjectPlaybackLoopCommand(
    std::optional<PlaybackLoopRange> newRange)
    : newRange_(std::move(newRange))
{
}

std::string SetProjectPlaybackLoopCommand::name() const
{
    return "SetProjectPlaybackLoop";
}

CommandResult SetProjectPlaybackLoopCommand::validate(const Project& project) const
{
    if (newRange_.has_value() && !isValidPlaybackLoopRange(*newRange_)) {
        return CommandResult::fail("Playback loop range is invalid.");
    }

    if (project.playbackLoopRange() == newRange_) {
        return CommandResult::fail("Playback loop range is already set to the requested value.");
    }

    return CommandResult::ok();
}

CommandResult SetProjectPlaybackLoopCommand::execute(Project& project)
{
    const auto validation = validate(project);
    if (!validation.success) {
        return validation;
    }

    if (!oldRangeCaptured_) {
        oldRange_ = project.playbackLoopRange();
        oldRangeCaptured_ = true;
    }

    if (!project.setPlaybackLoopRange(newRange_)) {
        return CommandResult::fail("Playback loop range could not be changed.");
    }

    return CommandResult::ok();
}

void SetProjectPlaybackLoopCommand::undo(Project& project)
{
    if (oldRangeCaptured_) {
        project.setPlaybackLoopRange(oldRange_);
    }
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

RenameTrackCommand::RenameTrackCommand(std::string trackId, std::string newName)
    : trackId_(std::move(trackId))
    , newName_(std::move(newName))
{
}

std::string RenameTrackCommand::name() const
{
    return "RenameTrack";
}

CommandResult RenameTrackCommand::validate(const Project& project) const
{
    if (newName_.empty()) {
        return CommandResult::fail("Track name must not be empty.");
    }

    if (!project.findTrackById(trackId_).has_value()) {
        return CommandResult::fail("Track does not exist.");
    }

    return CommandResult::ok();
}

CommandResult RenameTrackCommand::execute(Project& project)
{
    const auto track = project.findTrackById(trackId_);
    if (!track.has_value()) {
        return CommandResult::fail("Track does not exist.");
    }

    if (newName_.empty()) {
        return CommandResult::fail("Track name must not be empty.");
    }

    if (!oldName_.has_value()) {
        oldName_ = track->name;
    }

    if (!project.renameTrackById(trackId_, newName_)) {
        return CommandResult::fail("Track could not be renamed.");
    }

    return CommandResult::ok();
}

void RenameTrackCommand::undo(Project& project)
{
    if (oldName_.has_value()) {
        project.renameTrackById(trackId_, *oldName_);
    }
}

DeleteTrackCommand::DeleteTrackCommand(std::string trackId)
    : trackId_(std::move(trackId))
{
}

std::string DeleteTrackCommand::name() const
{
    return "DeleteTrack";
}

CommandResult DeleteTrackCommand::validate(const Project& project) const
{
    if (!project.findTrackById(trackId_).has_value()) {
        return CommandResult::fail("Track does not exist.");
    }

    return CommandResult::ok();
}

CommandResult DeleteTrackCommand::execute(Project& project)
{
    if (!deletedTrack_.has_value() || !deletedTrackIndex_.has_value()) {
        const auto track = project.findTrackById(trackId_);
        const auto trackIndex = project.trackIndexById(trackId_);
        if (!track.has_value() || !trackIndex.has_value()) {
            return CommandResult::fail("Track does not exist.");
        }

        deletedTrack_ = *track;
        deletedTrackIndex_ = *trackIndex;
        deletedClips_ = project.clipsForTrack(trackId_);
    }

    if (!project.removeTrackById(trackId_)) {
        return CommandResult::fail("Track could not be deleted.");
    }

    return CommandResult::ok();
}

void DeleteTrackCommand::undo(Project& project)
{
    if (!deletedTrack_.has_value() || !deletedTrackIndex_.has_value()) {
        return;
    }

    if (!project.insertExistingTrackAt(*deletedTrack_, *deletedTrackIndex_)) {
        return;
    }

    for (const auto& clip : deletedClips_) {
        project.insertExistingClip(clip);
    }
}

MoveTrackCommand::MoveTrackCommand(std::string trackId, std::size_t targetIndex)
    : trackId_(std::move(trackId))
    , targetIndex_(targetIndex)
{
}

std::string MoveTrackCommand::name() const
{
    return "MoveTrack";
}

CommandResult MoveTrackCommand::validate(const Project& project) const
{
    const auto currentIndex = project.trackIndexById(trackId_);
    if (!currentIndex.has_value()) {
        return CommandResult::fail("Track does not exist.");
    }

    if (targetIndex_ >= project.tracks().size()) {
        return CommandResult::fail("Track target index is invalid.");
    }

    if (*currentIndex == targetIndex_) {
        return CommandResult::fail("Track is already at target index.");
    }

    return CommandResult::ok();
}

CommandResult MoveTrackCommand::execute(Project& project)
{
    const auto currentIndex = project.trackIndexById(trackId_);
    if (!currentIndex.has_value()) {
        return CommandResult::fail("Track does not exist.");
    }

    if (targetIndex_ >= project.tracks().size()) {
        return CommandResult::fail("Track target index is invalid.");
    }

    if (*currentIndex == targetIndex_) {
        return CommandResult::fail("Track is already at target index.");
    }

    if (!oldIndex_.has_value()) {
        oldIndex_ = *currentIndex;
    }

    if (!project.moveTrackToIndex(trackId_, targetIndex_)) {
        return CommandResult::fail("Track could not be moved.");
    }

    return CommandResult::ok();
}

void MoveTrackCommand::undo(Project& project)
{
    if (oldIndex_.has_value()) {
        project.moveTrackToIndex(trackId_, *oldIndex_);
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

    auto leftClip = *originalClip_;
    const auto leftLength = splitTick_ - originalClip_->startTick;
    const auto splitOffset = splitTick_ - originalClip_->startTick;
    leftClip.lengthTick = leftLength;
    leftClip.midiNotes.clear();
    for (const auto& note : originalClip_->midiNotes) {
        if (note.startTick + note.lengthTick <= splitOffset) {
            leftClip.midiNotes.push_back(note);
        }
    }

    const auto currentLeftClip = project.findClipById(clipId_);
    if (!currentLeftClip.has_value() || !project.removeClipById(clipId_)) {
        return CommandResult::fail("Left split clip does not exist.");
    }

    if (!project.insertExistingClip(leftClip)) {
        project.insertExistingClip(*currentLeftClip);
        return CommandResult::fail("Left split clip could not be restored.");
    }

    if (!project.insertExistingClip(*rightClip_)) {
        project.removeClipById(leftClip.id);
        project.insertExistingClip(*currentLeftClip);
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
    project.removeClipById(originalClip_->id);
    project.insertExistingClip(*originalClip_);
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

TrimClipStartCommand::TrimClipStartCommand(std::string clipId, std::int64_t startTick)
    : clipId_(std::move(clipId))
    , startTick_(startTick)
{
}

std::string TrimClipStartCommand::name() const
{
    return "TrimClipStart";
}

CommandResult TrimClipStartCommand::validate(const Project& project) const
{
    const auto clip = project.findClipById(clipId_);
    if (!clip.has_value()) {
        return CommandResult::fail("Clip does not exist.");
    }

    const auto clipEndTick = clip->startTick + clip->lengthTick;
    if (startTick_ <= clip->startTick || startTick_ >= clipEndTick) {
        return CommandResult::fail("Trim start tick must be inside the clip.");
    }

    return CommandResult::ok();
}

CommandResult TrimClipStartCommand::execute(Project& project)
{
    const auto clip = project.findClipById(clipId_);
    if (!clip.has_value()) {
        return CommandResult::fail("Clip does not exist.");
    }

    if (!oldStartTick_.has_value() || !oldLengthTick_.has_value()) {
        oldStartTick_ = clip->startTick;
        oldLengthTick_ = clip->lengthTick;
    }

    if (!project.trimClipStartToTick(clipId_, startTick_)) {
        return CommandResult::fail("Clip start could not be trimmed.");
    }

    return CommandResult::ok();
}

void TrimClipStartCommand::undo(Project& project)
{
    if (oldStartTick_.has_value() && oldLengthTick_.has_value()) {
        project.setClipTiming(clipId_, *oldStartTick_, *oldLengthTick_);
    }
}

SetMidiClipStartKeepingNoteTimesCommand::SetMidiClipStartKeepingNoteTimesCommand(
    std::string clipId,
    std::int64_t startTick,
    std::int64_t lengthTick)
    : clipId_(std::move(clipId))
    , startTick_(startTick)
    , lengthTick_(lengthTick)
{
}

std::string SetMidiClipStartKeepingNoteTimesCommand::name() const
{
    return "SetMidiClipStartKeepingNoteTimes";
}

CommandResult SetMidiClipStartKeepingNoteTimesCommand::validate(const Project& project) const
{
    const auto clip = project.findClipById(clipId_);
    if (!clip.has_value()) {
        return CommandResult::fail("Clip does not exist.");
    }

    if (clip->type != ClipType::Midi) {
        return CommandResult::fail("Clip must be a MIDI clip.");
    }

    std::vector<MidiNoteTimingChange> changes;
    if (!collectMidiClipStartTimingChanges(*clip, startTick_, lengthTick_, changes)) {
        return CommandResult::fail("MIDI clip start timing is invalid.");
    }

    return CommandResult::ok();
}

CommandResult SetMidiClipStartKeepingNoteTimesCommand::execute(Project& project)
{
    const auto clip = project.findClipById(clipId_);
    if (!clip.has_value()) {
        return CommandResult::fail("Clip does not exist.");
    }

    std::vector<MidiNoteTimingChange> newTimings;
    if (!collectMidiClipStartTimingChanges(*clip, startTick_, lengthTick_, newTimings)) {
        return CommandResult::fail("MIDI clip start timing is invalid.");
    }

    if (!oldStartTick_.has_value() || !oldLengthTick_.has_value()) {
        oldStartTick_ = clip->startTick;
        oldLengthTick_ = clip->lengthTick;
        oldMidiNotes_ = clip->midiNotes;
    }

    const auto currentStartTick = clip->startTick;
    const auto currentLengthTick = clip->lengthTick;
    const auto currentTimings = noteTimingChangesFromNotes(clip->midiNotes);

    if (!applyClipTimingAndMidiNoteChanges(project, clipId_, startTick_, lengthTick_, newTimings)) {
        applyClipTimingAndMidiNoteChanges(project, clipId_, currentStartTick, currentLengthTick, currentTimings);
        return CommandResult::fail("MIDI clip start could not be changed.");
    }

    return CommandResult::ok();
}

void SetMidiClipStartKeepingNoteTimesCommand::undo(Project& project)
{
    if (!oldStartTick_.has_value() || !oldLengthTick_.has_value()) {
        return;
    }

    const auto oldTimings = noteTimingChangesFromNotes(oldMidiNotes_);
    applyClipTimingAndMidiNoteChanges(project, clipId_, *oldStartTick_, *oldLengthTick_, oldTimings);
}

TrimClipEndCommand::TrimClipEndCommand(std::string clipId, std::int64_t endTick)
    : clipId_(std::move(clipId))
    , endTick_(endTick)
{
}

std::string TrimClipEndCommand::name() const
{
    return "TrimClipEnd";
}

CommandResult TrimClipEndCommand::validate(const Project& project) const
{
    const auto clip = project.findClipById(clipId_);
    if (!clip.has_value()) {
        return CommandResult::fail("Clip does not exist.");
    }

    const auto clipEndTick = clip->startTick + clip->lengthTick;
    if (endTick_ <= clip->startTick || endTick_ >= clipEndTick) {
        return CommandResult::fail("Trim end tick must be inside the clip.");
    }

    return CommandResult::ok();
}

CommandResult TrimClipEndCommand::execute(Project& project)
{
    const auto clip = project.findClipById(clipId_);
    if (!clip.has_value()) {
        return CommandResult::fail("Clip does not exist.");
    }

    if (!oldStartTick_.has_value() || !oldLengthTick_.has_value()) {
        oldStartTick_ = clip->startTick;
        oldLengthTick_ = clip->lengthTick;
    }

    if (!project.trimClipEndToTick(clipId_, endTick_)) {
        return CommandResult::fail("Clip end could not be trimmed.");
    }

    return CommandResult::ok();
}

void TrimClipEndCommand::undo(Project& project)
{
    if (oldStartTick_.has_value() && oldLengthTick_.has_value()) {
        project.setClipTiming(clipId_, *oldStartTick_, *oldLengthTick_);
    }
}

AddMidiNoteCommand::AddMidiNoteCommand(
    std::string clipId,
    std::int64_t startTick,
    std::int64_t lengthTick,
    int noteNumber,
    int velocity,
    int channel)
    : clipId_(std::move(clipId))
    , startTick_(startTick)
    , lengthTick_(lengthTick)
    , noteNumber_(noteNumber)
    , velocity_(velocity)
    , channel_(channel)
{
}

std::string AddMidiNoteCommand::name() const
{
    return "AddMidiNote";
}

CommandResult AddMidiNoteCommand::validate(const Project& project) const
{
    const auto clip = project.findClipById(clipId_);
    if (!clip.has_value()) {
        return CommandResult::fail("Clip does not exist.");
    }

    MidiNoteEvent note { "requested-note", startTick_, lengthTick_, noteNumber_, velocity_, channel_ };
    if (!isValidMidiNoteValues(note) || !midiNoteTimingFitsClip(*clip, startTick_, lengthTick_)) {
        return CommandResult::fail("MIDI note value is invalid.");
    }

    return CommandResult::ok();
}

CommandResult AddMidiNoteCommand::execute(Project& project)
{
    if (createdNote_.has_value()) {
        if (!project.insertExistingMidiNote(clipId_, *createdNote_)) {
            return CommandResult::fail("MIDI note already exists or is no longer valid.");
        }
        return CommandResult::ok();
    }

    createdNote_ = project.createMidiNote(clipId_, startTick_, lengthTick_, noteNumber_, velocity_, channel_);
    if (!createdNote_.has_value()) {
        return CommandResult::fail("MIDI note could not be created.");
    }

    return CommandResult::ok();
}

void AddMidiNoteCommand::undo(Project& project)
{
    if (createdNote_.has_value()) {
        project.removeMidiNoteById(createdNote_->id);
    }
}

SetMidiNoteTimingCommand::SetMidiNoteTimingCommand(std::string noteId, std::int64_t startTick, std::int64_t lengthTick)
    : noteId_(std::move(noteId))
    , startTick_(startTick)
    , lengthTick_(lengthTick)
{
}

std::string SetMidiNoteTimingCommand::name() const
{
    return "SetMidiNoteTiming";
}

CommandResult SetMidiNoteTimingCommand::validate(const Project& project) const
{
    const auto note = project.findMidiNoteById(noteId_);
    const auto clip = clipContainingMidiNote(project, noteId_);
    if (!note.has_value() || !clip.has_value()) {
        return CommandResult::fail("MIDI note does not exist.");
    }

    if (!midiNoteTimingFitsClip(*clip, startTick_, lengthTick_)) {
        return CommandResult::fail("MIDI note timing is invalid.");
    }

    return CommandResult::ok();
}

CommandResult SetMidiNoteTimingCommand::execute(Project& project)
{
    const auto note = project.findMidiNoteById(noteId_);
    if (!note.has_value()) {
        return CommandResult::fail("MIDI note does not exist.");
    }

    if (!oldStartTick_.has_value() || !oldLengthTick_.has_value()) {
        oldStartTick_ = note->startTick;
        oldLengthTick_ = note->lengthTick;
    }

    if (!project.setMidiNoteTiming(noteId_, startTick_, lengthTick_)) {
        return CommandResult::fail("MIDI note timing is invalid.");
    }

    return CommandResult::ok();
}

void SetMidiNoteTimingCommand::undo(Project& project)
{
    if (oldStartTick_.has_value() && oldLengthTick_.has_value()) {
        project.setMidiNoteTiming(noteId_, *oldStartTick_, *oldLengthTick_);
    }
}

SetMidiNotePitchCommand::SetMidiNotePitchCommand(std::string noteId, int noteNumber)
    : noteId_(std::move(noteId))
    , noteNumber_(noteNumber)
{
}

std::string SetMidiNotePitchCommand::name() const
{
    return "SetMidiNotePitch";
}

CommandResult SetMidiNotePitchCommand::validate(const Project& project) const
{
    auto note = project.findMidiNoteById(noteId_);
    if (!note.has_value()) {
        return CommandResult::fail("MIDI note does not exist.");
    }

    note->noteNumber = noteNumber_;
    if (!isValidMidiNoteValues(*note)) {
        return CommandResult::fail("MIDI note pitch is invalid.");
    }

    return CommandResult::ok();
}

CommandResult SetMidiNotePitchCommand::execute(Project& project)
{
    const auto note = project.findMidiNoteById(noteId_);
    if (!note.has_value()) {
        return CommandResult::fail("MIDI note does not exist.");
    }

    if (!oldNoteNumber_.has_value()) {
        oldNoteNumber_ = note->noteNumber;
    }

    if (!project.setMidiNotePitch(noteId_, noteNumber_)) {
        return CommandResult::fail("MIDI note pitch is invalid.");
    }

    return CommandResult::ok();
}

void SetMidiNotePitchCommand::undo(Project& project)
{
    if (oldNoteNumber_.has_value()) {
        project.setMidiNotePitch(noteId_, *oldNoteNumber_);
    }
}

SetMidiNoteVelocityCommand::SetMidiNoteVelocityCommand(std::string noteId, int velocity)
    : noteId_(std::move(noteId))
    , velocity_(velocity)
{
}

std::string SetMidiNoteVelocityCommand::name() const
{
    return "SetMidiNoteVelocity";
}

CommandResult SetMidiNoteVelocityCommand::validate(const Project& project) const
{
    auto note = project.findMidiNoteById(noteId_);
    if (!note.has_value()) {
        return CommandResult::fail("MIDI note does not exist.");
    }

    note->velocity = velocity_;
    if (!isValidMidiNoteValues(*note)) {
        return CommandResult::fail("MIDI note velocity is invalid.");
    }

    return CommandResult::ok();
}

CommandResult SetMidiNoteVelocityCommand::execute(Project& project)
{
    const auto note = project.findMidiNoteById(noteId_);
    if (!note.has_value()) {
        return CommandResult::fail("MIDI note does not exist.");
    }

    if (!oldVelocity_.has_value()) {
        oldVelocity_ = note->velocity;
    }

    if (!project.setMidiNoteVelocity(noteId_, velocity_)) {
        return CommandResult::fail("MIDI note velocity is invalid.");
    }

    return CommandResult::ok();
}

void SetMidiNoteVelocityCommand::undo(Project& project)
{
    if (oldVelocity_.has_value()) {
        project.setMidiNoteVelocity(noteId_, *oldVelocity_);
    }
}

SetMidiNoteChannelCommand::SetMidiNoteChannelCommand(std::string noteId, int channel)
    : noteId_(std::move(noteId))
    , channel_(channel)
{
}

std::string SetMidiNoteChannelCommand::name() const
{
    return "SetMidiNoteChannel";
}

CommandResult SetMidiNoteChannelCommand::validate(const Project& project) const
{
    auto note = project.findMidiNoteById(noteId_);
    if (!note.has_value()) {
        return CommandResult::fail("MIDI note does not exist.");
    }

    note->channel = channel_;
    if (!isValidMidiNoteValues(*note)) {
        return CommandResult::fail("MIDI note channel is invalid.");
    }

    return CommandResult::ok();
}

CommandResult SetMidiNoteChannelCommand::execute(Project& project)
{
    const auto note = project.findMidiNoteById(noteId_);
    if (!note.has_value()) {
        return CommandResult::fail("MIDI note does not exist.");
    }

    if (!oldChannel_.has_value()) {
        oldChannel_ = note->channel;
    }

    if (!project.setMidiNoteChannel(noteId_, channel_)) {
        return CommandResult::fail("MIDI note channel is invalid.");
    }

    return CommandResult::ok();
}

void SetMidiNoteChannelCommand::undo(Project& project)
{
    if (oldChannel_.has_value()) {
        project.setMidiNoteChannel(noteId_, *oldChannel_);
    }
}

DeleteMidiNoteCommand::DeleteMidiNoteCommand(std::string noteId)
    : noteId_(std::move(noteId))
{
}

std::string DeleteMidiNoteCommand::name() const
{
    return "DeleteMidiNote";
}

CommandResult DeleteMidiNoteCommand::validate(const Project& project) const
{
    if (!project.findMidiNoteById(noteId_).has_value()) {
        return CommandResult::fail("MIDI note does not exist.");
    }

    return CommandResult::ok();
}

CommandResult DeleteMidiNoteCommand::execute(Project& project)
{
    if (!deletedNote_.has_value() || !owningClipId_.has_value()) {
        const auto note = project.findMidiNoteById(noteId_);
        const auto clip = clipContainingMidiNote(project, noteId_);
        if (!note.has_value() || !clip.has_value()) {
            return CommandResult::fail("MIDI note does not exist.");
        }

        deletedNote_ = *note;
        owningClipId_ = clip->id;
    }

    if (!project.removeMidiNoteById(noteId_)) {
        return CommandResult::fail("MIDI note could not be deleted.");
    }

    return CommandResult::ok();
}

void DeleteMidiNoteCommand::undo(Project& project)
{
    if (deletedNote_.has_value() && owningClipId_.has_value()) {
        project.insertExistingMidiNote(*owningClipId_, *deletedNote_);
    }
}

AddMarkerCommand::AddMarkerCommand(std::string markerName, std::int64_t tick)
    : markerName_(std::move(markerName))
    , tick_(tick)
{
}

std::string AddMarkerCommand::name() const
{
    return "AddMarker";
}

CommandResult AddMarkerCommand::validate(const Project&) const
{
    if (markerName_.empty()) {
        return CommandResult::fail("Marker name must not be empty.");
    }

    if (!isValidMarkerTick(tick_)) {
        return CommandResult::fail("Marker tick is invalid.");
    }

    return CommandResult::ok();
}

CommandResult AddMarkerCommand::execute(Project& project)
{
    if (createdMarker_.has_value()) {
        if (!project.insertExistingMarker(*createdMarker_)) {
            return CommandResult::fail("Marker already exists or is no longer valid.");
        }
        return CommandResult::ok();
    }

    createdMarker_ = project.createMarker(markerName_, tick_);
    if (!createdMarker_.has_value()) {
        return CommandResult::fail("Marker could not be created.");
    }

    return CommandResult::ok();
}

void AddMarkerCommand::undo(Project& project)
{
    if (createdMarker_.has_value()) {
        project.removeMarkerById(createdMarker_->id);
    }
}

RenameMarkerCommand::RenameMarkerCommand(std::string markerId, std::string newName)
    : markerId_(std::move(markerId))
    , newName_(std::move(newName))
{
}

std::string RenameMarkerCommand::name() const
{
    return "RenameMarker";
}

CommandResult RenameMarkerCommand::validate(const Project& project) const
{
    if (newName_.empty()) {
        return CommandResult::fail("Marker name must not be empty.");
    }

    if (!project.findMarkerById(markerId_).has_value()) {
        return CommandResult::fail("Marker does not exist.");
    }

    return CommandResult::ok();
}

CommandResult RenameMarkerCommand::execute(Project& project)
{
    const auto marker = project.findMarkerById(markerId_);
    if (!marker.has_value()) {
        return CommandResult::fail("Marker does not exist.");
    }

    if (newName_.empty()) {
        return CommandResult::fail("Marker name must not be empty.");
    }

    if (!oldName_.has_value()) {
        oldName_ = marker->name;
    }

    if (!project.renameMarkerById(markerId_, newName_)) {
        return CommandResult::fail("Marker could not be renamed.");
    }

    return CommandResult::ok();
}

void RenameMarkerCommand::undo(Project& project)
{
    if (oldName_.has_value()) {
        project.renameMarkerById(markerId_, *oldName_);
    }
}

MoveMarkerCommand::MoveMarkerCommand(std::string markerId, std::int64_t tick)
    : markerId_(std::move(markerId))
    , tick_(tick)
{
}

std::string MoveMarkerCommand::name() const
{
    return "MoveMarker";
}

CommandResult MoveMarkerCommand::validate(const Project& project) const
{
    const auto marker = project.findMarkerById(markerId_);
    if (!marker.has_value()) {
        return CommandResult::fail("Marker does not exist.");
    }

    if (!isValidMarkerTick(tick_)) {
        return CommandResult::fail("Marker tick is invalid.");
    }

    if (marker->tick == tick_) {
        return CommandResult::fail("Marker is already at target tick.");
    }

    return CommandResult::ok();
}

CommandResult MoveMarkerCommand::execute(Project& project)
{
    const auto marker = project.findMarkerById(markerId_);
    if (!marker.has_value()) {
        return CommandResult::fail("Marker does not exist.");
    }

    if (!isValidMarkerTick(tick_)) {
        return CommandResult::fail("Marker tick is invalid.");
    }

    if (marker->tick == tick_) {
        return CommandResult::fail("Marker is already at target tick.");
    }

    if (!oldTick_.has_value()) {
        oldTick_ = marker->tick;
    }

    if (!project.moveMarkerToTick(markerId_, tick_)) {
        return CommandResult::fail("Marker could not be moved.");
    }

    return CommandResult::ok();
}

void MoveMarkerCommand::undo(Project& project)
{
    if (oldTick_.has_value()) {
        project.moveMarkerToTick(markerId_, *oldTick_);
    }
}

DeleteMarkerCommand::DeleteMarkerCommand(std::string markerId)
    : markerId_(std::move(markerId))
{
}

std::string DeleteMarkerCommand::name() const
{
    return "DeleteMarker";
}

CommandResult DeleteMarkerCommand::validate(const Project& project) const
{
    if (!project.findMarkerById(markerId_).has_value()) {
        return CommandResult::fail("Marker does not exist.");
    }

    return CommandResult::ok();
}

CommandResult DeleteMarkerCommand::execute(Project& project)
{
    if (!deletedMarker_.has_value()) {
        const auto marker = project.findMarkerById(markerId_);
        if (!marker.has_value()) {
            return CommandResult::fail("Marker does not exist.");
        }
        deletedMarker_ = *marker;
    }

    if (!project.removeMarkerById(deletedMarker_->id)) {
        return CommandResult::fail("Marker could not be deleted.");
    }

    return CommandResult::ok();
}

void DeleteMarkerCommand::undo(Project& project)
{
    if (deletedMarker_.has_value()) {
        project.insertExistingMarker(*deletedMarker_);
    }
}

AddTempoEventCommand::AddTempoEventCommand(std::int64_t tick, double beatsPerMinute)
    : tick_(tick)
    , beatsPerMinute_(beatsPerMinute)
{
}

std::string AddTempoEventCommand::name() const
{
    return "AddTempoEvent";
}

CommandResult AddTempoEventCommand::validate(const Project& project) const
{
    if (!isValidMarkerTick(tick_)) {
        return CommandResult::fail("Tempo tick is invalid.");
    }

    if (!isValidTempoBpm(beatsPerMinute_)) {
        return CommandResult::fail("Tempo BPM is invalid.");
    }

    const auto duplicateTick = std::find_if(project.tempoEvents().begin(), project.tempoEvents().end(), [&](const TempoEvent& event) {
        return event.tick == tick_;
    });
    if (duplicateTick != project.tempoEvents().end()) {
        return CommandResult::fail("Tempo tick already exists.");
    }

    return CommandResult::ok();
}

CommandResult AddTempoEventCommand::execute(Project& project)
{
    if (createdEvent_.has_value()) {
        if (!project.insertExistingTempoEvent(*createdEvent_)) {
            return CommandResult::fail("Tempo event already exists or is no longer valid.");
        }
        return CommandResult::ok();
    }

    createdEvent_ = project.createTempoEvent(tick_, beatsPerMinute_);
    if (!createdEvent_.has_value()) {
        return CommandResult::fail("Tempo event could not be created.");
    }

    return CommandResult::ok();
}

void AddTempoEventCommand::undo(Project& project)
{
    if (createdEvent_.has_value()) {
        project.removeTempoEventById(createdEvent_->id);
    }
}

SetTempoEventBpmCommand::SetTempoEventBpmCommand(std::string tempoId, double beatsPerMinute)
    : tempoId_(std::move(tempoId))
    , beatsPerMinute_(beatsPerMinute)
{
}

std::string SetTempoEventBpmCommand::name() const
{
    return "SetTempoEventBpm";
}

CommandResult SetTempoEventBpmCommand::validate(const Project& project) const
{
    if (!isValidTempoBpm(beatsPerMinute_)) {
        return CommandResult::fail("Tempo BPM is invalid.");
    }

    if (!project.findTempoEventById(tempoId_).has_value()) {
        return CommandResult::fail("Tempo event does not exist.");
    }

    return CommandResult::ok();
}

CommandResult SetTempoEventBpmCommand::execute(Project& project)
{
    const auto event = project.findTempoEventById(tempoId_);
    if (!event.has_value()) {
        return CommandResult::fail("Tempo event does not exist.");
    }

    if (!isValidTempoBpm(beatsPerMinute_)) {
        return CommandResult::fail("Tempo BPM is invalid.");
    }

    if (!oldBeatsPerMinute_.has_value()) {
        oldBeatsPerMinute_ = event->beatsPerMinute;
    }

    if (!project.setTempoEventBpm(tempoId_, beatsPerMinute_)) {
        return CommandResult::fail("Tempo BPM could not be changed.");
    }

    return CommandResult::ok();
}

void SetTempoEventBpmCommand::undo(Project& project)
{
    if (oldBeatsPerMinute_.has_value()) {
        project.setTempoEventBpm(tempoId_, *oldBeatsPerMinute_);
    }
}

MoveTempoEventCommand::MoveTempoEventCommand(std::string tempoId, std::int64_t tick)
    : tempoId_(std::move(tempoId))
    , tick_(tick)
{
}

std::string MoveTempoEventCommand::name() const
{
    return "MoveTempoEvent";
}

CommandResult MoveTempoEventCommand::validate(const Project& project) const
{
    const auto event = project.findTempoEventById(tempoId_);
    if (!event.has_value()) {
        return CommandResult::fail("Tempo event does not exist.");
    }

    if (!isValidMarkerTick(tick_)) {
        return CommandResult::fail("Tempo tick is invalid.");
    }

    if (event->tick == 0) {
        return CommandResult::fail("Default tempo event cannot be moved.");
    }

    if (event->tick == tick_) {
        return CommandResult::fail("Tempo event is already at target tick.");
    }

    const auto duplicateTick = std::find_if(project.tempoEvents().begin(), project.tempoEvents().end(), [&](const TempoEvent& otherEvent) {
        return otherEvent.id != tempoId_ && otherEvent.tick == tick_;
    });
    if (duplicateTick != project.tempoEvents().end()) {
        return CommandResult::fail("Tempo tick already exists.");
    }

    return CommandResult::ok();
}

CommandResult MoveTempoEventCommand::execute(Project& project)
{
    const auto event = project.findTempoEventById(tempoId_);
    if (!event.has_value()) {
        return CommandResult::fail("Tempo event does not exist.");
    }

    if (!oldTick_.has_value()) {
        oldTick_ = event->tick;
    }

    if (!project.moveTempoEventToTick(tempoId_, tick_)) {
        return CommandResult::fail("Tempo event could not be moved.");
    }

    return CommandResult::ok();
}

void MoveTempoEventCommand::undo(Project& project)
{
    if (oldTick_.has_value()) {
        project.moveTempoEventToTick(tempoId_, *oldTick_);
    }
}

DeleteTempoEventCommand::DeleteTempoEventCommand(std::string tempoId)
    : tempoId_(std::move(tempoId))
{
}

std::string DeleteTempoEventCommand::name() const
{
    return "DeleteTempoEvent";
}

CommandResult DeleteTempoEventCommand::validate(const Project& project) const
{
    const auto event = project.findTempoEventById(tempoId_);
    if (!event.has_value()) {
        return CommandResult::fail("Tempo event does not exist.");
    }

    if (event->tick == 0) {
        return CommandResult::fail("Default tempo event cannot be deleted.");
    }

    return CommandResult::ok();
}

CommandResult DeleteTempoEventCommand::execute(Project& project)
{
    if (!deletedEvent_.has_value()) {
        const auto event = project.findTempoEventById(tempoId_);
        if (!event.has_value()) {
            return CommandResult::fail("Tempo event does not exist.");
        }
        deletedEvent_ = *event;
    }

    if (!project.removeTempoEventById(deletedEvent_->id)) {
        return CommandResult::fail("Tempo event could not be deleted.");
    }

    return CommandResult::ok();
}

void DeleteTempoEventCommand::undo(Project& project)
{
    if (deletedEvent_.has_value()) {
        project.insertExistingTempoEvent(*deletedEvent_);
    }
}

AddTimeSignatureEventCommand::AddTimeSignatureEventCommand(std::int64_t tick, int numerator, int denominator)
    : tick_(tick)
    , numerator_(numerator)
    , denominator_(denominator)
{
}

std::string AddTimeSignatureEventCommand::name() const
{
    return "AddTimeSignatureEvent";
}

CommandResult AddTimeSignatureEventCommand::validate(const Project& project) const
{
    if (!isValidMarkerTick(tick_)) {
        return CommandResult::fail("Time signature tick is invalid.");
    }

    if (!isValidTimeSignature(numerator_, denominator_)) {
        return CommandResult::fail("Time signature value is invalid.");
    }

    const auto duplicateTick = std::find_if(project.timeSignatureEvents().begin(), project.timeSignatureEvents().end(), [&](const TimeSignatureEvent& event) {
        return event.tick == tick_;
    });
    if (duplicateTick != project.timeSignatureEvents().end()) {
        return CommandResult::fail("Time signature tick already exists.");
    }

    return CommandResult::ok();
}

CommandResult AddTimeSignatureEventCommand::execute(Project& project)
{
    if (createdEvent_.has_value()) {
        if (!project.insertExistingTimeSignatureEvent(*createdEvent_)) {
            return CommandResult::fail("Time signature event already exists or is no longer valid.");
        }
        return CommandResult::ok();
    }

    createdEvent_ = project.createTimeSignatureEvent(tick_, numerator_, denominator_);
    if (!createdEvent_.has_value()) {
        return CommandResult::fail("Time signature event could not be created.");
    }

    return CommandResult::ok();
}

void AddTimeSignatureEventCommand::undo(Project& project)
{
    if (createdEvent_.has_value()) {
        project.removeTimeSignatureEventById(createdEvent_->id);
    }
}

SetTimeSignatureCommand::SetTimeSignatureCommand(std::string eventId, int numerator, int denominator)
    : eventId_(std::move(eventId))
    , numerator_(numerator)
    , denominator_(denominator)
{
}

std::string SetTimeSignatureCommand::name() const
{
    return "SetTimeSignature";
}

CommandResult SetTimeSignatureCommand::validate(const Project& project) const
{
    if (!isValidTimeSignature(numerator_, denominator_)) {
        return CommandResult::fail("Time signature value is invalid.");
    }

    if (!project.findTimeSignatureEventById(eventId_).has_value()) {
        return CommandResult::fail("Time signature event does not exist.");
    }

    return CommandResult::ok();
}

CommandResult SetTimeSignatureCommand::execute(Project& project)
{
    const auto event = project.findTimeSignatureEventById(eventId_);
    if (!event.has_value()) {
        return CommandResult::fail("Time signature event does not exist.");
    }

    if (!isValidTimeSignature(numerator_, denominator_)) {
        return CommandResult::fail("Time signature value is invalid.");
    }

    if (!oldNumerator_.has_value()) {
        oldNumerator_ = event->numerator;
        oldDenominator_ = event->denominator;
    }

    if (!project.setTimeSignature(eventId_, numerator_, denominator_)) {
        return CommandResult::fail("Time signature could not be changed.");
    }

    return CommandResult::ok();
}

void SetTimeSignatureCommand::undo(Project& project)
{
    if (oldNumerator_.has_value() && oldDenominator_.has_value()) {
        project.setTimeSignature(eventId_, *oldNumerator_, *oldDenominator_);
    }
}

MoveTimeSignatureEventCommand::MoveTimeSignatureEventCommand(std::string eventId, std::int64_t tick)
    : eventId_(std::move(eventId))
    , tick_(tick)
{
}

std::string MoveTimeSignatureEventCommand::name() const
{
    return "MoveTimeSignatureEvent";
}

CommandResult MoveTimeSignatureEventCommand::validate(const Project& project) const
{
    const auto event = project.findTimeSignatureEventById(eventId_);
    if (!event.has_value()) {
        return CommandResult::fail("Time signature event does not exist.");
    }

    if (!isValidMarkerTick(tick_)) {
        return CommandResult::fail("Time signature tick is invalid.");
    }

    if (event->tick == 0) {
        return CommandResult::fail("Default time signature event cannot be moved.");
    }

    if (event->tick == tick_) {
        return CommandResult::fail("Time signature event is already at target tick.");
    }

    const auto duplicateTick = std::find_if(project.timeSignatureEvents().begin(), project.timeSignatureEvents().end(), [&](const TimeSignatureEvent& otherEvent) {
        return otherEvent.id != eventId_ && otherEvent.tick == tick_;
    });
    if (duplicateTick != project.timeSignatureEvents().end()) {
        return CommandResult::fail("Time signature tick already exists.");
    }

    return CommandResult::ok();
}

CommandResult MoveTimeSignatureEventCommand::execute(Project& project)
{
    const auto event = project.findTimeSignatureEventById(eventId_);
    if (!event.has_value()) {
        return CommandResult::fail("Time signature event does not exist.");
    }

    if (!oldTick_.has_value()) {
        oldTick_ = event->tick;
    }

    if (!project.moveTimeSignatureEventToTick(eventId_, tick_)) {
        return CommandResult::fail("Time signature event could not be moved.");
    }

    return CommandResult::ok();
}

void MoveTimeSignatureEventCommand::undo(Project& project)
{
    if (oldTick_.has_value()) {
        project.moveTimeSignatureEventToTick(eventId_, *oldTick_);
    }
}

DeleteTimeSignatureEventCommand::DeleteTimeSignatureEventCommand(std::string eventId)
    : eventId_(std::move(eventId))
{
}

std::string DeleteTimeSignatureEventCommand::name() const
{
    return "DeleteTimeSignatureEvent";
}

CommandResult DeleteTimeSignatureEventCommand::validate(const Project& project) const
{
    const auto event = project.findTimeSignatureEventById(eventId_);
    if (!event.has_value()) {
        return CommandResult::fail("Time signature event does not exist.");
    }

    if (event->tick == 0) {
        return CommandResult::fail("Default time signature event cannot be deleted.");
    }

    return CommandResult::ok();
}

CommandResult DeleteTimeSignatureEventCommand::execute(Project& project)
{
    if (!deletedEvent_.has_value()) {
        const auto event = project.findTimeSignatureEventById(eventId_);
        if (!event.has_value()) {
            return CommandResult::fail("Time signature event does not exist.");
        }
        deletedEvent_ = *event;
    }

    if (!project.removeTimeSignatureEventById(deletedEvent_->id)) {
        return CommandResult::fail("Time signature event could not be deleted.");
    }

    return CommandResult::ok();
}

void DeleteTimeSignatureEventCommand::undo(Project& project)
{
    if (deletedEvent_.has_value()) {
        project.insertExistingTimeSignatureEvent(*deletedEvent_);
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

SetTrackViewStateCommand::SetTrackViewStateCommand(std::string trackId, TrackViewState newState)
    : trackId_(std::move(trackId))
    , newState_(newState)
{
}

std::string SetTrackViewStateCommand::name() const
{
    return "SetTrackViewState";
}

CommandResult SetTrackViewStateCommand::validate(const Project& project) const
{
    const auto track = project.findTrackById(trackId_);
    if (!track.has_value()) {
        return CommandResult::fail("Track does not exist.");
    }

    if (!isValidTrackViewState(track->type, newState_)) {
        return CommandResult::fail("Track view state is invalid.");
    }

    return CommandResult::ok();
}

CommandResult SetTrackViewStateCommand::execute(Project& project)
{
    const auto track = project.findTrackById(trackId_);
    if (!track.has_value()) {
        return CommandResult::fail("Track does not exist.");
    }

    if (!isValidTrackViewState(track->type, newState_)) {
        return CommandResult::fail("Track view state is invalid.");
    }

    if (!oldState_.has_value()) {
        oldState_ = track->view;
    }

    if (!project.setTrackViewState(trackId_, newState_)) {
        return CommandResult::fail("Track view state is invalid.");
    }

    return CommandResult::ok();
}

void SetTrackViewStateCommand::undo(Project& project)
{
    if (oldState_.has_value()) {
        project.setTrackViewState(trackId_, *oldState_);
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
