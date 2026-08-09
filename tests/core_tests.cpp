#include "AtomicFileReplace.h"
#include "AudioDisable.h"
#include "AudioPan.h"
#include "AudioProjectGraph.h"
#include "AudioSolo.h"
#include "AudioTrackPlayback.h"
#include "AudioMute.h"
#include "AudioGain.h"
#include "AudioMixer.h"
#include "AudioEngine.h"
#include "AudioEngineMidiBridge.h"
#include "Command.h"
#include "MidiDispatch.h"
#include "MidiOutputDevice.h"
#include "MidiPlayback.h"
#include "PreparedMidiPlaybackPlan.h"
#include "MidiOutputSession.h"
#include "MidiTrackRouter.h"
#include "PlaybackControl.h"
#include "PlaybackClock.h"
#include "ProjectFile.h"
#include "Project.h"
#include "ProjectMidiOutputGraph.h"
#include "ProjectPlaybackSession.h"
#include "ProjectSerializer.h"
#include "Transport.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

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

class FailingAudioSource final : public trackloom::AudioSource {
public:
    bool render(trackloom::AudioBlock, double) override
    {
        return false;
    }
};

class RecordingMidiEventReceiver final : public trackloom::MidiEventReceiver {
public:
    bool receiveMidiEvent(
        const trackloom::ScheduledMidiPlaybackEvent& event,
        const trackloom::MidiOutputMessage& message) override
    {
        events_.push_back(event);
        messages_.push_back(message);
        return true;
    }

    const std::vector<trackloom::ScheduledMidiPlaybackEvent>& events() const
    {
        return events_;
    }

    const std::vector<trackloom::MidiOutputMessage>& messages() const
    {
        return messages_;
    }

private:
    std::vector<trackloom::ScheduledMidiPlaybackEvent> events_;
    std::vector<trackloom::MidiOutputMessage> messages_;
};

class FailingMidiEventReceiver final : public trackloom::MidiEventReceiver {
public:
    explicit FailingMidiEventReceiver(int failAtCall)
        : failAtCall_(failAtCall)
    {
    }

    bool receiveMidiEvent(
        const trackloom::ScheduledMidiPlaybackEvent& event,
        const trackloom::MidiOutputMessage& message) override
    {
        events_.push_back(event);
        messages_.push_back(message);
        ++callCount_;
        return callCount_ != failAtCall_;
    }

    int callCount() const
    {
        return callCount_;
    }

    const std::vector<trackloom::ScheduledMidiPlaybackEvent>& events() const
    {
        return events_;
    }

    const std::vector<trackloom::MidiOutputMessage>& messages() const
    {
        return messages_;
    }

private:
    int failAtCall_ = 1;
    int callCount_ = 0;
    std::vector<trackloom::ScheduledMidiPlaybackEvent> events_;
    std::vector<trackloom::MidiOutputMessage> messages_;
};

class FakeMidiOutputDevicePort final : public trackloom::MidiOutputDevicePort {
public:
    FakeMidiOutputDevicePort(std::string id, std::string name)
        : info_ { std::move(id), std::move(name) }
    {
    }

    const trackloom::MidiOutputDeviceInfo& info() const override
    {
        return info_;
    }

    bool open() override
    {
        ++openCallCount_;
        if (!openShouldSucceed_) {
            return false;
        }

        open_ = true;
        return true;
    }

    void close() override
    {
        ++closeCallCount_;
        open_ = false;
    }

    bool isOpen() const override
    {
        return open_;
    }

    bool sendMidiMessage(const trackloom::MidiOutputMessage& message) override
    {
        ++sendCallCount_;
        if (!open_ || !sendShouldSucceed_) {
            return false;
        }

        messages_.push_back(message);
        return true;
    }

    void setOpenShouldSucceed(bool shouldSucceed)
    {
        openShouldSucceed_ = shouldSucceed;
    }

    void setSendShouldSucceed(bool shouldSucceed)
    {
        sendShouldSucceed_ = shouldSucceed;
    }

    int openCallCount() const
    {
        return openCallCount_;
    }

    int closeCallCount() const
    {
        return closeCallCount_;
    }

    int sendCallCount() const
    {
        return sendCallCount_;
    }

    const std::vector<trackloom::MidiOutputMessage>& messages() const
    {
        return messages_;
    }

private:
    trackloom::MidiOutputDeviceInfo info_;
    bool open_ = false;
    bool openShouldSucceed_ = true;
    bool sendShouldSucceed_ = true;
    int openCallCount_ = 0;
    int closeCallCount_ = 0;
    int sendCallCount_ = 0;
    std::vector<trackloom::MidiOutputMessage> messages_;
};

trackloom::ScheduledMidiPlaybackEvent makeScheduledMidiEvent(
    trackloom::MidiPlaybackEventType type,
    int sampleOffset,
    int channel,
    int noteNumber,
    int velocity,
    std::string trackId = "track-1")
{
    return {
        trackloom::MidiPlaybackEvent {
            type,
            trackId,
            "clip-1",
            "note-1",
            960,
            noteNumber,
            velocity,
            channel
        },
        sampleOffset
    };
}

std::filesystem::path makeTestDirectory(const std::string& name)
{
    const auto path = std::filesystem::temp_directory_path() / "trackloom_tests" / name;
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

void writeFileBytes(const std::filesystem::path& path, const std::string& contents)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    require(static_cast<bool>(output), "test file should be written");
}

std::string readFileBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "test file should be readable");
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}

bool containsRecoveryPath(
    const std::vector<std::filesystem::path>& recoveryPaths,
    const std::filesystem::path& expectedPath)
{
    for (const auto& recoveryPath : recoveryPaths) {
        if (recoveryPath == expectedPath) {
            return true;
        }
    }

    return false;
}

std::filesystem::path injectedRetainedRecoveryPath;
std::string injectedTemporaryProjectBytes;
int injectedProjectFileReplacementCallCount = 0;

trackloom::detail::TemporaryProjectWriteResult writeCanonicalProjectBytes(
    const std::filesystem::path& temporaryPath,
    const std::string& canonicalText)
{
    writeFileBytes(temporaryPath, canonicalText);
    return { true, "" };
}

trackloom::detail::TemporaryProjectWriteResult writeInjectedTemporaryProjectBytes(
    const std::filesystem::path& temporaryPath,
    const std::string&)
{
    writeFileBytes(temporaryPath, injectedTemporaryProjectBytes);
    return { true, "" };
}

trackloom::detail::TemporaryProjectWriteResult writeProjectThenReportFlushFailure(
    const std::filesystem::path& temporaryPath,
    const std::string& canonicalText)
{
    writeFileBytes(temporaryPath, canonicalText);
    return { false, "Injected temporary project flush failure." };
}

trackloom::detail::TemporaryProjectWriteResult writeProjectThenReportCloseFailure(
    const std::filesystem::path& temporaryPath,
    const std::string& canonicalText)
{
    writeFileBytes(temporaryPath, canonicalText);
    return { false, "Injected temporary project close failure." };
}

trackloom::AtomicFileReplaceResult recordAndInstallProjectFile(
    const std::filesystem::path& replacementPath,
    const std::filesystem::path& targetPath)
{
    ++injectedProjectFileReplacementCallCount;
    std::error_code copyError;
    std::filesystem::copy_file(
        replacementPath,
        targetPath,
        std::filesystem::copy_options::overwrite_existing,
        copyError);
    if (copyError) {
        return trackloom::AtomicFileReplaceResult::fail(
            "Test replacement could not install the target.",
            trackloom::AtomicFileTargetAvailability::Unknown,
            replacementPath);
    }

    std::error_code removeError;
    std::filesystem::remove(replacementPath, removeError);
    if (removeError) {
        return trackloom::AtomicFileReplaceResult::fail(
            "Test replacement could not consume the replacement.",
            trackloom::AtomicFileTargetAvailability::Available,
            replacementPath);
    }

    return trackloom::AtomicFileReplaceResult::ok();
}

trackloom::AtomicFileReplaceResult recordAndInstallProjectFileWithRecoveryPath(
    const std::filesystem::path& replacementPath,
    const std::filesystem::path& targetPath)
{
    const auto installed = recordAndInstallProjectFile(replacementPath, targetPath);
    if (!installed.success) {
        return installed;
    }

    return trackloom::AtomicFileReplaceResult::ok(injectedRetainedRecoveryPath);
}

bool throwDuringWorkspaceCleanup(const std::filesystem::path&, std::error_code&)
{
    throw std::runtime_error("Injected post-replacement cleanup failure.");
}

trackloom::AtomicFileReplaceResult replaceProjectFileAndRetainRecoveryPath(
    const std::filesystem::path& replacementPath,
    const std::filesystem::path& targetPath)
{
    std::error_code renameError;
    std::filesystem::rename(replacementPath, targetPath, renameError);
    if (renameError) {
        return trackloom::AtomicFileReplaceResult::fail(
            "Test replacement could not install the target.",
            trackloom::AtomicFileTargetAvailability::Unknown,
            replacementPath);
    }

    return trackloom::AtomicFileReplaceResult::ok(injectedRetainedRecoveryPath);
}

trackloom::AtomicFileReplaceResult replaceProjectFileAndLeaveWorkspaceSentinel(
    const std::filesystem::path& replacementPath,
    const std::filesystem::path& targetPath)
{
    std::error_code renameError;
    std::filesystem::rename(replacementPath, targetPath, renameError);
    if (renameError) {
        return trackloom::AtomicFileReplaceResult::fail(
            "Test replacement could not install the target.",
            trackloom::AtomicFileTargetAvailability::Unknown,
            replacementPath);
    }

    writeFileBytes(replacementPath.parent_path() / "cleanup-sentinel", "retain this workspace");
    return trackloom::AtomicFileReplaceResult::ok();
}

trackloom::AtomicFileReplaceResult replaceProjectFileWithoutRecovery(
    const std::filesystem::path& replacementPath,
    const std::filesystem::path& targetPath)
{
    std::error_code renameError;
    std::filesystem::rename(replacementPath, targetPath, renameError);
    if (renameError) {
        return trackloom::AtomicFileReplaceResult::fail(
            "Test replacement could not install the target.",
            trackloom::AtomicFileTargetAvailability::Unknown,
            replacementPath);
    }

    return trackloom::AtomicFileReplaceResult::ok();
}

bool removeWorkspaceThenReportNoRemoval(
    const std::filesystem::path& workspacePath,
    std::error_code& error)
{
    std::filesystem::remove(workspacePath, error);
    if (error) {
        return false;
    }

    error.clear();
    return false;
}

#ifdef _WIN32

class ScriptedWindowsAtomicFileOperations final : public trackloom::detail::WindowsAtomicFileOperations {
public:
    using QueryResult = trackloom::detail::WindowsPathQueryResult;
    using OperationResult = trackloom::detail::WindowsFileOperationResult;
    using ReplaceScript = std::function<OperationResult(
        ScriptedWindowsAtomicFileOperations&,
        const std::filesystem::path&,
        const std::filesystem::path&,
        const std::filesystem::path&,
        std::uint32_t)>;
    using MoveScript = std::function<OperationResult(
        ScriptedWindowsAtomicFileOperations&,
        const std::filesystem::path&,
        const std::filesystem::path&,
        std::uint32_t)>;
    using RemoveScript = std::function<OperationResult(
        ScriptedWindowsAtomicFileOperations&,
        const std::filesystem::path&)>;
    using DirectoryScript = std::function<OperationResult(
        ScriptedWindowsAtomicFileOperations&,
        const std::filesystem::path&)>;

    struct ReplaceCall {
        std::filesystem::path targetPath;
        std::filesystem::path replacementPath;
        std::filesystem::path backupPath;
        std::uint32_t flags = 0;
    };

    struct MoveCall {
        std::filesystem::path sourcePath;
        std::filesystem::path targetPath;
        std::uint32_t flags = 0;
    };

    QueryResult queryPath(const std::filesystem::path& path) override
    {
        auto scripted = queryResults.find(path);
        if (scripted != queryResults.end() && !scripted->second.empty()) {
            const auto result = scripted->second.front();
            scripted->second.pop_front();
            return result;
        }

        return { true, files.contains(path), trackloom::detail::windowsErrorSuccess };
    }

    OperationResult replaceFile(
        const std::filesystem::path& targetPath,
        const std::filesystem::path& replacementPath,
        const std::filesystem::path& backupPath,
        std::uint32_t flags) override
    {
        replaceCalls.push_back({ targetPath, replacementPath, backupPath, flags });
        if (!replaceScripts.empty()) {
            auto script = std::move(replaceScripts.front());
            replaceScripts.pop_front();
            return script(*this, targetPath, replacementPath, backupPath, flags);
        }

        if (!files.contains(targetPath) || !files.contains(replacementPath)) {
            return { false, trackloom::detail::windowsErrorFileNotFound };
        }
        if (!directories.contains(backupPath.parent_path())) {
            return { false, trackloom::detail::windowsErrorPathNotFound };
        }

        // ReplaceFileW 会覆盖已经存在的 backup；安全性必须来自私有 recovery 目录，而非预查询。
        files[backupPath] = files.at(targetPath);
        files.at(targetPath) = files.at(replacementPath);
        files.erase(replacementPath);
        return { true, trackloom::detail::windowsErrorSuccess };
    }

    OperationResult moveFile(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& targetPath,
        std::uint32_t flags) override
    {
        moveCalls.push_back({ sourcePath, targetPath, flags });
        if (!moveScripts.empty()) {
            auto script = std::move(moveScripts.front());
            moveScripts.pop_front();
            return script(*this, sourcePath, targetPath, flags);
        }

        if (!files.contains(sourcePath)) {
            return { false, trackloom::detail::windowsErrorFileNotFound };
        }
        if (files.contains(targetPath)) {
            return { false, trackloom::detail::windowsErrorAlreadyExists };
        }

        files.emplace(targetPath, files.at(sourcePath));
        files.erase(sourcePath);
        return { true, trackloom::detail::windowsErrorSuccess };
    }

    OperationResult removeFile(const std::filesystem::path& path) override
    {
        removeCalls.push_back(path);
        if (!removeScripts.empty()) {
            auto script = std::move(removeScripts.front());
            removeScripts.pop_front();
            return script(*this, path);
        }

        if (files.erase(path) == 0) {
            return { false, trackloom::detail::windowsErrorFileNotFound };
        }
        return { true, trackloom::detail::windowsErrorSuccess };
    }

    OperationResult createDirectory(const std::filesystem::path& path) override
    {
        createDirectoryCalls.push_back(path);
        if (!createDirectoryScripts.empty()) {
            auto script = std::move(createDirectoryScripts.front());
            createDirectoryScripts.pop_front();
            return script(*this, path);
        }

        if (directories.contains(path) || files.contains(path)) {
            return { false, trackloom::detail::windowsErrorAlreadyExists };
        }
        directories.insert(path);
        return { true, trackloom::detail::windowsErrorSuccess };
    }

    OperationResult removeDirectory(const std::filesystem::path& path) override
    {
        removeDirectoryCalls.push_back(path);
        if (!removeDirectoryScripts.empty()) {
            auto script = std::move(removeDirectoryScripts.front());
            removeDirectoryScripts.pop_front();
            return script(*this, path);
        }

        if (!directories.contains(path)) {
            return { false, trackloom::detail::windowsErrorPathNotFound };
        }
        for (const auto& [filePath, contents] : files) {
            static_cast<void>(contents);
            if (filePath.parent_path() == path) {
                return { false, trackloom::detail::windowsErrorAccessDenied };
            }
        }
        directories.erase(path);
        return { true, trackloom::detail::windowsErrorSuccess };
    }

    std::map<std::filesystem::path, std::string> files;
    std::set<std::filesystem::path> directories;
    std::map<std::filesystem::path, std::deque<QueryResult>> queryResults;
    std::deque<ReplaceScript> replaceScripts;
    std::deque<MoveScript> moveScripts;
    std::deque<RemoveScript> removeScripts;
    std::deque<DirectoryScript> createDirectoryScripts;
    std::deque<DirectoryScript> removeDirectoryScripts;
    std::vector<ReplaceCall> replaceCalls;
    std::vector<MoveCall> moveCalls;
    std::vector<std::filesystem::path> removeCalls;
    std::vector<std::filesystem::path> createDirectoryCalls;
    std::vector<std::filesystem::path> removeDirectoryCalls;
};

#endif

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

void midiPlaybackCollectsNoteOnAndOffInsideWindow()
{
    trackloom::Project project("MIDI Playback");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 960);
    const auto note = project.createMidiNote(clip->id, 120, 240, 60, 100, 1);

    require(note.has_value(), "note should be created before playback collection");
    const auto events = trackloom::collectMidiPlaybackEvents(project, 1000, 1400);

    require(events.size() == 2, "note should create on and off events inside playback window");
    require(events[0].type == trackloom::MidiPlaybackEventType::NoteOn, "first event should be note on");
    require(events[0].trackId == track.id, "note on should keep track id");
    require(events[0].clipId == clip->id, "note on should keep clip id");
    require(events[0].noteId == note->id, "note on should keep note id");
    require(events[0].absoluteTick == 1080, "note on should use clip plus relative note start");
    require(events[0].noteNumber == 60, "note on should keep pitch");
    require(events[0].velocity == 100, "note on should keep velocity");
    require(events[0].channel == 1, "note on should keep channel");
    require(events[1].type == trackloom::MidiPlaybackEventType::NoteOff, "second event should be note off");
    require(events[1].absoluteTick == 1320, "note off should use note end tick");
    require(events[1].velocity == 0, "note off should use zero velocity");
}

void midiPlaybackUsesHalfOpenWindow()
{
    trackloom::Project project("MIDI Playback");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 100, 960);
    const auto note = project.createMidiNote(clip->id, 0, 120, 64, 90, 2);

    require(note.has_value(), "note should be created before half-open window test");
    const auto startEvents = trackloom::collectMidiPlaybackEvents(project, 100, 220);
    const auto endEvents = trackloom::collectMidiPlaybackEvents(project, 220, 340);

    require(startEvents.size() == 1, "window should include event at start tick and exclude event at end tick");
    require(startEvents[0].type == trackloom::MidiPlaybackEventType::NoteOn, "start window should include note on");
    require(endEvents.size() == 1, "next window should include note off at its start tick");
    require(endEvents[0].type == trackloom::MidiPlaybackEventType::NoteOff, "next window should include note off");
}

void midiPlaybackSortsEventsDeterministically()
{
    trackloom::Project project("MIDI Playback");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto firstClip = project.createClip(track.id, "First", trackloom::ClipType::Midi, 0, 960);
    const auto secondClip = project.createClip(track.id, "Second", trackloom::ClipType::Midi, 0, 960);
    const auto endingNote = project.createMidiNote(firstClip->id, 0, 240, 60, 100, 1);
    const auto startingNote = project.createMidiNote(secondClip->id, 240, 120, 62, 100, 1);

    require(endingNote.has_value(), "ending note should exist before sorting test");
    require(startingNote.has_value(), "starting note should exist before sorting test");
    const auto events = trackloom::collectMidiPlaybackEvents(project, 240, 241);

    require(events.size() == 2, "same tick should include note off and note on");
    require(events[0].type == trackloom::MidiPlaybackEventType::NoteOff, "note off should sort before note on at same tick");
    require(events[0].noteId == endingNote->id, "ending note should produce first event");
    require(events[1].type == trackloom::MidiPlaybackEventType::NoteOn, "note on should sort after note off at same tick");
    require(events[1].noteId == startingNote->id, "starting note should produce second event");
}

void preparedMidiPlanOrdersNoteOffBeforeNoteOn()
{
    trackloom::Project project("Prepared");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Phrase", trackloom::ClipType::Midi, 0, 1920);
    require(clip.has_value(), "fixture clip should exist");
    require(project.createMidiNote(clip->id, 0, 960, 60, 100, 1).has_value(), "first note should exist");
    require(project.createMidiNote(clip->id, 960, 960, 64, 100, 1).has_value(), "second note should exist");

    trackloom::PreparedMidiPlaybackPlanBuildRequest request;
    request.projectSnapshot = project;
    request.sampleRate = 48000.0;
    request.maximumBlockFrames = 256;
    request.outputChannelCount = 2;
    request.outputChannelMask = 3;
    const auto result = trackloom::buildPreparedMidiPlaybackPlan(std::move(request));

    require(result.plan != nullptr, "valid project should build a plan");
    require(result.plan->events.size() == 4, "two notes should create four events");
    require(result.plan->events[1].samplePosition == 24000, "note off should land at tick 960");
    require(result.plan->events[1].type == trackloom::PreparedMidiEventType::NoteOff, "off must sort first");
    require(result.plan->events[2].type == trackloom::PreparedMidiEventType::NoteOn, "on must sort second");
}

void preparedMidiPlanBuildsDeterministicNonLoopSnapshot()
{
    trackloom::Project project("Prepared snapshot");
    const auto lead = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto hidden = project.createTrack("Hidden", trackloom::TrackType::Instrument);
    require(project.setTrackMixState(lead.id, {0.5f, 0.25f}), "lead mix should be set");
    require(project.setTrackMixState(hidden.id, {0.75f, -0.25f}), "hidden mix should be set");
    require(project.setTrackViewState(hidden.id, {true, false}), "hidden state should be set");
    require(project.createTempoEvent(960, 60.0).has_value(), "tempo change should exist");

    const auto leadClip = project.createClip(lead.id, "Lead phrase", trackloom::ClipType::Midi, 0, 2880);
    const auto hiddenClip = project.createClip(hidden.id, "Hidden phrase", trackloom::ClipType::Midi, 1920, 960);
    require(leadClip.has_value() && hiddenClip.has_value(), "fixture clips should exist");
    require(project.createMidiNote(leadClip->id, 0, 1920, 60, 100, 1).has_value(), "held lead note should exist");
    require(project.createMidiNote(leadClip->id, 960, 960, 60, 90, 1).has_value(), "overlapping lead note should exist");
    require(project.createMidiNote(hiddenClip->id, 0, 960, 67, 80, 2).has_value(), "hidden note should exist");

    trackloom::PreparedMidiPlaybackPlanBuildRequest request;
    request.projectSnapshot = project;
    request.sampleRate = 48000.0;
    request.maximumBlockFrames = 256;
    request.outputChannelCount = 2;
    request.outputChannelMask = 3;
    request.playbackStartSample = 24000;
    const auto result = trackloom::buildPreparedMidiPlaybackPlan(std::move(request));

    require(result.plan != nullptr, "valid project should build a plan");
    const auto& plan = *result.plan;
    require(plan.instrumentSlots.size() == 2, "instrument tracks should become ordered slots");
    require(plan.instrumentSlots[0].gain == 0.5f && plan.instrumentSlots[0].pan == 0.25f,
        "first slot should freeze lead mix");
    require(plan.instrumentSlots[1].gain == 0.75f && plan.instrumentSlots[1].pan == -0.25f,
        "second slot should freeze hidden mix");
    require(plan.initialChaseNoteOnEvents.size() == 1, "held note should receive one initial chase event");
    require(plan.initialChaseNoteOnEvents[0].samplePosition == 24000,
        "initial chase should use the absolute playback start sample");
    require(plan.initialChaseNoteOnEvents[0].noteInstanceId == 0,
        "first source note should keep instance zero");
    require(plan.initialChaseNoteOnEvents[0].eventOrdinal == 1,
        "initial chase should keep its deterministic ordinal");
    require(plan.events.size() == 5, "events before playback start should be omitted except later note offs");
    require(plan.events[0].samplePosition == 24000 && plan.events[0].type == trackloom::PreparedMidiEventType::NoteOn,
        "event at playback start should remain consumable");
    require(plan.events[1].samplePosition == 72000 && plan.events[1].type == trackloom::PreparedMidiEventType::NoteOff,
        "tempo-mapped note off should sort before simultaneous note on");
    require(plan.events[2].samplePosition == 72000 && plan.events[2].type == trackloom::PreparedMidiEventType::NoteOff,
        "overlapping same-pitch note off should remain separate");
    require(plan.events[3].samplePosition == 72000 && plan.events[3].type == trackloom::PreparedMidiEventType::NoteOn,
        "hidden track should still emit its simultaneous note on");
    require(plan.events[4].samplePosition == 120000 && plan.events[4].type == trackloom::PreparedMidiEventType::NoteOff,
        "hidden track note off should use the tempo map");
    require(plan.events[0].noteInstanceId == 1, "second overlapping note should receive the next dense instance id");
    require(plan.events[0].eventOrdinal == 3, "second overlapping note should keep its stable ordinal");
    require(plan.events[1].noteInstanceId == 0, "first note off should share the first note instance id");
    require(plan.events[1].eventOrdinal == 0, "first note off should keep its stable ordinal");
    require(plan.events[2].noteInstanceId == 1, "second note off should share the overlapping note instance id");
    require(plan.events[2].eventOrdinal == 2, "second note off should keep its stable ordinal");
    require(plan.events[3].noteInstanceId == 2, "hidden-track note should receive the next dense instance id");
    require(plan.events[3].eventOrdinal == 5, "hidden-track note should keep its stable ordinal");
    require(plan.events[4].noteInstanceId == 2, "hidden-track note off should reuse its note instance id");
    require(plan.events[4].eventOrdinal == 4, "hidden-track note off should keep its stable ordinal");
}

