#include "AudioDisable.h"
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

    require(saved.find("trackloom_project 2\n") == 0, "saved project should use format version 2");
    require(loaded.project.has_value(), "project with playback state should load");
    const auto loadedTrack = loaded.project->findTrackById(track.id);
    require(loadedTrack.has_value(), "loaded project should contain track");
    require(loadedTrack->playback == state, "loaded track should keep playback state");
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

}

int main()
{
    try {
        projectStartsEmpty();
        addTrackCommandSupportsUndoAndRedo();
        invalidCommandDoesNotModifyProject();
        newTrackPlaybackStateStartsDefault();
        setTrackPlaybackStateCommandSupportsUndoAndRedo();
        invalidPlaybackStateCommandDoesNotModifyProject();
        projectCanRoundTripThroughText();
        projectCanRoundTripTrackPlaybackState();
        versionOneProjectLoadsDefaultPlaybackState();
        invalidTrackPlaybackStateRecordIsRejected();
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
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "All core tests passed.\n";
    return 0;
}
