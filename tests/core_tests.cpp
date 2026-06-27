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

bool numbersNear(double actual, double expected)
{
    return std::fabs(actual - expected) <= 0.000001;
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

void projectCanRenameTrack()
{
    trackloom::Project project("Tracks");
    const auto track = project.createTrack("Piano", trackloom::TrackType::Instrument);
    trackloom::TrackPlaybackState playback;
    trackloom::TrackMixState mix;

    playback.muted = true;
    mix.gain = 0.50f;
    mix.pan = 0.25f;
    require(project.setTrackPlaybackState(track.id, playback), "project should set playback before rename");
    require(project.setTrackMixState(track.id, mix), "project should set mix before rename");

    require(project.renameTrackById(track.id, "Lead"), "track rename should succeed");

    const auto renamedTrack = project.findTrackById(track.id);
    require(renamedTrack.has_value(), "renamed track should still exist");
    require(renamedTrack->id == track.id, "rename should keep track id");
    require(renamedTrack->name == "Lead", "track should keep renamed value");
    require(renamedTrack->type == trackloom::TrackType::Instrument, "rename should keep track type");
    require(renamedTrack->playback == playback, "rename should keep playback state");
    require(renamedTrack->mix == mix, "rename should keep mix state");

    require(!project.renameTrackById(track.id, ""), "empty track name should fail");
    require(!project.renameTrackById("missing-track", "Bass"), "missing track rename should fail");
    require(project.findTrackById(track.id)->name == "Lead", "failed rename should not change track");
}

void projectCanMoveTrackToIndex()
{
    trackloom::Project project("Tracks");
    const auto leadTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto padTrack = project.createTrack("Pad", trackloom::TrackType::Instrument);
    const auto vocalTrack = project.createTrack("Vocal", trackloom::TrackType::Audio);
    trackloom::TrackPlaybackState padPlayback;
    trackloom::TrackMixState padMix;

    padPlayback.soloed = true;
    padMix.gain = 0.50f;
    padMix.pan = -0.25f;
    require(project.setTrackPlaybackState(padTrack.id, padPlayback), "project should set playback before track move");
    require(project.setTrackMixState(padTrack.id, padMix), "project should set mix before track move");
    const auto padClip = project.createClip(padTrack.id, "Pad Intro", trackloom::ClipType::Midi, 0, 960);
    require(padClip.has_value(), "pad clip should exist before track move");

    require(project.moveTrackToIndex(padTrack.id, 0), "track move should accept first index");

    require(project.tracks()[0].id == padTrack.id, "moved track should become first");
    require(project.tracks()[1].id == leadTrack.id, "previous first track should shift right");
    require(project.tracks()[2].id == vocalTrack.id, "later track should keep relative order");
    require(project.findTrackById(padTrack.id)->playback == padPlayback, "track move should keep playback state");
    require(project.findTrackById(padTrack.id)->mix == padMix, "track move should keep mix state");
    require(project.findClipById(padClip->id)->trackId == padTrack.id, "track move should not rewrite clip ownership");

    require(project.moveTrackToIndex(padTrack.id, 2), "track move should accept last index");
    require(project.tracks()[0].id == leadTrack.id, "moving to end should shift lead left");
    require(project.tracks()[1].id == vocalTrack.id, "moving to end should shift vocal left");
    require(project.tracks()[2].id == padTrack.id, "moved track should become last");
}

void projectRejectsInvalidTrackMoves()
{
    trackloom::Project project("Tracks");
    const auto leadTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto padTrack = project.createTrack("Pad", trackloom::TrackType::Instrument);
    const auto vocalTrack = project.createTrack("Vocal", trackloom::TrackType::Audio);

    require(!project.moveTrackToIndex("missing-track", 0), "missing track move should fail");
    require(!project.moveTrackToIndex(padTrack.id, 3), "out-of-range track index should fail");
    require(!project.moveTrackToIndex(padTrack.id, 1), "moving track to current index should fail");

    require(project.tracks()[0].id == leadTrack.id, "failed track moves should keep first track");
    require(project.tracks()[1].id == padTrack.id, "failed track moves should keep second track");
    require(project.tracks()[2].id == vocalTrack.id, "failed track moves should keep third track");
}

void newTrackViewStateStartsDefault()
{
    trackloom::Project project("View");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);

    require(!track.view.hidden, "new track should start visible");
    require(!track.view.collapsed, "new track should start expanded");
}

void projectCanSetTrackViewState()
{
    trackloom::Project project("View");
    const auto leadTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto folderTrack = project.createTrack("Folder", trackloom::TrackType::Folder);
    trackloom::TrackViewState hiddenState;
    trackloom::TrackViewState folderState;

    hiddenState.hidden = true;
    folderState.hidden = true;
    folderState.collapsed = true;

    require(project.setTrackViewState(leadTrack.id, hiddenState), "instrument track should accept hidden view state");
    require(project.findTrackById(leadTrack.id)->view == hiddenState, "hidden view state should be stored");
    require(!project.findTrackById(leadTrack.id)->playback.muted, "hidden should not change muted state");
    require(project.setTrackViewState(folderTrack.id, folderState), "folder track should accept collapsed view state");
    require(project.findTrackById(folderTrack.id)->view == folderState, "folder view state should be stored");
}

