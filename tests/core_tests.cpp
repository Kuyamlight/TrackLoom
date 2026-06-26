#include "AudioDisable.h"
#include "AudioPan.h"
#include "AudioProjectGraph.h"
#include "AudioSolo.h"
#include "AudioTrackPlayback.h"
#include "AudioMute.h"
#include "AudioGain.h"
#include "AudioMixer.h"
#include "AudioEngine.h"
#include "Command.h"
#include "ProjectFile.h"
#include "Project.h"
#include "ProjectSerializer.h"
#include "Transport.h"

#include <filesystem>
#include <iostream>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool containsNonZeroSample(const std::vector<float>& samples)
{
    for (const auto sample : samples) {
        if (std::fabs(sample) > 0.000001f) {
            return true;
        }
    }
    return false;
}

bool allSamplesNear(const std::vector<float>& samples, float expected)
{
    for (const auto sample : samples) {
        if (std::fabs(sample - expected) > 0.000001f) {
            return false;
        }
    }
    return true;
}

bool channelSamplesNear(const trackloom::AudioBlock& block, int channel, float expected)
{
    for (int frame = 0; frame < block.frameCount(); ++frame) {
        if (std::fabs(block.sampleAt(channel, frame) - expected) > 0.000001f) {
            return false;
        }
    }

    return true;
}

class ConstantAudioSource final : public trackloom::AudioSource {
public:
    explicit ConstantAudioSource(float value)
        : value_(value)
    {
    }

    bool render(trackloom::AudioBlock block, double sampleRate) override
    {
        if (!block.isValid() || sampleRate <= 0.0) {
            return false;
        }

        for (int channel = 0; channel < block.channelCount(); ++channel) {
            for (int frame = 0; frame < block.frameCount(); ++frame) {
                block.sampleAt(channel, frame) = value_;
            }
        }

        return true;
    }

private:
    float value_ = 0.0f;
};

class CountingAudioSource final : public trackloom::AudioSource {
public:
    explicit CountingAudioSource(float value)
        : value_(value)
    {
    }

    int renderCount() const
    {
        return renderCount_;
    }

    bool render(trackloom::AudioBlock block, double sampleRate) override
    {
        if (!block.isValid() || sampleRate <= 0.0) {
            return false;
        }

        ++renderCount_;
        for (int channel = 0; channel < block.channelCount(); ++channel) {
            for (int frame = 0; frame < block.frameCount(); ++frame) {
                block.sampleAt(channel, frame) = value_;
            }
        }

        return true;
    }

private:
    float value_ = 0.0f;
    int renderCount_ = 0;
};

std::filesystem::path makeTestDirectory(const std::string& name)
{
    const auto path = std::filesystem::temp_directory_path() / "trackloom_tests" / name;
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

void projectStartsEmpty()
{
    trackloom::Project project("Demo");

    require(project.formatVersion() == trackloom::Project::currentFormatVersion, "format version should match current format");
    require(project.name() == "Demo", "project name should be stored");
    require(project.tracks().empty(), "new project should not contain tracks");
    require(project.clips().empty(), "new project should not contain clips");
}

void addTrackCommandSupportsUndoAndRedo()
{
    trackloom::Project project;
    trackloom::CommandStack commands;

    auto result = commands.execute(
        project,
        std::make_unique<trackloom::AddTrackCommand>("Piano", trackloom::TrackType::Instrument));

    require(result.success, "add track command should succeed");
    require(project.tracks().size() == 1, "project should contain one track");
    require(project.tracks().front().id == "track-1", "first track id should be stable");

    require(commands.undo(project), "undo should be available");
    require(project.tracks().empty(), "undo should remove the track");

    require(commands.redo(project), "redo should be available");
    require(project.tracks().size() == 1, "redo should restore the track");
    require(project.tracks().front().id == "track-1", "redo should preserve track id");
}

void invalidCommandDoesNotModifyProject()
{
    trackloom::Project project;
    trackloom::CommandStack commands;

    auto result = commands.execute(
        project,
        std::make_unique<trackloom::AddTrackCommand>("", trackloom::TrackType::Audio));

    require(!result.success, "empty track name should fail validation");
    require(project.tracks().empty(), "failed command should not modify project");
    require(!commands.canUndo(), "failed command should not enter undo stack");
}

void projectCreatesMidiClipOnInstrumentTrack()
{
    trackloom::Project project("Clips");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);

    const auto clip = project.createClip(track.id, "Intro Melody", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "instrument track should accept midi clip");
    require(clip->id == "clip-1", "first clip id should be stable");
    require(clip->trackId == track.id, "clip should reference owning track");
    require(clip->name == "Intro Melody", "clip should keep name");
    require(clip->type == trackloom::ClipType::Midi, "clip should keep midi type");
    require(clip->startTick == 0, "clip should keep start tick");
    require(clip->lengthTick == 960, "clip should keep length tick");
    require(project.clips().size() == 1, "project should store created clip");
}

void projectCreatesAudioClipOnAudioTrack()
{
    trackloom::Project project("Clips");
    const auto track = project.createTrack("Vocal", trackloom::TrackType::Audio);

    const auto clip = project.createClip(track.id, "Vocal Take", trackloom::ClipType::Audio, 480, 1920);

    require(clip.has_value(), "audio track should accept audio clip");
    require(clip->id == "clip-1", "first audio clip id should be stable");
    require(clip->type == trackloom::ClipType::Audio, "clip should keep audio type");
    require(clip->startTick == 480, "audio clip should keep start tick");
    require(clip->lengthTick == 1920, "audio clip should keep length tick");
}

void projectRejectsInvalidClipRequests()
{
    trackloom::Project project("Clips");
    const auto instrument = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto audio = project.createTrack("Vocal", trackloom::TrackType::Audio);
    const auto folder = project.createTrack("Folder", trackloom::TrackType::Folder);

    require(!project.createClip("missing-track", "Missing", trackloom::ClipType::Midi, 0, 960).has_value(), "clip should reject missing track");
    require(!project.createClip(folder.id, "Folder Clip", trackloom::ClipType::Midi, 0, 960).has_value(), "folder track should reject clips");
    require(!project.createClip(audio.id, "Wrong Midi", trackloom::ClipType::Midi, 0, 960).has_value(), "audio track should reject midi clips");
    require(!project.createClip(instrument.id, "Wrong Audio", trackloom::ClipType::Audio, 0, 960).has_value(), "instrument track should reject audio clips");
    require(!project.createClip(instrument.id, "", trackloom::ClipType::Midi, 0, 960).has_value(), "clip should reject empty names");
    require(!project.createClip(instrument.id, "Negative Start", trackloom::ClipType::Midi, -1, 960).has_value(), "clip should reject negative start tick");
    require(!project.createClip(instrument.id, "Zero Length", trackloom::ClipType::Midi, 0, 0).has_value(), "clip should reject zero length");
    require(project.clips().empty(), "invalid clips should not modify project");
}

void removingTrackRemovesItsClips()
{
    trackloom::Project project("Clips");
    const auto firstTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto secondTrack = project.createTrack("Pad", trackloom::TrackType::Instrument);

    require(project.createClip(firstTrack.id, "Lead Clip", trackloom::ClipType::Midi, 0, 960).has_value(), "first track clip should be created");
    require(project.createClip(secondTrack.id, "Pad Clip", trackloom::ClipType::Midi, 960, 960).has_value(), "second track clip should be created");

    require(project.removeTrackById(firstTrack.id), "track removal should succeed");

    require(project.clips().size() == 1, "removing track should remove only clips on that track");
    require(project.clips().front().trackId == secondTrack.id, "remaining clip should belong to remaining track");
}

void addClipCommandSupportsUndoAndRedo()
{
    trackloom::Project project("Clips");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);

    auto result = commands.execute(
        project,
        std::make_unique<trackloom::AddClipCommand>(track.id, "Intro Melody", trackloom::ClipType::Midi, 0, 960));

    require(result.success, "add clip command should succeed");
    require(project.clips().size() == 1, "command should add clip");
    require(project.clips().front().id == "clip-1", "command should create stable clip id");

    require(commands.undo(project), "clip undo should be available");
    require(project.clips().empty(), "undo should remove clip");

    require(commands.redo(project), "clip redo should be available");
    require(project.clips().size() == 1, "redo should restore clip");
    require(project.clips().front().id == "clip-1", "redo should preserve clip id");
}

void invalidAddClipCommandDoesNotModifyProject()
{
    trackloom::Project project("Clips");
    trackloom::CommandStack commands;
    const auto folder = project.createTrack("Folder", trackloom::TrackType::Folder);

    auto result = commands.execute(
        project,
        std::make_unique<trackloom::AddClipCommand>(folder.id, "Bad Clip", trackloom::ClipType::Midi, 0, 960));

    require(!result.success, "invalid add clip command should fail");
    require(project.clips().empty(), "failed clip command should not modify project");
    require(!commands.canUndo(), "failed clip command should not enter undo stack");
}

void projectCanRenameClip()
{
    trackloom::Project project("Clips");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Intro", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "clip should be created before rename");
    require(project.renameClipById(clip->id, "Verse"), "clip rename should succeed");

    const auto renamedClip = project.findClipById(clip->id);
    require(renamedClip.has_value(), "renamed clip should still exist");
    require(renamedClip->name == "Verse", "clip should keep renamed value");
    require(renamedClip->trackId == track.id, "rename should not change track ownership");
    require(renamedClip->type == trackloom::ClipType::Midi, "rename should not change clip type");
    require(renamedClip->startTick == 0, "rename should not change start tick");
    require(renamedClip->lengthTick == 960, "rename should not change length tick");

    require(!project.renameClipById(clip->id, ""), "empty clip name should fail");
    require(!project.renameClipById("missing-clip", "Name"), "missing clip rename should fail");
    require(project.findClipById(clip->id)->name == "Verse", "failed rename should not modify clip");
}

void projectCanSetClipTiming()
{
    trackloom::Project project("Clips");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Intro", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "clip should be created before timing update");
    require(project.setClipTiming(clip->id, 480, 1920), "clip timing update should succeed");

    const auto retimedClip = project.findClipById(clip->id);
    require(retimedClip.has_value(), "retimed clip should still exist");
    require(retimedClip->name == "Intro", "timing update should not change name");
    require(retimedClip->trackId == track.id, "timing update should not change track ownership");
    require(retimedClip->startTick == 480, "clip should keep new start tick");
    require(retimedClip->lengthTick == 1920, "clip should keep new length tick");

    require(!project.setClipTiming(clip->id, -1, 960), "negative start should fail");
    require(!project.setClipTiming(clip->id, 0, 0), "zero length should fail");
    require(!project.setClipTiming("missing-clip", 0, 960), "missing clip timing should fail");
    require(project.findClipById(clip->id)->startTick == 480, "failed timing update should not change start tick");
    require(project.findClipById(clip->id)->lengthTick == 1920, "failed timing update should not change length tick");
}