void preparedMidiPlanRejectsInvalidRequests()
{
    trackloom::Project project("Limits");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Chord", trackloom::ClipType::Midi, 0, 960);
    require(clip.has_value(), "fixture clip should exist");
    require(project.createMidiNote(clip->id, 0, 900, 60, 100, 1).has_value(), "fixture note should exist");

    trackloom::PreparedMidiPlaybackPlanBuildRequest request;
    request.projectSnapshot = project;
    request.sampleRate = 48000.0;
    request.maximumBlockFrames = 256;
    request.outputChannelCount = 2;
    request.outputChannelMask = 3;

    auto invalidSampleRate = request;
    invalidSampleRate.sampleRate = 0.0;
    require(trackloom::buildPreparedMidiPlaybackPlan(std::move(invalidSampleRate)).failureReason
            == trackloom::PreparedMidiPlaybackPlanBuildFailureReason::InvalidSampleRate,
        "zero sample rate should be rejected");
    auto infiniteSampleRate = request;
    infiniteSampleRate.sampleRate = std::numeric_limits<double>::infinity();
    require(trackloom::buildPreparedMidiPlaybackPlan(std::move(infiniteSampleRate)).failureReason
            == trackloom::PreparedMidiPlaybackPlanBuildFailureReason::InvalidSampleRate,
        "infinite sample rate should be rejected");
    auto invalidBlockSize = request;
    invalidBlockSize.maximumBlockFrames = 0;
    require(trackloom::buildPreparedMidiPlaybackPlan(std::move(invalidBlockSize)).failureReason
            == trackloom::PreparedMidiPlaybackPlanBuildFailureReason::InvalidMaximumBlockFrames,
        "zero maximum block size should be rejected");
    auto invalidOutput = request;
    invalidOutput.outputChannelMask = 1;
    require(trackloom::buildPreparedMidiPlaybackPlan(std::move(invalidOutput)).failureReason
            == trackloom::PreparedMidiPlaybackPlanBuildFailureReason::InvalidOutputFormat,
        "inconsistent output channel mask should be rejected");
    auto monoOutput = request;
    monoOutput.outputChannelCount = 1;
    monoOutput.outputChannelMask = 1;
    require(trackloom::buildPreparedMidiPlaybackPlan(std::move(monoOutput)).plan != nullptr,
        "one channel with low mask bit zero should be accepted");
    auto unsupportedChannelCount = request;
    unsupportedChannelCount.outputChannelCount = 3;
    unsupportedChannelCount.outputChannelMask = 7;
    require(trackloom::buildPreparedMidiPlaybackPlan(std::move(unsupportedChannelCount)).failureReason
            == trackloom::PreparedMidiPlaybackPlanBuildFailureReason::InvalidOutputFormat,
        "more than two output channels should be rejected");
    auto invalidStart = request;
    invalidStart.playbackStartSample = -1;
    require(trackloom::buildPreparedMidiPlaybackPlan(std::move(invalidStart)).failureReason
            == trackloom::PreparedMidiPlaybackPlanBuildFailureReason::InvalidPlaybackStart,
        "negative playback start should be rejected");
    auto invalidLoop = request;
    invalidLoop.loopRange = trackloom::PlaybackLoopRange{0, 960};
    require(trackloom::buildPreparedMidiPlaybackPlan(std::move(invalidLoop)).failureReason
            == trackloom::PreparedMidiPlaybackPlanBuildFailureReason::InvalidLoopRange,
        "loop plans are deferred to Task 2 and should be rejected stably");

    auto invalidMixProject = project;
    auto& mutableTracks = const_cast<std::vector<trackloom::Track>&>(invalidMixProject.tracks());
    mutableTracks[0].mix.gain = -0.1f;
    request.projectSnapshot = invalidMixProject;
    require(trackloom::buildPreparedMidiPlaybackPlan(std::move(request)).failureReason
            == trackloom::PreparedMidiPlaybackPlanBuildFailureReason::InvalidTrackMix,
        "malformed negative track gain should be rejected by the plan builder");
}

void preparedMidiPlanRejectsSamplePositionThatRoundsPastInt64Maximum()
{
    trackloom::Project project("Overflow");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Near limit", trackloom::ClipType::Midi,
        std::numeric_limits<std::int64_t>::max() - 1, 1);
    require(clip.has_value(), "near-limit clip should exist");
    require(project.createMidiNote(clip->id, 0, 1, 60, 100, 1).has_value(), "near-limit note should exist");

    trackloom::PreparedMidiPlaybackPlanBuildRequest request;
    request.projectSnapshot = project;
    request.sampleRate = static_cast<double>(std::numeric_limits<std::int64_t>::max())
        / project.tickToSeconds(std::numeric_limits<std::int64_t>::max());
    request.maximumBlockFrames = 256;
    request.outputChannelCount = 2;
    request.outputChannelMask = 3;
    require(trackloom::buildPreparedMidiPlaybackPlan(std::move(request)).failureReason
            == trackloom::PreparedMidiPlaybackPlanBuildFailureReason::SamplePositionOverflow,
        "sample values that round past int64 maximum must be rejected before llround");
}

void preparedMidiPlanAcceptsMaximumSafelyRoundableSamplePosition()
{
    trackloom::Project project("Safe endpoint");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "One beat", trackloom::ClipType::Midi, 0, 960);
    require(clip.has_value(), "endpoint clip should exist");
    require(project.createMidiNote(clip->id, 0, 960, 60, 100, 1).has_value(), "endpoint note should exist");

    const auto maximumSafeDouble = std::nextafter(
        static_cast<double>(std::numeric_limits<std::int64_t>::max()), 0.0);
    trackloom::PreparedMidiPlaybackPlanBuildRequest request;
    request.projectSnapshot = project;
    request.sampleRate = maximumSafeDouble * 2.0;
    request.maximumBlockFrames = 256;
    request.outputChannelCount = 2;
    request.outputChannelMask = 3;
    const auto result = trackloom::buildPreparedMidiPlaybackPlan(std::move(request));

    require(result.plan != nullptr, "maximum safely roundable sample endpoint should build");
    require(result.plan->events.size() == 2, "one note should retain both events at the safe endpoint");
    require(result.plan->events[1].samplePosition == static_cast<std::int64_t>(maximumSafeDouble),
        "note off should preserve the largest safely roundable sample position");
}

void preparedMidiPlanUsesExistingTrackPlaybackRules()
{
    trackloom::Project project("Playback rules");
    const auto muted = project.createTrack("Muted", trackloom::TrackType::Instrument);
    const auto disabled = project.createTrack("Disabled", trackloom::TrackType::Instrument);
    const auto hidden = project.createTrack("Hidden", trackloom::TrackType::Instrument);
    const auto soloed = project.createTrack("Soloed", trackloom::TrackType::Instrument);
    require(project.setTrackPlaybackState(muted.id, {true, false, false}), "muted state should be set");
    require(project.setTrackPlaybackState(disabled.id, {false, false, true}), "disabled state should be set");
    require(project.setTrackViewState(hidden.id, {true, false}), "hidden state should be set");
    for (const auto& track : {muted, disabled, hidden, soloed}) {
        const auto clip = project.createClip(track.id, "Note", trackloom::ClipType::Midi, 0, 960);
        require(clip.has_value(), "fixture clip should exist");
        require(project.createMidiNote(clip->id, 0, 960, 60, 100, 1).has_value(), "fixture note should exist");
    }

    trackloom::PreparedMidiPlaybackPlanBuildRequest request;
    request.projectSnapshot = project;
    request.sampleRate = 48000.0;
    request.maximumBlockFrames = 256;
    request.outputChannelCount = 2;
    request.outputChannelMask = 3;
    auto result = trackloom::buildPreparedMidiPlaybackPlan(request);
    require(result.plan != nullptr && result.plan->instrumentSlots.size() == 4,
        "all instrument tracks should retain ordered slots");
    require(result.plan->events.size() == 4,
        "hidden and non-soloed active tracks should both emit without solo mode");

    require(project.setTrackPlaybackState(soloed.id, {false, true, false}), "solo state should be set");
    request.projectSnapshot = project;
    result = trackloom::buildPreparedMidiPlaybackPlan(std::move(request));
    require(result.plan != nullptr && result.plan->events.size() == 2,
        "solo mode should emit only the soloed track while hidden remains display-only");
    require(result.plan->events[0].instrumentSlotIndex == 3 && result.plan->events[1].instrumentSlotIndex == 3,
        "soloed track should retain its original project-order slot");
}

void midiPlaybackRespectsTrackPlaybackState()
{
    trackloom::Project project("MIDI Playback");
    const auto mutedTrack = project.createTrack("Muted", trackloom::TrackType::Instrument);
    const auto disabledTrack = project.createTrack("Disabled", trackloom::TrackType::Instrument);
    const auto soloedTrack = project.createTrack("Soloed", trackloom::TrackType::Instrument);
    const auto normalTrack = project.createTrack("Normal", trackloom::TrackType::Instrument);
    const auto mutedClip = project.createClip(mutedTrack.id, "Muted Clip", trackloom::ClipType::Midi, 0, 960);
    const auto disabledClip = project.createClip(disabledTrack.id, "Disabled Clip", trackloom::ClipType::Midi, 0, 960);
    const auto soloedClip = project.createClip(soloedTrack.id, "Soloed Clip", trackloom::ClipType::Midi, 0, 960);
    const auto normalClip = project.createClip(normalTrack.id, "Normal Clip", trackloom::ClipType::Midi, 0, 960);
    trackloom::TrackPlaybackState mutedState;
    trackloom::TrackPlaybackState disabledState;
    trackloom::TrackPlaybackState soloedState;

    mutedState.muted = true;
    disabledState.disabled = true;
    soloedState.soloed = true;
    require(project.setTrackPlaybackState(mutedTrack.id, mutedState), "muted state should be set");
    require(project.setTrackPlaybackState(disabledTrack.id, disabledState), "disabled state should be set");
    require(project.setTrackPlaybackState(soloedTrack.id, soloedState), "soloed state should be set");
    require(project.createMidiNote(mutedClip->id, 0, 120, 60, 100, 1).has_value(), "muted note should be created");
    require(project.createMidiNote(disabledClip->id, 0, 120, 61, 100, 1).has_value(), "disabled note should be created");
    require(project.createMidiNote(soloedClip->id, 0, 120, 62, 100, 1).has_value(), "soloed note should be created");
    require(project.createMidiNote(normalClip->id, 0, 120, 63, 100, 1).has_value(), "normal note should be created");

    const auto events = trackloom::collectMidiPlaybackEvents(project, 0, 1);

    require(events.size() == 1, "solo mode should output only soloed, unmuted, enabled track");
    require(events[0].trackId == soloedTrack.id, "event should come from soloed track");
}

void midiPlaybackHiddenTrackStillPlays()
{
    trackloom::Project project("MIDI Playback");
    const auto track = project.createTrack("Hidden", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Hidden Clip", trackloom::ClipType::Midi, 0, 960);
    trackloom::TrackViewState view;

    view.hidden = true;
    require(project.setTrackViewState(track.id, view), "hidden state should be set");
    require(project.createMidiNote(clip->id, 0, 120, 60, 100, 1).has_value(), "hidden-track note should be created");

    const auto events = trackloom::collectMidiPlaybackEvents(project, 0, 1);

    require(events.size() == 1, "hidden track should still produce playback event");
    require(events[0].trackId == track.id, "hidden track event should keep track id");
}

void midiPlaybackRejectsInvalidWindows()
{
    trackloom::Project project("MIDI Playback");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 0, 960);

    require(project.createMidiNote(clip->id, 0, 120, 60, 100, 1).has_value(), "note should be created before invalid window test");
    require(trackloom::collectMidiPlaybackEvents(project, -1, 120).empty(), "negative start should return no events");
    require(trackloom::collectMidiPlaybackEvents(project, 120, 120).empty(), "empty window should return no events");
    require(trackloom::collectMidiPlaybackEvents(project, 240, 120).empty(), "reversed window should return no events");
}

void midiPlaybackIgnoresAudioClips()
{
    trackloom::Project project("MIDI Playback");
    const auto audioTrack = project.createTrack("Vocal", trackloom::TrackType::Audio);
    const auto audioClip = project.createClip(audioTrack.id, "Take", trackloom::ClipType::Audio, 0, 960);

    require(audioClip.has_value(), "audio clip should exist before playback scheduler test");
    const auto events = trackloom::collectMidiPlaybackEvents(project, 0, 960);

    require(events.empty(), "audio clip should not produce midi playback events");
}

void midiPlaybackChasesActiveNoteAtWindowStart()
{
    trackloom::Project project("MIDI Playback");
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 1440);
    const auto note = project.createMidiNote(clip->id, 0, 960, 60, 100, 1);

    require(clip.has_value(), "chase clip should exist before playback collection");
    require(note.has_value(), "chase note should exist before playback collection");

    // 纯半开收集器不补发窗口开始前已经按下的音符，方便保留可预测的基础语义。
    const auto pureEvents = trackloom::collectMidiPlaybackEvents(project, 1200, 1680);
    require(pureEvents.empty(), "pure playback collection should not chase held notes");

    // chase-aware 收集器用于真实播放输出：从音符中间开始播放时，需要在窗口起点补 Note On。
    const auto chasedEvents = trackloom::collectMidiPlaybackEventsWithChase(project, 1200, 1680);

    require(chasedEvents.size() == 1, "chase collection should emit one held note at window start");
    require(chasedEvents[0].type == trackloom::MidiPlaybackEventType::NoteOn, "chase event should be note on");
    require(chasedEvents[0].trackId == track.id, "chase event should keep track id");
    require(chasedEvents[0].clipId == clip->id, "chase event should keep clip id");
    require(chasedEvents[0].noteId == note->id, "chase event should keep note id");
    require(chasedEvents[0].absoluteTick == 1200, "chase event should use the window start tick");
    require(chasedEvents[0].noteNumber == 60, "chase event should keep pitch");
    require(chasedEvents[0].velocity == 100, "chase event should keep original velocity");
    require(chasedEvents[0].channel == 1, "chase event should keep channel");
}

void playbackClockConvertsDefaultTempoBlockToTickWindow()
{
    trackloom::Project project("Clock");
    trackloom::Transport transport;

    require(transport.setSampleRate(48000.0), "sample rate should be set before clock test");
    require(transport.seekToSample(24000), "transport should seek before clock test");
    transport.play();

    const auto window = trackloom::playbackTickWindowForBlock(project, transport, 24000);

    require(window.has_value(), "playing transport should produce tick window");
    require(window->startTick == 960, "0.5 seconds at 120 BPM should start at tick 960");
    require(window->endTick == 1920, "1.0 seconds at 120 BPM should end at tick 1920");
    require(transport.currentSample() == 24000, "window calculation should not advance transport");
}

void playbackClockHandlesTempoChangeInsideBlock()
{
    trackloom::Project project("Clock");
    trackloom::Transport transport;

    require(project.createTempoEvent(960, 60.0).has_value(), "tempo change should exist before clock test");
    require(transport.setSampleRate(48000.0), "sample rate should be set before tempo clock test");
    require(transport.seekToSample(0), "transport should start at sample zero");
    transport.play();

    const auto window = trackloom::playbackTickWindowForBlock(project, transport, 72000);

    require(window.has_value(), "tempo-changing block should produce tick window");
    require(window->startTick == 0, "block should start at tick zero");
    require(window->endTick == 1920, "0.0s to 1.5s should cross 120 BPM then 60 BPM to tick 1920");
}

void playbackClockRejectsStoppedOrInvalidBlocks()
{
    trackloom::Project project("Clock");
    trackloom::Transport transport;

    require(transport.setSampleRate(48000.0), "sample rate should be set before invalid clock test");
    require(!trackloom::playbackTickWindowForBlock(project, transport, 24000).has_value(), "stopped transport should not produce playback window");
    transport.play();
    require(!trackloom::playbackTickWindowForBlock(project, transport, 0).has_value(), "zero frame block should not produce playback window");
    require(!trackloom::playbackTickWindowForBlock(project, transport, -1).has_value(), "negative frame block should not produce playback window");
}

void playbackClockCollectsMidiEventsForTransportBlock()
{
    trackloom::Project project("Clock");
    trackloom::Transport transport;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 960);

    require(clip.has_value(), "clip should exist before block event collection");
    const auto note = project.createMidiNote(clip->id, 0, 480, 60, 100, 1);
    require(note.has_value(), "note should exist before block event collection");
    require(transport.setSampleRate(48000.0), "sample rate should be set before block event collection");
    require(transport.seekToSample(24000), "transport should seek to note-on sample");
    transport.play();

    const auto events = trackloom::collectMidiPlaybackEventsForBlock(project, transport, 24000);

    require(events.size() == 2, "block should collect note on and note off");
    require(events[0].type == trackloom::MidiPlaybackEventType::NoteOn, "first block event should be note on");
    require(events[0].absoluteTick == 960, "note on should occur at block start tick");
    require(events[1].type == trackloom::MidiPlaybackEventType::NoteOff, "second block event should be note off");
    require(events[1].absoluteTick == 1440, "note off should occur inside block");
}

void playbackClockUsesHalfOpenBlockBoundary()
{
    trackloom::Project project("Clock");
    trackloom::Transport transport;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 1920, 960);

    require(clip.has_value(), "clip should exist before boundary test");
    require(project.createMidiNote(clip->id, 0, 960, 60, 100, 1).has_value(), "boundary note should exist");
    require(transport.setSampleRate(48000.0), "sample rate should be set before boundary test");
    require(transport.seekToSample(0), "transport should start at sample zero for boundary test");
    transport.play();

    const auto firstBlockEvents = trackloom::collectMidiPlaybackEventsForBlock(project, transport, 48000);
    require(firstBlockEvents.empty(), "first block should exclude event at right boundary");

    require(transport.seekToSample(48000), "transport should seek to second block");
    const auto secondBlockEvents = trackloom::collectMidiPlaybackEventsForBlock(project, transport, 24000);

    require(secondBlockEvents.size() == 1, "second block should include event at left boundary");
    require(secondBlockEvents[0].absoluteTick == 1920, "second block event should occur at boundary tick");
}

void playbackClockSchedulesMidiEventsWithSampleOffsets()
{
    trackloom::Project project("Clock");
    trackloom::Transport transport;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 960);

    require(clip.has_value(), "scheduled midi clip should exist");
    require(project.createMidiNote(clip->id, 0, 240, 60, 100, 1).has_value(), "scheduled midi note should exist");
    require(transport.setSampleRate(1920.0), "scheduled midi sample rate should be set");
    require(transport.seekToSample(960), "scheduled midi transport should seek to block start");
    transport.play();

    const auto events = trackloom::collectScheduledMidiPlaybackEventsForBlock(project, transport, 480);

    require(events.size() == 2, "scheduled block should contain note on and note off");
    require(events[0].event.type == trackloom::MidiPlaybackEventType::NoteOn, "first scheduled event should be note on");
    require(events[0].event.absoluteTick == 960, "note on should keep absolute tick");
    require(events[0].sampleOffset == 0, "note on at block start should use sample offset zero");
    require(events[1].event.type == trackloom::MidiPlaybackEventType::NoteOff, "second scheduled event should be note off");
    require(events[1].event.absoluteTick == 1200, "note off should keep absolute tick");
    require(events[1].sampleOffset == 240, "note off should use its block-local sample offset");
    require(transport.currentSample() == 960, "scheduled collection should not advance transport");
}

void playbackClockSchedulesChasedMidiAtBlockStart()
{
    trackloom::Project project("Clock");
    trackloom::Transport transport;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 1440);

    require(clip.has_value(), "scheduled chase midi clip should exist");
    require(project.createMidiNote(clip->id, 0, 960, 60, 100, 1).has_value(), "scheduled chase midi note should exist");
    require(transport.setSampleRate(1920.0), "scheduled chase sample rate should be set");
    require(transport.seekToSample(1200), "scheduled chase transport should seek inside held note");
    transport.play();

    const auto events = trackloom::collectScheduledMidiPlaybackEventsForBlock(
        project,
        transport,
        960,
        trackloom::MidiChaseMode::Enabled);

    require(events.size() == 2, "scheduled chase block should contain chased note on and real note off");
    require(events[0].event.type == trackloom::MidiPlaybackEventType::NoteOn, "scheduled chase first event should be note on");
    require(events[0].event.absoluteTick == 1200, "scheduled chase note on should use block start tick");
    require(events[0].sampleOffset == 0, "scheduled chase note on should be at sample offset zero");
    require(events[1].event.type == trackloom::MidiPlaybackEventType::NoteOff, "scheduled chase second event should be note off");
    require(events[1].event.absoluteTick == 1920, "scheduled chase note off should keep real note end tick");
    require(events[1].sampleOffset == 720, "scheduled chase note off should keep block-local release offset");
}

void playbackClockSchedulesMidiEventsAcrossTempoChange()
{
    trackloom::Project project("Clock");
    trackloom::Transport transport;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Slow Phrase", trackloom::ClipType::Midi, 1440, 960);

    require(project.createTempoEvent(960, 60.0).has_value(), "tempo change should exist before scheduled offset test");
    require(clip.has_value(), "tempo scheduled midi clip should exist");
    require(project.createMidiNote(clip->id, 0, 120, 64, 90, 1).has_value(), "tempo scheduled midi note should exist");
    require(transport.setSampleRate(960.0), "tempo scheduled sample rate should be set");
    require(transport.seekToSample(480), "tempo scheduled transport should seek to tempo-change sample");
    transport.play();

    const auto events = trackloom::collectScheduledMidiPlaybackEventsForBlock(project, transport, 960);

    require(events.size() == 2, "tempo scheduled block should contain note on and note off");
    require(events[0].event.absoluteTick == 1440, "tempo scheduled note on should keep absolute tick");
    require(events[0].sampleOffset == 480, "tempo scheduled note on should account for slower tempo after change");
    require(events[1].event.absoluteTick == 1560, "tempo scheduled note off should keep absolute tick");
    require(events[1].sampleOffset == 600, "tempo scheduled note off should account for slower tempo after change");
}

void playbackClockRejectsStoppedOrInvalidScheduledBlocks()
{
    trackloom::Project project("Clock");
    trackloom::Transport transport;

    require(transport.setSampleRate(1920.0), "scheduled invalid sample rate should be set");
    require(trackloom::collectScheduledMidiPlaybackEventsForBlock(project, transport, 480).empty(), "stopped transport should not schedule midi events");
    transport.play();
    require(trackloom::collectScheduledMidiPlaybackEventsForBlock(project, transport, 0).empty(), "zero frame block should not schedule midi events");
    require(trackloom::collectScheduledMidiPlaybackEventsForBlock(project, transport, -1).empty(), "negative frame block should not schedule midi events");
}

void playbackClockSplitsLoopedBlockAtLoopEnd()
{
    trackloom::Project project("Loop Clock");
    trackloom::Transport transport;
    const trackloom::PlaybackLoopRange loop { 960, 1920 };

    require(transport.setSampleRate(1920.0), "loop split sample rate should make one sample equal one tick");
    require(transport.seekToSample(1440), "loop split transport should start before loop end");
    transport.play();

    const auto windows = trackloom::playbackTickWindowsForLoopedBlock(project, transport, 960, loop);

    require(windows.size() == 2, "looped block should split at loop end");
    require(windows[0].window.startTick == 1440, "first looped window should start at transport tick");
    require(windows[0].window.endTick == 1920, "first looped window should end at loop right boundary");
    require(windows[0].sampleOffset == 0, "first looped window should start at block sample zero");
    require(windows[0].frameCount == 480, "first looped window should consume frames until loop end");
    require(windows[1].window.startTick == 960, "second looped window should wrap to loop start");
    require(windows[1].window.endTick == 1440, "second looped window should cover remaining loop ticks");
    require(windows[1].sampleOffset == 480, "second looped window should keep block-wide sample offset");
    require(windows[1].frameCount == 480, "second looped window should consume remaining frames");
    require(transport.currentSample() == 1440, "looped window calculation should not advance transport");
}

void playbackClockSchedulesMidiEventsAcrossLoopWrap()
{
    trackloom::Project project("Loop Clock");
    trackloom::Transport transport;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Loop Phrase", trackloom::ClipType::Midi, 960, 960);
    const trackloom::PlaybackLoopRange loop { 960, 1920 };

    require(clip.has_value(), "loop midi clip should exist before scheduling");
    require(project.createMidiNote(clip->id, 720, 120, 67, 80, 1).has_value(), "pre-wrap note should exist");
    require(project.createMidiNote(clip->id, 0, 120, 60, 100, 1).has_value(), "post-wrap note should exist");
    require(transport.setSampleRate(1920.0), "loop midi sample rate should make one sample equal one tick");
    require(transport.seekToSample(1440), "loop midi transport should start before loop end");
    transport.play();

    const auto events = trackloom::collectScheduledMidiPlaybackEventsForLoopedBlock(project, transport, 960, loop);

    require(events.size() == 4, "looped scheduler should collect events before and after wrap");
    require(events[0].event.type == trackloom::MidiPlaybackEventType::NoteOn, "first loop event should start pre-wrap note");
    require(events[0].event.absoluteTick == 1680, "first loop event should keep pre-wrap note-on tick");
    require(events[0].sampleOffset == 240, "first loop event should use pre-wrap block offset");
    require(events[1].event.type == trackloom::MidiPlaybackEventType::NoteOff, "second loop event should release pre-wrap note");
    require(events[1].event.absoluteTick == 1800, "second loop event should keep pre-wrap note-off tick");
    require(events[1].sampleOffset == 360, "second loop event should use pre-wrap release offset");
    require(events[2].event.type == trackloom::MidiPlaybackEventType::NoteOn, "third loop event should start wrapped note");
    require(events[2].event.absoluteTick == 960, "third loop event should keep wrapped note-on tick");
    require(events[2].sampleOffset == 480, "third loop event should keep block-wide wrapped offset");
    require(events[3].event.type == trackloom::MidiPlaybackEventType::NoteOff, "fourth loop event should release wrapped note");
    require(events[3].event.absoluteTick == 1080, "fourth loop event should keep wrapped note-off tick");
    require(events[3].sampleOffset == 600, "fourth loop event should keep block-wide wrapped release offset");
}

void playbackClockAddsLoopBoundaryNoteOffForLongNote()
{
    trackloom::Project project("Loop Clock");
    trackloom::Transport transport;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Loop Phrase", trackloom::ClipType::Midi, 960, 1440);
    const trackloom::PlaybackLoopRange loop { 960, 1920 };

    require(clip.has_value(), "loop boundary release clip should exist");

    // 这个音符在当前回绕前窗口内开始，但真实释放点超过循环右边界。
    // 调度器必须在回绕前补一个 Note Off，避免输出层把音符带入下一轮循环。
    require(project.createMidiNote(clip->id, 720, 480, 67, 80, 1).has_value(), "long loop note should exist");
    require(transport.setSampleRate(1920.0), "loop boundary release sample rate should make one sample equal one tick");
    require(transport.seekToSample(1440), "loop boundary release transport should start before loop end");
    transport.play();

    const auto events = trackloom::collectScheduledMidiPlaybackEventsForLoopedBlock(project, transport, 960, loop);

    require(events.size() == 2, "loop boundary release should add note off before wrap");
    require(events[0].event.type == trackloom::MidiPlaybackEventType::NoteOn, "loop boundary release should keep real note on first");
    require(events[0].event.absoluteTick == 1680, "loop boundary release note on should keep real tick");
    require(events[0].sampleOffset == 240, "loop boundary release note on should keep pre-wrap offset");
    require(events[1].event.type == trackloom::MidiPlaybackEventType::NoteOff, "loop boundary release should synthesize note off");
    require(events[1].event.noteId == events[0].event.noteId, "loop boundary release should target the same note");
    require(events[1].event.absoluteTick == 1920, "loop boundary release should happen at loop end tick");
    require(events[1].sampleOffset == 479, "loop boundary release should clamp to last pre-wrap sample");
}