void projectRejectsInvalidTrackViewState()
{
    trackloom::Project project("View");
    const auto leadTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto audioTrack = project.createTrack("Vocal", trackloom::TrackType::Audio);
    trackloom::TrackViewState collapsedState;

    collapsedState.collapsed = true;

    require(!project.setTrackViewState("missing-track", collapsedState), "missing track view state should fail");
    require(!project.setTrackViewState(leadTrack.id, collapsedState), "instrument track should reject collapsed view state");
    require(!project.setTrackViewState(audioTrack.id, collapsedState), "audio track should reject collapsed view state");
    require(!project.findTrackById(leadTrack.id)->view.collapsed, "failed view state should not collapse instrument track");
    require(!project.findTrackById(audioTrack.id)->view.collapsed, "failed view state should not collapse audio track");
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

void projectCanDuplicateMidiClipToInstrumentTrack()
{
    trackloom::Project project("Clips");
    const auto sourceTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto targetTrack = project.createTrack("Pad", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(sourceTrack.id, "Intro", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "midi clip should be created before duplicate");
    const auto duplicate = project.duplicateClipToTrackAtTick(clip->id, targetTrack.id, 1920);

    require(duplicate.has_value(), "midi clip duplicate should be created");
    require(duplicate->id == "clip-2", "duplicate should receive next stable clip id");
    require(duplicate->trackId == targetTrack.id, "duplicate should use target track");
    require(duplicate->name == "Intro", "duplicate should keep source name");
    require(duplicate->type == trackloom::ClipType::Midi, "duplicate should keep midi type");
    require(duplicate->startTick == 1920, "duplicate should use requested start tick");
    require(duplicate->lengthTick == 960, "duplicate should keep source length");
    require(project.findClipById(clip->id)->trackId == sourceTrack.id, "duplicate should not move source clip");
    require(project.clips().size() == 2, "project should contain source and duplicate clips");
}

void projectCanDuplicateAudioClipToAudioTrack()
{
    trackloom::Project project("Clips");
    const auto sourceTrack = project.createTrack("Vocal", trackloom::TrackType::Audio);
    const auto targetTrack = project.createTrack("Double", trackloom::TrackType::Audio);
    const auto clip = project.createClip(sourceTrack.id, "Take", trackloom::ClipType::Audio, 480, 1920);

    require(clip.has_value(), "audio clip should be created before duplicate");
    const auto duplicate = project.duplicateClipToTrackAtTick(clip->id, targetTrack.id, 3000);

    require(duplicate.has_value(), "audio clip duplicate should be created");
    require(duplicate->trackId == targetTrack.id, "audio duplicate should use target track");
    require(duplicate->type == trackloom::ClipType::Audio, "audio duplicate should keep audio type");
    require(duplicate->startTick == 3000, "audio duplicate should use requested start tick");
    require(duplicate->lengthTick == 1920, "audio duplicate should keep source length");
}

void projectRejectsInvalidClipDuplicates()
{
    trackloom::Project project("Clips");
    const auto instrument = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto audio = project.createTrack("Vocal", trackloom::TrackType::Audio);
    const auto folder = project.createTrack("Folder", trackloom::TrackType::Folder);
    const auto clip = project.createClip(instrument.id, "Intro", trackloom::ClipType::Midi, 120, 480);

    require(clip.has_value(), "clip should be created before invalid duplicates");
    require(!project.duplicateClipToTrackAtTick("missing-clip", instrument.id, 240).has_value(), "missing clip duplicate should fail");
    require(!project.duplicateClipToTrackAtTick(clip->id, "missing-track", 240).has_value(), "missing target track duplicate should fail");
    require(!project.duplicateClipToTrackAtTick(clip->id, audio.id, 240).has_value(), "incompatible target track duplicate should fail");
    require(!project.duplicateClipToTrackAtTick(clip->id, folder.id, 240).has_value(), "folder target duplicate should fail");
    require(!project.duplicateClipToTrackAtTick(clip->id, instrument.id, -1).has_value(), "negative start duplicate should fail");

    require(project.clips().size() == 1, "failed duplicate should not add clips");
    require(project.findClipById(clip->id)->startTick == 120, "failed duplicate should not change source start tick");
    require(project.findClipById(clip->id)->lengthTick == 480, "failed duplicate should not change source length");
}

void projectCanTrimClipStartWithinExistingRange()
{
    trackloom::Project project("Clips");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Intro", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "clip should be created before start trim");
    require(project.trimClipStartToTick(clip->id, 240), "clip start trim should succeed");

    const auto trimmedClip = project.findClipById(clip->id);
    require(trimmedClip.has_value(), "trimmed clip should still exist");
    require(trimmedClip->id == clip->id, "start trim should keep clip id");
    require(trimmedClip->trackId == track.id, "start trim should keep track");
    require(trimmedClip->name == "Intro", "start trim should keep name");
    require(trimmedClip->type == trackloom::ClipType::Midi, "start trim should keep type");
    require(trimmedClip->startTick == 240, "start trim should update start tick");
    require(trimmedClip->lengthTick == 720, "start trim should preserve old end tick");
}

void projectCanTrimClipEndWithinExistingRange()
{
    trackloom::Project project("Clips");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Intro", trackloom::ClipType::Midi, 120, 960);

    require(clip.has_value(), "clip should be created before end trim");
    require(project.trimClipEndToTick(clip->id, 600), "clip end trim should succeed");

    const auto trimmedClip = project.findClipById(clip->id);
    require(trimmedClip.has_value(), "end-trimmed clip should still exist");
    require(trimmedClip->startTick == 120, "end trim should keep start tick");
    require(trimmedClip->lengthTick == 480, "end trim should update length");
    require(trimmedClip->name == "Intro", "end trim should keep name");
}

void projectRejectsInvalidClipTrims()
{
    trackloom::Project project("Clips");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Intro", trackloom::ClipType::Midi, 120, 480);

    require(clip.has_value(), "clip should be created before invalid trims");
    require(!project.trimClipStartToTick("missing-clip", 240), "missing clip start trim should fail");
    require(!project.trimClipEndToTick("missing-clip", 240), "missing clip end trim should fail");
    require(!project.trimClipStartToTick(clip->id, 119), "start trim before start should fail");
    require(!project.trimClipStartToTick(clip->id, 120), "start trim at start should fail");
    require(!project.trimClipStartToTick(clip->id, 600), "start trim at end should fail");
    require(!project.trimClipStartToTick(clip->id, 601), "start trim after end should fail");
    require(!project.trimClipEndToTick(clip->id, 119), "end trim before start should fail");
    require(!project.trimClipEndToTick(clip->id, 120), "end trim at start should fail");
    require(!project.trimClipEndToTick(clip->id, 600), "end trim at end should fail");
    require(!project.trimClipEndToTick(clip->id, 601), "end trim after end should fail");

    const auto unchangedClip = project.findClipById(clip->id);
    require(unchangedClip.has_value(), "failed trim should keep original clip");
    require(unchangedClip->startTick == 120, "failed trim should not change start tick");
    require(unchangedClip->lengthTick == 480, "failed trim should not change length tick");
}

void newProjectStartsWithoutMarkers()
{
    trackloom::Project project("Markers");

    require(project.markers().empty(), "new project should not contain timeline markers");
}

void projectCanCreateTimelineMarker()
{
    trackloom::Project project("Markers");

    const auto marker = project.createMarker("Verse", 960);

    require(marker.has_value(), "timeline marker should be created");
    require(marker->id == "marker-1", "first marker id should be stable");
    require(marker->name == "Verse", "marker should keep name");
    require(marker->tick == 960, "marker should keep tick");
    require(project.markers().size() == 1, "project should store created marker");
    require(project.findMarkerById(marker->id).has_value(), "marker should be searchable by stable id");
}

void projectCanEditTimelineMarker()
{
    trackloom::Project project("Markers");
    const auto marker = project.createMarker("Verse", 960);

    require(marker.has_value(), "marker should be created before edit");
    require(project.renameMarkerById(marker->id, "Chorus"), "marker rename should succeed");
    require(project.moveMarkerToTick(marker->id, 1920), "marker move should succeed");

    const auto editedMarker = project.findMarkerById(marker->id);
    require(editedMarker.has_value(), "edited marker should still exist");
    require(editedMarker->id == marker->id, "marker edit should keep stable id");
    require(editedMarker->name == "Chorus", "marker rename should store new name");
    require(editedMarker->tick == 1920, "marker move should store new tick");
}

void projectRejectsInvalidTimelineMarkers()
{
    trackloom::Project project("Markers");
    const auto marker = project.createMarker("Verse", 960);
    trackloom::TimelineMarker duplicateMarker;
    trackloom::TimelineMarker missingIdMarker;

    require(marker.has_value(), "marker should be created before invalid marker tests");
    duplicateMarker.id = marker->id;
    duplicateMarker.name = "Duplicate";
    duplicateMarker.tick = 1920;
    missingIdMarker.name = "Missing Id";
    missingIdMarker.tick = 0;

    require(!project.createMarker("", 0).has_value(), "marker should reject empty names");
    require(!project.createMarker("Bad", -1).has_value(), "marker should reject negative tick");
    require(!project.insertExistingMarker(duplicateMarker), "marker should reject duplicate id");
    require(!project.insertExistingMarker(missingIdMarker), "marker should reject empty id");
    require(!project.renameMarkerById(marker->id, ""), "marker should reject empty rename");
    require(!project.renameMarkerById("missing-marker", "Name"), "missing marker rename should fail");
    require(!project.moveMarkerToTick(marker->id, -1), "marker should reject negative move tick");
    require(!project.moveMarkerToTick("missing-marker", 0), "missing marker move should fail");
    require(!project.removeMarkerById("missing-marker"), "missing marker delete should fail");

    const auto unchangedMarker = project.findMarkerById(marker->id);
    require(unchangedMarker.has_value(), "invalid marker operations should keep original marker");
    require(unchangedMarker->name == "Verse", "failed marker operations should keep original name");
    require(unchangedMarker->tick == 960, "failed marker operations should keep original tick");
    require(project.markers().size() == 1, "invalid marker operations should not add markers");
}

void newProjectStartsWithDefaultTempoEvent()
{
    trackloom::Project project("Tempo");

    require(trackloom::Project::ticksPerQuarterNote == 960, "tempo conversion should use 960 ticks per quarter note");
    require(project.tempoEvents().size() == 1, "new project should start with one default tempo event");
    require(project.tempoEvents().front().id == "tempo-1", "default tempo id should be stable");
    require(project.tempoEvents().front().tick == 0, "default tempo should start at tick zero");
    require(numbersNear(project.tempoEvents().front().beatsPerMinute, 120.0), "default tempo should be 120 BPM");
    require(numbersNear(project.tempoAtTick(0), 120.0), "tempo at tick zero should use default BPM");
}

void projectCanCreateTempoEvent()
{
    trackloom::Project project("Tempo");

    const auto laterTempo = project.createTempoEvent(1920, 90.0);
    const auto earlierTempo = project.createTempoEvent(960, 60.0);

    require(laterTempo.has_value(), "later tempo event should be created");
    require(earlierTempo.has_value(), "earlier tempo event should be created");
    require(laterTempo->id == "tempo-2", "first custom tempo id should follow default id");
    require(earlierTempo->id == "tempo-3", "second custom tempo id should advance");
    require(project.tempoEvents().size() == 3, "project should store default and custom tempo events");
    require(project.tempoEvents()[0].tick == 0, "default tempo should stay first");
    require(project.tempoEvents()[1].id == earlierTempo->id, "tempo events should be sorted by tick");
    require(project.tempoEvents()[2].id == laterTempo->id, "later tempo should stay after earlier tempo");
}

void projectCanConvertTicksToSeconds()
{
    trackloom::Project project("Tempo");

    require(numbersNear(project.tickToSeconds(0), 0.0), "tick zero should convert to zero seconds");
    require(numbersNear(project.tickToSeconds(960), 0.5), "960 ticks at 120 BPM should be half a second");
    require(numbersNear(project.tickToSeconds(1920), 1.0), "1920 ticks at 120 BPM should be one second");

    require(project.createTempoEvent(960, 60.0).has_value(), "tempo change should be created before conversion");
    require(numbersNear(project.tempoAtTick(959), 120.0), "tempo before change should use default BPM");
    require(numbersNear(project.tempoAtTick(960), 60.0), "tempo at change tick should use new BPM");
    require(numbersNear(project.tickToSeconds(1920), 1.5), "cross-tempo conversion should add both tempo segments");
    require(numbersNear(project.tickToSeconds(2880), 2.5), "later conversion should keep using latest tempo segment");
}

void projectRejectsInvalidTempoEvents()
{
    trackloom::Project project("Tempo");
    const auto tempo = project.createTempoEvent(960, 90.0);
    trackloom::TempoEvent duplicateId;
    trackloom::TempoEvent duplicateTick;
    trackloom::TempoEvent missingId;

    require(tempo.has_value(), "tempo event should be created before invalid tempo tests");
    duplicateId.id = tempo->id;
    duplicateId.tick = 1920;
    duplicateId.beatsPerMinute = 100.0;
    duplicateTick.id = "tempo-99";
    duplicateTick.tick = tempo->tick;
    duplicateTick.beatsPerMinute = 100.0;
    missingId.tick = 2880;
    missingId.beatsPerMinute = 100.0;

    require(!project.createTempoEvent(-1, 120.0).has_value(), "tempo should reject negative tick");
    require(!project.createTempoEvent(1920, 0.0).has_value(), "tempo should reject too-low BPM");
    require(!project.createTempoEvent(1920, 400.0).has_value(), "tempo should reject too-high BPM");
    require(!project.createTempoEvent(960, 100.0).has_value(), "tempo should reject duplicate tick");
    require(!project.insertExistingTempoEvent(duplicateId), "tempo should reject duplicate id");
    require(!project.insertExistingTempoEvent(duplicateTick), "tempo should reject duplicate tick");
    require(!project.insertExistingTempoEvent(missingId), "tempo should reject empty id");
    require(!project.setTempoEventBpm(tempo->id, 0.0), "tempo should reject invalid BPM update");
    require(!project.setTempoEventBpm("missing-tempo", 120.0), "missing tempo BPM update should fail");
    require(!project.moveTempoEventToTick("tempo-1", 480), "default tempo should not move away from tick zero");
    require(!project.moveTempoEventToTick(tempo->id, 0), "tempo move should reject duplicate default tick");
    require(!project.removeTempoEventById("tempo-1"), "default tempo should not be deleted");

    const auto unchangedTempo = project.findTempoEventById(tempo->id);
    require(unchangedTempo.has_value(), "invalid tempo operations should keep original tempo");
    require(unchangedTempo->tick == 960, "failed tempo operations should keep original tick");
    require(numbersNear(unchangedTempo->beatsPerMinute, 90.0), "failed tempo operations should keep original BPM");
    require(project.tempoEvents().size() == 2, "invalid tempo operations should not add tempo events");
}

void newProjectStartsWithDefaultTimeSignatureEvent()
{
    trackloom::Project project("Meter");

    require(project.timeSignatureEvents().size() == 1, "new project should start with one default time signature event");
    require(project.timeSignatureEvents().front().id == "meter-1", "default time signature id should be stable");
    require(project.timeSignatureEvents().front().tick == 0, "default time signature should start at tick zero");
    require(project.timeSignatureEvents().front().numerator == 4, "default time signature numerator should be 4");
    require(project.timeSignatureEvents().front().denominator == 4, "default time signature denominator should be 4");
    require(project.timeSignatureAtTick(0).numerator == 4, "time signature at tick zero should use default numerator");
    require(project.timeSignatureAtTick(0).denominator == 4, "time signature at tick zero should use default denominator");
}

void projectCanCreateTimeSignatureEvent()
{
    trackloom::Project project("Meter");

    const auto laterMeter = project.createTimeSignatureEvent(3840, 3, 4);
    const auto earlierMeter = project.createTimeSignatureEvent(1920, 6, 8);

    require(laterMeter.has_value(), "later time signature event should be created");
    require(earlierMeter.has_value(), "earlier time signature event should be created");
    require(laterMeter->id == "meter-2", "first custom time signature id should follow default id");
    require(earlierMeter->id == "meter-3", "second custom time signature id should advance");
    require(project.timeSignatureEvents().size() == 3, "project should store default and custom time signature events");
    require(project.timeSignatureEvents()[0].tick == 0, "default time signature should stay first");
    require(project.timeSignatureEvents()[1].id == earlierMeter->id, "time signature events should be sorted by tick");
    require(project.timeSignatureEvents()[2].id == laterMeter->id, "later time signature should stay after earlier event");
}

void projectCanQueryTimeSignatureAndMeasureLength()
{
    trackloom::Project project("Meter");

    require(project.ticksPerMeasureAtTick(0) == 3840, "4/4 measure should be 3840 ticks at 960 PPQ");
    require(project.createTimeSignatureEvent(3840, 3, 4).has_value(), "3/4 time signature should be created before query");
    require(project.createTimeSignatureEvent(7680, 6, 8).has_value(), "6/8 time signature should be created before query");

    require(project.timeSignatureAtTick(3839).numerator == 4, "time signature before change should use default numerator");
    require(project.timeSignatureAtTick(3840).numerator == 3, "time signature at change tick should use new numerator");
    require(project.timeSignatureAtTick(3840).denominator == 4, "time signature at change tick should use new denominator");
    require(project.ticksPerMeasureAtTick(3840) == 2880, "3/4 measure should be 2880 ticks");
    require(project.timeSignatureAtTick(7680).numerator == 6, "later time signature should use 6/8 numerator");
    require(project.timeSignatureAtTick(7680).denominator == 8, "later time signature should use 6/8 denominator");
    require(project.ticksPerMeasureAtTick(7680) == 2880, "6/8 measure should be 2880 ticks");
}

void projectRejectsInvalidTimeSignatureEvents()
{
    trackloom::Project project("Meter");
    const auto meter = project.createTimeSignatureEvent(3840, 3, 4);
    trackloom::TimeSignatureEvent duplicateId;
    trackloom::TimeSignatureEvent duplicateTick;
    trackloom::TimeSignatureEvent missingId;

    require(meter.has_value(), "time signature event should be created before invalid tests");
    duplicateId.id = meter->id;
    duplicateId.tick = 7680;
    duplicateId.numerator = 5;
    duplicateId.denominator = 4;
    duplicateTick.id = "meter-99";
    duplicateTick.tick = meter->tick;
    duplicateTick.numerator = 5;
    duplicateTick.denominator = 4;
    missingId.tick = 9600;
    missingId.numerator = 5;
    missingId.denominator = 4;

    require(!project.createTimeSignatureEvent(-1, 4, 4).has_value(), "time signature should reject negative tick");
    require(!project.createTimeSignatureEvent(7680, 0, 4).has_value(), "time signature should reject zero numerator");
    require(!project.createTimeSignatureEvent(7680, 33, 4).has_value(), "time signature should reject too-large numerator");
    require(!project.createTimeSignatureEvent(7680, 4, 3).has_value(), "time signature should reject non-power-of-two denominator");
    require(!project.createTimeSignatureEvent(3840, 5, 4).has_value(), "time signature should reject duplicate tick");
    require(!project.insertExistingTimeSignatureEvent(duplicateId), "time signature should reject duplicate id");
    require(!project.insertExistingTimeSignatureEvent(duplicateTick), "time signature should reject duplicate tick");
    require(!project.insertExistingTimeSignatureEvent(missingId), "time signature should reject empty id");
    require(!project.setTimeSignature(meter->id, 0, 4), "time signature should reject invalid numerator update");
    require(!project.setTimeSignature(meter->id, 4, 3), "time signature should reject invalid denominator update");
    require(!project.setTimeSignature("missing-meter", 4, 4), "missing time signature update should fail");
    require(!project.moveTimeSignatureEventToTick("meter-1", 480), "default time signature should not move away from tick zero");
    require(!project.moveTimeSignatureEventToTick(meter->id, 0), "time signature move should reject duplicate default tick");
    require(!project.removeTimeSignatureEventById("meter-1"), "default time signature should not be deleted");

    const auto unchangedMeter = project.findTimeSignatureEventById(meter->id);
    require(unchangedMeter.has_value(), "invalid time signature operations should keep original event");
    require(unchangedMeter->tick == 3840, "failed time signature operations should keep original tick");
    require(unchangedMeter->numerator == 3, "failed time signature operations should keep original numerator");
    require(unchangedMeter->denominator == 4, "failed time signature operations should keep original denominator");
    require(project.timeSignatureEvents().size() == 2, "invalid time signature operations should not add events");
}

void midiClipCanCreateMidiNote()
{
    trackloom::Project project("MIDI Notes");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 1920);

    require(clip.has_value(), "midi clip should be created before note");
    const auto note = project.createMidiNote(clip->id, 120, 480, 60, 100, 1);

    require(note.has_value(), "midi clip should accept midi note");
    require(note->id == "note-1", "first midi note id should be stable");
    require(note->startTick == 120, "note should keep clip-relative start tick");
    require(note->lengthTick == 480, "note should keep length tick");
    require(note->noteNumber == 60, "note should keep pitch");
    require(note->velocity == 100, "note should keep velocity");
    require(note->channel == 1, "note should keep channel");
    require(project.findMidiNoteById(note->id).has_value(), "project should find note by stable id");
    require(project.findClipById(clip->id)->midiNotes.size() == 1, "note should be stored inside owning clip");
    require(project.findClipById(clip->id)->midiNotes.front() == *note, "stored note should match created note");
}

void audioClipRejectsMidiNotes()
{
    trackloom::Project project("MIDI Notes");
    const auto track = project.createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = project.createClip(track.id, "Vocal Take", trackloom::ClipType::Audio, 0, 1920);

    require(clip.has_value(), "audio clip should be created before note rejection");
    require(!project.createMidiNote(clip->id, 0, 480, 60, 100, 1).has_value(), "audio clip should reject midi note");
    require(project.findClipById(clip->id)->midiNotes.empty(), "audio clip should remain without midi notes");
}

void projectRejectsInvalidMidiNotes()
{
    trackloom::Project project("MIDI Notes");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 0, 960);
    const auto note = project.createMidiNote(clip->id, 120, 240, 60, 100, 1);
    trackloom::MidiNoteEvent duplicateId;
    trackloom::MidiNoteEvent missingId;

    require(clip.has_value(), "midi clip should be created before invalid note tests");
    require(note.has_value(), "midi note should be created before invalid note tests");
    duplicateId = *note;
    duplicateId.startTick = 480;
    missingId.startTick = 480;
    missingId.lengthTick = 120;
    missingId.noteNumber = 64;
    missingId.velocity = 90;
    missingId.channel = 1;

    require(!project.createMidiNote("missing-clip", 0, 120, 60, 100, 1).has_value(), "missing clip should reject midi note");
    require(!project.createMidiNote(clip->id, -1, 120, 60, 100, 1).has_value(), "note should reject negative start");
    require(!project.createMidiNote(clip->id, 0, 0, 60, 100, 1).has_value(), "note should reject zero length");
    require(!project.createMidiNote(clip->id, 900, 120, 60, 100, 1).has_value(), "note should reject range beyond clip length");
    require(!project.createMidiNote(clip->id, 0, 120, -1, 100, 1).has_value(), "note should reject low pitch");
    require(!project.createMidiNote(clip->id, 0, 120, 128, 100, 1).has_value(), "note should reject high pitch");
    require(!project.createMidiNote(clip->id, 0, 120, 60, 0, 1).has_value(), "note should reject zero velocity");
    require(!project.createMidiNote(clip->id, 0, 120, 60, 128, 1).has_value(), "note should reject high velocity");
    require(!project.createMidiNote(clip->id, 0, 120, 60, 100, 0).has_value(), "note should reject low channel");
    require(!project.createMidiNote(clip->id, 0, 120, 60, 100, 17).has_value(), "note should reject high channel");
    require(!project.insertExistingMidiNote(clip->id, duplicateId), "note should reject duplicate id");
    require(!project.insertExistingMidiNote(clip->id, missingId), "note should reject empty id");
    require(!project.setMidiNoteTiming(note->id, 900, 120), "note should reject timing beyond clip range");
    require(!project.setMidiNotePitch(note->id, 128), "note should reject invalid pitch update");
    require(!project.setMidiNoteVelocity(note->id, 0), "note should reject invalid velocity update");
    require(!project.setMidiNoteChannel(note->id, 17), "note should reject invalid channel update");
    require(!project.removeMidiNoteById("missing-note"), "missing note delete should fail");

    const auto unchangedNote = project.findMidiNoteById(note->id);
    require(unchangedNote.has_value(), "invalid note operations should keep original note");
    require(unchangedNote.value() == *note, "failed note operations should not modify original note");
    require(project.findClipById(clip->id)->midiNotes.size() == 1, "invalid note operations should not add notes");
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

void duplicateClipCommandSupportsUndoAndRedo()
{
    trackloom::Project project("Clips");
    trackloom::CommandStack commands;
    const auto sourceTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto targetTrack = project.createTrack("Pad", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(sourceTrack.id, "Intro", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "clip should be created before command duplicate");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::DuplicateClipCommand>(clip->id, targetTrack.id, 1920));

    require(result.success, "duplicate clip command should succeed");
    require(project.clips().size() == 2, "duplicate command should create one new clip");
    const auto duplicateId = project.clips().back().id;
    require(project.findClipById(duplicateId)->trackId == targetTrack.id, "duplicate command should use target track");
    require(project.findClipById(duplicateId)->startTick == 1920, "duplicate command should use requested start tick");
    require(project.findClipById(clip->id)->trackId == sourceTrack.id, "duplicate command should not move source clip");

    require(commands.undo(project), "duplicate clip undo should be available");
    require(project.clips().size() == 1, "undo should remove duplicate clip");
    require(!project.findClipById(duplicateId).has_value(), "undo should remove created duplicate");
    require(project.findClipById(clip->id).has_value(), "undo should keep source clip");

    require(commands.redo(project), "duplicate clip redo should be available");
    require(project.clips().size() == 2, "redo should restore duplicate clip");
    require(project.findClipById(duplicateId).has_value(), "redo should reuse duplicate clip id");
    require(project.findClipById(duplicateId)->lengthTick == 960, "redo should restore duplicate length");
}

void invalidDuplicateClipCommandDoesNotModifyProject()
{
    trackloom::Project project("Clips");
    trackloom::CommandStack commands;
    const auto instrument = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto audio = project.createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = project.createClip(instrument.id, "Intro", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "clip should be created before invalid command duplicate");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::DuplicateClipCommand>(clip->id, audio.id, 1920));

    require(!result.success, "incompatible duplicate command should fail validation");
    require(project.clips().size() == 1, "failed duplicate command should not add clips");
    require(project.findClipById(clip->id)->trackId == instrument.id, "failed duplicate command should not move source");
    require(!commands.canUndo(), "failed duplicate command should not enter undo stack");
}