void projectCanMoveMidiClipBetweenInstrumentTracks()
{
    trackloom::Project project("Clips");
    const auto sourceTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto targetTrack = project.createTrack("Pad", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(sourceTrack.id, "Intro", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "midi clip should be created before track move");
    require(project.moveClipToTrack(clip->id, targetTrack.id), "midi clip should move to instrument track");

    const auto movedClip = project.findClipById(clip->id);
    require(movedClip.has_value(), "moved midi clip should still exist");
    require(movedClip->id == clip->id, "move should keep clip id");
    require(movedClip->trackId == targetTrack.id, "move should update clip track id");
    require(movedClip->name == "Intro", "move should keep clip name");
    require(movedClip->type == trackloom::ClipType::Midi, "move should keep clip type");
    require(movedClip->startTick == 0, "move should keep start tick");
    require(movedClip->lengthTick == 960, "move should keep length tick");
}

void projectCanMoveAudioClipBetweenAudioTracks()
{
    trackloom::Project project("Clips");
    const auto sourceTrack = project.createTrack("Vocal", trackloom::TrackType::Audio);
    const auto targetTrack = project.createTrack("Guitar", trackloom::TrackType::Audio);
    const auto clip = project.createClip(sourceTrack.id, "Take", trackloom::ClipType::Audio, 480, 1920);

    require(clip.has_value(), "audio clip should be created before track move");
    require(project.moveClipToTrack(clip->id, targetTrack.id), "audio clip should move to audio track");
    require(project.findClipById(clip->id)->trackId == targetTrack.id, "audio clip should keep target audio track");
}

void projectRejectsInvalidClipTrackMoves()
{
    trackloom::Project project("Clips");
    const auto instrument = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto audio = project.createTrack("Vocal", trackloom::TrackType::Audio);
    const auto folder = project.createTrack("Folder", trackloom::TrackType::Folder);
    const auto clip = project.createClip(instrument.id, "Intro", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "clip should be created before invalid track moves");
    require(!project.moveClipToTrack("missing-clip", instrument.id), "missing clip move should fail");
    require(!project.moveClipToTrack(clip->id, "missing-track"), "missing target track move should fail");
    require(!project.moveClipToTrack(clip->id, folder.id), "folder target move should fail");
    require(!project.moveClipToTrack(clip->id, audio.id), "incompatible target track move should fail");
    require(project.findClipById(clip->id)->trackId == instrument.id, "failed moves should not change clip track");
}

void projectCanSplitMidiClipAtInteriorTick()
{
    trackloom::Project project("Clips");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Intro", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "midi clip should be created before split");
    const auto rightClip = project.splitClipAtTick(clip->id, 360);

    require(rightClip.has_value(), "midi clip split should create right clip");
    const auto leftClip = project.findClipById(clip->id);
    require(leftClip.has_value(), "left split clip should still exist");
    require(leftClip->id == clip->id, "left split should keep original id");
    require(leftClip->trackId == track.id, "left split should keep track");
    require(leftClip->name == "Intro", "left split should keep name");
    require(leftClip->type == trackloom::ClipType::Midi, "left split should keep type");
    require(leftClip->startTick == 0, "left split should keep start tick");
    require(leftClip->lengthTick == 360, "left split should end at split tick");
    require(rightClip->id == "clip-2", "right split should receive next stable clip id");
    require(rightClip->trackId == track.id, "right split should keep track");
    require(rightClip->name == "Intro", "right split should keep name");
    require(rightClip->type == trackloom::ClipType::Midi, "right split should keep type");
    require(rightClip->startTick == 360, "right split should start at split tick");
    require(rightClip->lengthTick == 600, "right split should keep remaining length");
    require(project.clips().size() == 2, "project should contain both split clips");
}

void projectCanSplitAudioClipAtInteriorTick()
{
    trackloom::Project project("Clips");
    const auto track = project.createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = project.createClip(track.id, "Take", trackloom::ClipType::Audio, 480, 1920);

    require(clip.has_value(), "audio clip should be created before split");
    const auto rightClip = project.splitClipAtTick(clip->id, 960);

    require(rightClip.has_value(), "audio clip split should create right clip");
    require(project.findClipById(clip->id)->startTick == 480, "audio left split should keep original start");
    require(project.findClipById(clip->id)->lengthTick == 480, "audio left split should keep left length");
    require(rightClip->startTick == 960, "audio right split should start at split tick");
    require(rightClip->lengthTick == 1440, "audio right split should keep remaining length");
    require(rightClip->type == trackloom::ClipType::Audio, "audio right split should keep audio type");
}

void projectRejectsInvalidClipSplits()
{
    trackloom::Project project("Clips");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Intro", trackloom::ClipType::Midi, 120, 480);

    require(clip.has_value(), "clip should be created before invalid splits");
    require(!project.splitClipAtTick("missing-clip", 240).has_value(), "missing clip split should fail");
    require(!project.splitClipAtTick(clip->id, 119).has_value(), "split before start should fail");
    require(!project.splitClipAtTick(clip->id, 120).has_value(), "split at start should fail");
    require(!project.splitClipAtTick(clip->id, 600).has_value(), "split at end should fail");
    require(!project.splitClipAtTick(clip->id, 601).has_value(), "split after end should fail");

    const auto unchangedClip = project.findClipById(clip->id);
    require(unchangedClip.has_value(), "failed split should keep original clip");
    require(unchangedClip->startTick == 120, "failed split should not change start tick");
    require(unchangedClip->lengthTick == 480, "failed split should not change length tick");
    require(project.clips().size() == 1, "failed split should not add clips");
}

void renameClipCommandSupportsUndoAndRedo()
{
    trackloom::Project project("Clips");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Intro", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "clip should be created before command rename");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::RenameClipCommand>(clip->id, "Verse"));

    require(result.success, "rename clip command should succeed");
    require(project.findClipById(clip->id)->name == "Verse", "command should rename clip");

    require(commands.undo(project), "rename clip undo should be available");
    require(project.findClipById(clip->id)->name == "Intro", "undo should restore old clip name");

    require(commands.redo(project), "rename clip redo should be available");
    require(project.findClipById(clip->id)->name == "Verse", "redo should restore new clip name");
}

void invalidRenameClipCommandDoesNotModifyProject()
{
    trackloom::Project project("Clips");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Intro", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "clip should be created before invalid command rename");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::RenameClipCommand>(clip->id, ""));

    require(!result.success, "empty clip rename command should fail");
    require(project.findClipById(clip->id)->name == "Intro", "failed rename command should not modify clip");
    require(!commands.canUndo(), "failed rename command should not enter undo stack");
}

void setClipTimingCommandSupportsUndoAndRedo()
{
    trackloom::Project project("Clips");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Intro", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "clip should be created before command timing update");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::SetClipTimingCommand>(clip->id, 480, 1920));

    require(result.success, "set clip timing command should succeed");
    require(project.findClipById(clip->id)->startTick == 480, "command should update start tick");
    require(project.findClipById(clip->id)->lengthTick == 1920, "command should update length tick");

    require(commands.undo(project), "set clip timing undo should be available");
    require(project.findClipById(clip->id)->startTick == 0, "undo should restore old start tick");
    require(project.findClipById(clip->id)->lengthTick == 960, "undo should restore old length tick");

    require(commands.redo(project), "set clip timing redo should be available");
    require(project.findClipById(clip->id)->startTick == 480, "redo should restore new start tick");
    require(project.findClipById(clip->id)->lengthTick == 1920, "redo should restore new length tick");
}

void invalidSetClipTimingCommandDoesNotModifyProject()
{
    trackloom::Project project("Clips");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Intro", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "clip should be created before invalid command timing update");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::SetClipTimingCommand>(clip->id, 0, 0));

    require(!result.success, "zero-length timing command should fail");
    require(project.findClipById(clip->id)->startTick == 0, "failed timing command should not modify start tick");
    require(project.findClipById(clip->id)->lengthTick == 960, "failed timing command should not modify length tick");
    require(!commands.canUndo(), "failed timing command should not enter undo stack");
}

void deleteClipCommandSupportsUndoAndRedo()
{
    trackloom::Project project("Clips");
    trackloom::CommandStack commands;
    const auto firstTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto secondTrack = project.createTrack("Pad", trackloom::TrackType::Instrument);
    const auto firstClip = project.createClip(firstTrack.id, "Lead Intro", trackloom::ClipType::Midi, 0, 960);
    const auto secondClip = project.createClip(secondTrack.id, "Pad Intro", trackloom::ClipType::Midi, 960, 1920);

    require(firstClip.has_value(), "first clip should be created before delete");
    require(secondClip.has_value(), "second clip should be created before delete");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::DeleteClipCommand>(firstClip->id));

    require(result.success, "delete clip command should succeed");
    require(!project.findClipById(firstClip->id).has_value(), "delete command should remove target clip");
    require(project.findClipById(secondClip->id).has_value(), "delete command should keep unrelated clip");
    require(project.clips().size() == 1, "delete command should remove only one clip");

    require(commands.undo(project), "delete clip undo should be available");
    const auto restoredClip = project.findClipById(firstClip->id);
    require(restoredClip.has_value(), "undo should restore deleted clip");
    require(restoredClip->trackId == firstTrack.id, "undo should restore clip track id");
    require(restoredClip->name == "Lead Intro", "undo should restore clip name");
    require(restoredClip->type == trackloom::ClipType::Midi, "undo should restore clip type");
    require(restoredClip->startTick == 0, "undo should restore clip start tick");
    require(restoredClip->lengthTick == 960, "undo should restore clip length tick");
    require(project.findClipById(secondClip->id).has_value(), "undo should keep unrelated clip");

    require(commands.redo(project), "delete clip redo should be available");
    require(!project.findClipById(firstClip->id).has_value(), "redo should remove target clip again");
    require(project.findClipById(secondClip->id).has_value(), "redo should still keep unrelated clip");
}

void invalidDeleteClipCommandDoesNotModifyProject()
{
    trackloom::Project project("Clips");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Intro", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "clip should be created before invalid delete");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::DeleteClipCommand>("missing-clip"));

    require(!result.success, "missing clip delete command should fail");
    require(project.findClipById(clip->id).has_value(), "failed delete command should not remove clip");
    require(project.clips().size() == 1, "failed delete command should not change clip count");
    require(!commands.canUndo(), "failed delete command should not enter undo stack");
}

void moveClipToTrackCommandSupportsUndoAndRedo()
{
    trackloom::Project project("Clips");
    trackloom::CommandStack commands;
    const auto sourceTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto targetTrack = project.createTrack("Pad", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(sourceTrack.id, "Intro", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "clip should be created before command track move");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::MoveClipToTrackCommand>(clip->id, targetTrack.id));

    require(result.success, "move clip to track command should succeed");
    require(project.findClipById(clip->id)->trackId == targetTrack.id, "command should move clip to target track");

    require(commands.undo(project), "move clip to track undo should be available");
    require(project.findClipById(clip->id)->trackId == sourceTrack.id, "undo should restore source track");

    require(commands.redo(project), "move clip to track redo should be available");
    require(project.findClipById(clip->id)->trackId == targetTrack.id, "redo should restore target track");
}