void playbackClockChasesWrappedLoopWindowStart()
{
    trackloom::Project project("Loop Clock");
    trackloom::Transport transport;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 720, 480);
    const trackloom::PlaybackLoopRange loop { 960, 1920 };

    require(clip.has_value(), "wrapped chase clip should exist");
    require(project.createMidiNote(clip->id, 0, 360, 64, 90, 1).has_value(), "wrapped chase note should exist");
    require(transport.setSampleRate(1920.0), "wrapped chase sample rate should make one sample equal one tick");
    require(transport.seekToSample(1680), "wrapped chase transport should start before loop end");
    transport.play();

    const auto events = trackloom::collectScheduledMidiPlaybackEventsForLoopedBlock(
        project,
        transport,
        480,
        loop,
        trackloom::MidiChaseMode::Enabled);

    require(events.size() == 2, "wrapped loop window should chase held note at loop start");
    require(events[0].event.type == trackloom::MidiPlaybackEventType::NoteOn, "wrapped chase first event should be note on");
    require(events[0].event.absoluteTick == 960, "wrapped chase note on should use loop start tick");
    require(events[0].sampleOffset == 240, "wrapped chase note on should keep block-wide wrapped offset");
    require(events[1].event.type == trackloom::MidiPlaybackEventType::NoteOff, "wrapped chase second event should be note off");
    require(events[1].event.absoluteTick == 1080, "wrapped chase note off should keep real release tick");
    require(events[1].sampleOffset == 360, "wrapped chase note off should keep block-wide release offset");
}

void playbackClockRejectsInvalidLoopedBlocks()
{
    trackloom::Project project("Loop Clock");
    trackloom::Transport transport;
    const trackloom::PlaybackLoopRange validLoop { 960, 1920 };
    const trackloom::PlaybackLoopRange reversedLoop { 1920, 960 };
    const trackloom::PlaybackLoopRange negativeLoop { -1, 960 };

    require(transport.setSampleRate(1920.0), "invalid loop test sample rate should be set");
    require(trackloom::playbackTickWindowsForLoopedBlock(project, transport, 480, validLoop).empty(), "stopped transport should not produce looped windows");
    require(trackloom::collectScheduledMidiPlaybackEventsForLoopedBlock(project, transport, 480, validLoop).empty(), "stopped transport should not schedule looped events");
    transport.play();

    require(trackloom::playbackTickWindowsForLoopedBlock(project, transport, 0, validLoop).empty(), "zero frame looped block should be rejected");
    require(trackloom::playbackTickWindowsForLoopedBlock(project, transport, -1, validLoop).empty(), "negative frame looped block should be rejected");
    require(trackloom::playbackTickWindowsForLoopedBlock(project, transport, 480, reversedLoop).empty(), "reversed loop range should be rejected");
    require(trackloom::playbackTickWindowsForLoopedBlock(project, transport, 480, negativeLoop).empty(), "negative loop start should be rejected");
}

void playbackClockNormalizesTransportPositionIntoLoopRange()
{
    trackloom::Project project("Loop Clock");
    trackloom::Transport transport;
    const trackloom::PlaybackLoopRange loop { 960, 1920 };

    require(transport.setSampleRate(1920.0), "loop normalize sample rate should make one sample equal one tick");
    require(transport.seekToSample(2400), "loop normalize transport should seek past one loop cycle");
    transport.play();

    const auto windows = trackloom::playbackTickWindowsForLoopedBlock(project, transport, 480, loop);

    require(windows.size() == 1, "normalized transport position should produce one looped window");
    require(windows[0].window.startTick == 1440, "transport sample after loop cycle should normalize to tick 1440");
    require(windows[0].window.endTick == 1920, "normalized window should end at loop boundary");
    require(windows[0].sampleOffset == 0, "normalized window should still start at block sample zero");
    require(windows[0].frameCount == 480, "normalized window should cover the requested frames");
}

void playbackClockSplitsLoopedBlockAcrossTempoMappedLoop()
{
    trackloom::Project project("Loop Clock");
    trackloom::Transport transport;
    const trackloom::PlaybackLoopRange loop { 960, 1920 };

    require(project.createTempoEvent(960, 60.0).has_value(), "loop tempo change should exist before split test");
    require(transport.setSampleRate(960.0), "tempo loop sample rate should make slow-tempo tick math visible");
    require(transport.seekToSample(960), "tempo loop transport should start at tick 1440");
    transport.play();

    const auto windows = trackloom::playbackTickWindowsForLoopedBlock(project, transport, 960, loop);

    require(windows.size() == 2, "tempo-mapped looped block should split at loop end");
    require(windows[0].window.startTick == 1440, "tempo-mapped first window should start at normalized tick");
    require(windows[0].window.endTick == 1920, "tempo-mapped first window should end at loop boundary");
    require(windows[0].frameCount == 480, "tempo-mapped first window should consume slow-tempo frames to loop end");
    require(windows[1].window.startTick == 960, "tempo-mapped second window should wrap to loop start");
    require(windows[1].window.endTick == 1440, "tempo-mapped second window should cover remaining slow-tempo ticks");
    require(windows[1].sampleOffset == 480, "tempo-mapped wrapped window should keep block-wide offset");
    require(windows[1].frameCount == 480, "tempo-mapped wrapped window should consume remaining frames");
}

void midiDispatchConvertsNoteEventsToOutputMessages()
{
    const auto noteOn = makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 12, 1, 60, 100);
    const auto noteOff = makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOff, 24, 16, 60, 0);

    const auto noteOnMessage = trackloom::midiOutputMessageForEvent(noteOn);
    const auto noteOffMessage = trackloom::midiOutputMessageForEvent(noteOff);

    require(noteOnMessage.has_value(), "note on should convert to output message");
    require(noteOnMessage->sampleOffset == 12, "note on message should keep sample offset");
    require(noteOnMessage->statusByte == 0x90, "channel 1 note on should use 0x90 status");
    require(noteOnMessage->data1 == 60, "note on data1 should be note number");
    require(noteOnMessage->data2 == 100, "note on data2 should be velocity");

    require(noteOffMessage.has_value(), "note off should convert to output message");
    require(noteOffMessage->sampleOffset == 24, "note off message should keep sample offset");
    require(noteOffMessage->statusByte == 0x8F, "channel 16 note off should use 0x8F status");
    require(noteOffMessage->data1 == 60, "note off data1 should be note number");
    require(noteOffMessage->data2 == 0, "note off data2 should be release velocity zero for current scheduler");
}

void midiDispatchSendsEventsInOrder()
{
    const std::vector<trackloom::ScheduledMidiPlaybackEvent> events {
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOff, 0, 2, 60, 0),
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 120, 2, 64, 90),
    };
    RecordingMidiEventReceiver receiver;

    const auto result = trackloom::dispatchScheduledMidiEvents(events, receiver);

    require(result.success, "dispatch should succeed when receiver accepts every event");
    require(result.attemptedEventCount == 2, "dispatch should attempt every event");
    require(result.deliveredEventCount == 2, "dispatch should count delivered events");
    require(result.failedEventIndex == -1, "successful dispatch should not report failed index");
    require(receiver.events().size() == 2, "receiver should record two events");
    require(receiver.events()[0] == events[0], "receiver should get first event first");
    require(receiver.events()[1] == events[1], "receiver should get second event second");
    require(receiver.messages()[0].statusByte == 0x81, "first dispatch message should keep channel 2 note off");
    require(receiver.messages()[1].statusByte == 0x91, "second dispatch message should keep channel 2 note on");
    require(receiver.messages()[1].sampleOffset == 120, "second dispatch message should keep sample offset");
}

void midiDispatchAcceptsEmptyEventList()
{
    const std::vector<trackloom::ScheduledMidiPlaybackEvent> events;
    RecordingMidiEventReceiver receiver;

    const auto result = trackloom::dispatchScheduledMidiEvents(events, receiver);

    require(result.success, "empty dispatch should succeed");
    require(result.attemptedEventCount == 0, "empty dispatch should attempt no events");
    require(result.deliveredEventCount == 0, "empty dispatch should deliver no events");
    require(result.failedEventIndex == -1, "empty dispatch should not report failed index");
    require(receiver.messages().empty(), "empty dispatch should not call receiver");
}

void midiDispatchStopsWhenReceiverFails()
{
    const std::vector<trackloom::ScheduledMidiPlaybackEvent> events {
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 0, 1, 60, 100),
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 120, 1, 64, 90),
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 240, 1, 67, 80),
    };
    FailingMidiEventReceiver receiver(2);

    const auto result = trackloom::dispatchScheduledMidiEvents(events, receiver);

    require(!result.success, "dispatch should fail when receiver rejects an event");
    require(result.attemptedEventCount == 2, "dispatch should include the failed event in attempts");
    require(result.deliveredEventCount == 1, "dispatch should count only accepted events as delivered");
    require(result.failedEventIndex == 1, "dispatch should report zero-based failed event index");
    require(receiver.callCount() == 2, "dispatch should stop immediately after receiver failure");
    require(receiver.events().size() == 2, "receiver should not receive events after failure");
}

void midiDispatchRejectsInvalidScheduledEvents()
{
    RecordingMidiEventReceiver invalidChannelReceiver;
    const auto invalidChannel = makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 0, 0, 60, 100);
    const auto invalidChannelResult = trackloom::dispatchScheduledMidiEvents({ invalidChannel }, invalidChannelReceiver);
    require(!trackloom::midiOutputMessageForEvent(invalidChannel).has_value(), "channel zero should not convert");
    require(!invalidChannelResult.success, "invalid channel dispatch should fail");
    require(invalidChannelResult.attemptedEventCount == 1, "invalid channel should count as attempted");
    require(invalidChannelResult.deliveredEventCount == 0, "invalid channel should not be delivered");
    require(invalidChannelResult.failedEventIndex == 0, "invalid channel should fail at index zero");
    require(invalidChannelReceiver.messages().empty(), "invalid channel should not call receiver");

    RecordingMidiEventReceiver invalidNoteReceiver;
    const auto invalidNote = makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 0, 1, 128, 100);
    const auto invalidNoteResult = trackloom::dispatchScheduledMidiEvents({ invalidNote }, invalidNoteReceiver);
    require(!trackloom::midiOutputMessageForEvent(invalidNote).has_value(), "note above 127 should not convert");
    require(!invalidNoteResult.success, "invalid note dispatch should fail");
    require(invalidNoteReceiver.messages().empty(), "invalid note should not call receiver");

    RecordingMidiEventReceiver invalidVelocityReceiver;
    const auto invalidVelocity = makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 0, 1, 60, 128);
    const auto invalidVelocityResult = trackloom::dispatchScheduledMidiEvents({ invalidVelocity }, invalidVelocityReceiver);
    require(!trackloom::midiOutputMessageForEvent(invalidVelocity).has_value(), "velocity above 127 should not convert");
    require(!invalidVelocityResult.success, "invalid velocity dispatch should fail");
    require(invalidVelocityReceiver.messages().empty(), "invalid velocity should not call receiver");

    RecordingMidiEventReceiver invalidOffsetReceiver;
    const auto invalidOffset = makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, -1, 1, 60, 100);
    const auto invalidOffsetResult = trackloom::dispatchScheduledMidiEvents({ invalidOffset }, invalidOffsetReceiver);
    require(!trackloom::midiOutputMessageForEvent(invalidOffset).has_value(), "negative sample offset should not convert");
    require(!invalidOffsetResult.success, "invalid sample offset dispatch should fail");
    require(invalidOffsetReceiver.messages().empty(), "invalid sample offset should not call receiver");

    RecordingMidiEventReceiver invalidTypeReceiver;
    auto invalidType = makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 0, 1, 60, 100);
    invalidType.event.type = static_cast<trackloom::MidiPlaybackEventType>(99);
    const auto invalidTypeResult = trackloom::dispatchScheduledMidiEvents({ invalidType }, invalidTypeReceiver);
    require(!trackloom::midiOutputMessageForEvent(invalidType).has_value(), "unknown midi event type should not convert");
    require(!invalidTypeResult.success, "unknown midi event type dispatch should fail");
    require(invalidTypeReceiver.messages().empty(), "unknown midi event type should not call receiver");
}

void midiOutputDeviceSendsMessagesThroughOpenPort()
{
    FakeMidiOutputDevicePort port("midi-out-1", "USB MIDI Out");
    trackloom::MidiOutputDevice device(port);
    const auto event = makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 7, 1, 60, 100);
    const auto message = trackloom::midiOutputMessageForEvent(event);

    require(message.has_value(), "device send test message should be valid");
    require(device.open(), "midi output device should open the wrapped port");
    require(device.isOpen(), "midi output device should report open state");

    const auto sent = device.receiveMidiEvent(event, *message);

    require(sent, "midi output device should send through an open port");
    require(device.lastFailureReason() == trackloom::MidiOutputDeviceFailureReason::None,
        "successful midi output device send should clear failure reason");
    require(device.sentMessageCount() == 1, "midi output device should count successful sends");
    require(port.openCallCount() == 1, "midi output device should open port once");
    require(port.sendCallCount() == 1, "midi output device should call port send once");
    require(port.messages().size() == 1, "midi output device port should record one message");
    require(port.messages()[0] == *message, "midi output device should forward the exact dispatch message");
    require(device.info().id == "midi-out-1", "midi output device should expose port id");
    require(device.info().name == "USB MIDI Out", "midi output device should expose port name");
}

void midiOutputDeviceRejectsSendWhenPortIsClosed()
{
    FakeMidiOutputDevicePort port("midi-out-1", "USB MIDI Out");
    trackloom::MidiOutputDevice device(port);
    const auto event = makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 0, 1, 60, 100);
    const auto message = trackloom::midiOutputMessageForEvent(event);

    require(message.has_value(), "closed device test message should be valid");

    const auto sent = device.receiveMidiEvent(event, *message);

    require(!sent, "closed midi output device should reject sends");
    require(device.lastFailureReason() == trackloom::MidiOutputDeviceFailureReason::DeviceNotOpen,
        "closed midi output device should report device-not-open");
    require(device.sentMessageCount() == 0, "closed midi output device should not count failed send");
    require(port.sendCallCount() == 0, "closed midi output device should not call port send");
}

void midiOutputDeviceReportsOpenFailure()
{
    FakeMidiOutputDevicePort port("midi-out-1", "USB MIDI Out");
    trackloom::MidiOutputDevice device(port);
    port.setOpenShouldSucceed(false);

    const auto opened = device.open();

    require(!opened, "midi output device should report failed open");
    require(!device.isOpen(), "failed midi output device open should keep closed state");
    require(device.lastFailureReason() == trackloom::MidiOutputDeviceFailureReason::OpenRejected,
        "failed midi output device open should expose open-rejected reason");
    require(port.openCallCount() == 1, "failed midi output device open should still call port");
}

void midiOutputDeviceCanCloseAndReopenPort()
{
    FakeMidiOutputDevicePort port("midi-out-1", "USB MIDI Out");
    trackloom::MidiOutputDevice device(port);
    const auto event = makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 0, 1, 60, 100);
    const auto message = trackloom::midiOutputMessageForEvent(event);

    require(message.has_value(), "reopened device test message should be valid");
    require(device.open(), "midi output device should open before close");

    device.close();
    const auto closedSend = device.receiveMidiEvent(event, *message);

    require(!device.isOpen(), "closed midi output device should report closed state");
    require(!closedSend, "closed midi output device should reject sends after explicit close");
    require(device.lastFailureReason() == trackloom::MidiOutputDeviceFailureReason::DeviceNotOpen,
        "closed midi output device should report device-not-open after explicit close");
    require(port.closeCallCount() == 1, "midi output device should close port once");

    require(device.open(), "midi output device should reopen after explicit close");
    require(device.receiveMidiEvent(event, *message), "reopened midi output device should send messages again");
    require(device.sentMessageCount() == 1, "reopened midi output device should count successful post-reopen send");
}

void midiOutputDeviceFailureStopsDispatch()
{
    FakeMidiOutputDevicePort port("midi-out-1", "USB MIDI Out");
    trackloom::MidiOutputDevice device(port);
    port.setSendShouldSucceed(false);
    require(device.open(), "failing midi output device should still open before send failure");

    const auto result = trackloom::dispatchScheduledMidiEvents({
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 0, 1, 60, 100),
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOff, 120, 1, 60, 0),
    }, device);

    require(!result.success, "midi output device send failure should fail dispatch");
    require(result.attemptedEventCount == 1, "midi output device send failure should stop at first attempted event");
    require(result.deliveredEventCount == 0, "midi output device send failure should deliver no events");
    require(result.failedEventIndex == 0, "midi output device send failure should keep failed index");
    require(device.lastFailureReason() == trackloom::MidiOutputDeviceFailureReason::SendRejected,
        "midi output device should expose send-rejected reason");
    require(device.sentMessageCount() == 0, "midi output device should not count rejected sends");
    require(port.sendCallCount() == 1, "midi output device should stop before second port send");
}

void midiTrackRouterRoutesEventsByTrackId()
{
    RecordingMidiEventReceiver leadReceiver;
    RecordingMidiEventReceiver padReceiver;
    trackloom::MidiTrackRouter router;
    const std::vector<trackloom::ScheduledMidiPlaybackEvent> events {
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 0, 1, 60, 100, "lead-track"),
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 120, 2, 64, 90, "pad-track"),
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOff, 240, 1, 60, 0, "lead-track"),
    };

    require(router.rebuild({
        { "lead-track", &leadReceiver },
        { "pad-track", &padReceiver },
    }), "track router should accept valid receiver bindings");
    require(router.receiverCount() == 2, "track router should report bound receiver count");

    const auto result = trackloom::dispatchScheduledMidiEvents(events, router);

    require(result.success, "track router dispatch should succeed for known tracks");
    require(result.attemptedEventCount == 3, "track router dispatch should attempt every event");
    require(result.deliveredEventCount == 3, "track router dispatch should deliver every event");
    require(result.failedEventIndex == -1, "track router dispatch should not report failed index");
    require(leadReceiver.events().size() == 2, "lead receiver should get two lead-track events");
    require(padReceiver.events().size() == 1, "pad receiver should get one pad-track event");
    require(leadReceiver.events()[0] == events[0], "lead receiver should get first lead event");
    require(leadReceiver.events()[1] == events[2], "lead receiver should get second lead event");
    require(padReceiver.events()[0] == events[1], "pad receiver should get the pad event");
    require(leadReceiver.messages().size() == 2, "lead receiver should get two lead midi messages");
    require(padReceiver.messages().size() == 1, "pad receiver should get one pad midi message");
    require(leadReceiver.messages()[0].statusByte == 0x90, "lead note on should keep channel 1 status");
    require(leadReceiver.messages()[1].statusByte == 0x80, "lead note off should keep channel 1 status");
    require(padReceiver.messages()[0].statusByte == 0x91, "pad note on should keep channel 2 status");
}

void midiTrackRouterFailsWhenTrackHasNoReceiver()
{
    RecordingMidiEventReceiver leadReceiver;
    trackloom::MidiTrackRouter router;
    const std::vector<trackloom::ScheduledMidiPlaybackEvent> events {
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 0, 1, 60, 100, "missing-track"),
    };

    require(router.rebuild({ { "lead-track", &leadReceiver } }), "track router should accept the known lead binding");

    const auto result = trackloom::dispatchScheduledMidiEvents(events, router);

    require(!result.success, "track router dispatch should fail for missing track binding");
    require(result.attemptedEventCount == 1, "missing track should count as attempted");
    require(result.deliveredEventCount == 0, "missing track should not be delivered");
    require(result.failedEventIndex == 0, "missing track should fail at index zero");
    require(leadReceiver.events().empty(), "missing track should not call unrelated receiver");
}

void midiTrackRouterPreservesReceiverFailure()
{
    FailingMidiEventReceiver receiver(1);
    trackloom::MidiTrackRouter router;
    const std::vector<trackloom::ScheduledMidiPlaybackEvent> events {
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 0, 1, 60, 100, "lead-track"),
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 120, 1, 64, 90, "lead-track"),
    };

    require(router.rebuild({ { "lead-track", &receiver } }), "track router should accept a failing test receiver");

    const auto result = trackloom::dispatchScheduledMidiEvents(events, router);

    require(!result.success, "track router should report downstream receiver failure");
    require(result.attemptedEventCount == 1, "downstream failure should stop at first attempted event");
    require(result.deliveredEventCount == 0, "failed downstream event should not count as delivered");
    require(result.failedEventIndex == 0, "downstream failure should keep failed index");
    require(receiver.callCount() == 1, "track router should call failing receiver once");
}

void midiTrackRouterValidatesBindingsWithoutReplacingPreviousRoutes()
{
    RecordingMidiEventReceiver leadReceiver;
    RecordingMidiEventReceiver replacementReceiver;
    trackloom::MidiTrackRouter router;

    require(router.rebuild({ { "lead-track", &leadReceiver } }), "track router should accept the initial binding");
    require(!router.rebuild({ { "", &replacementReceiver } }), "track router should reject empty track id");
    require(!router.rebuild({ { "pad-track", nullptr } }), "track router should reject null receiver");
    require(!router.rebuild({
        { "lead-track", &leadReceiver },
        { "lead-track", &replacementReceiver },
    }), "track router should reject duplicate track ids");

    const auto result = trackloom::dispatchScheduledMidiEvents({
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 0, 1, 60, 100, "lead-track"),
    }, router);

    require(result.success, "valid old route should remain active after failed rebuilds");
    require(leadReceiver.events().size() == 1, "old lead receiver should still receive events");
    require(replacementReceiver.events().empty(), "rejected replacement receiver should not receive events");
    require(router.receiverCount() == 1, "failed rebuilds should preserve previous receiver count");
}

void midiTrackRouterAcceptsEmptyBindings()
{
    trackloom::MidiTrackRouter router;
    const std::vector<trackloom::ScheduledMidiPlaybackEvent> noEvents;

    require(router.rebuild({}), "track router should accept empty bindings for projects without routed instruments");
    require(router.receiverCount() == 0, "empty router should report zero receivers");

    const auto emptyResult = trackloom::dispatchScheduledMidiEvents(noEvents, router);
    require(emptyResult.success, "empty router should accept empty event lists");
    require(emptyResult.attemptedEventCount == 0, "empty router should attempt no events");

    const auto missingResult = trackloom::dispatchScheduledMidiEvents({
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 0, 1, 60, 100, "lead-track"),
    }, router);
    require(!missingResult.success, "empty router should fail when an event needs a track receiver");
    require(missingResult.attemptedEventCount == 1, "empty router missing route should count one attempted event");
    require(missingResult.deliveredEventCount == 0, "empty router missing route should deliver no events");
    require(missingResult.failedEventIndex == 0, "empty router should fail at the first routed event");
}

void projectMidiOutputGraphDispatchesRenderedInstrumentTracks()
{
    trackloom::Project project("Project MIDI Output");
    trackloom::AudioEngine engine;
    trackloom::Transport transport;
    trackloom::ProjectMidiOutputGraph graph;
    RecordingMidiEventReceiver leadReceiver;
    RecordingMidiEventReceiver padReceiver;
    std::vector<float> samples(2 * 480, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 480);
    const auto leadTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto padTrack = project.createTrack("Pad", trackloom::TrackType::Instrument);
    const auto leadClip = project.createClip(leadTrack.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 960);
    const auto padClip = project.createClip(padTrack.id, "Pad Phrase", trackloom::ClipType::Midi, 960, 960);

    require(leadClip.has_value(), "project midi graph lead clip should exist");
    require(padClip.has_value(), "project midi graph pad clip should exist");
    require(project.createMidiNote(leadClip->id, 0, 240, 60, 100, 1).has_value(), "project midi graph lead note should exist");
    require(project.createMidiNote(padClip->id, 0, 240, 64, 90, 2).has_value(), "project midi graph pad note should exist");
    require(engine.prepare(1920.0, 2, 480), "project midi graph engine prepare should succeed");
    require(transport.seekToSample(960), "project midi graph transport should seek to first note block");
    transport.play();

    trackloom::AudioEngineRenderResult renderResult;
    require(engine.renderNextBlockWithMidi(transport, block, project, renderResult), "project midi graph render should expose scheduled events");
    require(renderResult.scheduledMidiEvents.size() == 4, "project midi graph render should expose two note pairs");
    require(graph.rebuild(project, {
        { leadTrack.id, &leadReceiver },
        { padTrack.id, &padReceiver },
    }), "project midi graph should accept instrument track bindings");

    const auto dispatchResult = graph.dispatch(renderResult);

    require(dispatchResult.success, "project midi graph dispatch should succeed for bound instrument tracks");
    require(dispatchResult.attemptedEventCount == 4, "project midi graph dispatch should attempt both note on and note off pairs");
    require(dispatchResult.deliveredEventCount == 4, "project midi graph dispatch should deliver every rendered event");
    require(dispatchResult.failedEventIndex == -1, "successful project midi graph dispatch should not report failed index");
    require(leadReceiver.events().size() == 2, "lead receiver should get lead note on and off");
    require(padReceiver.events().size() == 2, "pad receiver should get pad note on and off");
    require(leadReceiver.events()[0].event.trackId == leadTrack.id, "lead receiver first event should keep lead track id");
    require(padReceiver.events()[0].event.trackId == padTrack.id, "pad receiver first event should keep pad track id");
    require(leadReceiver.messages().size() == 2, "lead receiver should get two project midi messages");
    require(padReceiver.messages().size() == 2, "pad receiver should get two project midi messages");
    require(leadReceiver.messages()[0].sampleOffset == 0, "lead note on should keep rendered sample offset");
    require(padReceiver.messages()[0].sampleOffset == 0, "pad note on should keep rendered sample offset");
    require(transport.currentSample() == 1440, "project midi graph dispatch should not move transport");
}

void projectMidiOutputGraphRejectsInvalidBindingsWithoutReplacingOldRoutes()
{
    trackloom::Project project("Project MIDI Output");
    trackloom::ProjectMidiOutputGraph graph;
    RecordingMidiEventReceiver leadReceiver;
    RecordingMidiEventReceiver replacementReceiver;
    const auto leadTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto audioTrack = project.createTrack("Vocal", trackloom::TrackType::Audio);
    const auto folderTrack = project.createTrack("Folder", trackloom::TrackType::Folder);

    require(graph.rebuild(project, { { leadTrack.id, &leadReceiver } }), "project midi graph should accept initial instrument binding");
    require(!graph.rebuild(project, { { "missing-track", &replacementReceiver } }), "project midi graph should reject missing track binding");
    require(!graph.rebuild(project, { { audioTrack.id, &replacementReceiver } }), "project midi graph should reject audio track binding");
    require(!graph.rebuild(project, { { folderTrack.id, &replacementReceiver } }), "project midi graph should reject folder track binding");
    require(!graph.rebuild(project, { { leadTrack.id, nullptr } }), "project midi graph should reject null receiver binding");
    require(!graph.rebuild(project, {
        { leadTrack.id, &leadReceiver },
        { leadTrack.id, &replacementReceiver },
    }), "project midi graph should reject duplicate track bindings");

    trackloom::AudioEngineRenderResult renderResult;
    renderResult.scheduledMidiEvents.push_back(
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 0, 1, 60, 100, leadTrack.id));

    const auto dispatchResult = graph.dispatch(renderResult);

    require(dispatchResult.success, "old project midi graph route should remain active after failed rebuilds");
    require(graph.receiverCount() == 1, "failed project midi graph rebuilds should preserve receiver count");
    require(leadReceiver.events().size() == 1, "old lead receiver should still receive events");
    require(replacementReceiver.events().empty(), "rejected replacement receiver should not receive events");
}