void deleteClipCommandRestoresMidiNotes()
{
    trackloom::Project project("MIDI Notes");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 0, 960);
    const auto note = project.createMidiNote(clip->id, 0, 240, 60, 100, 1);

    require(note.has_value(), "note should be created before clip delete");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::DeleteClipCommand>(clip->id));

    require(result.success, "delete clip with notes should succeed");
    require(!project.findClipById(clip->id).has_value(), "delete clip should remove clip");
    require(!project.findMidiNoteById(note->id).has_value(), "delete clip should remove nested note");

    require(commands.undo(project), "delete clip with notes undo should be available");
    require(project.findClipById(clip->id).has_value(), "undo should restore clip");
    require(project.findMidiNoteById(note->id).has_value(), "undo should restore nested note");
    require(project.findMidiNoteById(note->id).value() == *note, "undo should restore complete note state");
}

void deleteTrackCommandRestoresMidiNotes()
{
    trackloom::Project project("MIDI Notes");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 0, 960);
    const auto note = project.createMidiNote(clip->id, 0, 240, 60, 100, 1);

    require(note.has_value(), "note should be created before track delete");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::DeleteTrackCommand>(track.id));

    require(result.success, "delete track with notes should succeed");
    require(project.tracks().empty(), "delete track should remove track");
    require(project.clips().empty(), "delete track should remove clips");
    require(!project.findMidiNoteById(note->id).has_value(), "delete track should remove nested note");

    require(commands.undo(project), "delete track with notes undo should be available");
    require(project.findTrackById(track.id).has_value(), "undo should restore track");
    require(project.findClipById(clip->id).has_value(), "undo should restore clip");
    require(project.findMidiNoteById(note->id).has_value(), "undo should restore nested note");
}

void duplicateMidiClipCopiesNotesWithNewIds()
{
    trackloom::Project project("MIDI Notes");
    const auto sourceTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto targetTrack = project.createTrack("Pad", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(sourceTrack.id, "Lead Phrase", trackloom::ClipType::Midi, 0, 960);
    const auto note = project.createMidiNote(clip->id, 120, 240, 60, 100, 1);

    require(note.has_value(), "note should be created before clip duplicate");
    const auto duplicate = project.duplicateClipToTrackAtTick(clip->id, targetTrack.id, 1920);

    require(duplicate.has_value(), "duplicate midi clip should succeed");
    require(duplicate->midiNotes.size() == 1, "duplicate should copy midi notes");
    require(duplicate->midiNotes.front().id != note->id, "duplicate note should receive new stable id");
    require(duplicate->midiNotes.front().startTick == note->startTick, "duplicate note should keep relative start");
    require(duplicate->midiNotes.front().lengthTick == note->lengthTick, "duplicate note should keep length");
    require(duplicate->midiNotes.front().noteNumber == note->noteNumber, "duplicate note should keep pitch");
    require(project.findClipById(clip->id)->midiNotes.front().id == note->id, "duplicate should not rewrite source note id");
}

void splitMidiClipMovesRightSideNotes()
{
    trackloom::Project project("MIDI Notes");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 0, 960);
    const auto leftNote = project.createMidiNote(clip->id, 120, 120, 60, 100, 1);
    const auto rightNote = project.createMidiNote(clip->id, 600, 120, 64, 90, 1);

    require(leftNote.has_value(), "left note should be created before split");
    require(rightNote.has_value(), "right note should be created before split");
    const auto rightClip = project.splitClipAtTick(clip->id, 480);

    require(rightClip.has_value(), "split midi clip should succeed when notes do not cross split");
    require(project.findClipById(clip->id)->midiNotes.size() == 1, "left clip should keep left-side note");
    require(project.findClipById(clip->id)->midiNotes.front().id == leftNote->id, "left note id should stay on left clip");
    require(project.findClipById(rightClip->id)->midiNotes.size() == 1, "right clip should receive right-side note");
    require(project.findClipById(rightClip->id)->midiNotes.front().id == rightNote->id, "right note id should move to right clip");
    require(project.findClipById(rightClip->id)->midiNotes.front().startTick == 120, "right note start should become relative to right clip");
}

void splitMidiClipRejectsNotesCrossingSplitTick()
{
    trackloom::Project project("MIDI Notes");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 0, 960);
    const auto crossingNote = project.createMidiNote(clip->id, 360, 240, 60, 100, 1);

    require(crossingNote.has_value(), "crossing note should be created before split rejection");
    const auto rightClip = project.splitClipAtTick(clip->id, 480);

    require(!rightClip.has_value(), "split should reject note crossing split tick");
    require(project.clips().size() == 1, "failed split should not add right clip");
    require(project.findClipById(clip->id)->lengthTick == 960, "failed split should keep original clip length");
    require(project.findMidiNoteById(crossingNote->id).has_value(), "failed split should keep crossing note");
}

void clipTimingRejectsMidiNotesOutsideClipRange()
{
    trackloom::Project project("MIDI Notes");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 0, 960);
    const auto note = project.createMidiNote(clip->id, 720, 200, 60, 100, 1);

    require(note.has_value(), "late note should be created before clip timing rejection");
    require(!project.setClipTiming(clip->id, 0, 800), "clip timing should reject shortening that would exclude note");
    require(!project.trimClipEndToTick(clip->id, 800), "clip end trim should reject excluding note");
    require(project.findClipById(clip->id)->lengthTick == 960, "failed clip timing should keep original length");
    require(project.findMidiNoteById(note->id).has_value(), "failed clip timing should keep note");
}