void invalidMoveClipToTrackCommandDoesNotModifyProject()
{
    trackloom::Project project("Clips");
    trackloom::CommandStack commands;
    const auto instrument = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto audio = project.createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = project.createClip(instrument.id, "Intro", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "clip should be created before invalid command track move");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::MoveClipToTrackCommand>(clip->id, audio.id));

    require(!result.success, "incompatible move clip to track command should fail");
    require(project.findClipById(clip->id)->trackId == instrument.id, "failed move command should not modify clip track");
    require(!commands.canUndo(), "failed move command should not enter undo stack");
}

void splitClipCommandSupportsUndoAndRedo()
{
    trackloom::Project project("Clips");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Intro", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "clip should be created before command split");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::SplitClipCommand>(clip->id, 360));

    require(result.success, "split clip command should succeed");
    require(project.clips().size() == 2, "split command should create two clips");
    const auto rightClipId = project.clips().back().id;
    require(project.findClipById(clip->id)->lengthTick == 360, "split command should shrink left clip");
    require(project.findClipById(rightClipId)->startTick == 360, "split command should create right clip at split tick");

    require(commands.undo(project), "split clip undo should be available");
    require(project.clips().size() == 1, "undo should remove right split clip");
    require(project.findClipById(clip->id)->lengthTick == 960, "undo should restore original left length");
    require(!project.findClipById(rightClipId).has_value(), "undo should remove generated right clip");

    require(commands.redo(project), "split clip redo should be available");
    require(project.clips().size() == 2, "redo should restore right split clip");
    require(project.findClipById(clip->id)->lengthTick == 360, "redo should shrink left clip again");
    require(project.findClipById(rightClipId).has_value(), "redo should reuse original right clip id");
    require(project.findClipById(rightClipId)->lengthTick == 600, "redo should restore right split length");
}

void invalidSplitClipCommandDoesNotModifyProject()
{
    trackloom::Project project("Clips");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Intro", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "clip should be created before invalid command split");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::SplitClipCommand>(clip->id, 960));

    require(!result.success, "split at clip end should fail validation");
    require(project.clips().size() == 1, "failed split command should not add clips");
    require(project.findClipById(clip->id)->lengthTick == 960, "failed split command should not change length");
    require(!commands.canUndo(), "failed split command should not enter undo stack");
}

void newTrackPlaybackStateStartsDefault()
{
    trackloom::Project project("Playback");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);

    require(!track.playback.muted, "new track should start unmuted");
    require(!track.playback.soloed, "new track should start unsoloed");
    require(!track.playback.disabled, "new track should start enabled");
}

void setTrackPlaybackStateCommandSupportsUndoAndRedo()
{
    trackloom::Project project;
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);

    trackloom::TrackPlaybackState state;
    state.muted = true;
    state.soloed = true;
    state.disabled = true;

    auto result = commands.execute(
        project,
        std::make_unique<trackloom::SetTrackPlaybackStateCommand>(track.id, state));

    require(result.success, "playback state command should succeed");
    require(project.findTrackById(track.id)->playback == state, "command should write playback state");

    require(commands.undo(project), "playback state undo should be available");
    require(!project.findTrackById(track.id)->playback.muted, "undo should restore muted state");
    require(!project.findTrackById(track.id)->playback.soloed, "undo should restore soloed state");
    require(!project.findTrackById(track.id)->playback.disabled, "undo should restore disabled state");

    require(commands.redo(project), "playback state redo should be available");
    require(project.findTrackById(track.id)->playback == state, "redo should restore playback state");
}

void invalidPlaybackStateCommandDoesNotModifyProject()
{
    trackloom::Project project;
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);

    trackloom::TrackPlaybackState state;
    state.muted = true;

    auto result = commands.execute(
        project,
        std::make_unique<trackloom::SetTrackPlaybackStateCommand>("missing-track", state));

    require(!result.success, "missing track playback state command should fail");
    require(!project.findTrackById(track.id)->playback.muted, "failed command should not modify playback state");
    require(!commands.canUndo(), "failed playback command should not enter undo stack");
}

void newTrackMixStateStartsDefault()
{
    trackloom::Project project("Mix");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);

    require(track.mix.gain == 1.0f, "new track should start at unity gain");
    require(track.mix.pan == 0.0f, "new track should start centered");
}

void setTrackMixStateCommandSupportsUndoAndRedo()
{
    trackloom::Project project;
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);

    trackloom::TrackMixState state;
    state.gain = 0.25f;
    state.pan = -0.5f;

    auto result = commands.execute(
        project,
        std::make_unique<trackloom::SetTrackMixStateCommand>(track.id, state));

    require(result.success, "mix state command should succeed");
    require(project.findTrackById(track.id)->mix == state, "command should write mix state");

    require(commands.undo(project), "mix state undo should be available");
    require(project.findTrackById(track.id)->mix.gain == 1.0f, "undo should restore unity gain");

    require(commands.redo(project), "mix state redo should be available");
    require(project.findTrackById(track.id)->mix == state, "redo should restore mix state");
}

void invalidTrackMixStateCommandDoesNotModifyProject()
{
    trackloom::Project project;
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);

    trackloom::TrackMixState state;
    state.pan = 1.5f;

    auto result = commands.execute(
        project,
        std::make_unique<trackloom::SetTrackMixStateCommand>(track.id, state));

    require(!result.success, "out-of-range pan mix command should fail");
    require(project.findTrackById(track.id)->mix.gain == 1.0f, "failed command should not modify mix state");
    require(project.findTrackById(track.id)->mix.pan == 0.0f, "failed command should not modify pan state");
    require(!commands.canUndo(), "failed mix command should not enter undo stack");
}

void projectCanRoundTripThroughText()
{
    trackloom::Project project("Song");
    project.createTrack("Piano", trackloom::TrackType::Instrument);
    project.createTrack("Vocal", trackloom::TrackType::Audio);

    const auto saved = trackloom::saveProjectToText(project);
    const auto loaded = trackloom::loadProjectFromText(saved);

    require(loaded.project.has_value(), "saved project should load");
    require(loaded.project->name() == "Song", "loaded project should keep name");
    require(loaded.project->tracks().size() == 2, "loaded project should keep tracks");
    require(loaded.project->tracks()[0].type == trackloom::TrackType::Instrument, "first track type should survive");
    require(loaded.project->tracks()[1].type == trackloom::TrackType::Audio, "second track type should survive");
}

void projectCanRoundTripTrackPlaybackState()
{
    trackloom::Project project("Playback Song");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    trackloom::TrackPlaybackState state;
    state.muted = true;
    state.disabled = true;

    require(project.setTrackPlaybackState(track.id, state), "project should accept playback state update");

    const auto saved = trackloom::saveProjectToText(project);
    const auto loaded = trackloom::loadProjectFromText(saved);

    require(saved.find("trackloom_project 5\n") == 0, "saved project should use format version 5");
    require(loaded.project.has_value(), "project with playback state should load");
    const auto loadedTrack = loaded.project->findTrackById(track.id);
    require(loadedTrack.has_value(), "loaded project should contain track");
    require(loadedTrack->playback == state, "loaded track should keep playback state");
}

void projectCanRoundTripTrackMixState()
{
    trackloom::Project project("Mix Song");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    trackloom::TrackMixState state;
    state.gain = 0.25f;
    state.pan = -0.5f;

    require(project.setTrackMixState(track.id, state), "project should accept mix state update");

    const auto saved = trackloom::saveProjectToText(project);
    const auto loaded = trackloom::loadProjectFromText(saved);

    require(saved.find("trackloom_project 5\n") == 0, "saved mix project should use format version 5");
    require(saved.find("track_mix_state " + track.id + " gain=0.25 pan=-0.5\n") != std::string::npos, "saved project should include track mix state");
    require(loaded.project.has_value(), "project with mix state should load");
    const auto loadedTrack = loaded.project->findTrackById(track.id);
    require(loadedTrack.has_value(), "loaded project should contain mixed track");
    require(loadedTrack->mix == state, "loaded track should keep mix state");
}

void projectCanRoundTripTimelineClips()
{
    trackloom::Project project("Clip Song");
    const auto instrumentTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto audioTrack = project.createTrack("Vocal", trackloom::TrackType::Audio);

    require(project.createClip(instrumentTrack.id, "Intro Melody", trackloom::ClipType::Midi, 0, 960).has_value(), "midi clip should be created");
    require(project.createClip(audioTrack.id, "Vocal Take", trackloom::ClipType::Audio, 960, 1920).has_value(), "audio clip should be created");

    const auto saved = trackloom::saveProjectToText(project);
    const auto loaded = trackloom::loadProjectFromText(saved);

    require(saved.find("trackloom_project 5\n") == 0, "saved clip project should use format version 5");
    require(saved.find("clip clip-1 " + instrumentTrack.id + " Midi 0 960 Intro Melody\n") != std::string::npos, "saved project should include midi clip record");
    require(saved.find("clip clip-2 " + audioTrack.id + " Audio 960 1920 Vocal Take\n") != std::string::npos, "saved project should include audio clip record");
    require(loaded.project.has_value(), "project with clips should load");
    require(loaded.project->clips().size() == 2, "loaded project should keep clips");

    const auto& midiClip = loaded.project->clips()[0];
    require(midiClip.id == "clip-1", "loaded midi clip should keep id");
    require(midiClip.trackId == instrumentTrack.id, "loaded midi clip should keep track id");
    require(midiClip.name == "Intro Melody", "loaded midi clip should keep name");
    require(midiClip.type == trackloom::ClipType::Midi, "loaded midi clip should keep type");
    require(midiClip.startTick == 0, "loaded midi clip should keep start tick");
    require(midiClip.lengthTick == 960, "loaded midi clip should keep length tick");

    const auto& audioClip = loaded.project->clips()[1];
    require(audioClip.id == "clip-2", "loaded audio clip should keep id");
    require(audioClip.trackId == audioTrack.id, "loaded audio clip should keep track id");
    require(audioClip.name == "Vocal Take", "loaded audio clip should keep name");
    require(audioClip.type == trackloom::ClipType::Audio, "loaded audio clip should keep type");
    require(audioClip.startTick == 960, "loaded audio clip should keep start tick");
    require(audioClip.lengthTick == 1920, "loaded audio clip should keep length tick");
}

void projectCanRoundTripTimelineClipEdits()
{
    trackloom::Project project("Edited Clip Song");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Intro", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "clip should be created before edit round trip");
    require(project.renameClipById(clip->id, "Verse"), "clip rename should succeed before save");
    require(project.setClipTiming(clip->id, 480, 1920), "clip timing should update before save");

    const auto saved = trackloom::saveProjectToText(project);
    const auto loaded = trackloom::loadProjectFromText(saved);

    require(saved.find("clip " + clip->id + " " + track.id + " Midi 480 1920 Verse\n") != std::string::npos, "saved project should include edited clip record");
    require(loaded.project.has_value(), "project with edited clip should load");
    require(loaded.project->clips().size() == 1, "loaded project should keep edited clip");
    require(loaded.project->clips().front().name == "Verse", "loaded edited clip should keep name");
    require(loaded.project->clips().front().startTick == 480, "loaded edited clip should keep start tick");
    require(loaded.project->clips().front().lengthTick == 1920, "loaded edited clip should keep length tick");
}