void projectMidiOutputGraphReportsUnboundRenderedTracks()
{
    trackloom::Project project("Project MIDI Output");
    trackloom::ProjectMidiOutputGraph graph;
    RecordingMidiEventReceiver leadReceiver;
    const auto leadTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto padTrack = project.createTrack("Pad", trackloom::TrackType::Instrument);

    require(graph.rebuild(project, { { leadTrack.id, &leadReceiver } }), "project midi graph should accept lead binding");

    trackloom::AudioEngineRenderResult renderResult;
    renderResult.scheduledMidiEvents.push_back(
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 0, 1, 64, 90, padTrack.id));

    const auto dispatchResult = graph.dispatch(renderResult);

    require(!dispatchResult.success, "project midi graph should fail when rendered track has no receiver");
    require(dispatchResult.attemptedEventCount == 1, "unbound rendered track should count as attempted");
    require(dispatchResult.deliveredEventCount == 0, "unbound rendered track should not be delivered");
    require(dispatchResult.failedEventIndex == 0, "unbound rendered track should fail at index zero");
    require(leadReceiver.events().empty(), "unbound pad event should not call lead receiver");
}

void projectMidiOutputGraphPropagatesDownstreamFailure()
{
    trackloom::Project project("Project MIDI Output");
    trackloom::ProjectMidiOutputGraph graph;
    FailingMidiEventReceiver receiver(2);
    const auto leadTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);

    require(graph.rebuild(project, { { leadTrack.id, &receiver } }), "project midi graph should accept failing receiver binding");

    trackloom::AudioEngineRenderResult renderResult;
    renderResult.scheduledMidiEvents = {
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 0, 1, 60, 100, leadTrack.id),
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOff, 120, 1, 60, 0, leadTrack.id),
    };

    const auto dispatchResult = graph.dispatch(renderResult);

    require(!dispatchResult.success, "project midi graph should propagate downstream receiver failure");
    require(dispatchResult.attemptedEventCount == 2, "downstream failure should count the failed event as attempted");
    require(dispatchResult.deliveredEventCount == 1, "downstream failure should only count accepted events");
    require(dispatchResult.failedEventIndex == 1, "downstream failure should keep dispatch failed index");
    require(receiver.callCount() == 2, "project midi graph should stop after downstream failure");
}

void projectMidiOutputGraphAcceptsEmptyBindingsForEmptyRenderResult()
{
    trackloom::Project project("Project MIDI Output");
    trackloom::ProjectMidiOutputGraph graph;
    trackloom::AudioEngineRenderResult renderResult;

    require(graph.rebuild(project, {}), "project midi graph should accept empty bindings");

    const auto dispatchResult = graph.dispatch(renderResult);

    require(dispatchResult.success, "project midi graph should accept empty render results");
    require(dispatchResult.attemptedEventCount == 0, "empty project midi graph dispatch should attempt no events");
    require(dispatchResult.deliveredEventCount == 0, "empty project midi graph dispatch should deliver no events");
    require(dispatchResult.failedEventIndex == -1, "empty project midi graph dispatch should not report failed index");
    require(graph.receiverCount() == 0, "empty project midi graph should report zero receivers");
}

void midiOutputSessionTracksDeliveredNoteLifecycle()
{
    trackloom::Project project("MIDI Output Session");
    trackloom::MidiOutputSession session;
    RecordingMidiEventReceiver receiver;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);

    require(session.rebuild(project, { { track.id, &receiver } }), "midi output session should accept an instrument route");

    trackloom::AudioEngineRenderResult noteOnResult;
    noteOnResult.scheduledMidiEvents.push_back(
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 0, 1, 60, 100, track.id));

    const auto onDispatch = session.dispatch(noteOnResult);

    require(onDispatch.success, "midi output session should deliver note on");
    require(onDispatch.deliveredEventCount == 1, "delivered note on should count as delivered");
    require(session.activeNoteCount() == 1, "delivered note on should become active");
    require(receiver.events().size() == 1, "receiver should record delivered note on");

    trackloom::AudioEngineRenderResult noteOffResult;
    noteOffResult.scheduledMidiEvents.push_back(
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOff, 120, 1, 60, 0, track.id));

    const auto offDispatch = session.dispatch(noteOffResult);

    require(offDispatch.success, "midi output session should deliver note off");
    require(offDispatch.deliveredEventCount == 1, "delivered note off should count as delivered");
    require(session.activeNoteCount() == 0, "delivered note off should clear active note");
    require(receiver.events().size() == 2, "receiver should record note on and note off");
}

void midiOutputSessionIgnoresUndeliveredEvents()
{
    trackloom::Project project("MIDI Output Session");
    trackloom::MidiOutputSession session;
    FailingMidiEventReceiver receiver(1);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);

    require(session.rebuild(project, { { track.id, &receiver } }), "midi output session should accept failing receiver route");

    trackloom::AudioEngineRenderResult renderResult;
    renderResult.scheduledMidiEvents.push_back(
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 0, 1, 60, 100, track.id));

    const auto dispatchResult = session.dispatch(renderResult);

    require(!dispatchResult.success, "midi output session should report receiver failure");
    require(dispatchResult.deliveredEventCount == 0, "failed first event should not count as delivered");
    require(dispatchResult.failedEventIndex == 0, "failed first event should report index zero");
    require(session.activeNoteCount() == 0, "undelivered note on should not become active");
    require(receiver.callCount() == 1, "receiver failure should still count one attempted call");
}

void midiOutputSessionReleasesActiveNotes()
{
    trackloom::Project project("MIDI Output Session");
    trackloom::MidiOutputSession session;
    RecordingMidiEventReceiver receiver;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);

    require(session.rebuild(project, { { track.id, &receiver } }), "midi output session should accept release route");

    trackloom::AudioEngineRenderResult noteOnResult;
    noteOnResult.scheduledMidiEvents.push_back(
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 0, 1, 60, 100, track.id));
    require(session.dispatch(noteOnResult).success, "midi output session should deliver note before release");
    require(session.activeNoteCount() == 1, "note should be active before release");

    const auto releaseResult = session.releaseAllActiveNotes(32);

    require(releaseResult.success, "midi output session should release active notes");
    require(releaseResult.attemptedEventCount == 1, "release should attempt one note off");
    require(releaseResult.deliveredEventCount == 1, "release should deliver one note off");
    require(session.activeNoteCount() == 0, "successful release should clear active note");
    require(receiver.events().size() == 2, "receiver should record original note on and release note off");
    require(receiver.events()[1].event.type == trackloom::MidiPlaybackEventType::NoteOff, "release should generate note off event");
    require(receiver.messages()[1].sampleOffset == 32, "release should keep requested sample offset");
    require(receiver.messages()[1].statusByte == 0x80, "release should use note off status byte on channel one");
    require(receiver.messages()[1].data1 == 60, "release should keep active note pitch");
    require(receiver.messages()[1].data2 == 0, "release should send zero note off velocity");
}

void midiOutputSessionKeepsUndeliveredReleaseNotesActive()
{
    trackloom::Project project("MIDI Output Session");
    trackloom::MidiOutputSession session;
    FailingMidiEventReceiver receiver(4);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);

    require(session.rebuild(project, { { track.id, &receiver } }), "midi output session should accept partial release route");

    trackloom::AudioEngineRenderResult noteOnResult;
    noteOnResult.scheduledMidiEvents = {
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 0, 1, 60, 100, track.id),
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 4, 1, 64, 90, track.id),
    };
    require(session.dispatch(noteOnResult).success, "midi output session should deliver notes before partial release");
    require(session.activeNoteCount() == 2, "two delivered note ons should become active");

    const auto releaseResult = session.releaseAllActiveNotes(16);

    require(!releaseResult.success, "midi output session should report partial release failure");
    require(releaseResult.attemptedEventCount == 2, "partial release should attempt the failed event");
    require(releaseResult.deliveredEventCount == 1, "partial release should count only delivered note offs");
    require(releaseResult.failedEventIndex == 1, "partial release should report failed release index");
    require(session.activeNoteCount() == 1, "undelivered release note should remain active");
    require(receiver.callCount() == 4, "partial release should stop when receiver fails");
}

void midiOutputSessionReleaseClearsStackedMatchingNotes()
{
    trackloom::Project project("MIDI Output Session");
    trackloom::MidiOutputSession session;
    RecordingMidiEventReceiver receiver;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);

    require(session.rebuild(project, { { track.id, &receiver } }), "midi output session should accept stacked note route");

    trackloom::AudioEngineRenderResult noteOnResult;
    noteOnResult.scheduledMidiEvents = {
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 0, 1, 60, 100, track.id),
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 4, 1, 60, 90, track.id),
    };

    require(session.dispatch(noteOnResult).success, "midi output session should deliver stacked matching note ons");
    require(session.activeNoteCount() == 1, "matching note ons should share one active midi key");

    const auto releaseResult = session.releaseAllActiveNotes(12);

    require(releaseResult.success, "midi output session should release stacked matching note key");
    require(releaseResult.deliveredEventCount == 1, "stacked matching note release should need one note off");
    require(session.activeNoteCount() == 0, "panic-style release should clear the active key even after stacked note ons");
    require(receiver.events().size() == 3, "receiver should record two note ons and one release note off");
    require(receiver.events()[2].event.type == trackloom::MidiPlaybackEventType::NoteOff, "stacked matching note release should be note off");
}

void midiOutputSessionRejectsRebuildWhileNotesAreActive()
{
    trackloom::Project project("MIDI Output Session");
    trackloom::MidiOutputSession session;
    RecordingMidiEventReceiver leadReceiver;
    RecordingMidiEventReceiver padReceiver;
    const auto leadTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto padTrack = project.createTrack("Pad", trackloom::TrackType::Instrument);

    require(session.rebuild(project, { { leadTrack.id, &leadReceiver } }), "midi output session should accept initial route");

    trackloom::AudioEngineRenderResult leadNoteOn;
    leadNoteOn.scheduledMidiEvents.push_back(
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 0, 1, 60, 100, leadTrack.id));
    require(session.dispatch(leadNoteOn).success, "midi output session should deliver active lead note");
    require(session.activeNoteCount() == 1, "lead note should be active before rebuild");

    require(!session.rebuild(project, { { padTrack.id, &padReceiver } }), "midi output session should reject rebuild while notes are active");
    require(session.receiverCount() == 1, "rejected rebuild should keep previous route count");

    require(session.releaseAllActiveNotes(8).success, "midi output session should release active lead note before rebuild");
    require(session.rebuild(project, { { padTrack.id, &padReceiver } }), "midi output session should rebuild after all notes are released");

    trackloom::AudioEngineRenderResult padNoteOn;
    padNoteOn.scheduledMidiEvents.push_back(
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 0, 1, 64, 90, padTrack.id));
    require(session.dispatch(padNoteOn).success, "midi output session should deliver through rebuilt route");

    require(leadReceiver.events().size() == 2, "old route should only receive original note and release");
    require(padReceiver.events().size() == 1, "new route should receive pad note after rebuild");
}

void midiOutputSessionAcceptsEmptyDispatchAndRelease()
{
    trackloom::Project project("MIDI Output Session");
    trackloom::MidiOutputSession session;
    trackloom::AudioEngineRenderResult emptyResult;

    require(session.rebuild(project, {}), "midi output session should accept empty routes");

    const auto dispatchResult = session.dispatch(emptyResult);
    const auto releaseResult = session.releaseAllActiveNotes(0);

    require(dispatchResult.success, "midi output session should accept empty dispatch");
    require(dispatchResult.attemptedEventCount == 0, "empty dispatch should attempt no events");
    require(releaseResult.success, "midi output session should accept empty release");
    require(releaseResult.attemptedEventCount == 0, "empty release should attempt no events");
    require(session.activeNoteCount() == 0, "empty dispatch and release should keep no active notes");
}

void projectPlaybackSessionRendersAudioAndDispatchesMidi()
{
    trackloom::Project project("Project Playback Session");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    ConstantAudioSource source(0.50f);
    RecordingMidiEventReceiver receiver;
    std::vector<float> samples(2 * 480, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 480);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 960);

    require(clip.has_value(), "project playback session clip should exist");
    require(project.createMidiNote(clip->id, 0, 240, 60, 100, 1).has_value(), "project playback session note should exist");
    require(session.prepare(1920.0, 2, 480), "project playback session prepare should succeed");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }), "project playback session should rebuild audio graph");
    require(session.rebuildMidiOutput(project, { { track.id, &receiver } }), "project playback session should rebuild midi output");
    require(session.audioSourceCount() == 1, "project playback session should expose audio source count");
    require(session.midiReceiverCount() == 1, "project playback session should expose midi receiver count");
    require(transport.seekToSample(960), "project playback session transport should seek to note block");
    transport.play();

    const auto blockResult = session.renderNextBlock(transport, block, project);

    require(blockResult.renderSucceeded, "project playback session render should succeed");
    require(blockResult.renderResult.scheduledMidiEvents.size() == 2, "project playback session should collect note on and note off");
    require(blockResult.midiDispatch.success, "project playback session midi dispatch should succeed");
    require(blockResult.midiDispatch.deliveredEventCount == 2, "project playback session should dispatch both midi events");
    require(receiver.events().size() == 2, "project playback session receiver should record both midi events");
    require(allSamplesNear(samples, 0.50f), "project playback session should render bound audio source");
    require(transport.currentSample() == 1440, "project playback session should advance transport after render");
}

void projectPlaybackSessionChasesHeldMidiNote()
{
    trackloom::Project project("Project Playback Session");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    ConstantAudioSource source(0.50f);
    RecordingMidiEventReceiver receiver;
    std::vector<float> samples(2 * 960, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 960);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 1440);

    require(clip.has_value(), "project playback session chase clip should exist");
    require(project.createMidiNote(clip->id, 0, 960, 60, 100, 1).has_value(), "project playback session chase note should exist");
    require(session.prepare(1920.0, 2, 960), "project playback session chase prepare should succeed");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }), "project playback session chase should rebuild audio graph");
    require(session.rebuildMidiOutput(project, { { track.id, &receiver } }), "project playback session chase should rebuild midi output");
    require(transport.seekToSample(1200), "project playback session chase transport should seek inside held note");
    transport.play();

    const auto blockResult = session.renderNextBlock(transport, block, project);

    require(blockResult.renderSucceeded, "project playback session chase render should succeed");
    require(blockResult.renderResult.scheduledMidiEvents.size() == 2, "project playback session chase should collect note on and note off");
    require(blockResult.midiDispatch.success, "project playback session chase midi dispatch should succeed");
    require(blockResult.midiDispatch.deliveredEventCount == 2, "project playback session chase should dispatch both events");
    require(receiver.events().size() == 2, "project playback session chase receiver should record both events");
    require(receiver.events()[0].event.type == trackloom::MidiPlaybackEventType::NoteOn, "project playback session chase first event should be note on");
    require(receiver.events()[0].sampleOffset == 0, "project playback session chase note on should use sample offset zero");
    require(receiver.events()[1].event.type == trackloom::MidiPlaybackEventType::NoteOff, "project playback session chase second event should be note off");
    require(receiver.events()[1].sampleOffset == 720, "project playback session chase note off should keep release offset");
    require(session.activeMidiNoteCount() == 0, "project playback session chase should not leave active midi notes");
}

void projectPlaybackSessionDoesNotRepeatMidiChaseOnContinuousBlocks()
{
    trackloom::Project project("Project Playback Session");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    ConstantAudioSource source(0.25f);
    RecordingMidiEventReceiver receiver;
    std::vector<float> firstSamples(2 * 480, 0.0f);
    std::vector<float> secondSamples(2 * 480, 0.0f);
    trackloom::AudioBlock firstBlock(firstSamples.data(), 2, 480);
    trackloom::AudioBlock secondBlock(secondSamples.data(), 2, 480);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 1440);

    require(clip.has_value(), "continuous chase clip should exist");
    require(project.createMidiNote(clip->id, 0, 960, 60, 100, 1).has_value(), "continuous chase note should exist");
    require(session.prepare(1920.0, 2, 480), "continuous chase prepare should succeed");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }), "continuous chase should rebuild audio graph");
    require(session.rebuildMidiOutput(project, { { track.id, &receiver } }), "continuous chase should rebuild midi output");
    require(transport.seekToSample(1200), "continuous chase transport should seek inside held note");
    transport.play();

    const auto firstResult = session.renderNextBlock(transport, firstBlock, project);
    const auto secondResult = session.renderNextBlock(transport, secondBlock, project);

    require(firstResult.renderSucceeded, "continuous chase first render should succeed");
    require(secondResult.renderSucceeded, "continuous chase second render should succeed");
    require(receiver.events().size() == 2, "continuous chase should not repeat note on at the next block");
    require(receiver.events()[0].event.type == trackloom::MidiPlaybackEventType::NoteOn, "continuous chase first event should be note on");
    require(receiver.events()[0].sampleOffset == 0, "continuous chase note on should start first block");
    require(receiver.events()[1].event.type == trackloom::MidiPlaybackEventType::NoteOff, "continuous chase second event should be real note off");
    require(receiver.events()[1].sampleOffset == 240, "continuous chase note off should be inside second block");
    require(session.activeMidiNoteCount() == 0, "continuous chase should clear active note after real note off");
}

void projectPlaybackSessionCanRequestMidiChaseAfterSeek()
{
    trackloom::Project project("Project Playback Session");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    ConstantAudioSource source(0.10f);
    RecordingMidiEventReceiver receiver;
    std::vector<float> firstSamples(2 * 480, 0.0f);
    std::vector<float> secondSamples(2 * 960, 0.0f);
    trackloom::AudioBlock firstBlock(firstSamples.data(), 2, 480);
    trackloom::AudioBlock secondBlock(secondSamples.data(), 2, 960);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 1440);

    require(!session.requestMidiChaseOnNextBlock(), "unprepared session should reject chase requests");
    require(clip.has_value(), "requested chase clip should exist");
    require(project.createMidiNote(clip->id, 0, 960, 60, 100, 1).has_value(), "requested chase note should exist");
    require(session.prepare(1920.0, 2, 960), "requested chase prepare should succeed");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }), "requested chase should rebuild audio graph");
    require(session.rebuildMidiOutput(project, { { track.id, &receiver } }), "requested chase should rebuild midi output");
    require(transport.seekToSample(0), "requested chase transport should start before note");
    transport.play();

    const auto firstResult = session.renderNextBlock(transport, firstBlock, project);
    require(firstResult.renderSucceeded, "requested chase first render should succeed");
    require(receiver.events().empty(), "requested chase first render should not emit midi before note");

    require(transport.seekToSample(1200), "requested chase transport should seek inside held note");
    require(session.requestMidiChaseOnNextBlock(), "prepared session should accept chase request");

    const auto secondResult = session.renderNextBlock(transport, secondBlock, project);

    require(secondResult.renderSucceeded, "requested chase second render should succeed");
    require(receiver.events().size() == 2, "requested chase should emit chase note on and real note off after seek");
    require(receiver.events()[0].event.type == trackloom::MidiPlaybackEventType::NoteOn, "requested chase first event should be note on");
    require(receiver.events()[0].event.absoluteTick == 1200, "requested chase note on should use seeked block start");
    require(receiver.events()[0].sampleOffset == 0, "requested chase note on should use sample offset zero");
    require(receiver.events()[1].event.type == trackloom::MidiPlaybackEventType::NoteOff, "requested chase second event should be note off");
    require(receiver.events()[1].sampleOffset == 720, "requested chase note off should keep release offset");
    require(session.activeMidiNoteCount() == 0, "requested chase should leave no active midi notes after release");
}

void projectPlaybackSessionRendersLoopedBlockAndDispatchesMidi()
{
    trackloom::Project project("Project Playback Session");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    ConstantAudioSource source(0.50f);
    RecordingMidiEventReceiver receiver;
    std::vector<float> samples(2 * 960, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 960);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Loop Phrase", trackloom::ClipType::Midi, 960, 960);
    const trackloom::PlaybackLoopRange loop { 960, 1920 };

    require(clip.has_value(), "looped project playback clip should exist");
    require(project.createMidiNote(clip->id, 720, 120, 67, 80, 1).has_value(), "looped project playback pre-wrap note should exist");
    require(project.createMidiNote(clip->id, 0, 120, 60, 100, 1).has_value(), "looped project playback wrapped note should exist");
    require(session.prepare(1920.0, 2, 960), "looped project playback prepare should succeed");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }), "looped project playback should rebuild audio graph");
    require(session.rebuildMidiOutput(project, { { track.id, &receiver } }), "looped project playback should rebuild midi output");
    require(transport.seekToSample(1440), "looped project playback transport should seek before loop end");
    transport.play();

    const auto blockResult = session.renderNextLoopedBlock(transport, block, project, loop);

    require(blockResult.renderSucceeded, "looped project playback render should succeed");
    require(blockResult.renderResult.scheduledMidiEvents.size() == 4, "looped project playback should collect events across wrap");
    require(blockResult.midiDispatch.success, "looped project playback midi dispatch should succeed");
    require(blockResult.midiDispatch.deliveredEventCount == 4, "looped project playback should dispatch all looped events");
    require(receiver.events().size() == 4, "looped project playback receiver should record all looped events");
    require(receiver.events()[0].sampleOffset == 240, "looped project playback pre-wrap note on should keep sample offset");
    require(receiver.events()[1].sampleOffset == 360, "looped project playback pre-wrap note off should keep sample offset");
    require(receiver.events()[2].sampleOffset == 480, "looped project playback wrapped note on should keep block-wide sample offset");
    require(receiver.events()[3].sampleOffset == 600, "looped project playback wrapped note off should keep block-wide sample offset");
    require(allSamplesNear(samples, 0.50f), "looped project playback should render bound audio source");
    require(transport.currentSample() == 2400, "looped project playback should advance transport linearly after render");
    require(session.activeMidiNoteCount() == 0, "looped project playback note pairs should leave no active midi notes");
}

void projectPlaybackSessionReleasesLoopBoundaryLongNote()
{
    trackloom::Project project("Project Playback Session");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    ConstantAudioSource source(0.50f);
    RecordingMidiEventReceiver receiver;
    std::vector<float> samples(2 * 960, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 960);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Loop Phrase", trackloom::ClipType::Midi, 960, 1440);
    const trackloom::PlaybackLoopRange loop { 960, 1920 };

    require(clip.has_value(), "loop boundary session clip should exist");

    // 这个集成测试验证边界 Note Off 会真正进入输出会话，并清掉活动音符状态。
    require(project.createMidiNote(clip->id, 720, 480, 67, 80, 1).has_value(), "loop boundary session long note should exist");
    require(session.prepare(1920.0, 2, 960), "loop boundary session prepare should succeed");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }), "loop boundary session should rebuild audio graph");
    require(session.rebuildMidiOutput(project, { { track.id, &receiver } }), "loop boundary session should rebuild midi output");
    require(transport.seekToSample(1440), "loop boundary session transport should seek before loop end");
    transport.play();

    const auto blockResult = session.renderNextLoopedBlock(transport, block, project, loop);

    require(blockResult.renderSucceeded, "loop boundary session render should succeed");
    require(blockResult.renderResult.scheduledMidiEvents.size() == 2, "loop boundary session should schedule note on and boundary note off");
    require(blockResult.midiDispatch.success, "loop boundary session midi dispatch should succeed");
    require(blockResult.midiDispatch.deliveredEventCount == 2, "loop boundary session should dispatch both events");
    require(receiver.events().size() == 2, "loop boundary session receiver should record both events");
    require(receiver.events()[0].sampleOffset == 240, "loop boundary session note on should keep pre-wrap offset");
    require(receiver.events()[1].sampleOffset == 479, "loop boundary session note off should use last pre-wrap sample");
    require(session.activeMidiNoteCount() == 0, "loop boundary session should not leave an active midi note");
}

void projectPlaybackSessionRejectsInvalidLoopRangeWithoutDispatch()
{
    trackloom::Project project("Project Playback Session");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    ConstantAudioSource source(0.50f);
    RecordingMidiEventReceiver receiver;
    std::vector<float> samples(2 * 480, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 480);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Loop Phrase", trackloom::ClipType::Midi, 960, 960);
    const trackloom::PlaybackLoopRange invalidLoop { 1920, 960 };

    require(clip.has_value(), "invalid loop session clip should exist");
    require(project.createMidiNote(clip->id, 0, 120, 60, 100, 1).has_value(), "invalid loop session note should exist");
    require(session.prepare(1920.0, 2, 480), "invalid loop session prepare should succeed");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }), "invalid loop session should rebuild audio graph");
    require(session.rebuildMidiOutput(project, { { track.id, &receiver } }), "invalid loop session should rebuild midi output");
    require(transport.seekToSample(960), "invalid loop session transport should seek before render");
    transport.play();

    const auto blockResult = session.renderNextLoopedBlock(transport, block, project, invalidLoop);

    require(!blockResult.renderSucceeded, "invalid loop session render should fail");
    require(blockResult.renderResult.scheduledMidiEvents.empty(), "invalid loop session should expose no scheduled events");
    require(blockResult.midiDispatch.attemptedEventCount == 0, "invalid loop session should not dispatch midi");
    require(receiver.events().empty(), "invalid loop session receiver should not receive midi");
    require(transport.currentSample() == 960, "invalid loop session should not advance transport");
}