void trimClipStartCommandSupportsUndoAndRedo()
{
    trackloom::Project project("Clips");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Intro", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "clip should be created before command start trim");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::TrimClipStartCommand>(clip->id, 240));

    require(result.success, "trim clip start command should succeed");
    require(project.findClipById(clip->id)->startTick == 240, "start trim command should update start");
    require(project.findClipById(clip->id)->lengthTick == 720, "start trim command should preserve old end");

    require(commands.undo(project), "trim clip start undo should be available");
    require(project.findClipById(clip->id)->startTick == 0, "undo should restore original start");
    require(project.findClipById(clip->id)->lengthTick == 960, "undo should restore original length");

    require(commands.redo(project), "trim clip start redo should be available");
    require(project.findClipById(clip->id)->startTick == 240, "redo should restore trimmed start");
    require(project.findClipById(clip->id)->lengthTick == 720, "redo should restore trimmed length");
}

void trimClipEndCommandSupportsUndoAndRedo()
{
    trackloom::Project project("Clips");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Intro", trackloom::ClipType::Midi, 120, 960);

    require(clip.has_value(), "clip should be created before command end trim");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::TrimClipEndCommand>(clip->id, 600));

    require(result.success, "trim clip end command should succeed");
    require(project.findClipById(clip->id)->startTick == 120, "end trim command should keep start");
    require(project.findClipById(clip->id)->lengthTick == 480, "end trim command should update length");

    require(commands.undo(project), "trim clip end undo should be available");
    require(project.findClipById(clip->id)->startTick == 120, "undo should keep original start");
    require(project.findClipById(clip->id)->lengthTick == 960, "undo should restore original length");

    require(commands.redo(project), "trim clip end redo should be available");
    require(project.findClipById(clip->id)->startTick == 120, "redo should keep start");
    require(project.findClipById(clip->id)->lengthTick == 480, "redo should restore trimmed length");
}

void invalidTrimClipCommandDoesNotModifyProject()
{
    trackloom::Project project("Clips");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Intro", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "clip should be created before invalid command trim");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::TrimClipStartCommand>(clip->id, 960));

    require(!result.success, "trim at clip end should fail validation");
    require(project.findClipById(clip->id)->startTick == 0, "failed trim command should not change start");
    require(project.findClipById(clip->id)->lengthTick == 960, "failed trim command should not change length");
    require(!commands.canUndo(), "failed trim command should not enter undo stack");
}

void addMarkerCommandSupportsUndoAndRedo()
{
    trackloom::Project project("Markers");
    trackloom::CommandStack commands;

    auto result = commands.execute(
        project,
        std::make_unique<trackloom::AddMarkerCommand>("Verse", 960));

    require(result.success, "add marker command should succeed");
    require(project.markers().size() == 1, "add marker command should store marker");
    require(project.markers().front().id == "marker-1", "add marker command should create stable id");
    require(project.markers().front().name == "Verse", "add marker command should keep marker name");
    require(project.markers().front().tick == 960, "add marker command should keep marker tick");

    require(commands.undo(project), "add marker undo should be available");
    require(project.markers().empty(), "undo should remove marker");

    require(commands.redo(project), "add marker redo should be available");
    require(project.markers().size() == 1, "redo should restore marker");
    require(project.markers().front().id == "marker-1", "redo should preserve marker id");
}

void renameMarkerCommandSupportsUndoAndRedo()
{
    trackloom::Project project("Markers");
    trackloom::CommandStack commands;
    const auto marker = project.createMarker("Verse", 960);

    require(marker.has_value(), "marker should be created before command rename");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::RenameMarkerCommand>(marker->id, "Chorus"));

    require(result.success, "rename marker command should succeed");
    require(project.findMarkerById(marker->id)->name == "Chorus", "rename command should update marker name");

    require(commands.undo(project), "rename marker undo should be available");
    require(project.findMarkerById(marker->id)->name == "Verse", "undo should restore marker name");

    require(commands.redo(project), "rename marker redo should be available");
    require(project.findMarkerById(marker->id)->name == "Chorus", "redo should restore marker name");
}

void moveMarkerCommandSupportsUndoAndRedo()
{
    trackloom::Project project("Markers");
    trackloom::CommandStack commands;
    const auto marker = project.createMarker("Verse", 960);

    require(marker.has_value(), "marker should be created before command move");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::MoveMarkerCommand>(marker->id, 1920));

    require(result.success, "move marker command should succeed");
    require(project.findMarkerById(marker->id)->tick == 1920, "move command should update marker tick");

    require(commands.undo(project), "move marker undo should be available");
    require(project.findMarkerById(marker->id)->tick == 960, "undo should restore marker tick");

    require(commands.redo(project), "move marker redo should be available");
    require(project.findMarkerById(marker->id)->tick == 1920, "redo should restore marker tick");
}

void deleteMarkerCommandSupportsUndoAndRedo()
{
    trackloom::Project project("Markers");
    trackloom::CommandStack commands;
    const auto marker = project.createMarker("Verse", 960);

    require(marker.has_value(), "marker should be created before command delete");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::DeleteMarkerCommand>(marker->id));

    require(result.success, "delete marker command should succeed");
    require(project.markers().empty(), "delete marker command should remove marker");

    require(commands.undo(project), "delete marker undo should be available");
    require(project.markers().size() == 1, "undo should restore marker");
    require(project.markers().front() == *marker, "undo should restore complete marker state");

    require(commands.redo(project), "delete marker redo should be available");
    require(project.markers().empty(), "redo should delete marker again");
}

void invalidMarkerCommandDoesNotModifyProject()
{
    trackloom::Project project("Markers");
    trackloom::CommandStack commands;
    const auto marker = project.createMarker("Verse", 960);

    require(marker.has_value(), "marker should be created before invalid marker commands");
    auto addEmptyNameResult = commands.execute(
        project,
        std::make_unique<trackloom::AddMarkerCommand>("", 0));
    auto renameEmptyNameResult = commands.execute(
        project,
        std::make_unique<trackloom::RenameMarkerCommand>(marker->id, ""));
    auto moveNegativeTickResult = commands.execute(
        project,
        std::make_unique<trackloom::MoveMarkerCommand>(marker->id, -1));
    auto deleteMissingMarkerResult = commands.execute(
        project,
        std::make_unique<trackloom::DeleteMarkerCommand>("missing-marker"));

    require(!addEmptyNameResult.success, "empty marker add command should fail");
    require(!renameEmptyNameResult.success, "empty marker rename command should fail");
    require(!moveNegativeTickResult.success, "negative marker move command should fail");
    require(!deleteMissingMarkerResult.success, "missing marker delete command should fail");
    require(project.markers().size() == 1, "failed marker commands should keep marker count");
    require(project.markers().front() == *marker, "failed marker commands should not modify marker");
    require(!commands.canUndo(), "failed marker commands should not enter undo stack");
}

void addTempoEventCommandSupportsUndoAndRedo()
{
    trackloom::Project project("Tempo");
    trackloom::CommandStack commands;

    auto result = commands.execute(
        project,
        std::make_unique<trackloom::AddTempoEventCommand>(960, 90.0));

    require(result.success, "add tempo command should succeed");
    require(project.tempoEvents().size() == 2, "add tempo command should store tempo event");
    require(project.tempoEvents()[1].id == "tempo-2", "add tempo command should create stable id");
    require(project.tempoEvents()[1].tick == 960, "add tempo command should keep tick");
    require(numbersNear(project.tempoEvents()[1].beatsPerMinute, 90.0), "add tempo command should keep BPM");

    require(commands.undo(project), "add tempo undo should be available");
    require(project.tempoEvents().size() == 1, "undo should remove custom tempo event");

    require(commands.redo(project), "add tempo redo should be available");
    require(project.tempoEvents().size() == 2, "redo should restore tempo event");
    require(project.tempoEvents()[1].id == "tempo-2", "redo should preserve tempo id");
}

void setTempoEventBpmCommandSupportsUndoAndRedo()
{
    trackloom::Project project("Tempo");
    trackloom::CommandStack commands;

    auto result = commands.execute(
        project,
        std::make_unique<trackloom::SetTempoEventBpmCommand>("tempo-1", 100.0));

    require(result.success, "set tempo BPM command should succeed");
    require(numbersNear(project.findTempoEventById("tempo-1")->beatsPerMinute, 100.0), "command should update default tempo BPM");

    require(commands.undo(project), "set tempo BPM undo should be available");
    require(numbersNear(project.findTempoEventById("tempo-1")->beatsPerMinute, 120.0), "undo should restore default BPM");

    require(commands.redo(project), "set tempo BPM redo should be available");
    require(numbersNear(project.findTempoEventById("tempo-1")->beatsPerMinute, 100.0), "redo should restore new BPM");
}

void moveTempoEventCommandSupportsUndoAndRedo()
{
    trackloom::Project project("Tempo");
    trackloom::CommandStack commands;
    const auto tempo = project.createTempoEvent(960, 90.0);

    require(tempo.has_value(), "tempo should be created before command move");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::MoveTempoEventCommand>(tempo->id, 1920));

    require(result.success, "move tempo command should succeed");
    require(project.findTempoEventById(tempo->id)->tick == 1920, "move command should update tempo tick");

    require(commands.undo(project), "move tempo undo should be available");
    require(project.findTempoEventById(tempo->id)->tick == 960, "undo should restore tempo tick");

    require(commands.redo(project), "move tempo redo should be available");
    require(project.findTempoEventById(tempo->id)->tick == 1920, "redo should restore tempo tick");
}

void deleteTempoEventCommandSupportsUndoAndRedo()
{
    trackloom::Project project("Tempo");
    trackloom::CommandStack commands;
    const auto tempo = project.createTempoEvent(960, 90.0);

    require(tempo.has_value(), "tempo should be created before command delete");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::DeleteTempoEventCommand>(tempo->id));

    require(result.success, "delete tempo command should succeed");
    require(project.tempoEvents().size() == 1, "delete tempo command should remove custom tempo");
    require(!project.findTempoEventById(tempo->id).has_value(), "delete tempo command should remove target tempo");

    require(commands.undo(project), "delete tempo undo should be available");
    require(project.tempoEvents().size() == 2, "undo should restore tempo event");
    require(project.findTempoEventById(tempo->id).has_value(), "undo should restore target tempo");
    require(project.findTempoEventById(tempo->id).value() == *tempo, "undo should restore complete tempo state");

    require(commands.redo(project), "delete tempo redo should be available");
    require(project.tempoEvents().size() == 1, "redo should delete custom tempo again");
}

void invalidTempoCommandDoesNotModifyProject()
{
    trackloom::Project project("Tempo");
    trackloom::CommandStack commands;
    const auto tempo = project.createTempoEvent(960, 90.0);

    require(tempo.has_value(), "tempo should be created before invalid tempo commands");
    auto duplicateTickAddResult = commands.execute(
        project,
        std::make_unique<trackloom::AddTempoEventCommand>(960, 100.0));
    auto invalidBpmResult = commands.execute(
        project,
        std::make_unique<trackloom::SetTempoEventBpmCommand>(tempo->id, 0.0));
    auto moveDefaultResult = commands.execute(
        project,
        std::make_unique<trackloom::MoveTempoEventCommand>("tempo-1", 480));
    auto deleteDefaultResult = commands.execute(
        project,
        std::make_unique<trackloom::DeleteTempoEventCommand>("tempo-1"));

    require(!duplicateTickAddResult.success, "duplicate tick add tempo command should fail");
    require(!invalidBpmResult.success, "invalid BPM command should fail");
    require(!moveDefaultResult.success, "move default tempo command should fail");
    require(!deleteDefaultResult.success, "delete default tempo command should fail");
    require(project.tempoEvents().size() == 2, "failed tempo commands should keep tempo count");
    require(project.findTempoEventById(tempo->id).value() == *tempo, "failed tempo commands should not modify tempo");
    require(numbersNear(project.findTempoEventById("tempo-1")->beatsPerMinute, 120.0), "failed tempo commands should keep default BPM");
    require(!commands.canUndo(), "failed tempo commands should not enter undo stack");
}

void addTimeSignatureEventCommandSupportsUndoAndRedo()
{
    trackloom::Project project("Meter");
    trackloom::CommandStack commands;

    auto result = commands.execute(
        project,
        std::make_unique<trackloom::AddTimeSignatureEventCommand>(3840, 3, 4));

    require(result.success, "add time signature command should succeed");
    require(project.timeSignatureEvents().size() == 2, "add time signature command should store event");
    require(project.timeSignatureEvents()[1].id == "meter-2", "add time signature command should create stable id");
    require(project.timeSignatureEvents()[1].tick == 3840, "add time signature command should keep tick");
    require(project.timeSignatureEvents()[1].numerator == 3, "add time signature command should keep numerator");
    require(project.timeSignatureEvents()[1].denominator == 4, "add time signature command should keep denominator");

    require(commands.undo(project), "add time signature undo should be available");
    require(project.timeSignatureEvents().size() == 1, "undo should remove custom time signature event");

    require(commands.redo(project), "add time signature redo should be available");
    require(project.timeSignatureEvents().size() == 2, "redo should restore time signature event");
    require(project.timeSignatureEvents()[1].id == "meter-2", "redo should preserve time signature id");
}