void projectCanSaveAfterTimelineClipDeletion()
{
    trackloom::Project project("Deleted Clip Song");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto firstClip = project.createClip(track.id, "Deleted Intro", trackloom::ClipType::Midi, 0, 960);
    const auto secondClip = project.createClip(track.id, "Kept Verse", trackloom::ClipType::Midi, 960, 960);

    require(firstClip.has_value(), "first clip should be created before save-delete test");
    require(secondClip.has_value(), "second clip should be created before save-delete test");
    require(commands.execute(project, std::make_unique<trackloom::DeleteClipCommand>(firstClip->id)).success, "delete should succeed before save");

    const auto saved = trackloom::saveProjectToText(project);
    const auto loaded = trackloom::loadProjectFromText(saved);

    require(saved.find("clip " + firstClip->id + " ") == std::string::npos, "saved project should omit deleted clip record");
    require(saved.find("clip " + secondClip->id + " " + track.id + " Midi 960 960 Kept Verse\n") != std::string::npos, "saved project should keep unrelated clip record");
    require(loaded.project.has_value(), "project saved after clip deletion should load");
    require(!loaded.project->findClipById(firstClip->id).has_value(), "loaded project should not contain deleted clip");
    require(loaded.project->findClipById(secondClip->id).has_value(), "loaded project should contain kept clip");
    require(loaded.project->clips().size() == 1, "loaded project should contain only kept clip");
}