void projectPlaybackSessionKeepsAudioRenderWhenMidiDispatchFails()
{
    trackloom::Project project("Project Playback Session");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    ConstantAudioSource source(0.25f);
    FailingMidiEventReceiver receiver(1);
    std::vector<float> samples(2 * 480, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 480);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 960);

    require(clip.has_value(), "project playback session failing midi clip should exist");
    require(project.createMidiNote(clip->id, 0, 240, 60, 100, 1).has_value(), "project playback session failing midi note should exist");
    require(session.prepare(1920.0, 2, 480), "project playback session failing midi prepare should succeed");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }), "project playback session failing midi should rebuild audio graph");
    require(session.rebuildMidiOutput(project, { { track.id, &receiver } }), "project playback session failing midi should rebuild midi output");
    require(transport.seekToSample(960), "project playback session failing midi transport should seek");
    transport.play();

    const auto blockResult = session.renderNextBlock(transport, block, project);

    require(blockResult.renderSucceeded, "midi output failure should not turn audio render into failure");
    require(!blockResult.midiDispatch.success, "project playback session should report midi dispatch failure separately");
    require(blockResult.midiDispatch.failedEventIndex == 0, "project playback session should keep midi failure index");
    require(allSamplesNear(samples, 0.25f), "midi output failure should not erase rendered audio");
    require(transport.currentSample() == 1440, "midi output failure should not roll back transport");
    require(session.activeMidiNoteCount() == 0, "undelivered midi note should not become active through session");
}

void projectPlaybackSessionDoesNotDispatchMidiWhenAudioRenderFails()
{
    trackloom::Project project("Project Playback Session");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    FailingAudioSource source;
    RecordingMidiEventReceiver receiver;
    std::vector<float> samples(2 * 480, 1.0f);
    trackloom::AudioBlock block(samples.data(), 2, 480);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 960);

    require(clip.has_value(), "project playback session failing audio clip should exist");
    require(project.createMidiNote(clip->id, 0, 240, 60, 100, 1).has_value(), "project playback session failing audio note should exist");
    require(session.prepare(1920.0, 2, 480), "project playback session failing audio prepare should succeed");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }), "project playback session failing audio should rebuild audio graph");
    require(session.rebuildMidiOutput(project, { { track.id, &receiver } }), "project playback session failing audio should rebuild midi output");
    require(transport.seekToSample(960), "project playback session failing audio transport should seek");
    transport.play();

    const auto blockResult = session.renderNextBlock(transport, block, project);

    require(!blockResult.renderSucceeded, "project playback session should report audio render failure");
    require(blockResult.midiDispatch.attemptedEventCount == 0, "audio render failure should not attempt midi dispatch");
    require(receiver.events().empty(), "audio render failure should not dispatch midi events");
    require(allSamplesNear(samples, 0.0f), "audio render failure should clear output block");
    require(transport.currentSample() == 960, "audio render failure should not advance transport");
}

void projectPlaybackSessionRendersStoppedBlockWithoutMidi()
{
    trackloom::Project project("Project Playback Session");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    CountingAudioSource source(0.50f);
    RecordingMidiEventReceiver receiver;
    std::vector<float> samples(2 * 480, 1.0f);
    trackloom::AudioBlock block(samples.data(), 2, 480);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 960);

    require(clip.has_value(), "project playback session stopped clip should exist");
    require(project.createMidiNote(clip->id, 0, 240, 60, 100, 1).has_value(), "project playback session stopped note should exist");
    require(session.prepare(1920.0, 2, 480), "project playback session stopped prepare should succeed");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }), "project playback session stopped should rebuild audio graph");
    require(session.rebuildMidiOutput(project, { { track.id, &receiver } }), "project playback session stopped should rebuild midi output");
    require(transport.seekToSample(960), "project playback session stopped transport should seek");

    const auto blockResult = session.renderNextBlock(transport, block, project);

    require(blockResult.renderSucceeded, "stopped project playback session render should succeed");
    require(blockResult.renderResult.scheduledMidiEvents.empty(), "stopped render should not collect midi events");
    require(blockResult.midiDispatch.success, "stopped render should dispatch empty midi result successfully");
    require(receiver.events().empty(), "stopped render should not send midi events");
    require(source.renderCount() == 0, "stopped render should not process audio source");
    require(allSamplesNear(samples, 0.0f), "stopped render should clear audio block to silence");
    require(transport.currentSample() == 960, "stopped render should not advance transport");
}

void projectPlaybackSessionCanReleaseActiveMidiNotes()
{
    trackloom::Project project("Project Playback Session");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    ConstantAudioSource source(0.0f);
    RecordingMidiEventReceiver receiver;
    std::vector<float> samples(2 * 480, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 480);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 1920);

    require(clip.has_value(), "project playback session release clip should exist");
    require(project.createMidiNote(clip->id, 0, 960, 60, 100, 1).has_value(), "project playback session release note should exist");
    require(session.prepare(1920.0, 2, 480), "project playback session release prepare should succeed");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }), "project playback session release should rebuild audio graph");
    require(session.rebuildMidiOutput(project, { { track.id, &receiver } }), "project playback session release should rebuild midi output");
    require(transport.seekToSample(960), "project playback session release transport should seek");
    transport.play();

    const auto blockResult = session.renderNextBlock(transport, block, project);

    require(blockResult.renderSucceeded, "project playback session long-note render should succeed");
    require(blockResult.midiDispatch.success, "project playback session long-note midi dispatch should succeed");
    require(receiver.events().size() == 1, "long note first block should send only note on");
    require(session.activeMidiNoteCount() == 1, "long note note on should remain active after first block");

    const auto releaseResult = session.releaseActiveMidiNotes(24);

    require(releaseResult.success, "project playback session should release active midi notes");
    require(releaseResult.deliveredEventCount == 1, "project playback session release should deliver one note off");
    require(session.activeMidiNoteCount() == 0, "project playback session release should clear active midi note");
    require(receiver.events().size() == 2, "project playback session receiver should record note on and release note off");
    require(receiver.events()[1].event.type == trackloom::MidiPlaybackEventType::NoteOff, "project playback session release should generate note off");
    require(receiver.messages()[1].sampleOffset == 24, "project playback session release should keep requested sample offset");
}

void projectPlaybackSessionStopsAfterReleasingActiveMidiNotes()
{
    trackloom::Project project("Project Playback Session");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    ConstantAudioSource source(0.0f);
    RecordingMidiEventReceiver receiver;
    std::vector<float> samples(2 * 480, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 480);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 1920);

    require(clip.has_value(), "safe stop clip should exist");
    require(project.createMidiNote(clip->id, 0, 960, 60, 100, 1).has_value(), "safe stop note should exist");
    require(session.prepare(1920.0, 2, 480), "safe stop prepare should succeed");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }), "safe stop should rebuild audio graph");
    require(session.rebuildMidiOutput(project, { { track.id, &receiver } }), "safe stop should rebuild midi output");
    require(transport.seekToSample(960), "safe stop transport should seek");
    transport.play();

    const auto blockResult = session.renderNextBlock(transport, block, project);
    require(blockResult.renderSucceeded, "safe stop initial render should succeed");
    require(session.activeMidiNoteCount() == 1, "safe stop should have active note before stop");

    const auto stopResult = session.stopPlayback(transport, 32);

    require(stopResult.success, "safe stop should succeed after midi release");
    require(stopResult.transportChanged, "safe stop should report transport changed");
    require(stopResult.midiRelease.success, "safe stop should report successful midi release");
    require(stopResult.midiRelease.deliveredEventCount == 1, "safe stop should deliver release note off");
    require(!transport.isPlaying(), "safe stop should stop transport after release");
    require(session.activeMidiNoteCount() == 0, "safe stop should clear active midi notes");
    require(receiver.events().size() == 2, "safe stop receiver should record note on and release note off");
    require(receiver.events()[1].event.type == trackloom::MidiPlaybackEventType::NoteOff, "safe stop release should be note off");
    require(receiver.events()[1].sampleOffset == 32, "safe stop release should use requested sample offset");
}

void projectPlaybackSessionDoesNotStopWhenMidiReleaseFails()
{
    trackloom::Project project("Project Playback Session");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    ConstantAudioSource source(0.0f);
    FailingMidiEventReceiver receiver(2);
    std::vector<float> samples(2 * 480, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 480);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 1920);

    require(clip.has_value(), "failed safe stop clip should exist");
    require(project.createMidiNote(clip->id, 0, 960, 60, 100, 1).has_value(), "failed safe stop note should exist");
    require(session.prepare(1920.0, 2, 480), "failed safe stop prepare should succeed");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }), "failed safe stop should rebuild audio graph");
    require(session.rebuildMidiOutput(project, { { track.id, &receiver } }), "failed safe stop should rebuild midi output");
    require(transport.seekToSample(960), "failed safe stop transport should seek");
    transport.play();

    const auto blockResult = session.renderNextBlock(transport, block, project);
    require(blockResult.renderSucceeded, "failed safe stop initial render should succeed");
    require(blockResult.midiDispatch.success, "failed safe stop note on should be delivered");
    require(session.activeMidiNoteCount() == 1, "failed safe stop should have active note before stop");

    const auto stopResult = session.stopPlayback(transport, 16);

    require(!stopResult.success, "safe stop should fail when midi release fails");
    require(!stopResult.transportChanged, "safe stop should not change transport when release fails");
    require(!stopResult.midiRelease.success, "safe stop should expose failed midi release");
    require(transport.isPlaying(), "safe stop should keep transport playing when release fails");
    require(session.activeMidiNoteCount() == 1, "safe stop should keep active midi note after release failure");
}

void projectPlaybackSessionSeeksAfterReleasingActiveMidiNotes()
{
    trackloom::Project project("Project Playback Session");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    ConstantAudioSource source(0.0f);
    RecordingMidiEventReceiver receiver;
    std::vector<float> firstSamples(2 * 480, 0.0f);
    std::vector<float> secondSamples(2 * 960, 0.0f);
    trackloom::AudioBlock firstBlock(firstSamples.data(), 2, 480);
    trackloom::AudioBlock secondBlock(secondSamples.data(), 2, 960);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 1920);

    require(clip.has_value(), "safe seek clip should exist");
    require(project.createMidiNote(clip->id, 0, 960, 60, 100, 1).has_value(), "safe seek note should exist");
    require(session.prepare(1920.0, 2, 960), "safe seek prepare should succeed");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }), "safe seek should rebuild audio graph");
    require(session.rebuildMidiOutput(project, { { track.id, &receiver } }), "safe seek should rebuild midi output");
    require(transport.seekToSample(960), "safe seek transport should seek to note start");
    transport.play();

    const auto firstResult = session.renderNextBlock(transport, firstBlock, project);
    require(firstResult.renderSucceeded, "safe seek initial render should succeed");
    require(session.activeMidiNoteCount() == 1, "safe seek should have active note before seek");

    const auto seekResult = session.seekPlaybackToSample(transport, 1200, 20);

    require(seekResult.success, "safe seek should succeed after midi release");
    require(seekResult.transportChanged, "safe seek should report transport changed");
    require(seekResult.midiRelease.success, "safe seek should report successful release");
    require(transport.currentSample() == 1200, "safe seek should move transport after release");
    require(session.activeMidiNoteCount() == 0, "safe seek should clear active note before new block");

    const auto secondResult = session.renderNextBlock(transport, secondBlock, project);

    require(secondResult.renderSucceeded, "safe seek chased render should succeed");
    require(receiver.events().size() == 4, "safe seek should record original note, release, chased note, and real release");
    require(receiver.events()[1].event.type == trackloom::MidiPlaybackEventType::NoteOff, "safe seek should release before moving");
    require(receiver.events()[1].sampleOffset == 20, "safe seek release should use requested sample offset");
    require(receiver.events()[2].event.type == trackloom::MidiPlaybackEventType::NoteOn, "safe seek next block should chase held note");
    require(receiver.events()[2].event.absoluteTick == 1200, "safe seek chase should use new transport tick");
    require(receiver.events()[3].event.type == trackloom::MidiPlaybackEventType::NoteOff, "safe seek next block should release at real note end");
    require(session.activeMidiNoteCount() == 0, "safe seek should leave no active note after real release");
}

void projectPlaybackSessionRejectsInvalidSafeSeek()
{
    trackloom::Project project("Project Playback Session");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;

    require(transport.seekToSample(960), "invalid safe seek transport should start at known sample");
    const auto unpreparedResult = session.seekPlaybackToSample(transport, 1200, 0);
    require(!unpreparedResult.success, "unprepared safe seek should fail");
    require(!unpreparedResult.transportChanged, "unprepared safe seek should not change transport");
    require(transport.currentSample() == 960, "unprepared safe seek should keep transport sample");

    require(session.prepare(1920.0, 2, 480), "invalid safe seek prepare should succeed");
    const auto negativeResult = session.seekPlaybackToSample(transport, -1, 0);
    require(!negativeResult.success, "negative safe seek should fail");
    require(!negativeResult.transportChanged, "negative safe seek should not change transport");
    require(transport.currentSample() == 960, "negative safe seek should keep transport sample");
}

void projectPlaybackSessionSafelyRebuildsMidiOutputAfterRelease()
{
    trackloom::Project project("Project Playback Session");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    ConstantAudioSource source(0.0f);
    RecordingMidiEventReceiver oldReceiver;
    RecordingMidiEventReceiver newReceiver;
    std::vector<float> firstSamples(2 * 480, 0.0f);
    std::vector<float> secondSamples(2 * 480, 0.0f);
    trackloom::AudioBlock firstBlock(firstSamples.data(), 2, 480);
    trackloom::AudioBlock secondBlock(secondSamples.data(), 2, 480);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 1920);

    require(clip.has_value(), "safe midi rebuild clip should exist");
    require(project.createMidiNote(clip->id, 0, 1920, 60, 100, 1).has_value(), "safe midi rebuild note should exist");
    require(session.prepare(1920.0, 2, 480), "safe midi rebuild prepare should succeed");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }), "safe midi rebuild should rebuild audio graph");
    require(session.rebuildMidiOutput(project, { { track.id, &oldReceiver } }), "safe midi rebuild should bind old receiver");
    require(transport.seekToSample(960), "safe midi rebuild transport should seek");
    transport.play();

    const auto firstResult = session.renderNextBlock(transport, firstBlock, project);
    require(firstResult.renderSucceeded, "safe midi rebuild first render should succeed");
    require(session.activeMidiNoteCount() == 1, "safe midi rebuild should start with active note");

    const auto rebuildResult = session.rebuildMidiOutputSafely(project, { { track.id, &newReceiver } }, 40);

    require(rebuildResult.success, "safe midi rebuild should succeed after releasing old output");
    require(rebuildResult.midiOutputChanged, "safe midi rebuild should report output changed");
    require(rebuildResult.midiRelease.success, "safe midi rebuild should report release success");
    require(rebuildResult.midiRelease.deliveredEventCount == 1, "safe midi rebuild should release one active note");
    require(session.activeMidiNoteCount() == 0, "safe midi rebuild should clear old active note before switching");
    require(oldReceiver.events().size() == 2, "safe midi rebuild old receiver should get note on and release");
    require(oldReceiver.events()[1].event.type == trackloom::MidiPlaybackEventType::NoteOff, "safe midi rebuild should release through old receiver");
    require(oldReceiver.events()[1].sampleOffset == 40, "safe midi rebuild release should keep requested sample offset");
    require(newReceiver.events().empty(), "safe midi rebuild new receiver should not receive old release");

    const auto secondResult = session.renderNextBlock(transport, secondBlock, project);

    require(secondResult.renderSucceeded, "safe midi rebuild chased render should succeed");
    require(newReceiver.events().size() == 1, "safe midi rebuild new receiver should receive chased note");
    require(newReceiver.events()[0].event.type == trackloom::MidiPlaybackEventType::NoteOn, "safe midi rebuild chased event should be note on");
    require(newReceiver.events()[0].event.absoluteTick == 1440, "safe midi rebuild chased event should use current transport tick");
    require(session.activeMidiNoteCount() == 1, "safe midi rebuild chased note should become active on new route");
}

void projectPlaybackSessionKeepsMidiOutputWhenSafeRebuildReleaseFails()
{
    trackloom::Project project("Project Playback Session");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    ConstantAudioSource source(0.0f);
    FailingMidiEventReceiver oldReceiver(2);
    RecordingMidiEventReceiver newReceiver;
    std::vector<float> firstSamples(2 * 480, 0.0f);
    std::vector<float> secondSamples(2 * 480, 0.0f);
    trackloom::AudioBlock firstBlock(firstSamples.data(), 2, 480);
    trackloom::AudioBlock secondBlock(secondSamples.data(), 2, 480);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 960);

    require(clip.has_value(), "failed safe midi rebuild clip should exist");
    require(project.createMidiNote(clip->id, 0, 480, 60, 100, 1).has_value(), "failed safe midi rebuild note should exist");
    require(session.prepare(1920.0, 2, 480), "failed safe midi rebuild prepare should succeed");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }), "failed safe midi rebuild should rebuild audio graph");
    require(session.rebuildMidiOutput(project, { { track.id, &oldReceiver } }), "failed safe midi rebuild should bind old receiver");
    require(transport.seekToSample(960), "failed safe midi rebuild transport should seek");
    transport.play();

    const auto firstResult = session.renderNextBlock(transport, firstBlock, project);
    require(firstResult.renderSucceeded, "failed safe midi rebuild first render should succeed");
    require(firstResult.midiDispatch.success, "failed safe midi rebuild first note on should be delivered");
    require(session.activeMidiNoteCount() == 1, "failed safe midi rebuild should have active note before rebuild");

    const auto rebuildResult = session.rebuildMidiOutputSafely(project, { { track.id, &newReceiver } }, 16);

    require(!rebuildResult.success, "safe midi rebuild should fail when release fails");
    require(!rebuildResult.midiOutputChanged, "safe midi rebuild should not change output when release fails");
    require(!rebuildResult.midiRelease.success, "safe midi rebuild should expose release failure");
    require(session.activeMidiNoteCount() == 1, "safe midi rebuild should keep active note after release failure");
    require(newReceiver.events().empty(), "safe midi rebuild should not send events to new receiver after release failure");

    const auto secondResult = session.renderNextBlock(transport, secondBlock, project);

    require(secondResult.renderSucceeded, "safe midi rebuild old-route render should still succeed");
    require(oldReceiver.events().size() == 3, "safe midi rebuild old receiver should keep receiving after release failure");
    require(oldReceiver.events()[2].event.type == trackloom::MidiPlaybackEventType::NoteOff, "safe midi rebuild old receiver should get real note off");
    require(newReceiver.events().empty(), "safe midi rebuild should keep new receiver unused after release failure");
    require(session.activeMidiNoteCount() == 0, "safe midi rebuild should clear active note after real note off");
}

void projectPlaybackSessionKeepsMidiOutputWhenSafeRebuildBindingFails()
{
    trackloom::Project project("Project Playback Session");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    ConstantAudioSource source(0.0f);
    RecordingMidiEventReceiver oldReceiver;
    RecordingMidiEventReceiver invalidReceiver;
    std::vector<float> firstSamples(2 * 480, 0.0f);
    std::vector<float> secondSamples(2 * 480, 0.0f);
    trackloom::AudioBlock firstBlock(firstSamples.data(), 2, 480);
    trackloom::AudioBlock secondBlock(secondSamples.data(), 2, 480);
    const auto instrumentTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto audioTrack = project.createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = project.createClip(instrumentTrack.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 1920);

    require(clip.has_value(), "invalid safe midi rebuild clip should exist");
    require(project.createMidiNote(clip->id, 0, 1920, 60, 100, 1).has_value(), "invalid safe midi rebuild note should exist");
    require(session.prepare(1920.0, 2, 480), "invalid safe midi rebuild prepare should succeed");
    require(session.rebuildAudioGraph(project, { { instrumentTrack.id, &source } }), "invalid safe midi rebuild should rebuild audio graph");
    require(session.rebuildMidiOutput(project, { { instrumentTrack.id, &oldReceiver } }), "invalid safe midi rebuild should bind old receiver");
    require(transport.seekToSample(960), "invalid safe midi rebuild transport should seek");
    transport.play();

    const auto firstResult = session.renderNextBlock(transport, firstBlock, project);
    require(firstResult.renderSucceeded, "invalid safe midi rebuild first render should succeed");
    require(session.activeMidiNoteCount() == 1, "invalid safe midi rebuild should have active note before rebuild");

    const auto rebuildResult = session.rebuildMidiOutputSafely(project, { { audioTrack.id, &invalidReceiver } }, 8);

    require(!rebuildResult.success, "safe midi rebuild should fail for non-instrument binding");
    require(!rebuildResult.midiOutputChanged, "safe midi rebuild should not report output changed for invalid binding");
    require(rebuildResult.midiRelease.success, "safe midi rebuild should release before invalid binding is rejected");
    require(session.activeMidiNoteCount() == 0, "safe midi rebuild should clear note before invalid binding result");
    require(invalidReceiver.events().empty(), "invalid safe midi rebuild receiver should not receive events");

    const auto secondResult = session.renderNextBlock(transport, secondBlock, project);

    require(secondResult.renderSucceeded, "invalid safe midi rebuild old route render should succeed");
    require(oldReceiver.events().size() == 3, "invalid safe midi rebuild should keep old output route");
    require(oldReceiver.events()[2].event.type == trackloom::MidiPlaybackEventType::NoteOn, "invalid safe midi rebuild should chase through old route");
    require(invalidReceiver.events().empty(), "invalid safe midi rebuild should keep invalid receiver unused");
    require(session.activeMidiNoteCount() == 1, "invalid safe midi rebuild chased note should be active on old route");
}

void projectPlaybackSessionRejectsSafeMidiOutputRebuildBeforePrepare()
{
    trackloom::Project project("Project Playback Session");
    trackloom::ProjectPlaybackSession session;
    RecordingMidiEventReceiver receiver;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);

    const auto rebuildResult = session.rebuildMidiOutputSafely(project, { { track.id, &receiver } }, 0);

    require(!rebuildResult.success, "unprepared safe midi rebuild should fail");
    require(!rebuildResult.midiOutputChanged, "unprepared safe midi rebuild should not change midi output");
    require(session.midiReceiverCount() == 0, "unprepared safe midi rebuild should keep receiver count at zero");
    require(receiver.events().empty(), "unprepared safe midi rebuild should not send midi events");
}

void stopPlaybackCommandStopsAfterMidiRelease()
{
    trackloom::Project project("Playback Control");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    ConstantAudioSource source(0.0f);
    RecordingMidiEventReceiver receiver;
    std::vector<float> samples(2 * 480, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 480);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 1920);

    require(clip.has_value(), "stop command clip should exist");
    require(project.createMidiNote(clip->id, 0, 960, 60, 100, 1).has_value(), "stop command note should exist");
    require(session.prepare(1920.0, 2, 480), "stop command prepare should succeed");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }), "stop command should rebuild audio graph");
    require(session.rebuildMidiOutput(project, { { track.id, &receiver } }), "stop command should rebuild midi output");
    require(transport.seekToSample(960), "stop command transport should seek");
    transport.play();

    const auto renderResult = session.renderNextBlock(transport, block, project);
    require(renderResult.renderSucceeded, "stop command first render should succeed");
    require(session.activeMidiNoteCount() == 1, "stop command should have active note before command");

    trackloom::StopPlaybackCommand command(32);
    const auto result = command.execute(session, transport, project);

    require(command.name() == "StopPlayback", "stop command should expose stable name");
    require(result.success, "stop command should succeed");
    require(result.transportControl.success, "stop command should expose transport control success");
    require(result.transportControl.midiRelease.deliveredEventCount == 1, "stop command should release one note");
    require(!transport.isPlaying(), "stop command should stop transport");
    require(session.activeMidiNoteCount() == 0, "stop command should clear active note");
    require(receiver.events().size() == 2, "stop command receiver should record note on and note off");
    require(receiver.events()[1].event.type == trackloom::MidiPlaybackEventType::NoteOff, "stop command should send note off");
    require(receiver.events()[1].sampleOffset == 32, "stop command should keep release sample offset");
}

void seekPlaybackCommandSeeksAfterMidiReleaseAndRequestsChase()
{
    trackloom::Project project("Playback Control");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    ConstantAudioSource source(0.0f);
    RecordingMidiEventReceiver receiver;
    std::vector<float> firstSamples(2 * 480, 0.0f);
    std::vector<float> secondSamples(2 * 960, 0.0f);
    trackloom::AudioBlock firstBlock(firstSamples.data(), 2, 480);
    trackloom::AudioBlock secondBlock(secondSamples.data(), 2, 960);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 1920);

    require(clip.has_value(), "seek command clip should exist");
    require(project.createMidiNote(clip->id, 0, 960, 60, 100, 1).has_value(), "seek command note should exist");
    require(session.prepare(1920.0, 2, 960), "seek command prepare should succeed");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }), "seek command should rebuild audio graph");
    require(session.rebuildMidiOutput(project, { { track.id, &receiver } }), "seek command should rebuild midi output");
    require(transport.seekToSample(960), "seek command transport should seek to note start");
    transport.play();

    const auto firstResult = session.renderNextBlock(transport, firstBlock, project);
    require(firstResult.renderSucceeded, "seek command first render should succeed");
    require(session.activeMidiNoteCount() == 1, "seek command should have active note before command");

    trackloom::SeekPlaybackCommand command(1200, 20);
    const auto result = command.execute(session, transport, project);

    require(command.name() == "SeekPlayback", "seek command should expose stable name");
    require(result.success, "seek command should succeed");
    require(result.transportControl.success, "seek command should expose transport control success");
    require(result.transportControl.midiRelease.deliveredEventCount == 1, "seek command should release one note");
    require(transport.currentSample() == 1200, "seek command should move transport");
    require(session.activeMidiNoteCount() == 0, "seek command should clear active note before chased block");

    const auto secondResult = session.renderNextBlock(transport, secondBlock, project);

    require(secondResult.renderSucceeded, "seek command chased render should succeed");
    require(receiver.events().size() == 4, "seek command should record original note, release, chase, and real release");
    require(receiver.events()[1].event.type == trackloom::MidiPlaybackEventType::NoteOff, "seek command should release before seek");
    require(receiver.events()[2].event.type == trackloom::MidiPlaybackEventType::NoteOn, "seek command should request chase after seek");
    require(receiver.events()[2].event.absoluteTick == 1200, "seek command chased note should start at target sample tick");
}