void setTimeSignatureCommandSupportsUndoAndRedo()
{
    trackloom::Project project("Meter");
    trackloom::CommandStack commands;

    auto result = commands.execute(
        project,
        std::make_unique<trackloom::SetTimeSignatureCommand>("meter-1", 6, 8));

    require(result.success, "set time signature command should succeed");
    require(project.findTimeSignatureEventById("meter-1")->numerator == 6, "command should update default numerator");
    require(project.findTimeSignatureEventById("meter-1")->denominator == 8, "command should update default denominator");

    require(commands.undo(project), "set time signature undo should be available");
    require(project.findTimeSignatureEventById("meter-1")->numerator == 4, "undo should restore default numerator");
    require(project.findTimeSignatureEventById("meter-1")->denominator == 4, "undo should restore default denominator");

    require(commands.redo(project), "set time signature redo should be available");
    require(project.findTimeSignatureEventById("meter-1")->numerator == 6, "redo should restore new numerator");
    require(project.findTimeSignatureEventById("meter-1")->denominator == 8, "redo should restore new denominator");
}

void moveTimeSignatureEventCommandSupportsUndoAndRedo()
{
    trackloom::Project project("Meter");
    trackloom::CommandStack commands;
    const auto meter = project.createTimeSignatureEvent(3840, 3, 4);

    require(meter.has_value(), "time signature should be created before command move");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::MoveTimeSignatureEventCommand>(meter->id, 7680));

    require(result.success, "move time signature command should succeed");
    require(project.findTimeSignatureEventById(meter->id)->tick == 7680, "move command should update time signature tick");

    require(commands.undo(project), "move time signature undo should be available");
    require(project.findTimeSignatureEventById(meter->id)->tick == 3840, "undo should restore time signature tick");

    require(commands.redo(project), "move time signature redo should be available");
    require(project.findTimeSignatureEventById(meter->id)->tick == 7680, "redo should restore time signature tick");
}

void deleteTimeSignatureEventCommandSupportsUndoAndRedo()
{
    trackloom::Project project("Meter");
    trackloom::CommandStack commands;
    const auto meter = project.createTimeSignatureEvent(3840, 3, 4);

    require(meter.has_value(), "time signature should be created before command delete");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::DeleteTimeSignatureEventCommand>(meter->id));

    require(result.success, "delete time signature command should succeed");
    require(project.timeSignatureEvents().size() == 1, "delete time signature command should remove custom event");
    require(!project.findTimeSignatureEventById(meter->id).has_value(), "delete time signature command should remove target event");

    require(commands.undo(project), "delete time signature undo should be available");
    require(project.timeSignatureEvents().size() == 2, "undo should restore time signature event");
    require(project.findTimeSignatureEventById(meter->id).has_value(), "undo should restore target time signature event");
    require(project.findTimeSignatureEventById(meter->id).value() == *meter, "undo should restore complete time signature state");

    require(commands.redo(project), "delete time signature redo should be available");
    require(project.timeSignatureEvents().size() == 1, "redo should delete custom time signature again");
}

void invalidTimeSignatureCommandDoesNotModifyProject()
{
    trackloom::Project project("Meter");
    trackloom::CommandStack commands;
    const auto meter = project.createTimeSignatureEvent(3840, 3, 4);

    require(meter.has_value(), "time signature should be created before invalid commands");
    auto duplicateTickAddResult = commands.execute(
        project,
        std::make_unique<trackloom::AddTimeSignatureEventCommand>(3840, 5, 4));
    auto invalidValueResult = commands.execute(
        project,
        std::make_unique<trackloom::SetTimeSignatureCommand>(meter->id, 0, 4));
    auto moveDefaultResult = commands.execute(
        project,
        std::make_unique<trackloom::MoveTimeSignatureEventCommand>("meter-1", 480));
    auto deleteDefaultResult = commands.execute(
        project,
        std::make_unique<trackloom::DeleteTimeSignatureEventCommand>("meter-1"));

    require(!duplicateTickAddResult.success, "duplicate tick add time signature command should fail");
    require(!invalidValueResult.success, "invalid time signature command should fail");
    require(!moveDefaultResult.success, "move default time signature command should fail");
    require(!deleteDefaultResult.success, "delete default time signature command should fail");
    require(project.timeSignatureEvents().size() == 2, "failed time signature commands should keep event count");
    require(project.findTimeSignatureEventById(meter->id).value() == *meter, "failed commands should not modify time signature");
    require(project.findTimeSignatureEventById("meter-1")->numerator == 4, "failed commands should keep default numerator");
    require(project.findTimeSignatureEventById("meter-1")->denominator == 4, "failed commands should keep default denominator");
    require(!commands.canUndo(), "failed time signature commands should not enter undo stack");
}

void addMidiNoteCommandSupportsUndoAndRedo()
{
    trackloom::Project project("MIDI Notes");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "clip should be created before add note command");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::AddMidiNoteCommand>(clip->id, 0, 480, 60, 100, 1));

    require(result.success, "add midi note command should succeed");
    require(project.findClipById(clip->id)->midiNotes.size() == 1, "add command should store note");
    require(project.findClipById(clip->id)->midiNotes.front().id == "note-1", "add command should create stable note id");

    require(commands.undo(project), "add note undo should be available");
    require(project.findClipById(clip->id)->midiNotes.empty(), "undo should remove note");

    require(commands.redo(project), "add note redo should be available");
    require(project.findClipById(clip->id)->midiNotes.size() == 1, "redo should restore note");
    require(project.findClipById(clip->id)->midiNotes.front().id == "note-1", "redo should preserve note id");
}

void setMidiNoteTimingCommandSupportsUndoAndRedo()
{
    trackloom::Project project("MIDI Notes");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 0, 960);
    const auto note = project.createMidiNote(clip->id, 0, 240, 60, 100, 1);

    require(note.has_value(), "note should be created before timing command");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::SetMidiNoteTimingCommand>(note->id, 120, 360));

    require(result.success, "set midi note timing command should succeed");
    require(project.findMidiNoteById(note->id)->startTick == 120, "command should update note start");
    require(project.findMidiNoteById(note->id)->lengthTick == 360, "command should update note length");

    require(commands.undo(project), "set note timing undo should be available");
    require(project.findMidiNoteById(note->id)->startTick == 0, "undo should restore note start");
    require(project.findMidiNoteById(note->id)->lengthTick == 240, "undo should restore note length");

    require(commands.redo(project), "set note timing redo should be available");
    require(project.findMidiNoteById(note->id)->startTick == 120, "redo should restore note start");
    require(project.findMidiNoteById(note->id)->lengthTick == 360, "redo should restore note length");
}

void setMidiNotePitchCommandSupportsUndoAndRedo()
{
    trackloom::Project project("MIDI Notes");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 0, 960);
    const auto note = project.createMidiNote(clip->id, 0, 240, 60, 100, 1);

    require(note.has_value(), "note should be created before pitch command");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::SetMidiNotePitchCommand>(note->id, 64));

    require(result.success, "set midi note pitch command should succeed");
    require(project.findMidiNoteById(note->id)->noteNumber == 64, "command should update note pitch");

    require(commands.undo(project), "set note pitch undo should be available");
    require(project.findMidiNoteById(note->id)->noteNumber == 60, "undo should restore note pitch");

    require(commands.redo(project), "set note pitch redo should be available");
    require(project.findMidiNoteById(note->id)->noteNumber == 64, "redo should restore note pitch");
}

void setMidiNoteVelocityCommandSupportsUndoAndRedo()
{
    trackloom::Project project("MIDI Notes");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 0, 960);
    const auto note = project.createMidiNote(clip->id, 0, 240, 60, 100, 1);

    require(note.has_value(), "note should be created before velocity command");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::SetMidiNoteVelocityCommand>(note->id, 80));

    require(result.success, "set midi note velocity command should succeed");
    require(project.findMidiNoteById(note->id)->velocity == 80, "command should update note velocity");

    require(commands.undo(project), "set note velocity undo should be available");
    require(project.findMidiNoteById(note->id)->velocity == 100, "undo should restore note velocity");

    require(commands.redo(project), "set note velocity redo should be available");
    require(project.findMidiNoteById(note->id)->velocity == 80, "redo should restore note velocity");
}

void setMidiNoteChannelCommandSupportsUndoAndRedo()
{
    trackloom::Project project("MIDI Notes");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 0, 960);
    const auto note = project.createMidiNote(clip->id, 0, 240, 60, 100, 1);

    require(note.has_value(), "note should be created before channel command");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::SetMidiNoteChannelCommand>(note->id, 2));

    require(result.success, "set midi note channel command should succeed");
    require(project.findMidiNoteById(note->id)->channel == 2, "command should update note channel");

    require(commands.undo(project), "set note channel undo should be available");
    require(project.findMidiNoteById(note->id)->channel == 1, "undo should restore note channel");

    require(commands.redo(project), "set note channel redo should be available");
    require(project.findMidiNoteById(note->id)->channel == 2, "redo should restore note channel");
}

void deleteMidiNoteCommandSupportsUndoAndRedo()
{
    trackloom::Project project("MIDI Notes");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 0, 960);
    const auto note = project.createMidiNote(clip->id, 0, 240, 60, 100, 1);

    require(note.has_value(), "note should be created before delete command");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::DeleteMidiNoteCommand>(note->id));

    require(result.success, "delete midi note command should succeed");
    require(!project.findMidiNoteById(note->id).has_value(), "delete command should remove note");

    require(commands.undo(project), "delete note undo should be available");
    require(project.findMidiNoteById(note->id).has_value(), "undo should restore note");
    require(project.findMidiNoteById(note->id).value() == *note, "undo should restore complete note state");

    require(commands.redo(project), "delete note redo should be available");
    require(!project.findMidiNoteById(note->id).has_value(), "redo should delete note again");
}

void invalidMidiNoteCommandDoesNotModifyProject()
{
    trackloom::Project project("MIDI Notes");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 0, 960);
    const auto note = project.createMidiNote(clip->id, 0, 240, 60, 100, 1);

    require(note.has_value(), "note should be created before invalid commands");
    auto addOutOfRangeResult = commands.execute(
        project,
        std::make_unique<trackloom::AddMidiNoteCommand>(clip->id, 900, 120, 60, 100, 1));
    auto setInvalidTimingResult = commands.execute(
        project,
        std::make_unique<trackloom::SetMidiNoteTimingCommand>(note->id, 900, 120));
    auto setInvalidPitchResult = commands.execute(
        project,
        std::make_unique<trackloom::SetMidiNotePitchCommand>(note->id, 128));
    auto deleteMissingResult = commands.execute(
        project,
        std::make_unique<trackloom::DeleteMidiNoteCommand>("missing-note"));

    require(!addOutOfRangeResult.success, "out-of-range add note command should fail");
    require(!setInvalidTimingResult.success, "invalid timing note command should fail");
    require(!setInvalidPitchResult.success, "invalid pitch note command should fail");
    require(!deleteMissingResult.success, "missing note delete command should fail");
    require(project.findMidiNoteById(note->id).value() == *note, "failed note commands should not modify note");
    require(project.findClipById(clip->id)->midiNotes.size() == 1, "failed note commands should not add notes");
    require(!commands.canUndo(), "failed note commands should not enter undo stack");
}

void renameTrackCommandSupportsUndoAndRedo()
{
    trackloom::Project project("Tracks");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Piano", trackloom::TrackType::Instrument);

    auto result = commands.execute(
        project,
        std::make_unique<trackloom::RenameTrackCommand>(track.id, "Lead"));

    require(result.success, "rename track command should succeed");
    require(project.findTrackById(track.id)->name == "Lead", "rename command should update track name");

    require(commands.undo(project), "rename track undo should be available");
    require(project.findTrackById(track.id)->name == "Piano", "undo should restore original track name");

    require(commands.redo(project), "rename track redo should be available");
    require(project.findTrackById(track.id)->name == "Lead", "redo should restore renamed track name");
}