void projectCanRoundTripTimelineClipTrackMove()
{
    trackloom::Project project("Moved Clip Song");
    const auto sourceTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto targetTrack = project.createTrack("Pad", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(sourceTrack.id, "Moved Intro", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "clip should be created before track move round trip");
    require(project.moveClipToTrack(clip->id, targetTrack.id), "clip should move before save");

    const auto saved = trackloom::saveProjectToText(project);
    const auto loaded = trackloom::loadProjectFromText(saved);

    require(saved.find("clip " + clip->id + " " + targetTrack.id + " Midi 0 960 Moved Intro\n") != std::string::npos, "saved project should include moved clip target track");
    require(loaded.project.has_value(), "project with moved clip should load");
    const auto loadedClip = loaded.project->findClipById(clip->id);
    require(loadedClip.has_value(), "loaded project should keep moved clip");
    require(loadedClip->trackId == targetTrack.id, "loaded moved clip should keep target track");
}

void projectCanRoundTripTimelineClipSplit()
{
    trackloom::Project project("Split Clip Song");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Split Intro", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "clip should be created before split round trip");
    const auto rightClip = project.splitClipAtTick(clip->id, 360);
    require(rightClip.has_value(), "clip should split before save");

    const auto saved = trackloom::saveProjectToText(project);
    const auto loaded = trackloom::loadProjectFromText(saved);

    require(saved.find("clip " + clip->id + " " + track.id + " Midi 0 360 Split Intro\n") != std::string::npos, "saved project should include left split clip");
    require(saved.find("clip " + rightClip->id + " " + track.id + " Midi 360 600 Split Intro\n") != std::string::npos, "saved project should include right split clip");
    require(loaded.project.has_value(), "project with split clips should load");
    require(loaded.project->clips().size() == 2, "loaded project should keep both split clips");
    require(loaded.project->findClipById(clip->id)->lengthTick == 360, "loaded left split should keep shortened length");
    require(loaded.project->findClipById(rightClip->id)->startTick == 360, "loaded right split should keep start tick");
    require(loaded.project->findClipById(rightClip->id)->lengthTick == 600, "loaded right split should keep length");
}

void versionOneProjectLoadsDefaultPlaybackState()
{
    const std::string text =
        "trackloom_project 1\n"
        "name Old Song\n"
        "track track-1 Instrument Lead Piano\n";

    const auto loaded = trackloom::loadProjectFromText(text);

    require(loaded.project.has_value(), "version 1 project should still load");
    const auto track = loaded.project->findTrackById("track-1");
    require(track.has_value(), "version 1 track should load");
    require(!track->playback.muted, "version 1 track should default to unmuted");
    require(!track->playback.soloed, "version 1 track should default to unsoloed");
    require(!track->playback.disabled, "version 1 track should default to enabled");
    require(track->mix.gain == 1.0f, "version 1 track should default to unity gain");
    require(track->mix.pan == 0.0f, "version 1 track should default to centered pan");
}

void versionTwoProjectLoadsDefaultMixState()
{
    const std::string text =
        "trackloom_project 2\n"
        "name Playback Song\n"
        "track track-1 Instrument Lead Piano\n"
        "track_playback_state track-1 muted=1 soloed=0 disabled=0\n";

    const auto loaded = trackloom::loadProjectFromText(text);

    require(loaded.project.has_value(), "version 2 project should still load");
    const auto track = loaded.project->findTrackById("track-1");
    require(track.has_value(), "version 2 track should load");
    require(track->playback.muted, "version 2 playback state should still load");
    require(track->mix.gain == 1.0f, "version 2 track should default to unity gain");
    require(track->mix.pan == 0.0f, "version 2 track should default to centered pan");
}

void versionThreeProjectLoadsDefaultPanState()
{
    const std::string text =
        "trackloom_project 3\n"
        "name Gain Song\n"
        "track track-1 Instrument Lead Piano\n"
        "track_playback_state track-1 muted=0 soloed=0 disabled=0\n"
        "track_mix_state track-1 gain=0.25\n";

    const auto loaded = trackloom::loadProjectFromText(text);

    require(loaded.project.has_value(), "version 3 project should still load");
    const auto track = loaded.project->findTrackById("track-1");
    require(track.has_value(), "version 3 track should load");
    require(track->mix.gain == 0.25f, "version 3 mix gain should still load");
    require(track->mix.pan == 0.0f, "version 3 track should default to centered pan");
}

void versionFourProjectLoadsWithoutClips()
{
    const std::string text =
        "trackloom_project 4\n"
        "name Mix Song\n"
        "track track-1 Instrument Lead Piano\n"
        "track_playback_state track-1 muted=0 soloed=0 disabled=0\n"
        "track_mix_state track-1 gain=0.8 pan=-0.25\n";

    const auto loaded = trackloom::loadProjectFromText(text);

    require(loaded.project.has_value(), "version 4 project should still load");
    require(loaded.project->clips().empty(), "version 4 project should load without clips");
}

void invalidTrackPlaybackStateRecordIsRejected()
{
    const std::string text =
        "trackloom_project 2\n"
        "name Broken Song\n"
        "track_playback_state missing-track muted=1 soloed=0 disabled=0\n";

    const auto loaded = trackloom::loadProjectFromText(text);

    require(!loaded.project.has_value(), "unknown track playback state should fail");
    require(!loaded.error.empty(), "invalid playback state should report an error");
}

void invalidTrackMixStateRecordIsRejected()
{
    const std::string text =
        "trackloom_project 4\n"
        "name Broken Mix Song\n"
        "track track-1 Instrument Lead Piano\n"
        "track_playback_state track-1 muted=0 soloed=0 disabled=0\n"
        "track_mix_state track-1 gain=1 pan=1.5\n";

    const auto loaded = trackloom::loadProjectFromText(text);

    require(!loaded.project.has_value(), "out-of-range track pan state should fail");
    require(!loaded.error.empty(), "invalid mix state should report an error");
}

void invalidClipRecordIsRejected()
{
    const std::string text =
        "trackloom_project 5\n"
        "name Broken Clip Song\n"
        "track track-1 Instrument Lead Piano\n"
        "track_playback_state track-1 muted=0 soloed=0 disabled=0\n"
        "track_mix_state track-1 gain=1 pan=0\n"
        "clip clip-1 track-1 Midi 0 0 Bad Clip\n";

    const auto loaded = trackloom::loadProjectFromText(text);

    require(!loaded.project.has_value(), "zero-length clip record should fail");
    require(!loaded.error.empty(), "invalid clip record should report an error");
}

void projectCanRoundTripNamesWithSpaces()
{
    trackloom::Project project("Demo Song");
    project.createTrack("Lead Piano", trackloom::TrackType::Instrument);

    const auto saved = trackloom::saveProjectToText(project);
    const auto loaded = trackloom::loadProjectFromText(saved);

    require(loaded.project.has_value(), "project names with spaces should load");
    require(loaded.project->name() == "Demo Song", "project name should keep spaces");
    require(loaded.project->tracks().front().name == "Lead Piano", "track name should keep spaces");
}

void invalidTextIsRejected()
{
    const auto loaded = trackloom::loadProjectFromText("broken 99\n");

    require(!loaded.project.has_value(), "invalid project should not load");
    require(!loaded.error.empty(), "invalid project should report an error");
}

void projectCanSaveAndLoadFromFile()
{
    const auto directory = makeTestDirectory("save_and_load");
    const auto path = directory / "song.tlproj";

    trackloom::Project project("Saved Song");
    project.createTrack("Lead Piano", trackloom::TrackType::Instrument);

    const auto saved = trackloom::saveProjectToFileAtomically(project, path);
    const auto loaded = trackloom::loadProjectFromFile(path);

    require(saved.success, "saving project to file should succeed");
    require(loaded.project.has_value(), "saved project file should load");
    require(loaded.project->name() == "Saved Song", "loaded file should keep project name");
    require(loaded.project->tracks().front().name == "Lead Piano", "loaded file should keep track name");
}

void saveCreatesParentDirectories()
{
    const auto directory = makeTestDirectory("nested_parent");
    const auto path = directory / "level1" / "level2" / "song.tlproj";

    trackloom::Project project("Nested Song");
    const auto saved = trackloom::saveProjectToFileAtomically(project, path);

    require(saved.success, "saving should create missing parent directories");
    require(std::filesystem::exists(path), "project file should exist after save");
}

void saveReplacesExistingFile()
{
    const auto directory = makeTestDirectory("replace_existing");
    const auto path = directory / "song.tlproj";

    trackloom::Project first("Old Song");
    trackloom::Project second("New Song");

    require(trackloom::saveProjectToFileAtomically(first, path).success, "initial save should succeed");
    require(trackloom::saveProjectToFileAtomically(second, path).success, "replacement save should succeed");

    const auto loaded = trackloom::loadProjectFromFile(path);
    require(loaded.project.has_value(), "replaced project should load");
    require(loaded.project->name() == "New Song", "replacement should store new content");
}

void loadingMissingFileReportsError()
{
    const auto directory = makeTestDirectory("missing_file");
    const auto loaded = trackloom::loadProjectFromFile(directory / "missing.tlproj");

    require(!loaded.project.has_value(), "missing file should not load");
    require(!loaded.error.empty(), "missing file should report an error");
}

void savingEmptyPathReportsError()
{
    trackloom::Project project("No Path");
    const auto saved = trackloom::saveProjectToFileAtomically(project, {});

    require(!saved.success, "saving to empty path should fail");
    require(!saved.error.empty(), "empty path save should report an error");
}

void transportStartsStoppedAtSampleZero()
{
    trackloom::Transport transport;

    require(!transport.isPlaying(), "transport should start stopped");
    require(transport.currentSample() == 0, "transport should start at sample zero");
    require(transport.sampleRate() == 44100.0, "transport should default to 44100 Hz");
}

void playingTransportAdvancesBySamples()
{
    trackloom::Transport transport;

    transport.play();
    require(transport.advanceBySamples(512), "positive advance should be accepted");

    require(transport.isPlaying(), "advance should not stop playback");
    require(transport.currentSample() == 512, "playing transport should advance by sample count");
}

void stoppedTransportDoesNotAdvance()
{
    trackloom::Transport transport;

    require(transport.advanceBySamples(512), "positive advance should be accepted while stopped");
    require(transport.currentSample() == 0, "stopped transport should not advance");
}

void transportCanSeekBySample()
{
    trackloom::Transport transport;

    require(transport.seekToSample(2048), "sample seek should accept non-negative sample");
    require(transport.currentSample() == 2048, "sample seek should set current sample");
}

void transportCanSeekBySeconds()
{
    trackloom::Transport transport;

    require(transport.setSampleRate(48000.0), "valid sample rate should be accepted");
    require(transport.seekToSeconds(2.5), "second seek should accept non-negative time");

    require(transport.currentSample() == 120000, "second seek should convert using sample rate");
    require(transport.currentSeconds() == 2.5, "current seconds should match sample position");
}

void transportRejectsInvalidValuesWithoutChangingState()
{
    trackloom::Transport transport;

    require(transport.seekToSample(100), "initial seek should succeed");
    require(transport.setSampleRate(48000.0), "initial sample rate should succeed");

    require(!transport.seekToSample(-1), "negative sample seek should fail");
    require(!transport.seekToSeconds(-0.5), "negative second seek should fail");
    require(!transport.advanceBySamples(-128), "negative advance should fail");
    require(!transport.setSampleRate(0.0), "zero sample rate should fail");

    require(transport.currentSample() == 100, "invalid values should not change sample position");
    require(transport.sampleRate() == 48000.0, "invalid sample rate should not replace previous value");
}

void audioBlockCanClearSamples()
{
    std::vector<float> samples { 1.0f, -1.0f, 0.5f, 2.0f };
    trackloom::AudioBlock block(samples.data(), 2, 2);

    block.clear();

    for (const auto sample : samples) {
        require(sample == 0.0f, "audio block clear should zero every sample");
    }
}

void audioEngineAcceptsValidPrepareSettings()
{
    trackloom::AudioEngine engine;

    require(engine.prepare(48000.0, 2, 512), "valid prepare settings should be accepted");
    require(engine.isPrepared(), "engine should be prepared after valid settings");
    require(engine.sampleRate() == 48000.0, "engine should store sample rate");
    require(engine.channelCount() == 2, "engine should store channel count");
    require(engine.maxBlockFrames() == 512, "engine should store max block size");
}

void audioEngineRejectsInvalidPrepareSettings()
{
    trackloom::AudioEngine engine;

    require(!engine.prepare(0.0, 2, 512), "zero sample rate should be rejected");
    require(!engine.prepare(44100.0, 0, 512), "zero channel count should be rejected");
    require(!engine.prepare(44100.0, 2, 0), "zero max block size should be rejected");
    require(!engine.isPrepared(), "invalid prepare settings should not prepare engine");
}

void audioEngineClearsOutputAndAdvancesPlayingTransport()
{
    trackloom::AudioEngine engine;
    trackloom::Transport transport;
    std::vector<float> samples(2 * 4, 1.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(engine.prepare(48000.0, 2, 16), "engine prepare should succeed");
    transport.play();

    require(engine.renderNextBlock(transport, block), "valid render should succeed");

    for (const auto sample : samples) {
        require(sample == 0.0f, "render should clear output to silence");
    }
    require(transport.currentSample() == 4, "playing render should advance by frame count");
    require(transport.sampleRate() == 48000.0, "render should align transport sample rate");
}

void audioEngineClearsOutputWithoutAdvancingStoppedTransport()
{
    trackloom::AudioEngine engine;
    trackloom::Transport transport;
    std::vector<float> samples(2 * 4, 1.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(engine.prepare(44100.0, 2, 16), "engine prepare should succeed");
    require(engine.renderNextBlock(transport, block), "stopped render should still succeed");

    for (const auto sample : samples) {
        require(sample == 0.0f, "stopped render should still clear output");
    }
    require(transport.currentSample() == 0, "stopped render should not advance transport");
}

void audioEngineRejectsInvalidRenderRequestsWithoutAdvancing()
{
    trackloom::AudioEngine engine;
    trackloom::Transport transport;
    std::vector<float> samples(2 * 4, 1.0f);
    trackloom::AudioBlock validBlock(samples.data(), 2, 4);
    trackloom::AudioBlock wrongChannels(samples.data(), 1, 4);
    trackloom::AudioBlock tooLarge(samples.data(), 2, 4);

    transport.play();
    require(!engine.renderNextBlock(transport, validBlock), "unprepared render should fail");
    require(transport.currentSample() == 0, "failed render should not advance transport");

    require(engine.prepare(44100.0, 2, 2), "engine prepare should succeed");
    require(!engine.renderNextBlock(transport, wrongChannels), "channel mismatch should fail");
    require(!engine.renderNextBlock(transport, tooLarge), "oversized block should fail");
    require(transport.currentSample() == 0, "invalid render requests should not advance transport");
}

void audioEngineRendersToneWhileTransportIsPlaying()
{
    trackloom::AudioEngine engine;
    trackloom::Transport transport;
    trackloom::SineToneSource tone;
    std::vector<float> samples(2 * 32, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 32);

    require(engine.prepare(48000.0, 2, 64), "engine prepare should succeed");
    require(tone.setFrequency(440.0), "valid tone frequency should be accepted");
    require(tone.setGain(0.25f), "valid tone gain should be accepted");

    transport.play();
    require(engine.renderNextBlock(transport, block, &tone), "playing render should accept a tone source");

    require(containsNonZeroSample(samples), "playing tone render should produce non-zero samples");
    require(transport.currentSample() == 32, "tone render should advance playing transport");
}

void audioEngineDoesNotRenderToneWhileStopped()
{
    trackloom::AudioEngine engine;
    trackloom::Transport transport;
    trackloom::SineToneSource tone;
    std::vector<float> samples(2 * 32, 1.0f);
    trackloom::AudioBlock block(samples.data(), 2, 32);

    require(engine.prepare(48000.0, 2, 64), "engine prepare should succeed");
    require(tone.setFrequency(440.0), "valid tone frequency should be accepted");
    require(tone.setGain(0.25f), "valid tone gain should be accepted");

    require(engine.renderNextBlock(transport, block, &tone), "stopped render should accept a tone source");

    require(!containsNonZeroSample(samples), "stopped tone render should leave silence");
    require(transport.currentSample() == 0, "stopped tone render should not advance transport");
}

void sineToneSourceRejectsInvalidParameters()
{
    trackloom::SineToneSource tone;

    require(!tone.setFrequency(0.0), "zero tone frequency should be rejected");
    require(!tone.setFrequency(-440.0), "negative tone frequency should be rejected");
    require(!tone.setGain(-0.1f), "negative tone gain should be rejected");

    require(tone.setFrequency(440.0), "valid tone frequency should still be accepted");
    require(tone.setGain(0.0f), "zero tone gain should be accepted for silence");
}

void sourceMixerSumsPreparedSources()
{
    trackloom::SourceMixer mixer;
    ConstantAudioSource first(0.25f);
    ConstantAudioSource second(0.50f);
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(mixer.prepare(2, 8), "valid mixer prepare should succeed");
    require(mixer.addSource(&first), "first source should be accepted");
    require(mixer.addSource(&second), "second source should be accepted");

    require(mixer.render(block, 48000.0), "mixer render should succeed");

    require(mixer.sourceCount() == 2, "mixer should report added source count");
    require(allSamplesNear(samples, 0.75f), "mixer should sum source samples");
}

void sourceMixerRendersSilenceWhenEmpty()
{
    trackloom::SourceMixer mixer;
    std::vector<float> samples(2 * 4, 1.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(mixer.prepare(2, 8), "valid mixer prepare should succeed");
    require(mixer.render(block, 44100.0), "empty mixer render should succeed");

    require(allSamplesNear(samples, 0.0f), "empty mixer should clear output to silence");
}

void sourceMixerRejectsInvalidRequests()
{
    trackloom::SourceMixer mixer;
    ConstantAudioSource source(0.25f);
    std::vector<float> samples(2 * 4, 0.0f);
    std::vector<float> largeSamples(2 * 8, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);
    trackloom::AudioBlock wrongChannels(samples.data(), 1, 4);
    trackloom::AudioBlock tooLarge(largeSamples.data(), 2, 8);

    require(!mixer.prepare(0, 8), "zero mixer channel count should be rejected");
    require(!mixer.prepare(2, 0), "zero mixer max block size should be rejected");
    require(!mixer.addSource(nullptr), "null mixer source should be rejected");
    require(!mixer.render(block, 48000.0), "unprepared mixer render should fail");

    require(mixer.prepare(2, 4), "valid mixer prepare should succeed");
    require(mixer.addSource(&source), "valid source should be accepted");

    require(!mixer.render(wrongChannels, 48000.0), "mixer should reject channel mismatch");
    require(!mixer.render(tooLarge, 48000.0), "mixer should reject oversized block");
    require(!mixer.render(block, 0.0), "mixer should reject invalid sample rate");
}

void audioEngineRendersSourceMixerWhilePlaying()
{
    trackloom::AudioEngine engine;
    trackloom::Transport transport;
    trackloom::SourceMixer mixer;
    ConstantAudioSource first(0.20f);
    ConstantAudioSource second(0.30f);
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(engine.prepare(48000.0, 2, 16), "engine prepare should succeed");
    require(mixer.prepare(2, 16), "mixer prepare should succeed");
    require(mixer.addSource(&first), "first mixer source should be accepted");
    require(mixer.addSource(&second), "second mixer source should be accepted");

    transport.play();
    require(engine.renderNextBlock(transport, block, &mixer), "engine should render a mixer source");

    require(allSamplesNear(samples, 0.50f), "engine should receive summed mixer output");
    require(transport.currentSample() == 4, "engine should advance transport after mixer render");
}

void gainAudioSourceUsesUnityGainByDefault()
{
    ConstantAudioSource source(0.50f);
    trackloom::GainAudioSource gain;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(gain.setSource(&source), "gain source should accept a valid source");
    require(gain.render(block, 48000.0), "gain source render should succeed");

    require(allSamplesNear(samples, 0.50f), "default unity gain should preserve source samples");
}

void gainAudioSourceAppliesLinearGain()
{
    ConstantAudioSource source(0.50f);
    trackloom::GainAudioSource gain;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(gain.setSource(&source), "gain source should accept a valid source");
    require(gain.setGain(0.25f), "finite non-negative gain should be accepted");
    require(gain.render(block, 48000.0), "gain source render should succeed");

    require(allSamplesNear(samples, 0.125f), "gain source should scale samples linearly");
}

void gainAudioSourceCanMuteWithZeroGain()
{
    ConstantAudioSource source(0.50f);
    trackloom::GainAudioSource gain;
    std::vector<float> samples(2 * 4, 1.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(gain.setSource(&source), "gain source should accept a valid source");
    require(gain.setGain(0.0f), "zero gain should be accepted");
    require(gain.render(block, 48000.0), "gain source render should succeed");

    require(allSamplesNear(samples, 0.0f), "zero gain should mute source samples");
}

void gainAudioSourceRejectsInvalidSetup()
{
    ConstantAudioSource source(0.50f);
    trackloom::GainAudioSource gain;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(!gain.render(block, 48000.0), "gain source without input source should fail");
    require(!gain.setSource(nullptr), "null wrapped source should be rejected");
    require(gain.setSource(&source), "valid wrapped source should be accepted");
    require(gain.setGain(2.0f), "initial valid gain should be accepted");

    require(!gain.setGain(-1.0f), "negative gain should be rejected");
    require(!gain.setGain(std::numeric_limits<float>::infinity()), "infinite gain should be rejected");
    require(gain.render(block, 48000.0), "render after rejected gain should still succeed");

    require(allSamplesNear(samples, 1.0f), "rejected gain should not replace previous valid gain");
}

void sourceMixerSumsGainWrappedSources()
{
    ConstantAudioSource firstSource(1.0f);
    ConstantAudioSource secondSource(1.0f);
    trackloom::GainAudioSource firstGain;
    trackloom::GainAudioSource secondGain;
    trackloom::SourceMixer mixer;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(firstGain.setSource(&firstSource), "first gain source should accept input");
    require(secondGain.setSource(&secondSource), "second gain source should accept input");
    require(firstGain.setGain(0.25f), "first gain value should be accepted");
    require(secondGain.setGain(0.50f), "second gain value should be accepted");
    require(mixer.prepare(2, 8), "mixer prepare should succeed");
    require(mixer.addSource(&firstGain), "first gain source should be mixable");
    require(mixer.addSource(&secondGain), "second gain source should be mixable");

    require(mixer.render(block, 48000.0), "mixer should render gain-wrapped sources");

    require(allSamplesNear(samples, 0.75f), "mixer should sum gain-wrapped source outputs");
}

void panAudioSourcePassesThroughCenteredByDefault()
{
    ConstantAudioSource source(0.50f);
    trackloom::PanAudioSource pan;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(pan.setSource(&source), "pan source should accept a valid source");
    require(pan.pan() == 0.0f, "pan source should start centered");
    require(pan.render(block, 48000.0), "centered pan render should succeed");

    require(channelSamplesNear(block, 0, 0.50f), "centered pan should preserve left channel");
    require(channelSamplesNear(block, 1, 0.50f), "centered pan should preserve right channel");
}

void panAudioSourceCanMoveFullyLeft()
{
    ConstantAudioSource source(0.50f);
    trackloom::PanAudioSource pan;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(pan.setSource(&source), "pan source should accept a valid source");
    require(pan.setPan(-1.0f), "full-left pan should be accepted");
    require(pan.render(block, 48000.0), "full-left pan render should succeed");

    require(channelSamplesNear(block, 0, 0.50f), "full-left pan should keep left channel");
    require(channelSamplesNear(block, 1, 0.0f), "full-left pan should mute right channel");
}

void panAudioSourceCanMoveFullyRight()
{
    ConstantAudioSource source(0.50f);
    trackloom::PanAudioSource pan;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(pan.setSource(&source), "pan source should accept a valid source");
    require(pan.setPan(1.0f), "full-right pan should be accepted");
    require(pan.render(block, 48000.0), "full-right pan render should succeed");

    require(channelSamplesNear(block, 0, 0.0f), "full-right pan should mute left channel");
    require(channelSamplesNear(block, 1, 0.50f), "full-right pan should keep right channel");
}

void panAudioSourceCanMoveHalfLeft()
{
    ConstantAudioSource source(0.50f);
    trackloom::PanAudioSource pan;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(pan.setSource(&source), "pan source should accept a valid source");
    require(pan.setPan(-0.5f), "half-left pan should be accepted");
    require(pan.render(block, 48000.0), "half-left pan render should succeed");

    require(channelSamplesNear(block, 0, 0.50f), "half-left pan should keep left channel");
    require(channelSamplesNear(block, 1, 0.25f), "half-left pan should reduce right channel");
}

void panAudioSourceRejectsInvalidSetup()
{
    ConstantAudioSource source(0.50f);
    trackloom::PanAudioSource pan;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(!pan.render(block, 48000.0), "pan source without input source should fail");
    require(!pan.setSource(nullptr), "null pan input source should be rejected");
    require(pan.setSource(&source), "valid pan input source should be accepted");
    require(pan.setPan(0.25f), "initial valid pan should be accepted");

    require(!pan.setPan(-1.1f), "pan less than full-left should be rejected");
    require(!pan.setPan(1.1f), "pan greater than full-right should be rejected");
    require(!pan.setPan(std::numeric_limits<float>::infinity()), "infinite pan should be rejected");
    require(pan.render(block, 48000.0), "render after rejected pan should still succeed");

    require(channelSamplesNear(block, 0, 0.375f), "rejected pan should not replace previous left scale");
    require(channelSamplesNear(block, 1, 0.50f), "rejected pan should not replace previous right scale");
}

void sourceMixerSumsPanWrappedSources()
{
    ConstantAudioSource firstSource(1.0f);
    ConstantAudioSource secondSource(1.0f);
    trackloom::PanAudioSource firstPan;
    trackloom::PanAudioSource secondPan;
    trackloom::SourceMixer mixer;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(firstPan.setSource(&firstSource), "first pan source should accept input");
    require(secondPan.setSource(&secondSource), "second pan source should accept input");
    require(firstPan.setPan(-1.0f), "first source should pan left");
    require(secondPan.setPan(1.0f), "second source should pan right");
    require(mixer.prepare(2, 8), "mixer prepare should succeed");
    require(mixer.addSource(&firstPan), "first pan source should be mixable");
    require(mixer.addSource(&secondPan), "second pan source should be mixable");

    require(mixer.render(block, 48000.0), "mixer should render pan-wrapped sources");

    require(channelSamplesNear(block, 0, 1.0f), "mixer should keep left-panned source in left channel");
    require(channelSamplesNear(block, 1, 1.0f), "mixer should keep right-panned source in right channel");
}

void muteAudioSourcePassesThroughByDefault()
{
    ConstantAudioSource source(0.50f);
    trackloom::MuteAudioSource mute;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(mute.setSource(&source), "mute source should accept a valid source");
    require(!mute.isMuted(), "mute source should start unmuted");
    require(mute.render(block, 48000.0), "unmuted render should succeed");

    require(allSamplesNear(samples, 0.50f), "unmuted source should pass through samples");
}

void muteAudioSourceClearsOutputWhenMuted()
{
    ConstantAudioSource source(0.50f);
    trackloom::MuteAudioSource mute;
    std::vector<float> samples(2 * 4, 1.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(mute.setSource(&source), "mute source should accept a valid source");
    mute.setMuted(true);
    require(mute.isMuted(), "mute source should report muted state");
    require(mute.render(block, 48000.0), "muted render should succeed");

    require(allSamplesNear(samples, 0.0f), "muted source should clear output");
}

void muteAudioSourceStillProcessesWrappedSourceWhenMuted()
{
    CountingAudioSource source(0.50f);
    trackloom::MuteAudioSource mute;
    std::vector<float> samples(2 * 4, 1.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(mute.setSource(&source), "mute source should accept a valid source");
    mute.setMuted(true);
    require(mute.render(block, 48000.0), "muted render should succeed");

    require(source.renderCount() == 1, "muted source should still process wrapped source");
    require(allSamplesNear(samples, 0.0f), "muted source should clear processed output");
}

void muteAudioSourceCanUnmuteAfterMutedRender()
{
    CountingAudioSource source(0.50f);
    trackloom::MuteAudioSource mute;
    std::vector<float> samples(2 * 4, 1.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(mute.setSource(&source), "mute source should accept a valid source");
    mute.setMuted(true);
    require(mute.render(block, 48000.0), "muted render should succeed");
    mute.setMuted(false);
    require(mute.render(block, 48000.0), "unmuted render should succeed after muted render");

    require(source.renderCount() == 2, "source should process both muted and unmuted renders");
    require(allSamplesNear(samples, 0.50f), "unmuted source should restore source output");
}

void muteAudioSourceRejectsInvalidSetup()
{
    trackloom::MuteAudioSource mute;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(!mute.render(block, 48000.0), "mute source without input source should fail");
    require(!mute.setSource(nullptr), "null wrapped source should be rejected");
}

void sourceMixerSumsMutedAndUnmutedSources()
{
    ConstantAudioSource firstSource(0.25f);
    ConstantAudioSource secondSource(0.50f);
    trackloom::MuteAudioSource firstMute;
    trackloom::MuteAudioSource secondMute;
    trackloom::SourceMixer mixer;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(firstMute.setSource(&firstSource), "first mute source should accept input");
    require(secondMute.setSource(&secondSource), "second mute source should accept input");
    secondMute.setMuted(true);
    require(mixer.prepare(2, 8), "mixer prepare should succeed");
    require(mixer.addSource(&firstMute), "first mute source should be mixable");
    require(mixer.addSource(&secondMute), "second mute source should be mixable");

    require(mixer.render(block, 48000.0), "mixer should render mute-wrapped sources");

    require(allSamplesNear(samples, 0.25f), "mixer should sum only audible muted-wrapper outputs");
}

void disabledAudioSourcePassesThroughByDefault()
{
    CountingAudioSource source(0.50f);
    trackloom::DisabledAudioSource disabled;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(disabled.setSource(&source), "disabled source should accept a valid source");
    require(!disabled.isDisabled(), "disabled source should start enabled");
    require(disabled.render(block, 48000.0), "enabled disabled-source render should succeed");

    require(source.renderCount() == 1, "enabled disabled-source should process wrapped source");
    require(allSamplesNear(samples, 0.50f), "enabled disabled-source should pass through samples");
}

void disabledAudioSourceClearsOutputWithoutProcessingSource()
{
    CountingAudioSource source(0.50f);
    trackloom::DisabledAudioSource disabled;
    std::vector<float> samples(2 * 4, 1.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(disabled.setSource(&source), "disabled source should accept a valid source");
    disabled.setDisabled(true);
    require(disabled.isDisabled(), "disabled source should report disabled state");
    require(disabled.render(block, 48000.0), "disabled render should succeed");

    require(source.renderCount() == 0, "disabled source should skip wrapped source processing");
    require(allSamplesNear(samples, 0.0f), "disabled source should clear output");
}

void disabledAudioSourceCanReEnableAfterDisabledRender()
{
    CountingAudioSource source(0.50f);
    trackloom::DisabledAudioSource disabled;
    std::vector<float> samples(2 * 4, 1.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(disabled.setSource(&source), "disabled source should accept a valid source");
    disabled.setDisabled(true);
    require(disabled.render(block, 48000.0), "disabled render should succeed");
    disabled.setDisabled(false);
    require(disabled.render(block, 48000.0), "re-enabled render should succeed");

    require(source.renderCount() == 1, "source should only process after re-enabled render");
    require(allSamplesNear(samples, 0.50f), "re-enabled source should restore source output");
}

void disabledAudioSourceRejectsInvalidSetup()
{
    trackloom::DisabledAudioSource disabled;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(!disabled.render(block, 48000.0), "disabled source without input source should fail");
    require(!disabled.setSource(nullptr), "null wrapped source should be rejected");
}

void sourceMixerSumsDisabledAndEnabledSources()
{
    CountingAudioSource firstSource(0.25f);
    CountingAudioSource secondSource(0.50f);
    trackloom::DisabledAudioSource firstDisabled;
    trackloom::DisabledAudioSource secondDisabled;
    trackloom::SourceMixer mixer;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(firstDisabled.setSource(&firstSource), "first disabled source should accept input");
    require(secondDisabled.setSource(&secondSource), "second disabled source should accept input");
    secondDisabled.setDisabled(true);
    require(mixer.prepare(2, 8), "mixer prepare should succeed");
    require(mixer.addSource(&firstDisabled), "first disabled source should be mixable");
    require(mixer.addSource(&secondDisabled), "second disabled source should be mixable");

    require(mixer.render(block, 48000.0), "mixer should render disabled-wrapper sources");

    require(firstSource.renderCount() == 1, "enabled source should be processed by mixer");
    require(secondSource.renderCount() == 0, "disabled source should be skipped by mixer");
    require(allSamplesNear(samples, 0.25f), "mixer should sum only enabled disabled-wrapper outputs");
}

void soloAudioSourcePassesThroughWhenSoloModeInactive()
{
    CountingAudioSource source(0.50f);
    trackloom::SoloAudioSource solo;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(solo.setSource(&source), "solo source should accept a valid source");
    require(!solo.isSoloed(), "solo source should start not soloed");
    require(!solo.isSoloModeActive(), "solo mode should start inactive");
    require(solo.render(block, 48000.0), "inactive solo mode render should succeed");

    require(source.renderCount() == 1, "inactive solo mode should process wrapped source");
    require(allSamplesNear(samples, 0.50f), "inactive solo mode should pass through samples");
}

void soloAudioSourcePassesThroughWhenSoloedInSoloMode()
{
    CountingAudioSource source(0.50f);
    trackloom::SoloAudioSource solo;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(solo.setSource(&source), "solo source should accept a valid source");
    solo.setSoloed(true);
    solo.setSoloModeActive(true);
    require(solo.render(block, 48000.0), "soloed render should succeed in solo mode");

    require(source.renderCount() == 1, "soloed source should process wrapped source");
    require(allSamplesNear(samples, 0.50f), "soloed source should pass through in solo mode");
}

void soloAudioSourceClearsUnsoloedOutputWhileProcessing()
{
    CountingAudioSource source(0.50f);
    trackloom::SoloAudioSource solo;
    std::vector<float> samples(2 * 4, 1.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(solo.setSource(&source), "solo source should accept a valid source");
    solo.setSoloModeActive(true);
    require(solo.render(block, 48000.0), "unsoloed render should succeed in solo mode");

    require(source.renderCount() == 1, "unsoloed source should still process in solo mode");
    require(allSamplesNear(samples, 0.0f), "unsoloed source should be inaudible in solo mode");
}

void soloAudioSourceCanLeaveSoloMode()
{
    CountingAudioSource source(0.50f);
    trackloom::SoloAudioSource solo;
    std::vector<float> samples(2 * 4, 1.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(solo.setSource(&source), "solo source should accept a valid source");
    solo.setSoloModeActive(true);
    require(solo.render(block, 48000.0), "unsoloed render should succeed in solo mode");
    solo.setSoloModeActive(false);
    require(solo.render(block, 48000.0), "render should succeed after leaving solo mode");

    require(source.renderCount() == 2, "source should process both solo-mode and normal renders");
    require(allSamplesNear(samples, 0.50f), "leaving solo mode should restore source output");
}

void soloAudioSourceRejectsInvalidSetup()
{
    trackloom::SoloAudioSource solo;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(!solo.render(block, 48000.0), "solo source without input source should fail");
    require(!solo.setSource(nullptr), "null wrapped source should be rejected");
}

void sourceMixerSumsOnlyAudibleSoloSources()
{
    CountingAudioSource firstSource(0.25f);
    CountingAudioSource secondSource(0.50f);
    trackloom::SoloAudioSource firstSolo;
    trackloom::SoloAudioSource secondSolo;
    trackloom::SourceMixer mixer;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(firstSolo.setSource(&firstSource), "first solo source should accept input");
    require(secondSolo.setSource(&secondSource), "second solo source should accept input");
    firstSolo.setSoloed(true);
    firstSolo.setSoloModeActive(true);
    secondSolo.setSoloModeActive(true);
    require(mixer.prepare(2, 8), "mixer prepare should succeed");
    require(mixer.addSource(&firstSolo), "first solo source should be mixable");
    require(mixer.addSource(&secondSolo), "second solo source should be mixable");

    require(mixer.render(block, 48000.0), "mixer should render solo-wrapper sources");

    require(firstSource.renderCount() == 1, "soloed source should be processed by mixer");
    require(secondSource.renderCount() == 1, "unsoloed source should still be processed by mixer");
    require(allSamplesNear(samples, 0.25f), "mixer should sum only audible solo-wrapper outputs");
}

void trackPlaybackAudioSourcePassesThroughDefaultState()
{
    CountingAudioSource source(0.50f);
    trackloom::TrackPlaybackAudioSource playback;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(playback.setSource(&source), "track playback source should accept input");
    require(playback.render(block, 48000.0), "default playback render should succeed");

    require(source.renderCount() == 1, "default playback should process source");
    require(allSamplesNear(samples, 0.50f), "default playback should pass through source output");
}

void trackPlaybackAudioSourceMutesWhileProcessing()
{
    CountingAudioSource source(0.50f);
    trackloom::TrackPlaybackAudioSource playback;
    trackloom::TrackPlaybackState state;
    std::vector<float> samples(2 * 4, 1.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    state.muted = true;
    require(playback.setSource(&source), "track playback source should accept input");
    playback.setPlaybackState(state);
    require(playback.render(block, 48000.0), "muted playback render should succeed");

    require(source.renderCount() == 1, "muted playback should still process source");
    require(allSamplesNear(samples, 0.0f), "muted playback should clear output");
}

void trackPlaybackAudioSourceDisablesWithoutProcessing()
{
    CountingAudioSource source(0.50f);
    trackloom::TrackPlaybackAudioSource playback;
    trackloom::TrackPlaybackState state;
    std::vector<float> samples(2 * 4, 1.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    state.disabled = true;
    require(playback.setSource(&source), "track playback source should accept input");
    playback.setPlaybackState(state);
    require(playback.render(block, 48000.0), "disabled playback render should succeed");

    require(source.renderCount() == 0, "disabled playback should skip source processing");
    require(allSamplesNear(samples, 0.0f), "disabled playback should clear output");
}

void trackPlaybackAudioSourceSilencesUnsoloedTrackInSoloMode()
{
    CountingAudioSource source(0.50f);
    trackloom::TrackPlaybackAudioSource playback;
    std::vector<float> samples(2 * 4, 1.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(playback.setSource(&source), "track playback source should accept input");
    playback.setSoloModeActive(true);
    require(playback.render(block, 48000.0), "unsoloed solo-mode render should succeed");

    require(source.renderCount() == 1, "unsoloed solo-mode playback should still process source");
    require(allSamplesNear(samples, 0.0f), "unsoloed solo-mode playback should clear output");
}

void trackPlaybackAudioSourcePassesSoloedTrackInSoloMode()
{
    CountingAudioSource source(0.50f);
    trackloom::TrackPlaybackAudioSource playback;
    trackloom::TrackPlaybackState state;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    state.soloed = true;
    require(playback.setSource(&source), "track playback source should accept input");
    playback.setPlaybackState(state);
    playback.setSoloModeActive(true);
    require(playback.render(block, 48000.0), "soloed solo-mode render should succeed");

    require(source.renderCount() == 1, "soloed solo-mode playback should process source");
    require(allSamplesNear(samples, 0.50f), "soloed solo-mode playback should pass through output");
}

void sourceMixerSumsTrackPlaybackAudioSources()
{
    CountingAudioSource firstSource(0.25f);
    CountingAudioSource secondSource(0.50f);
    trackloom::TrackPlaybackAudioSource firstPlayback;
    trackloom::TrackPlaybackAudioSource secondPlayback;
    trackloom::TrackPlaybackState secondState;
    trackloom::SourceMixer mixer;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    secondState.muted = true;
    require(firstPlayback.setSource(&firstSource), "first playback source should accept input");
    require(secondPlayback.setSource(&secondSource), "second playback source should accept input");
    secondPlayback.setPlaybackState(secondState);
    require(mixer.prepare(2, 8), "mixer prepare should succeed");
    require(mixer.addSource(&firstPlayback), "first playback source should be mixable");
    require(mixer.addSource(&secondPlayback), "second playback source should be mixable");

    require(mixer.render(block, 48000.0), "mixer should render track playback sources");

    require(firstSource.renderCount() == 1, "audible playback source should be processed");
    require(secondSource.renderCount() == 1, "muted playback source should still be processed");
    require(allSamplesNear(samples, 0.25f), "mixer should sum only audible track playback outputs");
}

void trackPlaybackAudioSourceRejectsInvalidSetup()
{
    trackloom::TrackPlaybackAudioSource playback;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(!playback.render(block, 48000.0), "track playback source without input should fail");
    require(!playback.setSource(nullptr), "track playback source should reject null input");
}

void projectPlaybackGraphSumsBoundTracks()
{
    trackloom::Project project("Graph");
    const auto firstTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto secondTrack = project.createTrack("Pad", trackloom::TrackType::Instrument);
    ConstantAudioSource firstSource(0.25f);
    ConstantAudioSource secondSource(0.50f);
    trackloom::ProjectPlaybackGraph graph;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(graph.prepare(2, 8), "project graph prepare should succeed");
    require(graph.rebuild(project, {
        { firstTrack.id, &firstSource },
        { secondTrack.id, &secondSource },
    }), "project graph rebuild should accept valid bindings");
    require(graph.render(block, 48000.0), "project graph render should succeed");

    require(graph.sourceCount() == 2, "project graph should expose bound source count");
    require(!graph.isSoloModeActive(), "project graph should start without solo mode");
    require(allSamplesNear(samples, 0.75f), "project graph should sum bound tracks");
}

void projectPlaybackGraphAppliesProjectSoloMode()
{
    trackloom::Project project("Graph");
    const auto firstTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto secondTrack = project.createTrack("Pad", trackloom::TrackType::Instrument);
    CountingAudioSource firstSource(0.25f);
    CountingAudioSource secondSource(0.50f);
    trackloom::TrackPlaybackState secondState;
    trackloom::ProjectPlaybackGraph graph;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    secondState.soloed = true;
    require(project.setTrackPlaybackState(secondTrack.id, secondState), "project should set solo state");
    require(graph.prepare(2, 8), "project graph prepare should succeed");
    require(graph.rebuild(project, {
        { firstTrack.id, &firstSource },
        { secondTrack.id, &secondSource },
    }), "project graph rebuild should accept solo bindings");
    require(graph.render(block, 48000.0), "project graph solo render should succeed");

    require(graph.isSoloModeActive(), "project graph should detect solo mode from project");
    require(firstSource.renderCount() == 1, "unsoloed source should still process in solo mode");
    require(secondSource.renderCount() == 1, "soloed source should process in solo mode");
    require(allSamplesNear(samples, 0.50f), "project graph should output only soloed track");
}

void projectPlaybackGraphAppliesDisabledState()
{
    trackloom::Project project("Graph");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    CountingAudioSource source(0.50f);
    trackloom::TrackPlaybackState state;
    trackloom::ProjectPlaybackGraph graph;
    std::vector<float> samples(2 * 4, 1.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    state.disabled = true;
    require(project.setTrackPlaybackState(track.id, state), "project should set disabled state");
    require(graph.prepare(2, 8), "project graph prepare should succeed");
    require(graph.rebuild(project, { { track.id, &source } }), "project graph rebuild should accept disabled binding");
    require(graph.render(block, 48000.0), "project graph disabled render should succeed");

    require(source.renderCount() == 0, "disabled graph source should not process");
    require(allSamplesNear(samples, 0.0f), "disabled graph source should render silence");
}

void projectPlaybackGraphAppliesMutedState()
{
    trackloom::Project project("Graph");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    CountingAudioSource source(0.50f);
    trackloom::TrackPlaybackState state;
    trackloom::ProjectPlaybackGraph graph;
    std::vector<float> samples(2 * 4, 1.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    state.muted = true;
    require(project.setTrackPlaybackState(track.id, state), "project should set muted state");
    require(graph.prepare(2, 8), "project graph prepare should succeed");
    require(graph.rebuild(project, { { track.id, &source } }), "project graph rebuild should accept muted binding");
    require(graph.render(block, 48000.0), "project graph muted render should succeed");

    require(source.renderCount() == 1, "muted graph source should still process");
    require(allSamplesNear(samples, 0.0f), "muted graph source should render silence");
}

void projectPlaybackGraphAppliesTrackGain()
{
    trackloom::Project project("Graph");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    ConstantAudioSource source(0.50f);
    trackloom::TrackMixState mix;
    trackloom::ProjectPlaybackGraph graph;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    mix.gain = 0.25f;
    require(project.setTrackMixState(track.id, mix), "project should set graph track gain");
    require(graph.prepare(2, 8), "project graph prepare should succeed");
    require(graph.rebuild(project, { { track.id, &source } }), "project graph rebuild should accept gained binding");
    require(graph.render(block, 48000.0), "project graph gained render should succeed");

    require(allSamplesNear(samples, 0.125f), "project graph should apply track gain before mixing");
}

void projectPlaybackGraphAppliesTrackPanAfterGain()
{
    trackloom::Project project("Graph");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    ConstantAudioSource source(0.50f);
    trackloom::TrackMixState mix;
    trackloom::ProjectPlaybackGraph graph;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    mix.gain = 0.50f;
    mix.pan = -0.5f;
    require(project.setTrackMixState(track.id, mix), "project should set graph track pan");
    require(graph.prepare(2, 8), "project graph prepare should succeed");
    require(graph.rebuild(project, { { track.id, &source } }), "project graph rebuild should accept panned binding");
    require(graph.render(block, 48000.0), "project graph panned render should succeed");

    require(channelSamplesNear(block, 0, 0.25f), "project graph should keep gained left channel for half-left pan");
    require(channelSamplesNear(block, 1, 0.125f), "project graph should reduce gained right channel for half-left pan");
}

void projectPlaybackGraphRendersSilenceWhenEmpty()
{
    trackloom::Project project("Graph");
    trackloom::ProjectPlaybackGraph graph;
    std::vector<float> samples(2 * 4, 1.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(graph.prepare(2, 8), "project graph prepare should succeed");
    require(graph.rebuild(project, {}), "project graph rebuild should accept empty bindings");
    require(graph.render(block, 48000.0), "empty project graph render should succeed");

    require(graph.sourceCount() == 0, "empty project graph should have no sources");
    require(allSamplesNear(samples, 0.0f), "empty project graph should render silence");
}

void projectPlaybackGraphRejectsInvalidBindings()
{
    trackloom::Project project("Graph");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    ConstantAudioSource source(0.50f);
    trackloom::ProjectPlaybackGraph graph;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(!graph.rebuild(project, { { track.id, &source } }), "unprepared graph rebuild should fail");
    require(graph.prepare(2, 8), "project graph prepare should succeed");
    require(!graph.rebuild(project, { { "missing-track", &source } }), "unknown track binding should fail");
    require(!graph.rebuild(project, { { track.id, nullptr } }), "null source binding should fail");
    require(!graph.render(block, 0.0), "project graph should reject invalid sample rate through mixer");
}

}

int main()
{
    try {
        projectStartsEmpty();
        addTrackCommandSupportsUndoAndRedo();
        invalidCommandDoesNotModifyProject();
        projectCreatesMidiClipOnInstrumentTrack();
        projectCreatesAudioClipOnAudioTrack();
        projectRejectsInvalidClipRequests();
        removingTrackRemovesItsClips();
        addClipCommandSupportsUndoAndRedo();
        invalidAddClipCommandDoesNotModifyProject();
        projectCanRenameClip();
        projectCanSetClipTiming();
        projectCanMoveMidiClipBetweenInstrumentTracks();
        projectCanMoveAudioClipBetweenAudioTracks();
        projectRejectsInvalidClipTrackMoves();
        projectCanSplitMidiClipAtInteriorTick();
        projectCanSplitAudioClipAtInteriorTick();
        projectRejectsInvalidClipSplits();
        renameClipCommandSupportsUndoAndRedo();
        invalidRenameClipCommandDoesNotModifyProject();
        setClipTimingCommandSupportsUndoAndRedo();
        invalidSetClipTimingCommandDoesNotModifyProject();
        deleteClipCommandSupportsUndoAndRedo();
        invalidDeleteClipCommandDoesNotModifyProject();
        moveClipToTrackCommandSupportsUndoAndRedo();
        invalidMoveClipToTrackCommandDoesNotModifyProject();
        splitClipCommandSupportsUndoAndRedo();
        invalidSplitClipCommandDoesNotModifyProject();
        newTrackPlaybackStateStartsDefault();
        setTrackPlaybackStateCommandSupportsUndoAndRedo();
        invalidPlaybackStateCommandDoesNotModifyProject();
        newTrackMixStateStartsDefault();
        setTrackMixStateCommandSupportsUndoAndRedo();
        invalidTrackMixStateCommandDoesNotModifyProject();
        projectCanRoundTripThroughText();
        projectCanRoundTripTrackPlaybackState();
        projectCanRoundTripTrackMixState();
        projectCanRoundTripTimelineClips();
        projectCanRoundTripTimelineClipEdits();
        projectCanSaveAfterTimelineClipDeletion();
        projectCanRoundTripTimelineClipTrackMove();
        projectCanRoundTripTimelineClipSplit();
        versionOneProjectLoadsDefaultPlaybackState();
        versionTwoProjectLoadsDefaultMixState();
        versionThreeProjectLoadsDefaultPanState();
        versionFourProjectLoadsWithoutClips();
        invalidTrackPlaybackStateRecordIsRejected();
        invalidTrackMixStateRecordIsRejected();
        invalidClipRecordIsRejected();
        projectCanRoundTripNamesWithSpaces();
        invalidTextIsRejected();
        projectCanSaveAndLoadFromFile();
        saveCreatesParentDirectories();
        saveReplacesExistingFile();
        loadingMissingFileReportsError();
        savingEmptyPathReportsError();
        transportStartsStoppedAtSampleZero();
        playingTransportAdvancesBySamples();
        stoppedTransportDoesNotAdvance();
        transportCanSeekBySample();
        transportCanSeekBySeconds();
        transportRejectsInvalidValuesWithoutChangingState();
        audioBlockCanClearSamples();
        audioEngineAcceptsValidPrepareSettings();
        audioEngineRejectsInvalidPrepareSettings();
        audioEngineClearsOutputAndAdvancesPlayingTransport();
        audioEngineClearsOutputWithoutAdvancingStoppedTransport();
        audioEngineRejectsInvalidRenderRequestsWithoutAdvancing();
        audioEngineRendersToneWhileTransportIsPlaying();
        audioEngineDoesNotRenderToneWhileStopped();
        sineToneSourceRejectsInvalidParameters();
        sourceMixerSumsPreparedSources();
        sourceMixerRendersSilenceWhenEmpty();
        sourceMixerRejectsInvalidRequests();
        audioEngineRendersSourceMixerWhilePlaying();
        gainAudioSourceUsesUnityGainByDefault();
        gainAudioSourceAppliesLinearGain();
        gainAudioSourceCanMuteWithZeroGain();
        gainAudioSourceRejectsInvalidSetup();
        sourceMixerSumsGainWrappedSources();
        panAudioSourcePassesThroughCenteredByDefault();
        panAudioSourceCanMoveFullyLeft();
        panAudioSourceCanMoveFullyRight();
        panAudioSourceCanMoveHalfLeft();
        panAudioSourceRejectsInvalidSetup();
        sourceMixerSumsPanWrappedSources();
        muteAudioSourcePassesThroughByDefault();
        muteAudioSourceClearsOutputWhenMuted();
        muteAudioSourceStillProcessesWrappedSourceWhenMuted();
        muteAudioSourceCanUnmuteAfterMutedRender();
        muteAudioSourceRejectsInvalidSetup();
        sourceMixerSumsMutedAndUnmutedSources();
        disabledAudioSourcePassesThroughByDefault();
        disabledAudioSourceClearsOutputWithoutProcessingSource();
        disabledAudioSourceCanReEnableAfterDisabledRender();
        disabledAudioSourceRejectsInvalidSetup();
        sourceMixerSumsDisabledAndEnabledSources();
        soloAudioSourcePassesThroughWhenSoloModeInactive();
        soloAudioSourcePassesThroughWhenSoloedInSoloMode();
        soloAudioSourceClearsUnsoloedOutputWhileProcessing();
        soloAudioSourceCanLeaveSoloMode();
        soloAudioSourceRejectsInvalidSetup();
        sourceMixerSumsOnlyAudibleSoloSources();
        trackPlaybackAudioSourcePassesThroughDefaultState();
        trackPlaybackAudioSourceMutesWhileProcessing();
        trackPlaybackAudioSourceDisablesWithoutProcessing();
        trackPlaybackAudioSourceSilencesUnsoloedTrackInSoloMode();
        trackPlaybackAudioSourcePassesSoloedTrackInSoloMode();
        sourceMixerSumsTrackPlaybackAudioSources();
        trackPlaybackAudioSourceRejectsInvalidSetup();
        projectPlaybackGraphSumsBoundTracks();
        projectPlaybackGraphAppliesProjectSoloMode();
        projectPlaybackGraphAppliesDisabledState();
        projectPlaybackGraphAppliesMutedState();
        projectPlaybackGraphAppliesTrackGain();
        projectPlaybackGraphAppliesTrackPanAfterGain();
        projectPlaybackGraphRendersSilenceWhenEmpty();
        projectPlaybackGraphRejectsInvalidBindings();
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "All core tests passed.\n";
    return 0;
}