void seekPlaybackCommandRejectsNegativeTargetWithoutRelease()
{
    trackloom::Project project("Playback Control");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    ConstantAudioSource source(0.0f);
    RecordingMidiEventReceiver receiver;
    std::vector<float> samples(2 * 480, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 480);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 1920);

    require(clip.has_value(), "negative seek command clip should exist");
    require(project.createMidiNote(clip->id, 0, 960, 60, 100, 1).has_value(), "negative seek command note should exist");
    require(session.prepare(1920.0, 2, 480), "negative seek command prepare should succeed");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }), "negative seek command should rebuild audio graph");
    require(session.rebuildMidiOutput(project, { { track.id, &receiver } }), "negative seek command should rebuild midi output");
    require(transport.seekToSample(960), "negative seek command transport should seek");
    transport.play();

    const auto renderResult = session.renderNextBlock(transport, block, project);
    require(renderResult.renderSucceeded, "negative seek command first render should succeed");
    require(session.activeMidiNoteCount() == 1, "negative seek command should have active note before command");

    trackloom::SeekPlaybackCommand command(-1, 12);
    const auto result = command.execute(session, transport, project);

    require(!result.success, "negative seek command should fail");
    require(!result.transportControl.success, "negative seek command should not call safe seek");
    require(transport.currentSample() == 1440, "negative seek command should not move transport");
    require(session.activeMidiNoteCount() == 1, "negative seek command should not release active note");
    require(receiver.events().size() == 1, "negative seek command should not send release note off");
}

void rebuildMidiOutputCommandSwitchesRoutesSafely()
{
    trackloom::Project project("Playback Control");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    ConstantAudioSource source(0.0f);
    RecordingMidiEventReceiver oldReceiver;
    RecordingMidiEventReceiver newReceiver;
    std::vector<float> firstSamples(2 * 480, 0.0f);
    std::vector<float> secondSamples(2 * 480, 0.0f);
    trackloom::AudioBlock firstBlock(firstSamples.data(), 2, 480);
    trackloom::AudioBlock secondBlock(secondSamples.data(), 2, 480);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 1920);

    require(clip.has_value(), "rebuild command clip should exist");
    require(project.createMidiNote(clip->id, 0, 1920, 60, 100, 1).has_value(), "rebuild command note should exist");
    require(session.prepare(1920.0, 2, 480), "rebuild command prepare should succeed");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }), "rebuild command should rebuild audio graph");
    require(session.rebuildMidiOutput(project, { { track.id, &oldReceiver } }), "rebuild command should bind old receiver");
    require(transport.seekToSample(960), "rebuild command transport should seek");
    transport.play();

    const auto firstResult = session.renderNextBlock(transport, firstBlock, project);
    require(firstResult.renderSucceeded, "rebuild command first render should succeed");
    require(session.activeMidiNoteCount() == 1, "rebuild command should have active note before command");

    trackloom::RebuildMidiOutputCommand command({ { track.id, &newReceiver } }, 24);
    const auto result = command.execute(session, transport, project);

    require(command.name() == "RebuildMidiOutput", "rebuild command should expose stable name");
    require(result.success, "rebuild command should succeed");
    require(result.midiOutputRebuild.success, "rebuild command should expose safe rebuild success");
    require(result.midiOutputRebuild.midiRelease.deliveredEventCount == 1, "rebuild command should release old active note");
    require(oldReceiver.events().size() == 2, "rebuild command old receiver should receive note on and release");
    require(newReceiver.events().empty(), "rebuild command new receiver should not receive old release");

    const auto secondResult = session.renderNextBlock(transport, secondBlock, project);

    require(secondResult.renderSucceeded, "rebuild command chased render should succeed");
    require(newReceiver.events().size() == 1, "rebuild command should route chased note to new receiver");
    require(newReceiver.events()[0].event.type == trackloom::MidiPlaybackEventType::NoteOn, "rebuild command chased event should be note on");
}

void playbackControlCommandsRejectUnpreparedSession()
{
    trackloom::Project project("Playback Control");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    RecordingMidiEventReceiver receiver;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);

    trackloom::StopPlaybackCommand stopCommand(0);
    trackloom::SeekPlaybackCommand seekCommand(960, 0);
    trackloom::RebuildMidiOutputCommand rebuildCommand({ { track.id, &receiver } }, 0);

    const auto stopResult = stopCommand.execute(session, transport, project);
    const auto seekResult = seekCommand.execute(session, transport, project);
    const auto rebuildResult = rebuildCommand.execute(session, transport, project);

    require(!stopResult.success, "unprepared stop command should fail");
    require(!seekResult.success, "unprepared seek command should fail");
    require(!rebuildResult.success, "unprepared rebuild command should fail");
    require(!transport.isPlaying(), "unprepared playback commands should not start transport");
    require(transport.currentSample() == 0, "unprepared playback commands should not move transport");
    require(session.midiReceiverCount() == 0, "unprepared rebuild command should not bind receiver");
    require(receiver.events().empty(), "unprepared rebuild command should not send events");
}

void playbackControlCommandsReportStableFailureReasons()
{
    trackloom::Project project("Playback Control");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    RecordingMidiEventReceiver receiver;
    const auto instrumentTrack = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto audioTrack = project.createTrack("Audio", trackloom::TrackType::Audio);

    trackloom::StopPlaybackCommand unpreparedStop(0);
    trackloom::SeekPlaybackCommand negativeSeek(-1, 0);
    trackloom::RebuildMidiOutputCommand unpreparedRebuild({ { instrumentTrack.id, &receiver } }, 0);

    const auto unpreparedStopResult = unpreparedStop.execute(session, transport, project);
    const auto negativeSeekResult = negativeSeek.execute(session, transport, project);
    const auto unpreparedRebuildResult = unpreparedRebuild.execute(session, transport, project);

    require(unpreparedStopResult.failureReason == trackloom::PlaybackControlFailureReason::SessionNotPrepared,
        "unprepared stop should report stable session-not-prepared reason");
    require(negativeSeekResult.failureReason == trackloom::PlaybackControlFailureReason::InvalidTargetSample,
        "negative seek should report stable invalid-target reason");
    require(unpreparedRebuildResult.failureReason == trackloom::PlaybackControlFailureReason::SessionNotPrepared,
        "unprepared rebuild should report stable session-not-prepared reason");

    require(session.prepare(1920.0, 2, 480), "failure reason session should prepare");
    trackloom::RebuildMidiOutputCommand invalidRebuild({ { audioTrack.id, &receiver } }, 0);
    const auto invalidRebuildResult = invalidRebuild.execute(session, transport, project);

    require(invalidRebuildResult.failureReason == trackloom::PlaybackControlFailureReason::MidiOutputRebuildRejected,
        "invalid rebuild binding should report stable midi output rebuild rejection");
    require(invalidRebuildResult.midiOutputRebuild.failureReason == trackloom::ProjectPlaybackMidiOutputRebuildFailureReason::OutputBindingRejected,
        "invalid rebuild binding should expose lower-level output binding rejection");
}

void stopPlaybackCommandReportsMidiReleaseFailureReason()
{
    trackloom::Project project("Playback Control");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    ConstantAudioSource source(0.0f);
    FailingMidiEventReceiver receiver(2);
    std::vector<float> samples(2 * 480, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 480);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 1920);

    require(clip.has_value(), "release failure command clip should exist");
    require(project.createMidiNote(clip->id, 0, 960, 60, 100, 1).has_value(), "release failure command note should exist");
    require(session.prepare(1920.0, 2, 480), "release failure command prepare should succeed");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }), "release failure command should rebuild audio graph");
    require(session.rebuildMidiOutput(project, { { track.id, &receiver } }), "release failure command should rebuild midi output");
    require(transport.seekToSample(960), "release failure command transport should seek");
    transport.play();

    const auto renderResult = session.renderNextBlock(transport, block, project);
    require(renderResult.renderSucceeded, "release failure command first render should succeed");
    require(session.activeMidiNoteCount() == 1, "release failure command should have active note before stop");

    trackloom::StopPlaybackCommand command(0);
    const auto result = command.execute(session, transport, project);

    require(!result.success, "release failure command should fail");
    require(result.failureReason == trackloom::PlaybackControlFailureReason::MidiReleaseFailed,
        "release failure command should report stable midi release failure");
    require(result.transportControl.failureReason == trackloom::ProjectPlaybackControlFailureReason::MidiReleaseFailed,
        "release failure command should expose lower-level midi release failure");
    require(transport.isPlaying(), "release failure command should keep transport playing");
    require(session.activeMidiNoteCount() == 1, "release failure command should keep active note for retry");
}

void projectPlaybackSessionRejectsPrepareWhileMidiNotesAreActive()
{
    trackloom::Project project("Project Playback Session");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    ConstantAudioSource source(0.0f);
    RecordingMidiEventReceiver receiver;
    std::vector<float> samples(2 * 480, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 480);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 1920);

    require(clip.has_value(), "project playback session active prepare clip should exist");
    require(project.createMidiNote(clip->id, 0, 960, 60, 100, 1).has_value(), "project playback session active prepare note should exist");
    require(session.prepare(1920.0, 2, 480), "project playback session active prepare initial prepare should succeed");
    require(session.rebuildAudioGraph(project, { { track.id, &source } }), "project playback session active prepare should rebuild audio graph");
    require(session.rebuildMidiOutput(project, { { track.id, &receiver } }), "project playback session active prepare should rebuild midi output");
    require(transport.seekToSample(960), "project playback session active prepare transport should seek");
    transport.play();

    const auto blockResult = session.renderNextBlock(transport, block, project);

    require(blockResult.renderSucceeded, "project playback session active prepare render should succeed");
    require(session.activeMidiNoteCount() == 1, "project playback session active prepare should keep long note active");
    require(!session.prepare(48000.0, 2, 512), "project playback session should reject prepare while midi notes are active");
    require(session.isPrepared(), "rejected active prepare should keep previous prepared state");
    require(numbersNear(session.sampleRate(), 1920.0), "rejected active prepare should keep previous sample rate");
    require(session.maxBlockFrames() == 480, "rejected active prepare should keep previous max block frames");
    require(session.activeMidiNoteCount() == 1, "rejected active prepare should keep active midi note");
    require(session.releaseActiveMidiNotes(0).success, "project playback session should still release after rejected active prepare");
}

void projectPlaybackSessionRejectsUseBeforePrepare()
{
    trackloom::Project project("Project Playback Session");
    trackloom::ProjectPlaybackSession session;
    trackloom::Transport transport;
    ConstantAudioSource source(0.50f);
    RecordingMidiEventReceiver receiver;
    std::vector<float> samples(2 * 480, 1.0f);
    trackloom::AudioBlock block(samples.data(), 2, 480);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);

    transport.play();

    require(!session.isPrepared(), "new project playback session should start unprepared");
    require(!session.rebuildAudioGraph(project, { { track.id, &source } }), "unprepared session should reject audio rebuild");
    require(!session.rebuildMidiOutput(project, { { track.id, &receiver } }), "unprepared session should reject midi rebuild");

    const auto blockResult = session.renderNextBlock(transport, block, project);

    require(!blockResult.renderSucceeded, "unprepared session should reject render");
    require(blockResult.midiDispatch.attemptedEventCount == 0, "unprepared render should not dispatch midi");
    require(transport.currentSample() == 0, "unprepared render should not advance transport");
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

void setMidiClipStartKeepingNoteTimesCommandTrimsStartWithUndoAndRedo()
{
    trackloom::Project project("MIDI Clip Start");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Phrase", trackloom::ClipType::Midi, 0, 1920);
    const auto note = project.createMidiNote(clip->id, 480, 480, 64, 100, 1);

    require(clip.has_value() && note.has_value(),
        "MIDI clip start trim command test should create a clip and a note after the trim boundary");

    const auto result = commands.execute(
        project,
        std::make_unique<trackloom::SetMidiClipStartKeepingNoteTimesCommand>(clip->id, 480, 1440));

    require(result.success,
        "MIDI clip start trim command should succeed");
    require(project.findClipById(clip->id)->startTick == 480,
        "MIDI clip start trim command should move the clip start right");
    require(project.findClipById(clip->id)->lengthTick == 1440,
        "MIDI clip start trim command should preserve the old clip end");
    require(project.findMidiNoteById(note->id)->startTick == 0,
        "MIDI clip start trim command should shift the kept note left inside the clip");

    require(commands.undo(project),
        "MIDI clip start trim command undo should be available");
    require(project.findClipById(clip->id)->startTick == 0,
        "undoing MIDI clip start trim should restore the old clip start");
    require(project.findClipById(clip->id)->lengthTick == 1920,
        "undoing MIDI clip start trim should restore the old clip length");
    require(project.findMidiNoteById(note->id)->startTick == 480,
        "undoing MIDI clip start trim should restore the old note start");

    require(commands.redo(project),
        "MIDI clip start trim command redo should be available");
    require(project.findClipById(clip->id)->startTick == 480,
        "redoing MIDI clip start trim should restore the trimmed clip start");
    require(project.findClipById(clip->id)->lengthTick == 1440,
        "redoing MIDI clip start trim should restore the trimmed clip length");
    require(project.findMidiNoteById(note->id)->startTick == 0,
        "redoing MIDI clip start trim should restore the shifted note start");
}

void setMidiClipStartKeepingNoteTimesCommandExtendsStartWithUndoAndRedo()
{
    trackloom::Project project("MIDI Clip Start");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Phrase", trackloom::ClipType::Midi, 480, 1440);
    const auto note = project.createMidiNote(clip->id, 0, 480, 64, 100, 1);

    require(clip.has_value() && note.has_value(),
        "MIDI clip start extend command test should create a clip and a note at the clip start");

    const auto result = commands.execute(
        project,
        std::make_unique<trackloom::SetMidiClipStartKeepingNoteTimesCommand>(clip->id, 0, 1920));

    require(result.success,
        "MIDI clip start extend command should succeed");
    require(project.findClipById(clip->id)->startTick == 0,
        "MIDI clip start extend command should move the clip start left");
    require(project.findClipById(clip->id)->lengthTick == 1920,
        "MIDI clip start extend command should increase the clip length");
    require(project.findMidiNoteById(note->id)->startTick == 480,
        "MIDI clip start extend command should shift the kept note right inside the clip");

    require(commands.undo(project),
        "MIDI clip start extend command undo should be available");
    require(project.findClipById(clip->id)->startTick == 480,
        "undoing MIDI clip start extend should restore the old clip start");
    require(project.findClipById(clip->id)->lengthTick == 1440,
        "undoing MIDI clip start extend should restore the old clip length");
    require(project.findMidiNoteById(note->id)->startTick == 0,
        "undoing MIDI clip start extend should restore the old note start");

    require(commands.redo(project),
        "MIDI clip start extend command redo should be available");
    require(project.findClipById(clip->id)->startTick == 0,
        "redoing MIDI clip start extend should restore the extended clip start");
    require(project.findClipById(clip->id)->lengthTick == 1920,
        "redoing MIDI clip start extend should restore the extended clip length");
    require(project.findMidiNoteById(note->id)->startTick == 480,
        "redoing MIDI clip start extend should restore the shifted note start");
}