void invalidRenameTrackCommandDoesNotModifyProject()
{
    trackloom::Project project("Tracks");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Piano", trackloom::TrackType::Instrument);

    auto emptyNameResult = commands.execute(
        project,
        std::make_unique<trackloom::RenameTrackCommand>(track.id, ""));
    auto missingTrackResult = commands.execute(
        project,
        std::make_unique<trackloom::RenameTrackCommand>("missing-track", "Lead"));

    require(!emptyNameResult.success, "empty track rename command should fail validation");
    require(!missingTrackResult.success, "missing track rename command should fail validation");
    require(project.findTrackById(track.id)->name == "Piano", "failed track rename should not modify project");
    require(!commands.canUndo(), "failed track rename should not enter undo stack");
}

void deleteTrackCommandSupportsUndoAndRedo()
{
    trackloom::Project project("Tracks");
    trackloom::CommandStack commands;
    const auto leadTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto padTrack = project.createTrack("Pad", trackloom::TrackType::Instrument);
    const auto vocalTrack = project.createTrack("Vocal", trackloom::TrackType::Audio);
    trackloom::TrackPlaybackState padPlayback;
    trackloom::TrackMixState padMix;

    padPlayback.disabled = true;
    padMix.gain = 0.50f;
    padMix.pan = -0.25f;
    require(project.setTrackPlaybackState(padTrack.id, padPlayback), "project should set playback before delete");
    require(project.setTrackMixState(padTrack.id, padMix), "project should set mix before delete");

    const auto leadClip = project.createClip(leadTrack.id, "Lead Intro", trackloom::ClipType::Midi, 0, 960);
    const auto padClip = project.createClip(padTrack.id, "Pad Intro", trackloom::ClipType::Midi, 960, 960);
    const auto vocalClip = project.createClip(vocalTrack.id, "Vocal Take", trackloom::ClipType::Audio, 0, 1920);

    require(leadClip.has_value(), "lead clip should exist before track delete");
    require(padClip.has_value(), "pad clip should exist before track delete");
    require(vocalClip.has_value(), "vocal clip should exist before track delete");

    auto result = commands.execute(
        project,
        std::make_unique<trackloom::DeleteTrackCommand>(padTrack.id));

    require(result.success, "delete track command should succeed");
    require(project.tracks().size() == 2, "delete track should remove one track");
    require(project.tracks()[0].id == leadTrack.id, "delete should keep previous track order");
    require(project.tracks()[1].id == vocalTrack.id, "delete should close the track order gap");
    require(!project.findTrackById(padTrack.id).has_value(), "delete should remove target track");
    require(!project.findClipById(padClip->id).has_value(), "delete should remove target track clips");
    require(project.findClipById(leadClip->id).has_value(), "delete should keep other instrument clips");
    require(project.findClipById(vocalClip->id).has_value(), "delete should keep other audio clips");

    require(commands.undo(project), "delete track undo should be available");
    require(project.tracks().size() == 3, "undo should restore deleted track");
    require(project.tracks()[0].id == leadTrack.id, "undo should keep first track order");
    require(project.tracks()[1].id == padTrack.id, "undo should restore deleted track at original index");
    require(project.tracks()[2].id == vocalTrack.id, "undo should keep later track order");
    require(project.findTrackById(padTrack.id)->playback == padPlayback, "undo should restore playback state");
    require(project.findTrackById(padTrack.id)->mix == padMix, "undo should restore mix state");
    require(project.findClipById(padClip->id).has_value(), "undo should restore deleted track clip");
    require(project.findClipById(padClip->id)->trackId == padTrack.id, "restored clip should still reference restored track");

    require(commands.redo(project), "delete track redo should be available");
    require(project.tracks().size() == 2, "redo should remove track again");
    require(!project.findTrackById(padTrack.id).has_value(), "redo should remove restored track");
    require(!project.findClipById(padClip->id).has_value(), "redo should remove restored track clip");
}

void invalidDeleteTrackCommandDoesNotModifyProject()
{
    trackloom::Project project("Tracks");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Intro", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "clip should exist before invalid track delete");
    auto result = commands.execute(
        project,
        std::make_unique<trackloom::DeleteTrackCommand>("missing-track"));

    require(!result.success, "missing track delete command should fail validation");
    require(project.tracks().size() == 1, "failed track delete should not remove tracks");
    require(project.clips().size() == 1, "failed track delete should not remove clips");
    require(project.findTrackById(track.id).has_value(), "failed track delete should keep original track");
    require(project.findClipById(clip->id).has_value(), "failed track delete should keep original clip");
    require(!commands.canUndo(), "failed track delete should not enter undo stack");
}

void moveTrackCommandSupportsUndoAndRedo()
{
    trackloom::Project project("Tracks");
    trackloom::CommandStack commands;
    const auto leadTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto padTrack = project.createTrack("Pad", trackloom::TrackType::Instrument);
    const auto vocalTrack = project.createTrack("Vocal", trackloom::TrackType::Audio);

    auto result = commands.execute(
        project,
        std::make_unique<trackloom::MoveTrackCommand>(vocalTrack.id, 0));

    require(result.success, "move track command should succeed");
    require(project.tracks()[0].id == vocalTrack.id, "move command should place track at target index");
    require(project.tracks()[1].id == leadTrack.id, "move command should shift earlier tracks right");
    require(project.tracks()[2].id == padTrack.id, "move command should keep relative shifted order");

    require(commands.undo(project), "move track undo should be available");
    require(project.tracks()[0].id == leadTrack.id, "undo should restore first track");
    require(project.tracks()[1].id == padTrack.id, "undo should restore second track");
    require(project.tracks()[2].id == vocalTrack.id, "undo should restore moved track");

    require(commands.redo(project), "move track redo should be available");
    require(project.tracks()[0].id == vocalTrack.id, "redo should move track to target index again");
    require(project.tracks()[1].id == leadTrack.id, "redo should shift first track right again");
    require(project.tracks()[2].id == padTrack.id, "redo should shift second track right again");
}

void invalidMoveTrackCommandDoesNotModifyProject()
{
    trackloom::Project project("Tracks");
    trackloom::CommandStack commands;
    const auto leadTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto padTrack = project.createTrack("Pad", trackloom::TrackType::Instrument);

    auto sameIndexResult = commands.execute(
        project,
        std::make_unique<trackloom::MoveTrackCommand>(padTrack.id, 1));
    auto missingTrackResult = commands.execute(
        project,
        std::make_unique<trackloom::MoveTrackCommand>("missing-track", 0));
    auto outOfRangeResult = commands.execute(
        project,
        std::make_unique<trackloom::MoveTrackCommand>(padTrack.id, 2));

    require(!sameIndexResult.success, "same-index track move command should fail validation");
    require(!missingTrackResult.success, "missing track move command should fail validation");
    require(!outOfRangeResult.success, "out-of-range track move command should fail validation");
    require(project.tracks()[0].id == leadTrack.id, "failed track move should keep first track");
    require(project.tracks()[1].id == padTrack.id, "failed track move should keep second track");
    require(!commands.canUndo(), "failed track move should not enter undo stack");
}

void setTrackViewStateCommandSupportsUndoAndRedo()
{
    trackloom::Project project("View");
    trackloom::CommandStack commands;
    const auto folderTrack = project.createTrack("Folder", trackloom::TrackType::Folder);
    trackloom::TrackViewState state;

    state.hidden = true;
    state.collapsed = true;

    auto result = commands.execute(
        project,
        std::make_unique<trackloom::SetTrackViewStateCommand>(folderTrack.id, state));

    require(result.success, "view state command should succeed");
    require(project.findTrackById(folderTrack.id)->view == state, "view state command should write state");

    require(commands.undo(project), "view state undo should be available");
    require(!project.findTrackById(folderTrack.id)->view.hidden, "undo should restore visible state");
    require(!project.findTrackById(folderTrack.id)->view.collapsed, "undo should restore expanded state");

    require(commands.redo(project), "view state redo should be available");
    require(project.findTrackById(folderTrack.id)->view == state, "redo should restore view state");
}

void invalidTrackViewStateCommandDoesNotModifyProject()
{
    trackloom::Project project("View");
    trackloom::CommandStack commands;
    const auto leadTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    trackloom::TrackViewState state;

    state.collapsed = true;

    auto collapsedInstrumentResult = commands.execute(
        project,
        std::make_unique<trackloom::SetTrackViewStateCommand>(leadTrack.id, state));
    auto missingTrackResult = commands.execute(
        project,
        std::make_unique<trackloom::SetTrackViewStateCommand>("missing-track", state));

    require(!collapsedInstrumentResult.success, "collapsed instrument view command should fail");
    require(!missingTrackResult.success, "missing track view command should fail");
    require(!project.findTrackById(leadTrack.id)->view.hidden, "failed view command should not hide track");
    require(!project.findTrackById(leadTrack.id)->view.collapsed, "failed view command should not collapse track");
    require(!commands.canUndo(), "failed view command should not enter undo stack");
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

void projectCanRoundTripTrackRename()
{
    trackloom::Project project("Renamed Track Song");
    const auto track = project.createTrack("Piano", trackloom::TrackType::Instrument);

    require(project.renameTrackById(track.id, "Lead"), "track should rename before save");

    const auto saved = trackloom::saveProjectToText(project);
    const auto loaded = trackloom::loadProjectFromText(saved);

    require(saved.find("trackloom_project 10\n") == 0, "track rename should use current project format version");
    require(loaded.project.has_value(), "project with renamed track should load");
    require(loaded.project->findTrackById(track.id).has_value(), "loaded project should keep renamed track id");
    require(loaded.project->findTrackById(track.id)->name == "Lead", "loaded project should keep renamed track name");
}

void projectCanSaveAfterTrackDeletion()
{
    trackloom::Project project("Deleted Track Song");
    trackloom::CommandStack commands;
    const auto leadTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto padTrack = project.createTrack("Pad", trackloom::TrackType::Instrument);
    const auto leadClip = project.createClip(leadTrack.id, "Lead Intro", trackloom::ClipType::Midi, 0, 960);
    const auto padClip = project.createClip(padTrack.id, "Pad Intro", trackloom::ClipType::Midi, 960, 960);

    require(leadClip.has_value(), "lead clip should exist before track delete save");
    require(padClip.has_value(), "pad clip should exist before track delete save");

    auto result = commands.execute(
        project,
        std::make_unique<trackloom::DeleteTrackCommand>(padTrack.id));

    require(result.success, "track delete command should succeed before save");

    const auto saved = trackloom::saveProjectToText(project);
    const auto loaded = trackloom::loadProjectFromText(saved);

    require(loaded.project.has_value(), "project after track delete should load");
    require(loaded.project->tracks().size() == 1, "loaded project should keep remaining track only");
    require(loaded.project->findTrackById(leadTrack.id).has_value(), "loaded project should keep undeleted track");
    require(!loaded.project->findTrackById(padTrack.id).has_value(), "loaded project should not restore deleted track");
    require(loaded.project->clips().size() == 1, "loaded project should keep only undeleted track clips");
    require(loaded.project->findClipById(leadClip->id).has_value(), "loaded project should keep undeleted clip");
    require(!loaded.project->findClipById(padClip->id).has_value(), "loaded project should not keep deleted track clip");
}

void projectCanRoundTripTrackReorder()
{
    trackloom::Project project("Reordered Track Song");
    const auto leadTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto padTrack = project.createTrack("Pad", trackloom::TrackType::Instrument);
    const auto vocalTrack = project.createTrack("Vocal", trackloom::TrackType::Audio);
    const auto padClip = project.createClip(padTrack.id, "Pad Intro", trackloom::ClipType::Midi, 0, 960);

    require(padClip.has_value(), "pad clip should exist before track reorder save");
    require(project.moveTrackToIndex(padTrack.id, 2), "track should reorder before save");

    const auto saved = trackloom::saveProjectToText(project);
    const auto loaded = trackloom::loadProjectFromText(saved);

    require(saved.find("trackloom_project 10\n") == 0, "track reorder should use current project format version");
    require(loaded.project.has_value(), "project with reordered tracks should load");
    require(loaded.project->tracks()[0].id == leadTrack.id, "loaded project should keep first track order");
    require(loaded.project->tracks()[1].id == vocalTrack.id, "loaded project should keep shifted track order");
    require(loaded.project->tracks()[2].id == padTrack.id, "loaded project should keep moved track order");
    require(loaded.project->findClipById(padClip->id)->trackId == padTrack.id, "loaded reordered project should keep clip ownership");
}

void projectCanRoundTripTrackViewState()
{
    trackloom::Project project("View Song");
    const auto leadTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto folderTrack = project.createTrack("Folder", trackloom::TrackType::Folder);
    trackloom::TrackViewState leadState;
    trackloom::TrackViewState folderState;

    leadState.hidden = true;
    folderState.hidden = true;
    folderState.collapsed = true;
    require(project.setTrackViewState(leadTrack.id, leadState), "project should accept hidden track view state");
    require(project.setTrackViewState(folderTrack.id, folderState), "project should accept folder track view state");

    const auto saved = trackloom::saveProjectToText(project);
    const auto loaded = trackloom::loadProjectFromText(saved);

    require(saved.find("trackloom_project 10\n") == 0, "saved view project should use current format version");
    require(saved.find("track_view_state " + leadTrack.id + " hidden=1 collapsed=0\n") != std::string::npos, "saved project should include hidden state");
    require(saved.find("track_view_state " + folderTrack.id + " hidden=1 collapsed=1\n") != std::string::npos, "saved project should include folder collapsed state");
    require(loaded.project.has_value(), "project with view state should load");
    require(loaded.project->findTrackById(leadTrack.id)->view == leadState, "loaded project should keep hidden state");
    require(loaded.project->findTrackById(folderTrack.id)->view == folderState, "loaded project should keep folder collapsed state");
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

    require(saved.find("trackloom_project 10\n") == 0, "saved project should use current format version");
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

    require(saved.find("trackloom_project 10\n") == 0, "saved mix project should use current format version");
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

    require(saved.find("trackloom_project 10\n") == 0, "saved clip project should use current format version");
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

void projectCanRoundTripTimelineClipDuplicate()
{
    trackloom::Project project("Duplicate Clip Song");
    const auto sourceTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto targetTrack = project.createTrack("Pad", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(sourceTrack.id, "Copied Intro", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "clip should be created before duplicate round trip");
    const auto duplicate = project.duplicateClipToTrackAtTick(clip->id, targetTrack.id, 1920);
    require(duplicate.has_value(), "clip should duplicate before save");

    const auto saved = trackloom::saveProjectToText(project);
    const auto loaded = trackloom::loadProjectFromText(saved);

    require(saved.find("clip " + clip->id + " " + sourceTrack.id + " Midi 0 960 Copied Intro\n") != std::string::npos, "saved project should include source clip");
    require(saved.find("clip " + duplicate->id + " " + targetTrack.id + " Midi 1920 960 Copied Intro\n") != std::string::npos, "saved project should include duplicate clip");
    require(loaded.project.has_value(), "project with duplicated clips should load");
    require(loaded.project->clips().size() == 2, "loaded project should keep source and duplicate clips");
    require(loaded.project->findClipById(duplicate->id)->trackId == targetTrack.id, "loaded duplicate should keep target track");
    require(loaded.project->findClipById(duplicate->id)->startTick == 1920, "loaded duplicate should keep start tick");
    require(loaded.project->findClipById(duplicate->id)->lengthTick == 960, "loaded duplicate should keep source length");
}

void projectCanRoundTripTimelineClipTrim()
{
    trackloom::Project project("Trim Clip Song");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto startTrimmedClip = project.createClip(track.id, "Start Trimmed", trackloom::ClipType::Midi, 0, 960);
    const auto endTrimmedClip = project.createClip(track.id, "End Trimmed", trackloom::ClipType::Midi, 960, 960);

    require(startTrimmedClip.has_value(), "start-trimmed clip should be created before round trip");
    require(endTrimmedClip.has_value(), "end-trimmed clip should be created before round trip");
    require(project.trimClipStartToTick(startTrimmedClip->id, 240), "start trim should succeed before save");
    require(project.trimClipEndToTick(endTrimmedClip->id, 1440), "end trim should succeed before save");

    const auto saved = trackloom::saveProjectToText(project);
    const auto loaded = trackloom::loadProjectFromText(saved);

    require(saved.find("clip " + startTrimmedClip->id + " " + track.id + " Midi 240 720 Start Trimmed\n") != std::string::npos, "saved project should include start-trimmed clip");
    require(saved.find("clip " + endTrimmedClip->id + " " + track.id + " Midi 960 480 End Trimmed\n") != std::string::npos, "saved project should include end-trimmed clip");
    require(loaded.project.has_value(), "project with trimmed clips should load");
    require(loaded.project->findClipById(startTrimmedClip->id)->startTick == 240, "loaded start-trimmed clip should keep start");
    require(loaded.project->findClipById(startTrimmedClip->id)->lengthTick == 720, "loaded start-trimmed clip should keep length");
    require(loaded.project->findClipById(endTrimmedClip->id)->startTick == 960, "loaded end-trimmed clip should keep start");
    require(loaded.project->findClipById(endTrimmedClip->id)->lengthTick == 480, "loaded end-trimmed clip should keep length");
}

void projectCanRoundTripTimelineMarkers()
{
    trackloom::Project project("Marker Song");
    const auto introMarker = project.createMarker("Intro", 0);
    const auto verseMarker = project.createMarker("Verse", 960);

    require(introMarker.has_value(), "intro marker should be created before round trip");
    require(verseMarker.has_value(), "verse marker should be created before round trip");
    require(project.renameMarkerById(verseMarker->id, "Verse A"), "marker should rename before save");
    require(project.moveMarkerToTick(verseMarker->id, 1920), "marker should move before save");

    const auto saved = trackloom::saveProjectToText(project);
    const auto loaded = trackloom::loadProjectFromText(saved);

    require(saved.find("trackloom_project 10\n") == 0, "saved marker project should use current format version");
    require(saved.find("marker " + introMarker->id + " 0 Intro\n") != std::string::npos, "saved project should include intro marker");
    require(saved.find("marker " + verseMarker->id + " 1920 Verse A\n") != std::string::npos, "saved project should include edited marker");
    require(loaded.project.has_value(), "project with markers should load");
    require(loaded.project->markers().size() == 2, "loaded project should keep markers");
    require(loaded.project->findMarkerById(introMarker->id)->name == "Intro", "loaded intro marker should keep name");
    require(loaded.project->findMarkerById(introMarker->id)->tick == 0, "loaded intro marker should keep tick");
    require(loaded.project->findMarkerById(verseMarker->id)->name == "Verse A", "loaded edited marker should keep name with spaces");
    require(loaded.project->findMarkerById(verseMarker->id)->tick == 1920, "loaded edited marker should keep moved tick");
}

void projectCanSaveAfterTimelineMarkerDeletion()
{
    trackloom::Project project("Deleted Marker Song");
    trackloom::CommandStack commands;
    const auto deletedMarker = project.createMarker("Deleted Intro", 0);
    const auto keptMarker = project.createMarker("Kept Verse", 960);

    require(deletedMarker.has_value(), "deleted marker should be created before save-delete test");
    require(keptMarker.has_value(), "kept marker should be created before save-delete test");
    require(commands.execute(project, std::make_unique<trackloom::DeleteMarkerCommand>(deletedMarker->id)).success, "delete marker should succeed before save");

    const auto saved = trackloom::saveProjectToText(project);
    const auto loaded = trackloom::loadProjectFromText(saved);

    require(saved.find("marker " + deletedMarker->id + " ") == std::string::npos, "saved project should omit deleted marker");
    require(saved.find("marker " + keptMarker->id + " 960 Kept Verse\n") != std::string::npos, "saved project should keep unrelated marker");
    require(loaded.project.has_value(), "project saved after marker deletion should load");
    require(!loaded.project->findMarkerById(deletedMarker->id).has_value(), "loaded project should not contain deleted marker");
    require(loaded.project->findMarkerById(keptMarker->id).has_value(), "loaded project should contain kept marker");
    require(loaded.project->markers().size() == 1, "loaded project should contain only kept marker");
}

void projectCanRoundTripTempoEvents()
{
    trackloom::Project project("Tempo Song");
    require(project.setTempoEventBpm("tempo-1", 100.0), "default tempo should update before round trip");
    const auto customTempo = project.createTempoEvent(960, 60.0);

    require(customTempo.has_value(), "custom tempo should be created before round trip");
    const auto saved = trackloom::saveProjectToText(project);
    const auto loaded = trackloom::loadProjectFromText(saved);

    require(saved.find("trackloom_project 10\n") == 0, "saved tempo project should use current format version");
    require(saved.find("tempo tempo-1 0 100\n") != std::string::npos, "saved project should include default tempo record");
    require(saved.find("tempo " + customTempo->id + " 960 60\n") != std::string::npos, "saved project should include custom tempo record");
    require(loaded.project.has_value(), "project with tempo events should load");
    require(loaded.project->tempoEvents().size() == 2, "loaded project should keep tempo events");
    require(numbersNear(loaded.project->findTempoEventById("tempo-1")->beatsPerMinute, 100.0), "loaded default tempo should keep BPM");
    require(loaded.project->findTempoEventById(customTempo->id)->tick == 960, "loaded custom tempo should keep tick");
    require(numbersNear(loaded.project->findTempoEventById(customTempo->id)->beatsPerMinute, 60.0), "loaded custom tempo should keep BPM");
    require(numbersNear(loaded.project->tickToSeconds(1920), 1.6), "loaded tempo map should preserve tick-to-seconds conversion");
}

void projectCanRoundTripTimeSignatureEvents()
{
    trackloom::Project project("Meter Song");
    require(project.setTimeSignature("meter-1", 6, 8), "default time signature should update before round trip");
    const auto customMeter = project.createTimeSignatureEvent(3840, 3, 4);

    require(customMeter.has_value(), "custom time signature should be created before round trip");
    const auto saved = trackloom::saveProjectToText(project);
    const auto loaded = trackloom::loadProjectFromText(saved);

    require(saved.find("trackloom_project 10\n") == 0, "saved time signature project should use current format version");
    require(saved.find("time_signature meter-1 0 6 8\n") != std::string::npos, "saved project should include default time signature record");
    require(saved.find("time_signature " + customMeter->id + " 3840 3 4\n") != std::string::npos, "saved project should include custom time signature record");
    require(loaded.project.has_value(), "project with time signature events should load");
    require(loaded.project->timeSignatureEvents().size() == 2, "loaded project should keep time signature events");
    require(loaded.project->findTimeSignatureEventById("meter-1")->numerator == 6, "loaded default time signature should keep numerator");
    require(loaded.project->findTimeSignatureEventById("meter-1")->denominator == 8, "loaded default time signature should keep denominator");
    require(loaded.project->findTimeSignatureEventById(customMeter->id)->tick == 3840, "loaded custom time signature should keep tick");
    require(loaded.project->findTimeSignatureEventById(customMeter->id)->numerator == 3, "loaded custom time signature should keep numerator");
    require(loaded.project->findTimeSignatureEventById(customMeter->id)->denominator == 4, "loaded custom time signature should keep denominator");
    require(loaded.project->ticksPerMeasureAtTick(3840) == 2880, "loaded time signature map should preserve measure length");
}

void projectCanRoundTripMidiNotes()
{
    trackloom::Project project("MIDI Song");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 0, 960);
    const auto note = project.createMidiNote(clip->id, 120, 240, 60, 100, 1);

    require(note.has_value(), "note should be created before round trip");
    const auto saved = trackloom::saveProjectToText(project);
    const auto loaded = trackloom::loadProjectFromText(saved);

    require(saved.find("trackloom_project 10\n") == 0, "saved midi note project should use current format version");
    require(saved.find("midi_note " + clip->id + " " + note->id + " 120 240 60 100 1\n") != std::string::npos, "saved project should include midi note record");
    require(loaded.project.has_value(), "project with midi notes should load");
    const auto loadedClip = loaded.project->findClipById(clip->id);
    require(loadedClip.has_value(), "loaded project should keep midi clip");
    require(loadedClip->midiNotes.size() == 1, "loaded midi clip should keep note");
    require(loaded.project->findMidiNoteById(note->id).has_value(), "loaded project should find note by id");
    require(loaded.project->findMidiNoteById(note->id).value() == *note, "loaded note should keep complete state");
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

void versionFiveProjectLoadsDefaultTrackViewState()
{
    const std::string text =
        "trackloom_project 5\n"
        "name Clip Song\n"
        "track track-1 Instrument Lead Piano\n"
        "track_playback_state track-1 muted=0 soloed=0 disabled=0\n"
        "track_mix_state track-1 gain=1 pan=0\n"
        "clip clip-1 track-1 Midi 0 960 Intro\n";

    const auto loaded = trackloom::loadProjectFromText(text);

    require(loaded.project.has_value(), "version 5 project should load with default view state");
    const auto track = loaded.project->findTrackById("track-1");
    require(track.has_value(), "version 5 project should contain track");
    require(!track->view.hidden, "version 5 track should default to visible");
    require(!track->view.collapsed, "version 5 track should default to expanded");
}

void versionSixProjectLoadsWithoutTimelineMarkers()
{
    const std::string text =
        "trackloom_project 6\n"
        "name View Song\n"
        "track track-1 Instrument Lead Piano\n"
        "track_playback_state track-1 muted=0 soloed=0 disabled=0\n"
        "track_mix_state track-1 gain=1 pan=0\n"
        "track_view_state track-1 hidden=1 collapsed=0\n"
        "clip clip-1 track-1 Midi 0 960 Intro\n";

    const auto loaded = trackloom::loadProjectFromText(text);

    require(loaded.project.has_value(), "version 6 project should load without markers");
    require(loaded.project->markers().empty(), "version 6 project should default to no markers");
}

void versionSevenProjectLoadsDefaultTempoMap()
{
    const std::string text =
        "trackloom_project 7\n"
        "name Marker Song\n"
        "marker marker-1 0 Intro\n";

    const auto loaded = trackloom::loadProjectFromText(text);

    require(loaded.project.has_value(), "version 7 project should load with default tempo map");
    require(loaded.project->tempoEvents().size() == 1, "version 7 project should have one default tempo event");
    require(loaded.project->tempoEvents().front().id == "tempo-1", "version 7 default tempo id should be stable");
    require(loaded.project->tempoEvents().front().tick == 0, "version 7 default tempo should start at tick zero");
    require(numbersNear(loaded.project->tempoEvents().front().beatsPerMinute, 120.0), "version 7 default tempo should be 120 BPM");
}

void versionEightProjectLoadsDefaultTimeSignatureMap()
{
    const std::string text =
        "trackloom_project 8\n"
        "name Tempo Song\n"
        "tempo tempo-1 0 120\n";

    const auto loaded = trackloom::loadProjectFromText(text);

    require(loaded.project.has_value(), "version 8 project should load with default time signature map");
    require(loaded.project->timeSignatureEvents().size() == 1, "version 8 project should have one default time signature event");
    require(loaded.project->timeSignatureEvents().front().id == "meter-1", "version 8 default time signature id should be stable");
    require(loaded.project->timeSignatureEvents().front().tick == 0, "version 8 default time signature should start at tick zero");
    require(loaded.project->timeSignatureEvents().front().numerator == 4, "version 8 default numerator should be 4");
    require(loaded.project->timeSignatureEvents().front().denominator == 4, "version 8 default denominator should be 4");
}

void versionNineProjectLoadsMidiClipsWithoutNotes()
{
    const std::string text =
        "trackloom_project 9\n"
        "name Meter Song\n"
        "tempo tempo-1 0 120\n"
        "time_signature meter-1 0 4 4\n"
        "track track-1 Instrument Lead\n"
        "track_playback_state track-1 muted=0 soloed=0 disabled=0\n"
        "track_mix_state track-1 gain=1 pan=0\n"
        "track_view_state track-1 hidden=0 collapsed=0\n"
        "clip clip-1 track-1 Midi 0 960 Lead Phrase\n";

    const auto loaded = trackloom::loadProjectFromText(text);

    require(loaded.project.has_value(), "version 9 project should load with empty midi note lists");
    const auto clip = loaded.project->findClipById("clip-1");
    require(clip.has_value(), "version 9 midi clip should load");
    require(clip->midiNotes.empty(), "version 9 midi clip should default to no midi notes");
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

void invalidTrackViewStateRecordIsRejected()
{
    const std::string text =
        "trackloom_project 6\n"
        "name Broken View Song\n"
        "track track-1 Instrument Lead Piano\n"
        "track_playback_state track-1 muted=0 soloed=0 disabled=0\n"
        "track_mix_state track-1 gain=1 pan=0\n"
        "track_view_state track-1 hidden=0 collapsed=1\n";

    const auto loaded = trackloom::loadProjectFromText(text);

    require(!loaded.project.has_value(), "invalid view state should be rejected");
    require(!loaded.error.empty(), "invalid view state should report an error");
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

void invalidMarkerRecordIsRejected()
{
    const std::string text =
        "trackloom_project 7\n"
        "name Broken Marker Song\n"
        "marker marker-1 -1 Bad Marker\n";

    const auto loaded = trackloom::loadProjectFromText(text);

    require(!loaded.project.has_value(), "negative marker tick should fail");
    require(!loaded.error.empty(), "invalid marker record should report an error");
}

void invalidTempoRecordIsRejected()
{
    const std::string text =
        "trackloom_project 8\n"
        "name Broken Tempo Song\n"
        "tempo tempo-2 -1 120\n";

    const auto loaded = trackloom::loadProjectFromText(text);

    require(!loaded.project.has_value(), "negative tempo tick should fail");
    require(!loaded.error.empty(), "invalid tempo record should report an error");
}

void invalidTimeSignatureRecordIsRejected()
{
    const std::string text =
        "trackloom_project 9\n"
        "name Broken Meter Song\n"
        "time_signature meter-2 3840 4 3\n";

    const auto loaded = trackloom::loadProjectFromText(text);

    require(!loaded.project.has_value(), "invalid time signature denominator should fail");
    require(!loaded.error.empty(), "invalid time signature record should report an error");
}

void invalidMidiNoteRecordIsRejected()
{
    const std::string text =
        "trackloom_project 10\n"
        "name Broken MIDI Song\n"
        "track track-1 Instrument Lead\n"
        "track_playback_state track-1 muted=0 soloed=0 disabled=0\n"
        "track_mix_state track-1 gain=1 pan=0\n"
        "track_view_state track-1 hidden=0 collapsed=0\n"
        "clip clip-1 track-1 Midi 0 960 Lead Phrase\n"
        "midi_note clip-1 note-1 0 120 128 100 1\n";

    const auto loaded = trackloom::loadProjectFromText(text);

    require(!loaded.project.has_value(), "invalid midi note pitch should fail");
    require(!loaded.error.empty(), "invalid midi note record should report an error");
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

void projectPlaybackGraphIgnoresHiddenState()
{
    trackloom::Project project("Graph");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    CountingAudioSource source(0.50f);
    trackloom::TrackViewState state;
    trackloom::ProjectPlaybackGraph graph;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    state.hidden = true;
    require(project.setTrackViewState(track.id, state), "project should set hidden view state");
    require(graph.prepare(2, 8), "project graph prepare should succeed");
    require(graph.rebuild(project, { { track.id, &source } }), "project graph rebuild should accept hidden track binding");
    require(graph.render(block, 48000.0), "project graph hidden render should succeed");

    require(source.renderCount() == 1, "hidden graph source should still process");
    require(allSamplesNear(samples, 0.50f), "hidden graph source should remain audible");
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
        projectCanRenameTrack();
        projectCanMoveTrackToIndex();
        projectRejectsInvalidTrackMoves();
        newTrackViewStateStartsDefault();
        projectCanSetTrackViewState();
        projectRejectsInvalidTrackViewState();
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
        projectCanDuplicateMidiClipToInstrumentTrack();
        projectCanDuplicateAudioClipToAudioTrack();
        projectRejectsInvalidClipDuplicates();
        projectCanTrimClipStartWithinExistingRange();
        projectCanTrimClipEndWithinExistingRange();
        projectRejectsInvalidClipTrims();
        newProjectStartsWithoutMarkers();
        projectCanCreateTimelineMarker();
        projectCanEditTimelineMarker();
        projectRejectsInvalidTimelineMarkers();
        newProjectStartsWithDefaultTempoEvent();
        projectCanCreateTempoEvent();
        projectCanConvertTicksToSeconds();
        projectRejectsInvalidTempoEvents();
        newProjectStartsWithDefaultTimeSignatureEvent();
        projectCanCreateTimeSignatureEvent();
        projectCanQueryTimeSignatureAndMeasureLength();
        projectRejectsInvalidTimeSignatureEvents();
        midiClipCanCreateMidiNote();
        audioClipRejectsMidiNotes();
        projectRejectsInvalidMidiNotes();
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
        duplicateClipCommandSupportsUndoAndRedo();
        invalidDuplicateClipCommandDoesNotModifyProject();
        deleteClipCommandRestoresMidiNotes();
        deleteTrackCommandRestoresMidiNotes();
        duplicateMidiClipCopiesNotesWithNewIds();
        splitMidiClipMovesRightSideNotes();
        splitMidiClipRejectsNotesCrossingSplitTick();
        clipTimingRejectsMidiNotesOutsideClipRange();
        trimClipStartCommandSupportsUndoAndRedo();
        trimClipEndCommandSupportsUndoAndRedo();
        invalidTrimClipCommandDoesNotModifyProject();
        addMarkerCommandSupportsUndoAndRedo();
        renameMarkerCommandSupportsUndoAndRedo();
        moveMarkerCommandSupportsUndoAndRedo();
        deleteMarkerCommandSupportsUndoAndRedo();
        invalidMarkerCommandDoesNotModifyProject();
        addTempoEventCommandSupportsUndoAndRedo();
        setTempoEventBpmCommandSupportsUndoAndRedo();
        moveTempoEventCommandSupportsUndoAndRedo();
        deleteTempoEventCommandSupportsUndoAndRedo();
        invalidTempoCommandDoesNotModifyProject();
        addTimeSignatureEventCommandSupportsUndoAndRedo();
        setTimeSignatureCommandSupportsUndoAndRedo();
        moveTimeSignatureEventCommandSupportsUndoAndRedo();
        deleteTimeSignatureEventCommandSupportsUndoAndRedo();
        invalidTimeSignatureCommandDoesNotModifyProject();
        addMidiNoteCommandSupportsUndoAndRedo();
        setMidiNoteTimingCommandSupportsUndoAndRedo();
        setMidiNotePitchCommandSupportsUndoAndRedo();
        setMidiNoteVelocityCommandSupportsUndoAndRedo();
        setMidiNoteChannelCommandSupportsUndoAndRedo();
        deleteMidiNoteCommandSupportsUndoAndRedo();
        invalidMidiNoteCommandDoesNotModifyProject();
        renameTrackCommandSupportsUndoAndRedo();
        invalidRenameTrackCommandDoesNotModifyProject();
        deleteTrackCommandSupportsUndoAndRedo();
        invalidDeleteTrackCommandDoesNotModifyProject();
        moveTrackCommandSupportsUndoAndRedo();
        invalidMoveTrackCommandDoesNotModifyProject();
        setTrackViewStateCommandSupportsUndoAndRedo();
        invalidTrackViewStateCommandDoesNotModifyProject();
        newTrackPlaybackStateStartsDefault();
        setTrackPlaybackStateCommandSupportsUndoAndRedo();
        invalidPlaybackStateCommandDoesNotModifyProject();
        newTrackMixStateStartsDefault();
        setTrackMixStateCommandSupportsUndoAndRedo();
        invalidTrackMixStateCommandDoesNotModifyProject();
        projectCanRoundTripThroughText();
        projectCanRoundTripTrackRename();
        projectCanSaveAfterTrackDeletion();
        projectCanRoundTripTrackReorder();
        projectCanRoundTripTrackViewState();
        projectCanRoundTripTrackPlaybackState();
        projectCanRoundTripTrackMixState();
        projectCanRoundTripTimelineClips();
        projectCanRoundTripTimelineClipEdits();
        projectCanSaveAfterTimelineClipDeletion();
        projectCanRoundTripTimelineClipTrackMove();
        projectCanRoundTripTimelineClipSplit();
        projectCanRoundTripTimelineClipDuplicate();
        projectCanRoundTripTimelineClipTrim();
        projectCanRoundTripTimelineMarkers();
        projectCanSaveAfterTimelineMarkerDeletion();
        projectCanRoundTripTempoEvents();
        projectCanRoundTripTimeSignatureEvents();
        projectCanRoundTripMidiNotes();
        versionOneProjectLoadsDefaultPlaybackState();
        versionTwoProjectLoadsDefaultMixState();
        versionThreeProjectLoadsDefaultPanState();
        versionFourProjectLoadsWithoutClips();
        versionFiveProjectLoadsDefaultTrackViewState();
        versionSixProjectLoadsWithoutTimelineMarkers();
        versionSevenProjectLoadsDefaultTempoMap();
        versionEightProjectLoadsDefaultTimeSignatureMap();
        versionNineProjectLoadsMidiClipsWithoutNotes();
        invalidTrackPlaybackStateRecordIsRejected();
        invalidTrackMixStateRecordIsRejected();
        invalidTrackViewStateRecordIsRejected();
        invalidClipRecordIsRejected();
        invalidMarkerRecordIsRejected();
        invalidTempoRecordIsRejected();
        invalidTimeSignatureRecordIsRejected();
        invalidMidiNoteRecordIsRejected();
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
        projectPlaybackGraphIgnoresHiddenState();
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