void invalidSetMidiClipStartKeepingNoteTimesCommandDoesNotModifyProject()
{
    trackloom::Project project("MIDI Clip Start");
    trackloom::CommandStack commands;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Phrase", trackloom::ClipType::Midi, 0, 1920);
    const auto note = project.createMidiNote(clip->id, 0, 480, 64, 100, 1);

    require(clip.has_value() && note.has_value(),
        "invalid MIDI clip start command test should create a note that crosses the trim boundary");

    const auto result = commands.execute(
        project,
        std::make_unique<trackloom::SetMidiClipStartKeepingNoteTimesCommand>(clip->id, 480, 1440));

    require(!result.success,
        "MIDI clip start command should reject trimming that would drop an existing note");
    require(project.findClipById(clip->id)->startTick == 0,
        "failed MIDI clip start command should keep the old clip start");
    require(project.findClipById(clip->id)->lengthTick == 1920,
        "failed MIDI clip start command should keep the old clip length");
    require(project.findMidiNoteById(note->id)->startTick == 0,
        "failed MIDI clip start command should keep the old note start");
    require(!commands.canUndo(),
        "failed MIDI clip start command should not enter undo history");
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

void saveReportsRetainedHelperRecoveryPathAfterSuccessfulReplacement()
{
    const auto directory = makeTestDirectory("save_retained_helper_recovery");
    const auto targetPath = directory / "song.tlproj";
    injectedRetainedRecoveryPath = directory / "helper-retained-recovery";

    const auto saved = trackloom::detail::saveProjectToFileAtomicallyWithReplaceOperation(
        trackloom::Project("Saved With Helper Recovery"),
        targetPath,
        replaceProjectFileAndRetainRecoveryPath);
    const auto loaded = trackloom::loadProjectFromFile(targetPath);

    require(saved.success, "a replacement that installs the target should report save success");
    require(loaded.project.has_value() && loaded.project->name() == "Saved With Helper Recovery",
        "successful replacement should still save the requested project");
    require(!saved.warning.empty(), "retained helper recovery data should be reported as a warning");
    require(containsRecoveryPath(saved.recoveryPaths, injectedRetainedRecoveryPath),
        "save warning should expose the helper recovery path");
}

void saveReportsWorkspaceCleanupFailureAfterSuccessfulReplacement()
{
    const auto directory = makeTestDirectory("save_workspace_cleanup_warning");
    const auto targetPath = directory / "song.tlproj";
    auto workspacePath = targetPath;
    workspacePath += ".trackloom-save-workspace";
    const auto sentinelPath = workspacePath / "cleanup-sentinel";

    const auto saved = trackloom::detail::saveProjectToFileAtomicallyWithReplaceOperation(
        trackloom::Project("Saved With Workspace Recovery"),
        targetPath,
        replaceProjectFileAndLeaveWorkspaceSentinel);
    const auto loaded = trackloom::loadProjectFromFile(targetPath);

    require(saved.success, "a replacement followed by workspace cleanup failure should still report save success");
    require(loaded.project.has_value() && loaded.project->name() == "Saved With Workspace Recovery",
        "workspace cleanup failure should not roll back the saved target");
    require(!saved.warning.empty(), "workspace cleanup failure should be reported as a warning");
    require(containsRecoveryPath(saved.recoveryPaths, workspacePath),
        "workspace cleanup warning should expose the retained workspace");
    require(std::filesystem::exists(sentinelPath),
        "workspace cleanup failure must preserve the sentinel left after replacement");
}

void saveReportsThrownCleanupAfterSuccessfulReplacementAsWarning()
{
    const auto directory = makeTestDirectory("save_cleanup_exception_warning");
    const auto targetPath = directory / "song.tlproj";
    auto workspacePath = targetPath;
    workspacePath += ".trackloom-save-workspace";
    injectedRetainedRecoveryPath = directory / "helper-retained-recovery";

    const auto oldProjectText = trackloom::saveProjectToText(trackloom::Project("Old Project"));
    writeFileBytes(targetPath, oldProjectText);
    injectedProjectFileReplacementCallCount = 0;

    const auto saved = trackloom::detail::saveProjectToFileAtomicallyWithOperations(
        trackloom::Project("Installed New Project"),
        targetPath,
        writeCanonicalProjectBytes,
        recordAndInstallProjectFileWithRecoveryPath,
        throwDuringWorkspaceCleanup);
    const auto loaded = trackloom::loadProjectFromFile(targetPath);

    require(injectedProjectFileReplacementCallCount == 1,
        "the test replacement must install the new target before cleanup throws");
    require(saved.success,
        "cleanup exceptions after a successful replacement must not report that saving failed");
    require(!saved.warning.empty(),
        "cleanup exceptions after a successful replacement must produce a recovery warning");
    require(containsRecoveryPath(saved.recoveryPaths, workspacePath),
        "cleanup exceptions after replacement must conservatively expose the save workspace");
    require(containsRecoveryPath(saved.recoveryPaths, injectedRetainedRecoveryPath),
        "cleanup exceptions must preserve any recovery path reported by the replacement helper");
    require(loaded.project.has_value() && loaded.project->name() == "Installed New Project",
        "the successfully installed target must remain loadable with the new project");
    require(readFileBytes(targetPath) != oldProjectText,
        "the old target must already have been replaced when cleanup throws");
    require(std::filesystem::exists(workspacePath),
        "the workspace must remain available for inspection after cleanup throws");
}

void saveDoesNotWarnWhenWorkspaceWasRemovedDespiteFalseCleanupResult()
{
    const auto directory = makeTestDirectory("save_workspace_removed_without_result");
    const auto targetPath = directory / "song.tlproj";
    auto workspacePath = targetPath;
    workspacePath += ".trackloom-save-workspace";

    const auto saved = trackloom::detail::saveProjectToFileAtomicallyWithOperations(
        trackloom::Project("Saved Without Workspace Warning"),
        targetPath,
        replaceProjectFileWithoutRecovery,
        removeWorkspaceThenReportNoRemoval);

    require(saved.success, "a target installed before workspace removal should report save success");
    require(saved.warning.empty(), "an already removed workspace should not produce a cleanup warning");
    require(saved.recoveryPaths.empty(), "an already removed workspace should not report recovery paths");
    require(!std::filesystem::exists(workspacePath),
        "the deterministic cleanup operation should remove the workspace before reporting false");
}

void saveRejectsParseableButTruncatedTemporaryProjectBytes()
{
    const auto directory = makeTestDirectory("save_rejects_parseable_truncation");
    const auto targetPath = directory / "song.tlproj";
    auto workspacePath = targetPath;
    workspacePath += ".trackloom-save-workspace";
    const std::string oldTargetBytes { "original\0project\r\nbytes", 23 };
    writeFileBytes(targetPath, oldTargetBytes);

    trackloom::Project project("New Complete Project");
    project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto canonicalText = trackloom::saveProjectToText(project);
    const auto headerEnd = canonicalText.find('\n');
    const auto nameEnd = canonicalText.find('\n', headerEnd + 1);
    require(headerEnd != std::string::npos && nameEnd != std::string::npos,
        "canonical project text should contain header and name lines");
    injectedTemporaryProjectBytes = canonicalText.substr(0, nameEnd + 1);
    require(injectedTemporaryProjectBytes.size() < canonicalText.size(),
        "the truncated project fixture must be shorter than the canonical project text");
    require(trackloom::loadProjectFromText(injectedTemporaryProjectBytes).project.has_value(),
        "the truncated project fixture must independently prove that it remains parseable");
    injectedProjectFileReplacementCallCount = 0;

    const auto saved = trackloom::detail::saveProjectToFileAtomicallyWithOperations(
        project,
        targetPath,
        writeInjectedTemporaryProjectBytes,
        recordAndInstallProjectFile,
        removeWorkspaceThenReportNoRemoval);

    require(!saved.success, "a parseable but truncated temporary project must not be installed");
    require(saved.error == "Temporary project file bytes did not match the serialized project.",
        "the truncated temporary project must fail specifically at byte-integrity verification");
    require(injectedProjectFileReplacementCallCount == 0,
        "byte-integrity failure must be detected before atomic replacement");
    require(readFileBytes(targetPath) == oldTargetBytes,
        "byte-integrity failure must preserve the old target byte for byte");
    require(!std::filesystem::exists(workspacePath),
        "a clean pre-replacement integrity failure should remove the owned save workspace");
}

void saveRejectsReportedTemporaryProjectFlushFailure()
{
    const auto directory = makeTestDirectory("save_rejects_flush_failure");
    const auto targetPath = directory / "song.tlproj";
    auto workspacePath = targetPath;
    workspacePath += ".trackloom-save-workspace";
    const std::string oldTargetBytes { "old\0flush\r\nbytes", 16 };
    writeFileBytes(targetPath, oldTargetBytes);
    injectedProjectFileReplacementCallCount = 0;

    const auto saved = trackloom::detail::saveProjectToFileAtomicallyWithOperations(
        trackloom::Project("Flush Failure"),
        targetPath,
        writeProjectThenReportFlushFailure,
        recordAndInstallProjectFile,
        removeWorkspaceThenReportNoRemoval);

    require(!saved.success, "a reported temporary project flush failure must fail the save");
    require(saved.error.find("flush") != std::string::npos,
        "a temporary project flush failure should retain its diagnostic");
    require(injectedProjectFileReplacementCallCount == 0,
        "a temporary project flush failure must prevent atomic replacement");
    require(readFileBytes(targetPath) == oldTargetBytes,
        "a temporary project flush failure must preserve the old target byte for byte");
    require(!std::filesystem::exists(workspacePath),
        "a clean pre-replacement flush failure should remove the owned save workspace");
}

void saveRejectsReportedTemporaryProjectCloseFailure()
{
    const auto directory = makeTestDirectory("save_rejects_close_failure");
    const auto targetPath = directory / "song.tlproj";
    auto workspacePath = targetPath;
    workspacePath += ".trackloom-save-workspace";
    const std::string oldTargetBytes { "old\0close\r\nbytes", 16 };
    writeFileBytes(targetPath, oldTargetBytes);
    injectedProjectFileReplacementCallCount = 0;

    const auto saved = trackloom::detail::saveProjectToFileAtomicallyWithOperations(
        trackloom::Project("Close Failure"),
        targetPath,
        writeProjectThenReportCloseFailure,
        recordAndInstallProjectFile,
        removeWorkspaceThenReportNoRemoval);

    require(!saved.success, "a reported temporary project close failure must fail the save");
    require(saved.error.find("close") != std::string::npos,
        "a temporary project close failure should retain its diagnostic");
    require(injectedProjectFileReplacementCallCount == 0,
        "a temporary project close failure must prevent atomic replacement");
    require(readFileBytes(targetPath) == oldTargetBytes,
        "a temporary project close failure must preserve the old target byte for byte");
    require(!std::filesystem::exists(workspacePath),
        "a clean pre-replacement close failure should remove the owned save workspace");
}

void saveRejectsNullInjectedFileOperationsWithoutTouchingTarget()
{
    const auto directory = makeTestDirectory("save_rejects_null_operations");
    const std::string oldTargetBytes { "old\0callback\r\nbytes", 19 };

    const auto verifyRejectedCallbacks = [&](const std::string& fileName,
                                             trackloom::detail::TemporaryProjectWriteOperation writeOperation,
                                             trackloom::detail::AtomicFileReplaceOperation replaceOperation,
                                             trackloom::detail::SaveWorkspaceRemoveOperation removeOperation) {
        const auto targetPath = directory / fileName;
        auto workspacePath = targetPath;
        workspacePath += ".trackloom-save-workspace";
        writeFileBytes(targetPath, oldTargetBytes);

        const auto saved = trackloom::detail::saveProjectToFileAtomicallyWithOperations(
            trackloom::Project("Rejected Callback"),
            targetPath,
            writeOperation,
            replaceOperation,
            removeOperation);

        require(!saved.success, "null injected project-file operations must fail safely");
        require(readFileBytes(targetPath) == oldTargetBytes,
            "null injected project-file operations must preserve the old target byte for byte");
        require(!std::filesystem::exists(workspacePath),
            "null injected project-file operations must be rejected before creating a workspace");
    };

    verifyRejectedCallbacks(
        "null-writer.tlproj",
        nullptr,
        recordAndInstallProjectFile,
        removeWorkspaceThenReportNoRemoval);
    verifyRejectedCallbacks(
        "null-replacer.tlproj",
        writeCanonicalProjectBytes,
        nullptr,
        removeWorkspaceThenReportNoRemoval);
    verifyRejectedCallbacks(
        "null-remover.tlproj",
        writeCanonicalProjectBytes,
        recordAndInstallProjectFile,
        nullptr);
}

void atomicFileReplaceReplacesExistingTarget()
{
    const auto directory = makeTestDirectory("atomic_replace_existing");
    const auto replacementPath = directory / "song.tlproj.tmp";
    const auto targetPath = directory / "song.tlproj";
    writeFileBytes(targetPath, "old project bytes\n");
    writeFileBytes(replacementPath, "new project bytes\n");

    const auto result = trackloom::replaceFileAtomically(replacementPath, targetPath);

    require(result.success, "atomic replacement should replace an existing target");
    require(readFileBytes(targetPath) == "new project bytes\n", "target should contain replacement bytes");
    require(!std::filesystem::exists(replacementPath), "replacement path should be consumed");
}

void atomicFileReplaceInstallsWhenTargetIsMissing()
{
    const auto directory = makeTestDirectory("atomic_install_missing_target");
    const auto replacementPath = directory / "song.tlproj.tmp";
    const auto targetPath = directory / "song.tlproj";
    writeFileBytes(replacementPath, "new project bytes\n");

    const auto result = trackloom::replaceFileAtomically(replacementPath, targetPath);

    require(result.success, "atomic replacement should install a missing target");
    require(readFileBytes(targetPath) == "new project bytes\n", "installed target should contain replacement bytes");
    require(!std::filesystem::exists(replacementPath), "installed replacement path should be consumed");
}

void atomicFileReplaceMissingReplacementPreservesExistingTarget()
{
    const auto directory = makeTestDirectory("atomic_missing_replacement");
    const auto replacementPath = directory / "missing.tlproj.tmp";
    const auto targetPath = directory / "song.tlproj";
    const std::string originalBytes { "existing\0project\r\nbytes", 23 };
    writeFileBytes(targetPath, originalBytes);

    const auto result = trackloom::replaceFileAtomically(replacementPath, targetPath);

    require(!result.success, "missing replacement should fail");
    require(!result.error.empty(), "missing replacement should report an error");
    require(readFileBytes(targetPath) == originalBytes, "failed replacement should preserve target bytes exactly");
}

#ifdef _WIN32

void atomicFileReplaceReportsReplacementQueryFailureWithoutTouchingTarget()
{
    const std::filesystem::path replacementPath = "C:/fake/song.trackloom.tmp";
    const std::filesystem::path targetPath = "C:/fake/song.trackloom";
    ScriptedWindowsAtomicFileOperations operations;
    operations.files[targetPath] = "old project bytes";
    operations.files[replacementPath] = "new project bytes";
    operations.queryResults[replacementPath].push_back({
        false,
        false,
        trackloom::detail::windowsErrorAccessDenied
    });

    const auto result = trackloom::detail::replaceFileAtomicallyWithWindowsOperations(
        replacementPath,
        targetPath,
        operations);

    require(!result.success, "replacement query failure should fail");
    require(result.targetAvailability == trackloom::AtomicFileTargetAvailability::Unknown,
        "target availability should remain unknown when replacement cannot be queried");
    require(result.recoveryPath == replacementPath, "query failure should retain the potential replacement path");
    require(operations.files.at(targetPath) == "old project bytes", "replacement query failure should preserve target");
    require(operations.files.at(replacementPath) == "new project bytes", "replacement query failure should preserve replacement");
    require(operations.replaceCalls.empty() && operations.moveCalls.empty(), "query failure should not mutate files");
}

void atomicFileReplaceReportsTargetQueryFailureWithoutTouchingFiles()
{
    const std::filesystem::path replacementPath = "C:/fake/song.trackloom.tmp";
    const std::filesystem::path targetPath = "C:/fake/song.trackloom";
    ScriptedWindowsAtomicFileOperations operations;
    operations.files[targetPath] = "old project bytes";
    operations.files[replacementPath] = "new project bytes";
    operations.queryResults[targetPath].push_back({
        false,
        false,
        trackloom::detail::windowsErrorAccessDenied
    });

    const auto result = trackloom::detail::replaceFileAtomicallyWithWindowsOperations(
        replacementPath,
        targetPath,
        operations);

    require(!result.success, "target query failure should fail");
    require(result.targetAvailability == trackloom::AtomicFileTargetAvailability::Unknown,
        "failed target query should report unknown availability");
    require(result.recoveryPath == replacementPath, "target query failure should identify replacement recovery");
    require(operations.files.at(targetPath) == "old project bytes", "target query failure should preserve target");
    require(operations.files.at(replacementPath) == "new project bytes", "target query failure should preserve replacement");
    require(operations.replaceCalls.empty() && operations.moveCalls.empty(), "target query failure should not mutate files");
}

void atomicFileReplaceDoesNotOverwriteTargetThatAppearsBeforeMove()
{
    const std::filesystem::path replacementPath = "C:/fake/song.trackloom.tmp";
    const std::filesystem::path targetPath = "C:/fake/song.trackloom";
    ScriptedWindowsAtomicFileOperations operations;
    operations.files[replacementPath] = "new project bytes";
    operations.moveScripts.push_back([](auto& backend, const auto&, const auto& target, std::uint32_t) {
        backend.files[target] = "external project bytes";
        return ScriptedWindowsAtomicFileOperations::OperationResult {
            false,
            trackloom::detail::windowsErrorAlreadyExists
        };
    });

    const auto result = trackloom::detail::replaceFileAtomicallyWithWindowsOperations(
        replacementPath,
        targetPath,
        operations);

    require(!result.success, "target appearance should fail instead of overwriting it");
    require(result.targetAvailability == trackloom::AtomicFileTargetAvailability::Available,
        "appeared target should be reported available");
    require(result.recoveryPath == replacementPath, "appeared target should leave replacement as recovery");
    require(operations.files.at(targetPath) == "external project bytes", "appeared target should not be overwritten");
    require(operations.files.at(replacementPath) == "new project bytes", "failed move should preserve replacement");
    require(operations.replaceCalls.empty(), "missing-target branch must not replace a newly appeared target");
}

void atomicFileReplacePreservesReplacementWhenTargetDisappearsBeforeReplace()
{
    const std::filesystem::path replacementPath = "C:/fake/song.trackloom.tmp";
    const std::filesystem::path targetPath = "C:/fake/song.trackloom";
    ScriptedWindowsAtomicFileOperations operations;
    operations.files[targetPath] = "old project bytes";
    operations.files[replacementPath] = "new project bytes";
    operations.replaceScripts.push_back([](auto& backend, const auto& target, const auto&, const auto&, std::uint32_t) {
        backend.files.erase(target);
        return ScriptedWindowsAtomicFileOperations::OperationResult {
            false,
            trackloom::detail::windowsErrorFileNotFound
        };
    });

    const auto result = trackloom::detail::replaceFileAtomicallyWithWindowsOperations(
        replacementPath,
        targetPath,
        operations);

    require(!result.success, "disappeared target should fail without changing operation type");
    require(result.targetAvailability == trackloom::AtomicFileTargetAvailability::Missing,
        "disappeared target should be reported missing");
    require(result.recoveryPath == replacementPath, "disappeared target should preserve replacement recovery");
    require(operations.files.at(replacementPath) == "new project bytes", "replacement should survive target race");
    require(operations.moveCalls.empty(), "target disappearance must not silently install replacement");
}

void atomicFileReplaceError1175PreservesBothOriginalFiles()
{
    const std::filesystem::path replacementPath = "C:/fake/song.trackloom.tmp";
    const std::filesystem::path targetPath = "C:/fake/song.trackloom";
    ScriptedWindowsAtomicFileOperations operations;
    operations.files[targetPath] = "old project bytes";
    operations.files[replacementPath] = "new project bytes";
    operations.replaceScripts.push_back([](auto&, const auto&, const auto&, const auto&, std::uint32_t) {
        return ScriptedWindowsAtomicFileOperations::OperationResult {
            false,
            trackloom::detail::windowsErrorUnableToRemoveReplaced
        };
    });

    const auto result = trackloom::detail::replaceFileAtomicallyWithWindowsOperations(
        replacementPath,
        targetPath,
        operations);

    require(!result.success, "1175 should remain a reported failure");
    require(result.targetAvailability == trackloom::AtomicFileTargetAvailability::Available,
        "1175 should report target available");
    require(result.recoveryPath == replacementPath, "1175 should identify replacement recovery");
    require(operations.files.at(targetPath) == "old project bytes", "1175 should preserve old target");
    require(operations.files.at(replacementPath) == "new project bytes", "1175 should preserve replacement");
    require(operations.moveCalls.empty(), "1175 should not enter recovery moves");
}

void atomicFileReplaceError1176PreservesOriginalNamesWithBackup()
{
    const std::filesystem::path replacementPath = "C:/fake/song.trackloom.tmp";
    const std::filesystem::path targetPath = "C:/fake/song.trackloom";
    ScriptedWindowsAtomicFileOperations operations;
    operations.files[targetPath] = "old project bytes";
    operations.files[replacementPath] = "new project bytes";
    operations.replaceScripts.push_back([](
                                            auto&,
                                            const auto&,
                                            const auto&,
                                            const auto&,
                                            std::uint32_t) {
        return ScriptedWindowsAtomicFileOperations::OperationResult {
            false,
            trackloom::detail::windowsErrorUnableToMoveReplacement
        };
    });

    const auto result = trackloom::detail::replaceFileAtomicallyWithWindowsOperations(
        replacementPath,
        targetPath,
        operations);

    require(!result.success, "1176 should remain a reported failure");
    require(result.targetAvailability == trackloom::AtomicFileTargetAvailability::Available,
        "1176 with backup should report the original target as available");
    require(result.recoveryPath == replacementPath, "1176 should identify the preserved replacement");
    require(operations.files.at(targetPath) == "old project bytes", "1176 should preserve the old target bytes");
    require(operations.files.at(replacementPath) == "new project bytes", "1176 should preserve replacement bytes");
    require(operations.replaceCalls.size() == 1, "1176 should make one ReplaceFileW attempt");
    require(operations.replaceCalls.front().flags == 0, "ReplaceFileW should use supported flags only");
    require(!operations.files.contains(operations.replaceCalls.front().backupPath),
        "1176 should not invent a backup that the API did not create");
}

void atomicFileReplaceError1177InstallsReplacementAndRemovesBackup()
{
    const std::filesystem::path replacementPath = "C:/fake/song.trackloom.tmp";
    const std::filesystem::path targetPath = "C:/fake/song.trackloom";
    ScriptedWindowsAtomicFileOperations operations;
    operations.files[targetPath] = "old project bytes";
    operations.files[replacementPath] = "new project bytes";
    operations.replaceScripts.push_back([](auto& backend, const auto& target, const auto&, const auto& backup, std::uint32_t) {
        backend.files.emplace(backup, backend.files.at(target));
        backend.files.erase(target);
        return ScriptedWindowsAtomicFileOperations::OperationResult {
            false,
            trackloom::detail::windowsErrorUnableToMoveReplacement2
        };
    });

    const auto result = trackloom::detail::replaceFileAtomicallyWithWindowsOperations(
        replacementPath,
        targetPath,
        operations);

    const auto backupPath = operations.replaceCalls.front().backupPath;
    require(result.success, "1177 should recover by installing the replacement when target remains absent");
    require(result.targetAvailability == trackloom::AtomicFileTargetAvailability::Available,
        "recovered 1177 should report target available");
    require(operations.files.at(targetPath) == "new project bytes", "1177 recovery should install new bytes");
    require(!operations.files.contains(replacementPath), "installed replacement should be consumed");
    require(!operations.files.contains(backupPath), "successful recovery should best-effort remove backup");
    require(operations.moveCalls.size() == 1, "1177 install recovery should need one move");
    require(operations.moveCalls.front().flags == trackloom::detail::windowsMoveFileWriteThrough,
        "1177 install recovery should use write-through move");
}

void atomicFileReplaceError1177RestoresTargetWhenInstallFails()
{
    const std::filesystem::path replacementPath = "C:/fake/song.trackloom.tmp";
    const std::filesystem::path targetPath = "C:/fake/song.trackloom";
    ScriptedWindowsAtomicFileOperations operations;
    operations.files[targetPath] = "old project bytes";
    operations.files[replacementPath] = "new project bytes";
    operations.replaceScripts.push_back([](auto& backend, const auto& target, const auto&, const auto& backup, std::uint32_t) {
        backend.files.emplace(backup, backend.files.at(target));
        backend.files.erase(target);
        return ScriptedWindowsAtomicFileOperations::OperationResult {
            false,
            trackloom::detail::windowsErrorUnableToMoveReplacement2
        };
    });
    operations.moveScripts.push_back([](auto&, const auto&, const auto&, std::uint32_t) {
        return ScriptedWindowsAtomicFileOperations::OperationResult {
            false,
            trackloom::detail::windowsErrorAccessDenied
        };
    });

    const auto result = trackloom::detail::replaceFileAtomicallyWithWindowsOperations(
        replacementPath,
        targetPath,
        operations);

    const auto backupPath = operations.replaceCalls.front().backupPath;
    require(!result.success, "1177 restored old target should still report new save failure");
    require(result.targetAvailability == trackloom::AtomicFileTargetAvailability::Available,
        "restored 1177 target should be reported available");
    require(result.recoveryPath == replacementPath, "restored 1177 should identify preserved new replacement");
    require(operations.files.at(targetPath) == "old project bytes", "1177 should restore old target bytes");
    require(operations.files.at(replacementPath) == "new project bytes", "failed install should preserve new replacement");
    require(!operations.files.contains(backupPath), "restored backup should be consumed");
    require(operations.moveCalls.size() == 2, "1177 should attempt install then restore");
}

void atomicFileReplaceError1177RestoresBackupIfReplacementDisappears()
{
    const std::filesystem::path replacementPath = "C:/fake/song.trackloom.tmp";
    const std::filesystem::path targetPath = "C:/fake/song.trackloom";
    ScriptedWindowsAtomicFileOperations operations;
    operations.files[targetPath] = "old project bytes";
    operations.files[replacementPath] = "new project bytes";
    operations.replaceScripts.push_back([](auto& backend, const auto& target, const auto&, const auto& backup, std::uint32_t) {
        backend.files.emplace(backup, backend.files.at(target));
        backend.files.erase(target);
        return ScriptedWindowsAtomicFileOperations::OperationResult {
            false,
            trackloom::detail::windowsErrorUnableToMoveReplacement2
        };
    });
    operations.moveScripts.push_back([](auto& backend, const auto& source, const auto&, std::uint32_t) {
        backend.files.erase(source);
        return ScriptedWindowsAtomicFileOperations::OperationResult {
            false,
            trackloom::detail::windowsErrorFileNotFound
        };
    });

    const auto result = trackloom::detail::replaceFileAtomicallyWithWindowsOperations(
        replacementPath,
        targetPath,
        operations);

    require(!result.success, "externally disappeared replacement should fail the save");
    require(result.targetAvailability == trackloom::AtomicFileTargetAvailability::Available,
        "backup restoration should leave target available");
    require(result.recoveryPath == targetPath, "restored target should be the remaining recovery copy");
    require(operations.files.at(targetPath) == "old project bytes", "old target should be restored");
    require(!operations.files.contains(replacementPath), "result should not report a replacement that disappeared");
}

void atomicFileReplaceError1177PreservesBothCopiesWhenRecoveryFails()
{
    const std::filesystem::path replacementPath = "C:/fake/song.trackloom.tmp";
    const std::filesystem::path targetPath = "C:/fake/song.trackloom";
    ScriptedWindowsAtomicFileOperations operations;
    operations.files[targetPath] = "old project bytes";
    operations.files[replacementPath] = "new project bytes";
    operations.replaceScripts.push_back([](
                                            auto& backend,
                                            const auto& target,
                                            const auto&,
                                            const auto& backup,
                                            std::uint32_t) {
        backend.files.emplace(backup, backend.files.at(target));
        backend.files.erase(target);
        return ScriptedWindowsAtomicFileOperations::OperationResult {
            false,
            trackloom::detail::windowsErrorUnableToMoveReplacement2
        };
    });
    for (int attempt = 0; attempt < 2; ++attempt) {
        operations.moveScripts.push_back([](auto&, const auto&, const auto&, std::uint32_t) {
            return ScriptedWindowsAtomicFileOperations::OperationResult {
                false,
                trackloom::detail::windowsErrorAccessDenied
            };
        });
    }

    const auto result = trackloom::detail::replaceFileAtomicallyWithWindowsOperations(
        replacementPath,
        targetPath,
        operations);

    const auto backupPath = operations.replaceCalls.front().backupPath;
    require(!result.success, "1177 should fail when install and restore both fail");
    require(result.targetAvailability == trackloom::AtomicFileTargetAvailability::Missing,
        "1177 should report the target missing after both recovery moves fail");
    require(result.recoveryPath == backupPath,
        "1177 should report the unpredictable backup path when target restoration fails");
    require(operations.files.at(replacementPath) == "new project bytes", "1177 should preserve new bytes");
    require(operations.files.at(backupPath) == "old project bytes", "1177 should preserve old bytes in backup");
    require(!operations.files.contains(targetPath), "failed recovery should not fabricate a target");
    require(operations.moveCalls.size() == 2, "1177 should try install then restore exactly once each");
}

void atomicFileReplacePreservesPreexistingRecoveryDirectory()
{
    const std::filesystem::path replacementPath = "C:/fake/song.trackloom.tmp";
    const std::filesystem::path targetPath = "C:/fake/song.trackloom";
    auto recoveryDirectory = targetPath;
    recoveryDirectory += ".trackloom-recovery";
    const auto sentinelPath = recoveryDirectory / "SENTINEL";
    ScriptedWindowsAtomicFileOperations operations;
    operations.files[targetPath] = "old project bytes";
    operations.files[replacementPath] = "new project bytes";
    operations.directories.insert(recoveryDirectory);
    operations.files[sentinelPath] = "sentinel bytes";

    const auto result = trackloom::detail::replaceFileAtomicallyWithWindowsOperations(
        replacementPath,
        targetPath,
        operations);

    require(!result.success, "preexisting recovery directory should block replacement");
    require(result.recoveryPath == recoveryDirectory, "failure should report the recovery directory for inspection");
    require(operations.files.at(targetPath) == "old project bytes", "blocked replacement should preserve target");
    require(operations.files.at(replacementPath) == "new project bytes", "blocked replacement should preserve replacement");
    require(operations.files.at(sentinelPath) == "sentinel bytes", "helper must not overwrite recovery sentinel");
    require(operations.replaceCalls.empty() && operations.moveCalls.empty(),
        "helper must not enter ReplaceFileW or MoveFileExW without recovery ownership");
    require(operations.removeDirectoryCalls.empty(), "helper must not remove a directory it did not create");
}

void atomicFileReplaceReportsRecoveryDirectoryCreationError()
{
    const std::filesystem::path replacementPath = "C:/fake/song.trackloom.tmp";
    const std::filesystem::path targetPath = "C:/fake/song.trackloom";
    auto recoveryDirectory = targetPath;
    recoveryDirectory += ".trackloom-recovery";
    ScriptedWindowsAtomicFileOperations operations;
    operations.files[targetPath] = "old project bytes";
    operations.files[replacementPath] = "new project bytes";
    operations.createDirectoryScripts.push_back([](auto&, const auto&) {
        return ScriptedWindowsAtomicFileOperations::OperationResult {
            false,
            trackloom::detail::windowsErrorAccessDenied
        };
    });

    const auto result = trackloom::detail::replaceFileAtomicallyWithWindowsOperations(
        replacementPath,
        targetPath,
        operations);

    require(!result.success, "recovery directory creation error should fail");
    require(result.recoveryPath == recoveryDirectory, "creation error should report the possible recovery directory");
    require(operations.files.at(targetPath) == "old project bytes", "creation error should preserve target");
    require(operations.files.at(replacementPath) == "new project bytes", "creation error should preserve replacement");
    require(operations.replaceCalls.empty() && operations.moveCalls.empty(),
        "creation error should prevent all replacement operations");
}

void atomicFileReplaceReportsRecoveryDirectoryCleanupFailure()
{
    const std::filesystem::path replacementPath = "C:/fake/song.trackloom.tmp";
    const std::filesystem::path targetPath = "C:/fake/song.trackloom";
    auto recoveryDirectory = targetPath;
    recoveryDirectory += ".trackloom-recovery";
    ScriptedWindowsAtomicFileOperations operations;
    operations.files[targetPath] = "old project bytes";
    operations.files[replacementPath] = "new project bytes";
    operations.removeDirectoryScripts.push_back([](auto&, const auto&) {
        return ScriptedWindowsAtomicFileOperations::OperationResult {
            false,
            trackloom::detail::windowsErrorAccessDenied
        };
    });

    const auto result = trackloom::detail::replaceFileAtomicallyWithWindowsOperations(
        replacementPath,
        targetPath,
        operations);

    require(result.success, "empty recovery directory cleanup failure should not undo successful replacement");
    require(result.recoveryPath == recoveryDirectory, "cleanup failure should report the directory requiring inspection");
    require(operations.files.at(targetPath) == "new project bytes", "cleanup failure should retain new target");
    require(operations.directories.contains(recoveryDirectory), "failed cleanup should leave owned recovery directory");
}

void atomicFileReplaceBackupCleanupFailureKeepsSuccessfulTarget()
{
    const std::filesystem::path replacementPath = "C:/fake/song.trackloom.tmp";
    const std::filesystem::path targetPath = "C:/fake/song.trackloom";
    ScriptedWindowsAtomicFileOperations operations;
    operations.files[targetPath] = "old project bytes";
    operations.files[replacementPath] = "new project bytes";
    operations.removeScripts.push_back([](auto&, const auto&) {
        return ScriptedWindowsAtomicFileOperations::OperationResult {
            false,
            trackloom::detail::windowsErrorAccessDenied
        };
    });

    const auto result = trackloom::detail::replaceFileAtomicallyWithWindowsOperations(
        replacementPath,
        targetPath,
        operations);

    const auto backupPath = operations.replaceCalls.front().backupPath;
    require(result.success, "backup cleanup failure must not reverse a successful replacement");
    require(result.targetAvailability == trackloom::AtomicFileTargetAvailability::Available,
        "successful replacement should report target available");
    require(result.recoveryPath == backupPath, "leftover backup should be exposed to the caller");
    require(operations.files.at(targetPath) == "new project bytes", "cleanup failure should keep new target");
    require(operations.files.at(backupPath) == "old project bytes", "failed cleanup should leave old backup intact");
}

void atomicFileReplaceSupportsChineseWindowsPaths()
{
    const auto directory = makeTestDirectory("atomic_chinese_paths") / L"中文目录";
    std::filesystem::create_directories(directory);

    const auto existingTarget = directory / L"已有工程.trackloom";
    const auto existingReplacement = directory / L"替换内容.tmp";
    writeFileBytes(existingTarget, "old project bytes");
    writeFileBytes(existingReplacement, "new project bytes");
    const auto replaced = trackloom::replaceFileAtomically(existingReplacement, existingTarget);

    require(replaced.success, "existing Chinese target path should be replaceable");
    require(readFileBytes(existingTarget) == "new project bytes", "Chinese target should contain replacement bytes");
    require(!std::filesystem::exists(existingReplacement), "Chinese replacement path should be consumed");

    const auto missingTarget = directory / L"新建工程.trackloom";
    const auto missingReplacement = directory / L"安装内容.tmp";
    writeFileBytes(missingReplacement, "installed project bytes");
    const auto installed = trackloom::replaceFileAtomically(missingReplacement, missingTarget);

    require(installed.success, "missing Chinese target path should be installable");
    require(readFileBytes(missingTarget) == "installed project bytes", "installed Chinese target should keep bytes");
    require(!std::filesystem::exists(missingReplacement), "installed Chinese replacement should be consumed");
}

void savePreservesTemporaryRecoveryAfterRealWindowsReplaceFailure()
{
    const auto directory = makeTestDirectory("preserve_temporary_after_replace_failure");
    const auto targetPath = directory / "song.trackloom";
    auto workspacePath = targetPath;
    workspacePath += ".trackloom-save-workspace";
    const auto temporaryPath = workspacePath / "replacement.tlproj";

    require(trackloom::saveProjectToFileAtomically(trackloom::Project("Old Song"), targetPath).success,
        "test setup should create the old target");

    const auto targetHandle = CreateFileW(
        targetPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    require(targetHandle != INVALID_HANDLE_VALUE, "test should lock target without delete sharing");

    const auto result = trackloom::saveProjectToFileAtomically(trackloom::Project("New Song"), targetPath);
    CloseHandle(targetHandle);

    const auto oldTarget = trackloom::loadProjectFromFile(targetPath);
    const auto newRecovery = trackloom::loadProjectFromFile(temporaryPath);
    require(!result.success, "locked target should make atomic replacement fail");
    require(oldTarget.project.has_value() && oldTarget.project->name() == "Old Song",
        "failed replacement should preserve the old target project");
    require(newRecovery.project.has_value() && newRecovery.project->name() == "New Song",
        "ProjectFile should preserve the validated temporary replacement");
    require(result.error.find(".trackloom-save-workspace") != std::string::npos,
        "ProjectFile should report the preserved save workspace");
}

#endif

void saveRefusesToOverwriteExistingTemporaryRecoveryFile()
{
    const auto directory = makeTestDirectory("preserve_existing_temporary_file");
    const auto targetPath = directory / "song.trackloom";
    auto temporaryPath = targetPath;
    temporaryPath += ".tmp";
    const std::string recoveryBytes { "recovery\0bytes\r\n", 16 };
    writeFileBytes(temporaryPath, recoveryBytes);

    const auto result = trackloom::saveProjectToFileAtomically(trackloom::Project("New Song"), targetPath);

    require(!result.success, "save should fail when its fixed temporary path already exists");
    require(readFileBytes(temporaryPath) == recoveryBytes, "save should preserve an existing temporary file byte for byte");
    require(!std::filesystem::exists(targetPath), "refusing the save should not create the target");
    require(result.error.find(".tmp") != std::string::npos, "save failure should report the recovery path");
}

void savePreservesPreexistingWorkspaceAndCompetingTemporaryFile()
{
    const auto directory = makeTestDirectory("preserve_preexisting_save_workspace");
    const auto targetPath = directory / "song.trackloom";
    auto workspacePath = targetPath;
    workspacePath += ".trackloom-save-workspace";
    const auto competingTemporaryPath = workspacePath / "replacement.tlproj";
    const auto sentinelPath = workspacePath / "SENTINEL";
    std::filesystem::create_directory(workspacePath);
    const std::string competingBytes { "competing\0recovery\r\n", 20 };
    writeFileBytes(competingTemporaryPath, competingBytes);
    writeFileBytes(sentinelPath, "sentinel bytes");

    const auto result = trackloom::saveProjectToFileAtomically(trackloom::Project("New Song"), targetPath);

    require(!result.success, "preexisting save workspace should block a competing save");
    require(readFileBytes(competingTemporaryPath) == competingBytes,
        "save must not truncate or delete a competing workspace replacement");
    require(readFileBytes(sentinelPath) == "sentinel bytes", "save must not delete competing workspace contents");
    require(!std::filesystem::exists(targetPath), "blocked save should not create the target");
    require(result.error.find(".trackloom-save-workspace") != std::string::npos,
        "blocked save should report the workspace requiring inspection");
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

void audioEngineCollectsMidiEventsForRenderedBlock()
{
    trackloom::Project project("Engine MIDI");
    trackloom::AudioEngine engine;
    trackloom::Transport transport;
    ConstantAudioSource source(0.25f);
    std::vector<float> samples(2 * 480, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 480);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 960, 960);

    require(clip.has_value(), "engine midi clip should exist before render");
    require(project.createMidiNote(clip->id, 0, 240, 60, 100, 1).has_value(), "engine midi note should exist before render");
    require(engine.prepare(1920.0, 2, 480), "engine prepare should set test sample rate");
    require(transport.setSampleRate(960.0), "transport starts with a different sample rate");
    require(transport.seekToSample(960), "transport should seek to the note-on block");
    transport.play();

    trackloom::AudioEngineRenderResult result;
    require(engine.renderNextBlockWithMidi(transport, block, &source, project, result), "midi-aware engine render should succeed");

    require(allSamplesNear(samples, 0.25f), "midi-aware render should still render the audio source");
    require(transport.sampleRate() == 1920.0, "engine render should align transport sample rate before midi collection");
    require(transport.currentSample() == 1440, "midi-aware render should advance transport after collection");
    require(result.midiEvents.size() == 2, "engine render should collect note on and note off");
    require(result.midiEvents[0].type == trackloom::MidiPlaybackEventType::NoteOn, "first engine midi event should be note on");
    require(result.midiEvents[0].absoluteTick == 960, "engine midi note on should use pre-advance block start");
    require(result.midiEvents[1].type == trackloom::MidiPlaybackEventType::NoteOff, "second engine midi event should be note off");
    require(result.midiEvents[1].absoluteTick == 1200, "engine midi note off should occur inside the block");
}

void audioEngineClearsMidiEventsWhenStopped()
{
    trackloom::Project project("Engine MIDI");
    trackloom::AudioEngine engine;
    trackloom::Transport transport;
    std::vector<float> samples(2 * 480, 1.0f);
    trackloom::AudioBlock block(samples.data(), 2, 480);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Lead Phrase", trackloom::ClipType::Midi, 0, 960);

    require(clip.has_value(), "stopped engine midi clip should exist");
    require(project.createMidiNote(clip->id, 0, 240, 60, 100, 1).has_value(), "stopped engine midi note should exist");
    require(engine.prepare(1920.0, 2, 480), "engine prepare should succeed for stopped midi render");

    trackloom::AudioEngineRenderResult result;
    result.midiEvents.push_back(trackloom::MidiPlaybackEvent {
        trackloom::MidiPlaybackEventType::NoteOn,
        "old-track",
        "old-clip",
        "old-note",
        0,
        60,
        100,
        1
    });

    require(engine.renderNextBlockWithMidi(transport, block, project, result), "stopped midi-aware render should still succeed");

    require(result.midiEvents.empty(), "stopped midi-aware render should clear stale midi events");
    require(allSamplesNear(samples, 0.0f), "stopped midi-aware render should clear audio output");
    require(transport.currentSample() == 0, "stopped midi-aware render should not advance transport");
}

void audioEngineClearsMidiEventsWhenRenderRequestFails()
{
    trackloom::Project project("Engine MIDI");
    trackloom::AudioEngine engine;
    trackloom::Transport transport;
    std::vector<float> samples(2 * 4, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 4);

    require(transport.seekToSample(128), "transport should seek before failed midi render");
    transport.play();

    trackloom::AudioEngineRenderResult result;
    result.midiEvents.push_back(trackloom::MidiPlaybackEvent {
        trackloom::MidiPlaybackEventType::NoteOn,
        "old-track",
        "old-clip",
        "old-note",
        0,
        60,
        100,
        1
    });

    require(!engine.renderNextBlockWithMidi(transport, block, project, result), "unprepared midi-aware render should fail");

    require(result.midiEvents.empty(), "failed midi-aware render should clear stale midi events");
    require(transport.currentSample() == 128, "failed midi-aware render should not advance transport");
}

void audioEngineCollectsMidiEventsBeforeAdvancingTransport()
{
    trackloom::Project project("Engine MIDI");
    trackloom::AudioEngine engine;
    trackloom::Transport transport;
    std::vector<float> samples(2 * 480, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 480);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Boundary Phrase", trackloom::ClipType::Midi, 480, 960);

    require(clip.has_value(), "boundary engine midi clip should exist");
    require(project.createMidiNote(clip->id, 0, 120, 64, 90, 1).has_value(), "boundary engine midi note should exist");
    require(engine.prepare(1920.0, 2, 480), "engine prepare should succeed for boundary midi render");
    transport.play();

    trackloom::AudioEngineRenderResult result;
    require(engine.renderNextBlockWithMidi(transport, block, project, result), "first boundary render should succeed");

    require(result.midiEvents.empty(), "first block should exclude midi event at the right boundary");
    require(transport.currentSample() == 480, "first boundary render should advance after collection");

    require(engine.renderNextBlockWithMidi(transport, block, project, result), "second boundary render should succeed");

    require(result.midiEvents.size() == 2, "second block should collect boundary note on and note off");
    require(result.midiEvents[0].absoluteTick == 480, "second block should include event at left boundary");
    require(result.midiEvents[1].absoluteTick == 600, "second block should include note off inside the block");
    require(transport.currentSample() == 960, "second boundary render should advance after collection");
}

void audioEngineExposesScheduledMidiEventsForRenderedBlock()
{
    trackloom::Project project("Engine MIDI");
    trackloom::AudioEngine engine;
    trackloom::Transport transport;
    std::vector<float> samples(2 * 480, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 480);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Scheduled Phrase", trackloom::ClipType::Midi, 960, 960);

    require(clip.has_value(), "engine scheduled clip should exist");
    require(project.createMidiNote(clip->id, 0, 240, 60, 100, 1).has_value(), "engine scheduled note should exist");
    require(engine.prepare(1920.0, 2, 480), "engine prepare should succeed for scheduled midi render");
    require(transport.seekToSample(960), "engine scheduled transport should seek to block start");
    transport.play();

    trackloom::AudioEngineRenderResult result;
    require(engine.renderNextBlockWithMidi(transport, block, project, result), "engine scheduled midi render should succeed");

    require(result.midiEvents.size() == 2, "engine should still expose raw midi events");
    require(result.scheduledMidiEvents.size() == 2, "engine should expose scheduled midi events");
    require(result.scheduledMidiEvents[0].event == result.midiEvents[0], "scheduled event should wrap the first raw event");
    require(result.scheduledMidiEvents[0].sampleOffset == 0, "engine scheduled note on should use offset zero");
    require(result.scheduledMidiEvents[1].event == result.midiEvents[1], "scheduled event should wrap the second raw event");
    require(result.scheduledMidiEvents[1].sampleOffset == 240, "engine scheduled note off should use block-local offset");
    require(transport.currentSample() == 1440, "engine scheduled render should advance after collection");
}

void audioEngineCollectsLoopedMidiEventsForRenderedBlock()
{
    trackloom::Project project("Engine MIDI");
    trackloom::AudioEngine engine;
    trackloom::Transport transport;
    ConstantAudioSource source(0.25f);
    std::vector<float> samples(2 * 960, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 960);
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Loop Phrase", trackloom::ClipType::Midi, 960, 960);
    const trackloom::PlaybackLoopRange loop { 960, 1920 };

    require(clip.has_value(), "engine looped clip should exist");
    require(project.createMidiNote(clip->id, 720, 120, 67, 80, 1).has_value(), "engine looped pre-wrap note should exist");
    require(project.createMidiNote(clip->id, 0, 120, 60, 100, 1).has_value(), "engine looped wrapped note should exist");
    require(engine.prepare(1920.0, 2, 960), "engine looped prepare should succeed");
    require(transport.seekToSample(1440), "engine looped transport should seek before loop end");
    transport.play();

    trackloom::AudioEngineRenderResult result;
    require(engine.renderNextBlockWithLoopedMidi(transport, block, &source, project, loop, result), "engine looped midi render should succeed");

    require(allSamplesNear(samples, 0.25f), "engine looped midi render should still render audio source");
    require(result.midiEvents.size() == 4, "engine looped render should expose raw midi events across wrap");
    require(result.scheduledMidiEvents.size() == 4, "engine looped render should expose scheduled midi events across wrap");
    require(result.scheduledMidiEvents[0].event.absoluteTick == 1680, "engine looped first event should be pre-wrap note on");
    require(result.scheduledMidiEvents[0].sampleOffset == 240, "engine looped pre-wrap note on should keep sample offset");
    require(result.scheduledMidiEvents[1].event.absoluteTick == 1800, "engine looped second event should be pre-wrap note off");
    require(result.scheduledMidiEvents[1].sampleOffset == 360, "engine looped pre-wrap note off should keep sample offset");
    require(result.scheduledMidiEvents[2].event.absoluteTick == 960, "engine looped third event should be wrapped note on");
    require(result.scheduledMidiEvents[2].sampleOffset == 480, "engine looped wrapped note on should keep block-wide sample offset");
    require(result.scheduledMidiEvents[3].event.absoluteTick == 1080, "engine looped fourth event should be wrapped note off");
    require(result.scheduledMidiEvents[3].sampleOffset == 600, "engine looped wrapped note off should keep block-wide sample offset");
    require(transport.currentSample() == 2400, "engine looped render should advance transport linearly after collection");
}

void audioEngineRejectsInvalidLoopRangeWithoutAdvancing()
{
    trackloom::Project project("Engine MIDI");
    trackloom::AudioEngine engine;
    trackloom::Transport transport;
    std::vector<float> samples(2 * 480, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 480);
    const trackloom::PlaybackLoopRange invalidLoop { 1920, 960 };

    require(engine.prepare(1920.0, 2, 480), "engine invalid loop prepare should succeed");
    require(transport.seekToSample(960), "engine invalid loop transport should seek before render");
    transport.play();

    trackloom::AudioEngineRenderResult result;
    result.midiEvents.push_back(trackloom::MidiPlaybackEvent {
        trackloom::MidiPlaybackEventType::NoteOn,
        "old-track",
        "old-clip",
        "old-note",
        0,
        60,
        100,
        1
    });
    result.scheduledMidiEvents.push_back(trackloom::ScheduledMidiPlaybackEvent {
        result.midiEvents.front(),
        12
    });

    require(!engine.renderNextBlockWithLoopedMidi(transport, block, project, invalidLoop, result), "engine invalid loop render should fail");

    require(result.midiEvents.empty(), "engine invalid loop render should clear stale midi events");
    require(result.scheduledMidiEvents.empty(), "engine invalid loop render should clear stale scheduled events");
    require(transport.currentSample() == 960, "engine invalid loop render should not advance transport");
}

void audioEngineMidiBridgeDispatchesRenderedScheduledEvents()
{
    trackloom::Project project("Engine MIDI");
    trackloom::AudioEngine engine;
    trackloom::Transport transport;
    std::vector<float> samples(2 * 480, 0.0f);
    trackloom::AudioBlock block(samples.data(), 2, 480);
    RecordingMidiEventReceiver receiver;
    const auto track = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(track.id, "Bridge Phrase", trackloom::ClipType::Midi, 960, 960);

    require(clip.has_value(), "bridge clip should exist before render");
    require(project.createMidiNote(clip->id, 0, 240, 60, 100, 1).has_value(), "bridge note should exist before render");
    require(engine.prepare(1920.0, 2, 480), "bridge engine prepare should succeed");
    require(transport.seekToSample(960), "bridge transport should seek to note-on block");
    transport.play();

    trackloom::AudioEngineRenderResult result;
    require(engine.renderNextBlockWithMidi(transport, block, project, result), "bridge render should produce scheduled events");

    const auto dispatchResult = trackloom::dispatchAudioEngineMidiEvents(result, receiver);

    require(dispatchResult.success, "bridge dispatch should succeed for rendered scheduled events");
    require(dispatchResult.attemptedEventCount == 2, "bridge dispatch should attempt both rendered events");
    require(dispatchResult.deliveredEventCount == 2, "bridge dispatch should deliver both rendered events");
    require(dispatchResult.failedEventIndex == -1, "successful bridge dispatch should not report failed index");
    require(receiver.events().size() == 2, "bridge receiver should record two scheduled events");
    require(receiver.messages().size() == 2, "bridge receiver should record two midi output messages");
    require(receiver.events()[0] == result.scheduledMidiEvents[0], "bridge receiver should get first rendered event unchanged");
    require(receiver.events()[1] == result.scheduledMidiEvents[1], "bridge receiver should get second rendered event unchanged");
    require(receiver.messages()[0].statusByte == 0x90, "bridge note on should become midi note-on status");
    require(receiver.messages()[0].sampleOffset == 0, "bridge note on should keep rendered sample offset");
    require(receiver.messages()[1].statusByte == 0x80, "bridge note off should become midi note-off status");
    require(receiver.messages()[1].sampleOffset == 240, "bridge note off should keep rendered sample offset");
    require(transport.currentSample() == 1440, "bridge dispatch should not rewind or advance transport");
}

void audioEngineMidiBridgeAcceptsEmptyRenderResult()
{
    trackloom::AudioEngineRenderResult result;
    RecordingMidiEventReceiver receiver;

    const auto dispatchResult = trackloom::dispatchAudioEngineMidiEvents(result, receiver);

    require(dispatchResult.success, "empty bridge dispatch should succeed");
    require(dispatchResult.attemptedEventCount == 0, "empty bridge dispatch should attempt no events");
    require(dispatchResult.deliveredEventCount == 0, "empty bridge dispatch should deliver no events");
    require(dispatchResult.failedEventIndex == -1, "empty bridge dispatch should not report failed index");
    require(receiver.messages().empty(), "empty bridge dispatch should not call receiver");
}

void audioEngineMidiBridgeStopsWhenReceiverFails()
{
    trackloom::AudioEngineRenderResult result;
    result.scheduledMidiEvents = {
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 0, 1, 60, 100),
        makeScheduledMidiEvent(trackloom::MidiPlaybackEventType::NoteOn, 120, 1, 64, 90)
    };
    FailingMidiEventReceiver receiver(2);

    const auto dispatchResult = trackloom::dispatchAudioEngineMidiEvents(result, receiver);

    require(!dispatchResult.success, "bridge dispatch should fail when receiver rejects an event");
    require(dispatchResult.attemptedEventCount == 2, "bridge dispatch should count the failed event as attempted");
    require(dispatchResult.deliveredEventCount == 1, "bridge dispatch should count only accepted events as delivered");
    require(dispatchResult.failedEventIndex == 1, "bridge dispatch should preserve zero-based failure index");
    require(receiver.callCount() == 2, "bridge dispatch should stop immediately after receiver failure");
    require(receiver.messages().size() == 2, "receiver should only see events up to the failure");
}

void audioEngineClearsScheduledMidiEventsWhenStoppedOrFailed()
{
    trackloom::Project project("Engine MIDI");
    trackloom::AudioEngine engine;
    trackloom::Transport stoppedTransport;
    trackloom::Transport failedTransport;
    std::vector<float> samples(2 * 480, 1.0f);
    trackloom::AudioBlock block(samples.data(), 2, 480);

    require(engine.prepare(1920.0, 2, 480), "engine prepare should succeed before stopped scheduled render");

    trackloom::AudioEngineRenderResult stoppedResult;
    stoppedResult.scheduledMidiEvents.push_back(trackloom::ScheduledMidiPlaybackEvent {
        trackloom::MidiPlaybackEvent {
            trackloom::MidiPlaybackEventType::NoteOn,
            "old-track",
            "old-clip",
            "old-note",
            0,
            60,
            100,
            1
        },
        12
    });

    require(engine.renderNextBlockWithMidi(stoppedTransport, block, project, stoppedResult), "stopped scheduled render should succeed");
    require(stoppedResult.scheduledMidiEvents.empty(), "stopped scheduled render should clear stale scheduled events");

    trackloom::AudioEngine unpreparedEngine;
    trackloom::AudioEngineRenderResult failedResult;
    failedResult.scheduledMidiEvents.push_back(trackloom::ScheduledMidiPlaybackEvent {
        trackloom::MidiPlaybackEvent {
            trackloom::MidiPlaybackEventType::NoteOn,
            "old-track",
            "old-clip",
            "old-note",
            0,
            60,
            100,
            1
        },
        24
    });

    require(failedTransport.seekToSample(128), "failed scheduled transport should seek before render");
    failedTransport.play();
    require(!unpreparedEngine.renderNextBlockWithMidi(failedTransport, block, project, failedResult), "unprepared scheduled render should fail");
    require(failedResult.scheduledMidiEvents.empty(), "failed scheduled render should clear stale scheduled events");
    require(failedTransport.currentSample() == 128, "failed scheduled render should not advance transport");
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
        midiPlaybackCollectsNoteOnAndOffInsideWindow();
        midiPlaybackUsesHalfOpenWindow();
        midiPlaybackSortsEventsDeterministically();
        preparedMidiPlanOrdersNoteOffBeforeNoteOn();
        preparedMidiPlanBuildsDeterministicNonLoopSnapshot();
        preparedMidiPlanRejectsInvalidRequests();
        preparedMidiPlanRejectsSamplePositionThatRoundsPastInt64Maximum();
        preparedMidiPlanAcceptsMaximumSafelyRoundableSamplePosition();
        preparedMidiPlanUsesExistingTrackPlaybackRules();
        midiPlaybackRespectsTrackPlaybackState();
        midiPlaybackHiddenTrackStillPlays();
        midiPlaybackRejectsInvalidWindows();
        midiPlaybackIgnoresAudioClips();
        midiPlaybackChasesActiveNoteAtWindowStart();
        playbackClockConvertsDefaultTempoBlockToTickWindow();
        playbackClockHandlesTempoChangeInsideBlock();
        playbackClockRejectsStoppedOrInvalidBlocks();
        playbackClockCollectsMidiEventsForTransportBlock();
        playbackClockUsesHalfOpenBlockBoundary();
        playbackClockSchedulesMidiEventsWithSampleOffsets();
        playbackClockSchedulesChasedMidiAtBlockStart();
        playbackClockSchedulesMidiEventsAcrossTempoChange();
        playbackClockRejectsStoppedOrInvalidScheduledBlocks();
        playbackClockSplitsLoopedBlockAtLoopEnd();
        playbackClockSchedulesMidiEventsAcrossLoopWrap();
        playbackClockAddsLoopBoundaryNoteOffForLongNote();
        playbackClockChasesWrappedLoopWindowStart();
        playbackClockRejectsInvalidLoopedBlocks();
        playbackClockNormalizesTransportPositionIntoLoopRange();
        playbackClockSplitsLoopedBlockAcrossTempoMappedLoop();
        midiDispatchConvertsNoteEventsToOutputMessages();
        midiDispatchSendsEventsInOrder();
        midiDispatchAcceptsEmptyEventList();
        midiDispatchStopsWhenReceiverFails();
        midiDispatchRejectsInvalidScheduledEvents();
        midiOutputDeviceSendsMessagesThroughOpenPort();
        midiOutputDeviceRejectsSendWhenPortIsClosed();
        midiOutputDeviceReportsOpenFailure();
        midiOutputDeviceCanCloseAndReopenPort();
        midiOutputDeviceFailureStopsDispatch();
        midiTrackRouterRoutesEventsByTrackId();
        midiTrackRouterFailsWhenTrackHasNoReceiver();
        midiTrackRouterPreservesReceiverFailure();
        midiTrackRouterValidatesBindingsWithoutReplacingPreviousRoutes();
        midiTrackRouterAcceptsEmptyBindings();
        projectMidiOutputGraphDispatchesRenderedInstrumentTracks();
        projectMidiOutputGraphRejectsInvalidBindingsWithoutReplacingOldRoutes();
        projectMidiOutputGraphReportsUnboundRenderedTracks();
        projectMidiOutputGraphPropagatesDownstreamFailure();
        projectMidiOutputGraphAcceptsEmptyBindingsForEmptyRenderResult();
        midiOutputSessionTracksDeliveredNoteLifecycle();
        midiOutputSessionIgnoresUndeliveredEvents();
        midiOutputSessionReleasesActiveNotes();
        midiOutputSessionKeepsUndeliveredReleaseNotesActive();
        midiOutputSessionReleaseClearsStackedMatchingNotes();
        midiOutputSessionRejectsRebuildWhileNotesAreActive();
        midiOutputSessionAcceptsEmptyDispatchAndRelease();
        projectPlaybackSessionRendersAudioAndDispatchesMidi();
        projectPlaybackSessionChasesHeldMidiNote();
        projectPlaybackSessionDoesNotRepeatMidiChaseOnContinuousBlocks();
        projectPlaybackSessionCanRequestMidiChaseAfterSeek();
        projectPlaybackSessionRendersLoopedBlockAndDispatchesMidi();
        projectPlaybackSessionReleasesLoopBoundaryLongNote();
        projectPlaybackSessionRejectsInvalidLoopRangeWithoutDispatch();
        projectPlaybackSessionKeepsAudioRenderWhenMidiDispatchFails();
        projectPlaybackSessionDoesNotDispatchMidiWhenAudioRenderFails();
        projectPlaybackSessionRendersStoppedBlockWithoutMidi();
        projectPlaybackSessionCanReleaseActiveMidiNotes();
        projectPlaybackSessionStopsAfterReleasingActiveMidiNotes();
        projectPlaybackSessionDoesNotStopWhenMidiReleaseFails();
        projectPlaybackSessionSeeksAfterReleasingActiveMidiNotes();
        projectPlaybackSessionRejectsInvalidSafeSeek();
        projectPlaybackSessionSafelyRebuildsMidiOutputAfterRelease();
        projectPlaybackSessionKeepsMidiOutputWhenSafeRebuildReleaseFails();
        projectPlaybackSessionKeepsMidiOutputWhenSafeRebuildBindingFails();
        projectPlaybackSessionRejectsSafeMidiOutputRebuildBeforePrepare();
        stopPlaybackCommandStopsAfterMidiRelease();
        seekPlaybackCommandSeeksAfterMidiReleaseAndRequestsChase();
        seekPlaybackCommandRejectsNegativeTargetWithoutRelease();
        rebuildMidiOutputCommandSwitchesRoutesSafely();
        playbackControlCommandsRejectUnpreparedSession();
        playbackControlCommandsReportStableFailureReasons();
        stopPlaybackCommandReportsMidiReleaseFailureReason();
        projectPlaybackSessionRejectsPrepareWhileMidiNotesAreActive();
        projectPlaybackSessionRejectsUseBeforePrepare();
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
        setMidiClipStartKeepingNoteTimesCommandTrimsStartWithUndoAndRedo();
        setMidiClipStartKeepingNoteTimesCommandExtendsStartWithUndoAndRedo();
        invalidSetMidiClipStartKeepingNoteTimesCommandDoesNotModifyProject();
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
        saveReportsRetainedHelperRecoveryPathAfterSuccessfulReplacement();
        saveReportsWorkspaceCleanupFailureAfterSuccessfulReplacement();
        saveReportsThrownCleanupAfterSuccessfulReplacementAsWarning();
        saveDoesNotWarnWhenWorkspaceWasRemovedDespiteFalseCleanupResult();
        saveRejectsParseableButTruncatedTemporaryProjectBytes();
        saveRejectsReportedTemporaryProjectFlushFailure();
        saveRejectsReportedTemporaryProjectCloseFailure();
        saveRejectsNullInjectedFileOperationsWithoutTouchingTarget();
        atomicFileReplaceReplacesExistingTarget();
        atomicFileReplaceInstallsWhenTargetIsMissing();
        atomicFileReplaceMissingReplacementPreservesExistingTarget();
#ifdef _WIN32
        atomicFileReplaceReportsReplacementQueryFailureWithoutTouchingTarget();
        atomicFileReplaceReportsTargetQueryFailureWithoutTouchingFiles();
        atomicFileReplaceDoesNotOverwriteTargetThatAppearsBeforeMove();
        atomicFileReplacePreservesReplacementWhenTargetDisappearsBeforeReplace();
        atomicFileReplaceError1175PreservesBothOriginalFiles();
        atomicFileReplaceError1176PreservesOriginalNamesWithBackup();
        atomicFileReplaceError1177InstallsReplacementAndRemovesBackup();
        atomicFileReplaceError1177RestoresTargetWhenInstallFails();
        atomicFileReplaceError1177RestoresBackupIfReplacementDisappears();
        atomicFileReplaceError1177PreservesBothCopiesWhenRecoveryFails();
        atomicFileReplacePreservesPreexistingRecoveryDirectory();
        atomicFileReplaceReportsRecoveryDirectoryCreationError();
        atomicFileReplaceReportsRecoveryDirectoryCleanupFailure();
        atomicFileReplaceBackupCleanupFailureKeepsSuccessfulTarget();
        atomicFileReplaceSupportsChineseWindowsPaths();
        savePreservesTemporaryRecoveryAfterRealWindowsReplaceFailure();
#endif
        saveRefusesToOverwriteExistingTemporaryRecoveryFile();
        savePreservesPreexistingWorkspaceAndCompetingTemporaryFile();
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
        audioEngineCollectsMidiEventsForRenderedBlock();
        audioEngineClearsMidiEventsWhenStopped();
        audioEngineClearsMidiEventsWhenRenderRequestFails();
        audioEngineCollectsMidiEventsBeforeAdvancingTransport();
        audioEngineExposesScheduledMidiEventsForRenderedBlock();
        audioEngineCollectsLoopedMidiEventsForRenderedBlock();
        audioEngineRejectsInvalidLoopRangeWithoutAdvancing();
        audioEngineMidiBridgeDispatchesRenderedScheduledEvents();
        audioEngineMidiBridgeAcceptsEmptyRenderResult();
        audioEngineMidiBridgeStopsWhenReceiverFails();
        audioEngineClearsScheduledMidiEventsWhenStoppedOrFailed();
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
