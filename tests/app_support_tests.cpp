#include "AppAudioClipActions.h"
#include "AppAudioSettings.h"
#include "AppCommandDispatcher.h"
#include "AppCommandPalette.h"
#include "AppCommandPaletteSession.h"
#include "AppCommandShortcuts.h"
#include "AppShortcutSettings.h"
#include "AppShortcutStatus.h"
#include "AppProjectFileActions.h"
#include "AppRecentProjects.h"
#include "AppMidiClipActions.h"
#include "AppMidiNoteActions.h"
#include "AppMainMenu.h"
#include "AppPlaybackActions.h"
#include "AppProjectSession.h"
#include "AppProjectStatus.h"
#include "AppTimelineStatus.h"
#include "AppTrackActions.h"
#include "AppTrackListStatus.h"
#include "AppTrackStateActions.h"
#include "Command.h"
#include "TrackLoomAppInfo.h"

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <cstdlib>
#endif

#include <filesystem>
#include <fstream>
#include <atomic>
#include <iostream>
#include <latch>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {

const trackloom::AppLoopPlaybackState disabledLoopState;

void configureTestFailureOutput()
{
#if defined(_MSC_VER)
    // MSVC Debug CRT 默认会在 abort/assert 时弹窗；测试应直接写 stderr，避免 CTest 被弹窗卡住。
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_error_mode(_OUT_TO_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
}

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << message << '\n';
        throw std::runtime_error(message);
    }
}

std::filesystem::path testWorkspace();
void removeTestWorkspace();

class FakeRealtimePlaybackHost final : public trackloom::RealtimePlaybackHost {
public:
    FakeRealtimePlaybackHost()
    {
        snapshot_.format = {
            1, "fake-device", "Fake Speakers", 48000.0, 256, 2, 3, true
        };
        snapshot_.realtime.state = trackloom::RealtimePlaybackState::Stopped;
    }

    trackloom::AudioDeviceFormatSnapshot deviceFormatSnapshot() const override
    {
        std::scoped_lock lock(mutex_);
        return snapshot_.format;
    }

    trackloom::RealtimePlaybackHostResult installAndStart(
        std::unique_ptr<const trackloom::PreparedMidiPlaybackPlan> plan) override
    {
        std::scoped_lock lock(mutex_);
        ++installCallCount;
        installedPlan = std::move(plan);
        if (!installResult.success) {
            return installResult;
        }
        snapshot_.realtime.projectSamplePosition = installedPlan->playbackStartSample;
        snapshot_.realtime.renderedSampleCount = 0;
        snapshot_.realtime.state = trackloom::RealtimePlaybackState::Playing;
        snapshot_.callbackRunning = true;
        snapshot_.planInstalled = true;
        return installResult;
    }

    bool requestStop() noexcept override
    {
        std::scoped_lock lock(mutex_);
        ++stopCallCount;
        if (requestStopResult) {
            snapshot_.realtime.state = trackloom::RealtimePlaybackState::Stopping;
        }
        return requestStopResult;
    }

    void serviceNonRealtime() override
    {
        std::scoped_lock lock(mutex_);
        ++serviceCallCount;
    }

    void hardStopAndReset() noexcept override
    {
        std::scoped_lock lock(mutex_);
        snapshot_.realtime.state = trackloom::RealtimePlaybackState::Stopped;
        snapshot_.callbackRunning = false;
        snapshot_.planInstalled = false;
        installedPlan.reset();
    }

    trackloom::RealtimePlaybackHostSnapshot snapshot() const override
    {
        std::scoped_lock lock(mutex_);
        return snapshot_;
    }

    void setDeviceAvailable(bool available)
    {
        std::scoped_lock lock(mutex_);
        snapshot_.format.available = available;
    }

    void setFormatGeneration(std::uint64_t generation)
    {
        std::scoped_lock lock(mutex_);
        snapshot_.format.generation = generation;
    }

    void setRealtimeState(trackloom::RealtimePlaybackState state)
    {
        std::scoped_lock lock(mutex_);
        snapshot_.realtime.state = state;
    }

    void setRealtimePosition(std::int64_t sample, std::uint64_t rendered)
    {
        std::scoped_lock lock(mutex_);
        snapshot_.realtime.projectSamplePosition = sample;
        snapshot_.realtime.renderedSampleCount = rendered;
    }

    trackloom::RealtimePlaybackHostResult installResult {
        true, trackloom::RealtimePlaybackHostFailureReason::None, "started"
    };
    bool requestStopResult = true;
    int installCallCount = 0;
    int stopCallCount = 0;
    int serviceCallCount = 0;
    std::unique_ptr<const trackloom::PreparedMidiPlaybackPlan> installedPlan;

private:
    mutable std::mutex mutex_;
    trackloom::RealtimePlaybackHostSnapshot snapshot_;
};

trackloom::PreparedMidiPlaybackPlanBuildResult makeFakePreparedPlan(
    const trackloom::PreparedMidiPlaybackPlanBuildRequest& request)
{
    auto plan = std::make_unique<trackloom::PreparedMidiPlaybackPlan>();
    plan->sampleRate = request.sampleRate;
    plan->maximumBlockFrames = request.maximumBlockFrames;
    plan->outputChannelCount = request.outputChannelCount;
    plan->outputChannelMask = request.outputChannelMask;
    plan->playbackStartSample = request.playbackStartSample;
    return { trackloom::PreparedMidiPlaybackPlanBuildFailureReason::None, std::move(plan) };
}

void pollPlaybackUntilWorkerSettles(
    trackloom::AppPlaybackController& playback,
    const trackloom::AppProjectSession& session)
{
    for (int attempt = 0; attempt < 10000; ++attempt) {
        playback.poll(session, disabledLoopState);
        const auto state = playback.status().state;
        if (state != trackloom::AppPlaybackState::Preparing
            && state != trackloom::AppPlaybackState::Stopping) {
            return;
        }
        std::this_thread::yield();
    }
    require(false, "playback worker should settle within the bounded polling loop");
}

void appInfoExposesStableDesktopIdentity()
{
    const auto info = trackloom::desktopAppInfo();

    require(info.applicationName == "TrackLoom",
        "desktop app info should expose the public application name");
    require(info.applicationVersion == "0.1.0",
        "desktop app info should match the CMake project version");
    require(info.organizationName == "TrackLoom",
        "desktop app info should expose the local settings organization name");
}

void missingAudioSettingsUseDocumentedDefaults()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "missing-audio-settings.txt";
    const auto loaded = trackloom::loadAppAudioSettings(path);
    require(loaded.kind == trackloom::AppAudioSettingsLoadKind::Missing,
            "missing settings should have a stable load kind");
    require(loaded.settings.requestedSampleRate == 48000.0,
            "missing settings should request 48 kHz");
    require(loaded.settings.requestedBufferFrames == 256,
            "missing settings should request 256 frames");
    require(loaded.settings.requestedOutputChannels == 2,
            "missing settings should request stereo");
    require(trackloom::appMainMenuCommandId(
                trackloom::AppMainMenuCommand::OpenAudioSettings) == 1303,
            "audio settings command id must stay stable");
}

void audioSettingsSaveAndLoadUseTheDocumentedFiveLineFormat()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "settings" / "audio-settings.txt";
    const trackloom::AppAudioSettings settings { "USB \xE8\x80\xB3\xE6\x9C\xBA", 44100.0, 512, 1 };

    require(trackloom::saveAppAudioSettings(settings, path),
            "audio settings should save to a new local settings file");

    std::ifstream input(path, std::ios::binary);
    std::string serialized((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    require(serialized
                == "trackloom_audio_settings 1\n"
                   "output_device \"USB \xE8\x80\xB3\xE6\x9C\xBA\"\n"
                   "sample_rate 44100\n"
                   "buffer_frames 512\n"
                   "output_channels 1\n",
            "audio settings should use the documented fixed five-line UTF-8 format");

    const auto loaded = trackloom::loadAppAudioSettings(path);
    require(loaded.kind == trackloom::AppAudioSettingsLoadKind::Loaded,
            "valid audio settings should report loaded");
    require(loaded.settings.outputDeviceName == settings.outputDeviceName,
            "audio settings should round-trip the quoted UTF-8 device name");
    require(loaded.settings.requestedSampleRate == settings.requestedSampleRate,
            "audio settings should round-trip the requested sample rate");
    require(loaded.settings.requestedBufferFrames == settings.requestedBufferFrames,
            "audio settings should round-trip the requested buffer size");
    require(loaded.settings.requestedOutputChannels == settings.requestedOutputChannels,
            "audio settings should round-trip the requested output channels");
}

void audioSettingsSavePreservesUnicodeSettingsPaths()
{
    removeTestWorkspace();
    const auto path = testWorkspace()
        / std::filesystem::path(L"audio-\U0001F3A7")
        / L"audio-settings.txt";
    const trackloom::AppAudioSettings settings { "Unicode Path Output", 48000.0, 256, 2 };

    require(trackloom::saveAppAudioSettings(settings, path),
            "audio settings should save below a Unicode local settings path");
    const auto loaded = trackloom::loadAppAudioSettings(path);
    require(loaded.kind == trackloom::AppAudioSettingsLoadKind::Loaded
                && loaded.settings.outputDeviceName == settings.outputDeviceName,
            "audio settings should load from the same Unicode local settings path");
}

void invalidAudioSettingsLoadAsDefaultsAndCannotOverwriteSavedSettings()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "settings" / "audio-settings.txt";
    const trackloom::AppAudioSettings valid { "Studio Output", 48000.0, 256, 2 };
    require(trackloom::saveAppAudioSettings(valid, path),
            "invalid settings test should save a valid baseline file");

    std::ifstream originalInput(path, std::ios::binary);
    const std::string original((std::istreambuf_iterator<char>(originalInput)),
        std::istreambuf_iterator<char>());
    const trackloom::AppAudioSettings invalid { "Broken", std::numeric_limits<double>::infinity(), 0, 3 };
    require(!trackloom::saveAppAudioSettings(invalid, path),
            "invalid settings should not be saved");
    std::ifstream afterFailedSaveInput(path, std::ios::binary);
    const std::string afterFailedSave((std::istreambuf_iterator<char>(afterFailedSaveInput)),
        std::istreambuf_iterator<char>());
    require(afterFailedSave == original,
            "a failed save should leave the existing local settings file unchanged");
    require(valid.outputDeviceName == "Studio Output" && valid.requestedBufferFrames == 256,
            "a failed save should not mutate caller-owned settings");

    const std::string invalidFiles[] = {
        "trackloom_audio_settings 2\noutput_device \"Output\"\nsample_rate 48000\nbuffer_frames 256\noutput_channels 2\n",
        "trackloom_audio_settings 1\noutput_device \"Output\"\nsample_rate nan\nbuffer_frames 256\noutput_channels 2\n",
        "trackloom_audio_settings 1\noutput_device \"Output\"\nsample_rate inf\nbuffer_frames 256\noutput_channels 2\n",
        "trackloom_audio_settings 1\noutput_device \"Output\"\nsample_rate 48000\nbuffer_frames 0\noutput_channels 2\n",
        "trackloom_audio_settings 1\noutput_device \"Output\"\nsample_rate 48000\nbuffer_frames 256\noutput_channels 3\n",
        "trackloom_audio_settings 1\noutput_device Output\nsample_rate 48000\nbuffer_frames 256\noutput_channels 2\n",
        "trackloom_audio_settings 1\noutput_device \"Output\"\nsample_rate 48000\nbuffer_frames 256\noutput_channels 2\ntrailing\n"
        , "trackloom_audio_settings 1 output_device \"Output\" sample_rate 48000 buffer_frames 256 output_channels 2\n"
    };
    for (const auto& invalidFile : invalidFiles) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << invalidFile;
        output.close();
        const auto loaded = trackloom::loadAppAudioSettings(path);
        require(loaded.kind == trackloom::AppAudioSettingsLoadKind::Invalid,
                "invalid audio settings must expose the stable invalid kind");
        require(loaded.settings.requestedSampleRate == 48000.0
                    && loaded.settings.requestedBufferFrames == 256
                    && loaded.settings.requestedOutputChannels == 2,
                "invalid audio settings must fall back to documented defaults");
        require(!loaded.warning.empty(),
                "invalid audio settings must provide a warning");
    }

    const auto blockedParent = testWorkspace() / "blocked-parent";
    {
        std::ofstream blocker(blockedParent, std::ios::binary);
        blocker << "not a directory";
    }
    require(!trackloom::saveAppAudioSettings(valid, blockedParent / "audio-settings.txt"),
            "audio settings save should fail when its parent is a file");
    std::ifstream unchangedInput(path, std::ios::binary);
    const std::string afterFilesystemFailure(
        (std::istreambuf_iterator<char>(unchangedInput)),
        std::istreambuf_iterator<char>());
    require(afterFilesystemFailure == invalidFiles[sizeof(invalidFiles) / sizeof(invalidFiles[0]) - 1],
            "filesystem save failure should not overwrite an existing settings file");
}

void audioSettingsCommandAppearsInToolsAndDispatches()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Audio Settings Command");
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host);
    trackloom::AppRecentProjects recent;
    const auto menu = trackloom::describeAppMainMenu(session, playback, recent);
    const auto commandId = trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::OpenAudioSettings);
    const auto palette = trackloom::describeAppCommandPalette(menu);
    const trackloom::AppCommandPaletteItem* paletteItem = nullptr;
    for (const auto& item : palette.items) {
        if (item.commandId == commandId) {
            paletteItem = &item;
            break;
        }
    }
    require(menu.groups[5].items.back().commandId == commandId
                && menu.groups[5].items.back().enabled
                && menu.groups[5].items.back().label == "音频设置…",
            "tools menu should expose enabled audio settings command");
    require(paletteItem != nullptr && paletteItem->label == "音频设置…",
            "command palette should make audio settings searchable");

    int calls = 0;
    trackloom::AppCommandHandlers handlers;
    handlers.openAudioSettings = [&] { ++calls; };
    const auto dispatched = trackloom::dispatchAppCommand(commandId, handlers);
    require(dispatched.executed && dispatched.command == trackloom::AppCommandKind::OpenAudioSettings
                && calls == 1,
            "audio settings command should dispatch its handler once");
    const auto missing = trackloom::dispatchAppCommand(commandId, {});
    require(!missing.executed && missing.kind == trackloom::AppCommandDispatchResultKind::MissingHandler,
            "audio settings command should report a missing handler");
}

void projectPlaybackGenerationTracksOnlyPossibleContentChanges()
{
    trackloom::AppProjectSession session;
    const auto initial = session.projectEditGeneration();
    session.editProject().rename("Changed");
    require(session.projectEditGeneration() == initial + 1,
        "editable project access should conservatively advance generation once");
    const auto afterEdit = session.projectEditGeneration();
    require(!session.undoProjectEdit(), "direct edit should clear command history");
    require(session.projectEditGeneration() == afterEdit,
        "failed undo must not advance project generation");
    const auto snapshot = session.capturePlaybackSnapshot();
    require(snapshot.projectEditGeneration == afterEdit,
        "snapshot and project copy must share one generation");
    require(snapshot.project.name() == "Changed",
        "snapshot should copy the matching project state");
}

void projectPlaybackGenerationTracksCommandsFilesAndIndependentSnapshots()
{
    removeTestWorkspace();
    std::filesystem::create_directories(testWorkspace());
    const auto savedPath = testWorkspace() / "generation-saved.trackloom-test";
    const auto openedPath = testWorkspace() / "generation-opened.trackloom-test";

    trackloom::AppProjectSession session;
    require(session.projectEditGeneration() == 0,
        "new project sessions should begin at generation zero");

    const auto commandGeneration = session.projectEditGeneration();
    const auto command = session.executeProjectCommand(
        std::make_unique<trackloom::AddTrackCommand>("Lead", trackloom::TrackType::Instrument));
    require(command.success, "generation test command should succeed");
    require(session.projectEditGeneration() == commandGeneration + 1,
        "successful commands should advance generation exactly once");

    const auto undoGeneration = session.projectEditGeneration();
    require(session.undoProjectEdit(), "generation test undo should succeed");
    require(session.projectEditGeneration() == undoGeneration + 1,
        "successful undo should advance generation exactly once");
    const auto redoGeneration = session.projectEditGeneration();
    require(session.redoProjectEdit(), "generation test redo should succeed");
    require(session.projectEditGeneration() == redoGeneration + 1,
        "successful redo should advance generation exactly once");

    const auto failedGeneration = session.projectEditGeneration();
    require(!session.executeProjectCommand(nullptr).success,
        "null project command should fail");
    require(session.projectEditGeneration() == failedGeneration,
        "failed commands should not advance generation");

    const auto newGeneration = session.projectEditGeneration();
    session.createNewProject("Saved");
    require(session.projectEditGeneration() == newGeneration + 1,
        "creating a project should advance generation exactly once");
    const auto beforeSave = session.projectEditGeneration();
    require(session.saveAs(savedPath).success, "generation test save-as should succeed");
    require(session.projectEditGeneration() == beforeSave,
        "successful save-as should not advance generation");
    require(session.save().success, "generation test save should succeed");
    require(session.projectEditGeneration() == beforeSave,
        "successful save should not advance generation");

    trackloom::AppProjectSession source;
    source.createNewProject("Opened");
    require(source.saveAs(openedPath).success, "generation test source save should succeed");
    const auto beforeOpen = session.projectEditGeneration();
    require(session.openFrom(openedPath).success, "generation test open should succeed");
    require(session.projectEditGeneration() == beforeOpen + 1,
        "successful open should advance generation exactly once");
    const auto afterOpen = session.projectEditGeneration();
    require(!session.openFrom(testWorkspace() / "missing.trackloom-test").success,
        "missing project open should fail");
    require(session.projectEditGeneration() == afterOpen,
        "failed open should not advance generation");

    const auto snapshot = session.capturePlaybackSnapshot();
    session.editProject().rename("Later Edit");
    require(snapshot.projectEditGeneration == afterOpen,
        "captured snapshot should keep its matching generation");
    require(snapshot.project.name() == "Opened",
        "captured snapshot should remain independent from later edits");
}

std::filesystem::path testWorkspace()
{
    return std::filesystem::temp_directory_path() / "trackloom_app_session_tests";
}

void removeTestWorkspace()
{
    std::error_code ignoredError;
    std::filesystem::remove_all(testWorkspace(), ignoredError);
}

std::filesystem::path injectedSessionRecoveryPathOne;
std::filesystem::path injectedSessionRecoveryPathTwo;

trackloom::FileOperationResult saveProjectWithInjectedRecoveryWarning(
    const trackloom::Project&,
    const std::filesystem::path&)
{
    return trackloom::FileOperationResult::ok(
        "Recovery data requires inspection.",
        { injectedSessionRecoveryPathOne, injectedSessionRecoveryPathTwo });
}

const trackloom::AppCommandPaletteItem* findPaletteItem(
    const trackloom::AppCommandPaletteStatus& palette,
    int commandId)
{
    for (const auto& item : palette.items) {
        if (item.commandId == commandId) {
            return &item;
        }
    }

    return nullptr;
}

const trackloom::AppShortcutStatusRow* findShortcutStatusRow(
    const trackloom::AppShortcutStatus& status,
    int commandId)
{
    for (const auto& row : status.rows) {
        if (row.commandId == commandId) {
            return &row;
        }
    }

    return nullptr;
}

trackloom::AppCommandPaletteStatus sampleCommandPaletteForSession()
{
    trackloom::AppCommandPaletteStatus palette;
    palette.items.push_back({
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::UndoProject),
        false,
        "编辑",
        "撤销",
        "Ctrl+Z"
    });
    palette.items.push_back({
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SaveProject),
        true,
        "文件",
        "保存",
        "Ctrl+S"
    });
    palette.items.push_back({
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SaveProjectAs),
        true,
        "文件",
        "另存为...",
        "Ctrl+Shift+S"
    });
    palette.items.push_back({
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::StopProject),
        false,
        "播放",
        "停止",
        ""
    });
    palette.items.push_back({
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::PlayProject),
        true,
        "播放",
        "播放",
        ""
    });

    return palette;
}

trackloom::AppCommandPaletteStatus sampleLongCommandPaletteForSession()
{
    trackloom::AppCommandPaletteStatus palette;
    palette.items.push_back({
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::UndoProject),
        false,
        "编辑",
        "撤销",
        "Ctrl+Z"
    });
    palette.items.push_back({
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::NewProject),
        true,
        "文件",
        "新建工程",
        "Ctrl+N"
    });
    palette.items.push_back({
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::OpenProject),
        true,
        "文件",
        "打开工程",
        "Ctrl+O"
    });
    palette.items.push_back({
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SaveProject),
        true,
        "文件",
        "保存",
        "Ctrl+S"
    });
    palette.items.push_back({
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SaveProjectAs),
        true,
        "文件",
        "另存为...",
        "Ctrl+Shift+S"
    });
    palette.items.push_back({
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::StopProject),
        false,
        "播放",
        "停止",
        ""
    });
    palette.items.push_back({
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::PlayProject),
        true,
        "播放",
        "播放",
        ""
    });
    palette.items.push_back({
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::RewindProject),
        true,
        "播放",
        "回到开头",
        ""
    });
    palette.items.push_back({
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::AddInstrumentTrack),
        true,
        "轨道",
        "添加乐器轨",
        ""
    });
    palette.items.push_back({
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::AddAudioTrack),
        true,
        "轨道",
        "添加音频轨",
        ""
    });
    palette.items.push_back({
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::AddFolderTrack),
        true,
        "轨道",
        "添加文件夹",
        ""
    });
    palette.items.push_back({
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::OpenCommandPalette),
        true,
        "工具",
        "命令面板...",
        "Ctrl+K, Ctrl+Shift+P"
    });

    return palette;
}

void projectSessionTracksNewProjectAndDirtyState()
{
    trackloom::AppProjectSession session;

    session.createNewProject("Sketch");

    require(session.project().name() == "Sketch",
        "new app project session should expose the requested project name");
    require(!session.currentProjectPath().has_value(),
        "new app project session should not have a file path before save-as");
    require(!session.isDirty(),
        "new app project session should start clean until the user edits it");

    session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);

    require(session.isDirty(),
        "requesting an editable project should mark the app session dirty");
    require(session.project().tracks().size() == 1,
        "editable project access should modify the session project");
}

void projectSessionSavesAndOpensProjectFile()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "saved-project.trackloom-test";

    trackloom::AppProjectSession savedSession;
    savedSession.createNewProject("Saved Project");
    savedSession.editProject().createTrack("Lead", trackloom::TrackType::Instrument);

    const auto save = savedSession.saveAs(path);

    require(save.success, "app project session should save to a chosen file path");
    require(savedSession.currentProjectPath().has_value(),
        "save-as should record the current project path");
    require(savedSession.currentProjectPath().value() == path,
        "save-as should keep the exact file path chosen by the caller");
    require(!savedSession.isDirty(),
        "successful save-as should clear dirty state");
    require(std::filesystem::exists(path),
        "successful save-as should create the project file");

    trackloom::AppProjectSession openedSession;
    const auto open = openedSession.openFrom(path);

    require(open.success, "app project session should open a saved project file");
    require(openedSession.project().name() == "Saved Project",
        "opened app project session should restore the project name");
    require(openedSession.project().tracks().size() == 1,
        "opened app project session should restore project tracks");
    require(openedSession.currentProjectPath().has_value(),
        "opened app project session should remember the loaded file path");
    require(openedSession.currentProjectPath().value() == path,
        "opened app project session should record the loaded file path exactly");
    require(!openedSession.isDirty(),
        "opening a project file should start from a clean state");
}

void projectSessionKeepsCurrentProjectWhenOpenFails()
{
    removeTestWorkspace();
    const auto missingPath = testWorkspace() / "missing.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Keep Me");
    session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);

    const auto open = session.openFrom(missingPath);

    require(!open.success, "opening a missing project file should fail");
    require(session.project().name() == "Keep Me",
        "failed open should keep the current project name");
    require(session.project().tracks().size() == 1,
        "failed open should keep the current project contents");
    require(session.isDirty(),
        "failed open should keep the previous dirty state");
    require(!session.currentProjectPath().has_value(),
        "failed open should keep the previous project path");
}

void projectSessionRejectsSaveWithoutPath()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Unsaved");

    const auto save = session.save();

    require(!save.success,
        "saving without a current path should fail and ask the caller to use save-as");
    require(save.failureReason == trackloom::AppProjectSessionFailureReason::MissingProjectPath,
        "saving without a current path should expose a stable missing-path reason");
    require(!session.currentProjectPath().has_value(),
        "failed save without path should not invent a project path");
    require(!session.isDirty(),
        "failed save without path should keep the previous dirty state");
}

void projectSessionRunsCoreCommandsThroughUndoRedoHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Command History");

    const auto result = session.executeProjectCommand(
        std::make_unique<trackloom::AddTrackCommand>("Lead", trackloom::TrackType::Instrument));

    require(result.success,
        "app project session should execute valid core commands");
    require(session.project().tracks().size() == 1,
        "executed app command should mutate the current project");
    require(session.project().tracks()[0].name == "Lead",
        "executed app command should preserve the command payload");
    require(session.isDirty(),
        "successful app command execution should mark the session dirty");
    require(session.canUndoProjectEdit(),
        "successful app command execution should create undo history");
    require(!session.canRedoProjectEdit(),
        "executing a new command should not create redo history");

    require(session.undoProjectEdit(),
        "app project session should undo the last executed command");
    require(session.project().tracks().empty(),
        "undo should restore the project state before the command");
    require(!session.canUndoProjectEdit(),
        "undoing the only command should empty undo history");
    require(session.canRedoProjectEdit(),
        "undo should make the command available for redo");

    require(session.redoProjectEdit(),
        "app project session should redo the last undone command");
    require(session.project().tracks().size() == 1,
        "redo should reapply the command to the current project");
    require(session.project().tracks()[0].name == "Lead",
        "redo should restore the same command payload");
    require(session.canUndoProjectEdit(),
        "redo should put the command back into undo history");
    require(!session.canRedoProjectEdit(),
        "redoing the only command should empty redo history");
}

void projectSessionDoesNotDirtyOrRecordFailedCoreCommands()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Failed Command");

    const auto result = session.executeProjectCommand(
        std::make_unique<trackloom::RenameTrackCommand>("missing-track", "Renamed"));

    require(!result.success,
        "app project session should report failed core commands");
    require(!session.isDirty(),
        "failed core commands should not mark the session dirty");
    require(!session.canUndoProjectEdit(),
        "failed core commands should not create undo history");
    require(!session.canRedoProjectEdit(),
        "failed core commands should not create redo history");
}

void projectSessionRejectsNullCoreCommandWithoutMutation()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Null Command");

    const auto result = session.executeProjectCommand(nullptr);

    require(!result.success,
        "null commands should fail instead of crashing or executing");
    require(!session.isDirty(),
        "null commands should not mark the session dirty");
    require(!session.canUndoProjectEdit(),
        "null commands should not create undo history");
    require(!session.canRedoProjectEdit(),
        "null commands should not create redo history");
}

void projectSessionClearsCommandHistoryWhenCreatingNewProject()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Before Reset");

    const auto result = session.executeProjectCommand(
        std::make_unique<trackloom::AddTrackCommand>("Lead", trackloom::TrackType::Instrument));
    require(result.success && session.canUndoProjectEdit(),
        "history reset test should create undoable setup work");

    session.createNewProject("After Reset");

    require(session.project().name() == "After Reset",
        "creating a new project should replace the current project");
    require(session.project().tracks().empty(),
        "creating a new project should not keep old project tracks");
    require(!session.canUndoProjectEdit(),
        "creating a new project should clear undo history from the previous project");
    require(!session.canRedoProjectEdit(),
        "creating a new project should clear redo history from the previous project");
}

void projectSessionDirectEditClearsCommandHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Direct Edit Boundary");

    const auto result = session.executeProjectCommand(
        std::make_unique<trackloom::AddTrackCommand>("Undoable", trackloom::TrackType::Instrument));
    require(result.success && session.canUndoProjectEdit(),
        "direct edit boundary test should start with undoable command history");

    session.editProject().createTrack("Legacy Direct Edit", trackloom::TrackType::Audio);

    require(session.project().tracks().size() == 2,
        "direct edit should still mutate the project");
    require(!session.canUndoProjectEdit(),
        "direct edit should clear older undo history so undo cannot skip over untracked edits");
    require(!session.canRedoProjectEdit(),
        "direct edit should clear redo history for the same reason");
}

void projectStatusDescribesUnsavedDirtyProject()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Sketch");
    session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);

    const auto status = trackloom::describeAppProjectSession(session);

    require(status.projectName == "Sketch",
        "project status should expose the current project name");
    require(!status.hasProjectPath,
        "project status should keep unsaved projects separate from saved file paths");
    require(status.dirty,
        "project status should expose dirty state for the desktop UI");
    require(status.trackCount == 1,
        "project status should count tracks for the desktop UI");
    require(status.windowTitle == "Sketch* - TrackLoom",
        "dirty project status should mark the window title");
    require(status.statusLine.find("未保存工程") != std::string::npos,
        "unsaved project status should tell the user that no project file exists yet");
    require(status.statusLine.find("有未保存修改") != std::string::npos,
        "dirty project status should tell the user that changes need saving");
}

void projectStatusDescribesSavedCleanProject()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "saved-status.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Saved");
    const auto save = session.saveAs(path);

    require(save.success, "status test project should save before describing saved state");

    const auto status = trackloom::describeAppProjectSession(session);

    require(status.projectName == "Saved",
        "saved project status should expose the current project name");
    require(status.hasProjectPath,
        "saved project status should expose that a project file exists");
    require(status.projectPath == path.string(),
        "saved project status should expose the exact current project path string");
    require(!status.dirty,
        "saved project status should expose clean state after save");
    require(status.windowTitle == "Saved - TrackLoom",
        "clean project status should not mark the window title as dirty");
    require(status.statusLine.find("已保存") != std::string::npos,
        "clean project status should tell the user that the project is saved");
    require(status.statusLine.find(path.string()) != std::string::npos,
        "saved project status should include the current project path");
}

void projectFileActionAddsDefaultTrackLoomExtension()
{
    const auto noExtension = testWorkspace() / "song";
    const auto withExtension = testWorkspace() / "song.trackloom";
    const auto customExtension = testWorkspace() / "song.trackloom-test";

    require(trackloom::withTrackLoomProjectExtension(noExtension) == withExtension,
        "save-as should add the default TrackLoom extension when the user omits one");
    require(trackloom::withTrackLoomProjectExtension(customExtension) == customExtension,
        "save-as should keep an explicit existing extension");
}

void projectFileActionFeedbackExplainsSaveWithoutPath()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Unsaved");

    const auto feedback = trackloom::describeAppProjectFileActionResult(
        trackloom::AppProjectFileAction::Save,
        session.save());

    require(!feedback.success,
        "save feedback should report failure when the session has no file path");
    require(feedback.kind == trackloom::AppProjectFileActionFeedbackKind::NeedsSaveAs,
        "save feedback should expose a stable needs-save-as kind");
    require(feedback.message.find("另存为") != std::string::npos,
        "save feedback should tell the user to use save-as");
}

void projectFileActionFeedbackDescribesCanceledOpen()
{
    const auto feedback = trackloom::describeCanceledAppProjectFileAction(
        trackloom::AppProjectFileAction::Open);

    require(!feedback.success,
        "canceled open feedback should not be reported as a successful file action");
    require(feedback.kind == trackloom::AppProjectFileActionFeedbackKind::Canceled,
        "canceled open feedback should expose a stable canceled kind");
    require(feedback.message.find("取消打开") != std::string::npos,
        "canceled open feedback should clearly describe the canceled action");
}

void projectFileActionFeedbackDescribesSuccessfulSaveAs()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "saved-feedback.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Saved Feedback");

    const auto feedback = trackloom::describeAppProjectFileActionResult(
        trackloom::AppProjectFileAction::SaveAs,
        session.saveAs(path));

    require(feedback.success,
        "save-as feedback should report success after a successful save");
    require(feedback.kind == trackloom::AppProjectFileActionFeedbackKind::Success,
        "save-as feedback should expose a stable success kind");
    require(feedback.message.find("另存为") != std::string::npos,
        "save-as feedback should describe the completed save-as action");
}

void projectFileActionFeedbackWarnsAfterSuccessfulSave()
{
    const auto ordinarySuccess = trackloom::describeAppProjectFileActionResult(
        trackloom::AppProjectFileAction::Save,
        trackloom::AppProjectSessionResult::ok());
    const auto warning = std::string("Recovery workspace still needs inspection.");
    const auto retainedPath = std::filesystem::path("C:/recovery/project.trackloom-save-workspace");
    const auto feedback = trackloom::describeAppProjectFileActionResult(
        trackloom::AppProjectFileAction::Save,
        trackloom::AppProjectSessionResult::ok(warning, { retainedPath }));

    require(feedback.success,
        "a saved project with retained recovery data should remain a successful file action");
    require(feedback.kind == trackloom::AppProjectFileActionFeedbackKind::Warning,
        "a saved project with retained recovery data should expose a stable warning kind");
    require(feedback.message.find(ordinarySuccess.message) != std::string::npos,
        "warning feedback should retain the normal successful save message");
    require(feedback.message.find(warning) != std::string::npos,
        "warning feedback should visibly explain the retained recovery data");
}

void projectFileActionWarningPresentationKeepsCompleteUnicodeDetails()
{
    const auto warning = std::string(
        "工程已经安全保存，但清理阶段留下了恢复数据；请在再次保存前逐项检查以下位置。"
        "这段说明刻意较长，用来证明状态栏只保留摘要，而详情不会被截断。");
    const auto firstPathText = std::string(
        "C:/恢复数据/很长的毕业设计工程名称/第一次清理遗留/trackloom-save-workspace");
    const auto secondPathText = std::string(
        "D:/音乐项目/织音-TrackLoom/第二个恢复位置/trackloom-recovery/backup.trackloom");
    const auto firstPath = std::filesystem::path(std::u8string(
        u8"C:/恢复数据/很长的毕业设计工程名称/第一次清理遗留/trackloom-save-workspace"));
    const auto secondPath = std::filesystem::path(std::u8string(
        u8"D:/音乐项目/织音-TrackLoom/第二个恢复位置/trackloom-recovery/backup.trackloom"));
    const auto feedback = trackloom::describeAppProjectFileActionResult(
        trackloom::AppProjectFileAction::Save,
        trackloom::AppProjectSessionResult::ok(warning, { firstPath, secondPath }));

    const auto presentation = trackloom::describeAppProjectFileActionPresentation(feedback);

    require(presentation.showWarningDetails,
        "warning feedback should explicitly instruct the JUCE UI to show warning details");
    require(!presentation.summary.empty() && presentation.summary.size() < presentation.details.size(),
        "warning presentation should keep a concise status summary separate from long details");
    require(presentation.details.find(warning) != std::string::npos,
        "warning presentation details should retain the complete warning text");
    require(presentation.details.find(firstPathText) != std::string::npos
            && presentation.details.find(secondPathText) != std::string::npos,
        "warning presentation details should retain every Unicode recovery path");
}

void projectFileActionOrdinaryPresentationsDoNotRequestWarningDetails()
{
    const auto success = trackloom::describeAppProjectFileActionResult(
        trackloom::AppProjectFileAction::Save,
        trackloom::AppProjectSessionResult::ok());
    const auto failure = trackloom::describeAppProjectFileActionResult(
        trackloom::AppProjectFileAction::Open,
        trackloom::AppProjectSessionResult::fail(
            trackloom::AppProjectSessionFailureReason::OpenFailed,
            "missing project"));
    const auto canceled = trackloom::describeCanceledAppProjectFileAction(
        trackloom::AppProjectFileAction::Open);

    for (const auto* feedback : { &success, &failure, &canceled }) {
        const auto presentation = trackloom::describeAppProjectFileActionPresentation(*feedback);
        require(!presentation.showWarningDetails,
            "success, failure and informational cancellation feedback must not open a warning dialog");
        require(presentation.details.empty(),
            "ordinary file action feedback should not retain stale warning details");
        require(presentation.summary == feedback->message,
            "ordinary file action presentation should preserve the existing status message");
    }
}

void projectSessionSaveAsPropagatesRecoveryWarningToFileActionFeedback()
{
    removeTestWorkspace();
    const auto targetPath = testWorkspace() / "warning-save.trackloom-test";
    injectedSessionRecoveryPathOne = testWorkspace() / "helper-recovery";
    injectedSessionRecoveryPathTwo = testWorkspace() / "workspace-recovery";

    trackloom::AppProjectSession session(saveProjectWithInjectedRecoveryWarning);
    session.createNewProject("Warning Save");
    session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);

    const auto saved = session.saveAs(targetPath);
    const auto feedback = trackloom::describeAppProjectFileActionResult(
        trackloom::AppProjectFileAction::SaveAs,
        saved);

    require(saved.success, "injected successful save should remain successful through the app session");
    require(saved.warning.find("Recovery data requires inspection.") != std::string::npos,
        "app session should retain the core save warning");
    require(saved.recoveryPaths.size() == 2,
        "app session should retain every core recovery path");
    require(saved.recoveryPaths.at(0) == injectedSessionRecoveryPathOne
            && saved.recoveryPaths.at(1) == injectedSessionRecoveryPathTwo,
        "app session should preserve the ordered core recovery paths");
    require(session.currentProjectPath().has_value() && session.currentProjectPath().value() == targetPath,
        "successful warning save should still set the current project path");
    require(!session.isDirty(), "successful warning save should clear dirty state");
    require(feedback.success && feedback.kind == trackloom::AppProjectFileActionFeedbackKind::Warning,
        "successful warning save should produce warning feedback");
    require(feedback.message.find("Recovery data requires inspection.") != std::string::npos,
        "warning feedback should include the core warning text");
    require(feedback.recoveryPaths.size() == 2
            && feedback.recoveryPaths.at(0) == injectedSessionRecoveryPathOne
            && feedback.recoveryPaths.at(1) == injectedSessionRecoveryPathTwo,
        "warning feedback should preserve every structured recovery path");
    const auto presentation = trackloom::describeAppProjectFileActionPresentation(feedback);
    require(presentation.details.find(injectedSessionRecoveryPathOne.string()) != std::string::npos
            && presentation.details.find(injectedSessionRecoveryPathTwo.string()) != std::string::npos,
        "warning presentation should visibly include every structured recovery path");
}

void projectFileActionFeedbackDeduplicatesExactStructuredRecoveryPaths()
{
    const auto prefixPath = std::filesystem::path("C:/recovery/project");
    const auto longerPath = std::filesystem::path("C:/recovery/project-copy");
    const auto feedback = trackloom::describeAppProjectFileActionResult(
        trackloom::AppProjectFileAction::Save,
        trackloom::AppProjectSessionResult::ok(
            "Recovery data requires inspection.",
            { prefixPath, longerPath, prefixPath }));

    require(feedback.success && feedback.kind == trackloom::AppProjectFileActionFeedbackKind::Warning,
        "structured recovery paths should retain successful warning feedback");
    require(feedback.recoveryPaths.size() == 2,
        "feedback should remove only exactly duplicated structured recovery paths");
    require(feedback.recoveryPaths.at(0) == prefixPath
            && feedback.recoveryPaths.at(1) == longerPath,
        "structured recovery path deduplication should preserve order and distinct prefix paths");
}

void projectSessionNullSaveOperationFallsBackToCoreSave()
{
    removeTestWorkspace();
    const auto targetPath = testWorkspace() / "null-save-operation.trackloom-test";

    trackloom::AppProjectSession session(nullptr);
    session.createNewProject("Null Save Operation");
    session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);

    const auto saved = session.saveAs(targetPath);

    require(saved.success, "a null injected save operation should fall back to the core save operation");
    require(std::filesystem::exists(targetPath), "null save operation fallback should write the target project");
    require(session.currentProjectPath().has_value() && session.currentProjectPath().value() == targetPath,
        "null save operation fallback should preserve normal save-as session state");
    require(!session.isDirty(), "null save operation fallback should clear dirty state after saving");
}

void recentProjectsKeepNewestUniquePathsWithinLimit()
{
    trackloom::AppRecentProjects recent(3);
    const auto first = testWorkspace() / "first.trackloom";
    const auto second = testWorkspace() / "second.trackloom";
    const auto third = testWorkspace() / "third.trackloom";
    const auto fourth = testWorkspace() / "fourth.trackloom";

    // 最近工程列表按“最新在前”展示；重复打开同一工程时只移动位置，不保留重复项。
    recent.record(first);
    recent.record(second);
    recent.record(third);
    recent.record(second);
    recent.record(fourth);

    const auto& paths = recent.paths();
    require(paths.size() == 3,
        "recent projects should trim old entries beyond the configured limit");
    require(paths[0] == fourth,
        "most recently recorded project should appear first");
    require(paths[1] == second,
        "recording an existing project should move it near the front without duplication");
    require(paths[2] == third,
        "recent projects should keep remaining entries in newest-first order");
}

void recentProjectsSaveAndLoadUtf8TextFile()
{
    removeTestWorkspace();
    const auto settingsPath = testWorkspace() / "settings" / "recent-projects.txt";
    const auto first = testWorkspace() / "织音草稿.trackloom";
    const auto second = testWorkspace() / "arrangement.trackloom";

    trackloom::AppRecentProjects saved;
    saved.record(first);
    saved.record(second);

    require(trackloom::saveAppRecentProjects(saved, settingsPath),
        "recent projects should save to a simple local settings file");
    require(std::filesystem::exists(settingsPath),
        "saving recent projects should create the settings file");

    const auto loaded = trackloom::loadAppRecentProjects(settingsPath);

    require(loaded.paths().size() == 2,
        "loading recent projects should restore saved entries");
    require(loaded.paths()[0] == second,
        "loaded recent projects should preserve newest-first order");
    require(loaded.paths()[1] == first,
        "loaded recent projects should preserve UTF-8 project paths");
}

void recentProjectsLoadMissingFileAsEmptyList()
{
    removeTestWorkspace();
    const auto missingPath = testWorkspace() / "missing" / "recent-projects.txt";

    const auto loaded = trackloom::loadAppRecentProjects(missingPath);

    require(loaded.paths().empty(),
        "missing recent-project settings should load as an empty list");
}

void recentProjectsStatusDescribesEmptyAndStoredProjects()
{
    trackloom::AppRecentProjects empty;

    const auto emptyStatus = trackloom::describeAppRecentProjects(empty);

    require(emptyStatus.rows.empty(),
        "empty recent projects status should not expose phantom rows");
    require(emptyStatus.emptyMessage.find("暂无最近工程") != std::string::npos,
        "empty recent projects status should explain that no recent projects exist");

    trackloom::AppRecentProjects recent;
    const auto first = testWorkspace() / "first.trackloom";
    const auto second = testWorkspace() / "second.trackloom";
    recent.record(first);
    recent.record(second);

    const auto status = trackloom::describeAppRecentProjects(recent);

    require(status.rows.size() == 2,
        "recent projects status should expose one row per stored project");
    require(status.rows[0].number == 1 && status.rows[0].path == second,
        "recent projects status should keep newest-first ordering");
    require(status.rows[0].displayName == "second.trackloom",
        "recent projects status should expose the file name for compact UI display");
    require(status.rows[0].fullPath == second.string(),
        "recent projects status should expose the full path for tooltips or details");
    require(status.rows[0].summary.find("second.trackloom") != std::string::npos,
        "recent projects status row should include the display file name");
}

void recentProjectsRecordAndSaveUpdatesMemoryAndSettingsFile()
{
    removeTestWorkspace();
    const auto settingsPath = testWorkspace() / "settings" / "recent-projects.txt";
    const auto projectPath = testWorkspace() / "saved-project.trackloom";

    trackloom::AppRecentProjects recent;
    const auto result = trackloom::recordAndSaveAppRecentProject(
        recent,
        projectPath,
        settingsPath);

    require(result.recorded,
        "record-and-save should update the in-memory recent project list");
    require(result.saved,
        "record-and-save should persist the recent project list when settings path is writable");
    require(recent.paths().size() == 1 && recent.paths()[0] == projectPath,
        "record-and-save should store the project path in memory");

    const auto loaded = trackloom::loadAppRecentProjects(settingsPath);

    require(loaded.paths().size() == 1 && loaded.paths()[0] == projectPath,
        "record-and-save should persist a reloadable recent project path");
}

void recentProjectsRecordAndSaveKeepsMemoryWhenSettingsCannotSave()
{
    const auto projectPath = testWorkspace() / "unsaved-settings.trackloom";

    trackloom::AppRecentProjects recent;
    const auto result = trackloom::recordAndSaveAppRecentProject(
        recent,
        projectPath,
        {});

    require(result.recorded,
        "record-and-save should still update memory when settings persistence fails");
    require(!result.saved,
        "record-and-save should report failed persistence separately from memory update");
    require(recent.paths().size() == 1 && recent.paths()[0] == projectPath,
        "failed settings persistence should not roll back the in-memory recent project");
}

void recentProjectsOpenByNumberLoadsProjectAndPromotesSelection()
{
    removeTestWorkspace();
    const auto settingsPath = testWorkspace() / "settings" / "recent-projects.txt";
    const auto firstPath = testWorkspace() / "first-recent.trackloom";
    const auto secondPath = testWorkspace() / "second-recent.trackloom";

    trackloom::AppProjectSession firstProject;
    firstProject.createNewProject("First Recent");
    require(firstProject.saveAs(firstPath).success,
        "recent open test should save the first project");

    trackloom::AppProjectSession secondProject;
    secondProject.createNewProject("Second Recent");
    require(secondProject.saveAs(secondPath).success,
        "recent open test should save the second project");

    trackloom::AppRecentProjects recent;
    recent.record(firstPath);
    recent.record(secondPath);

    trackloom::AppProjectSession session;
    session.createNewProject("Current");
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host);
    trackloom::AppLoopPlaybackState loopState;

    // 编号 2 打开当前列表里的第二个工程；打开后它应移动到最近列表最前。
    const auto feedback = trackloom::openAppRecentProjectByNumber(
        session,
        playback,
        loopState,
        recent,
        2,
        settingsPath);

    require(feedback.success,
        "opening a selected recent project should succeed");
    require(feedback.kind == trackloom::AppRecentProjectOpenFeedbackKind::Success,
        "successful recent open should expose a stable success kind");
    require(session.project().name() == "First Recent",
        "recent open should load the selected project into the app session");
    require(session.currentProjectPath().has_value() && session.currentProjectPath().value() == firstPath,
        "recent open should set the current project path");
    require(!session.isDirty(),
        "recent open should leave the loaded project clean");
    require(recent.paths().size() == 2 && recent.paths()[0] == firstPath,
        "recent open should promote the opened project to the front");

    const auto persisted = trackloom::loadAppRecentProjects(settingsPath);
    require(persisted.paths().size() == 2 && persisted.paths()[0] == firstPath,
        "recent open should persist the promoted recent-project order");
}

void recentProjectsOpenByNumberRejectsDirtySessionWithoutMutation()
{
    removeTestWorkspace();
    const auto recentPath = testWorkspace() / "dirty-reject.trackloom";

    trackloom::AppProjectSession savedProject;
    savedProject.createNewProject("Recent Clean");
    require(savedProject.saveAs(recentPath).success,
        "dirty recent open test should save the recent project");

    trackloom::AppRecentProjects recent;
    recent.record(recentPath);

    trackloom::AppProjectSession session;
    session.createNewProject("Dirty Current");
    session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host);
    trackloom::AppLoopPlaybackState loopState;

    const auto feedback = trackloom::openAppRecentProjectByNumber(
        session,
        playback,
        loopState,
        recent,
        1,
        testWorkspace() / "settings" / "recent-projects.txt");

    require(!feedback.success,
        "recent open should reject a dirty current session");
    require(feedback.kind == trackloom::AppRecentProjectOpenFeedbackKind::DirtyProject,
        "dirty recent open should expose a stable dirty-project kind");
    require(session.project().name() == "Dirty Current",
        "dirty recent open should keep the current project");
    require(session.isDirty(),
        "dirty recent open should keep dirty state");
    require(recent.paths().size() == 1 && recent.paths()[0] == recentPath,
        "dirty recent open should not mutate the recent-project list");
}

void recentProjectsOpenByNumberRejectsMissingSelection()
{
    trackloom::AppProjectSession session;
    session.createNewProject("No Selection");
    trackloom::AppRecentProjects recent;
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host);
    trackloom::AppLoopPlaybackState loopState;

    const auto feedback = trackloom::openAppRecentProjectByNumber(
        session,
        playback,
        loopState,
        recent,
        1,
        testWorkspace() / "settings" / "recent-projects.txt");

    require(!feedback.success,
        "recent open should reject a missing recent-project number");
    require(feedback.kind == trackloom::AppRecentProjectOpenFeedbackKind::MissingRecentProject,
        "missing recent open should expose a stable missing-selection kind");
    require(session.project().name() == "No Selection",
        "missing recent open should keep the current project");
}

void recentProjectsOpenByNumberRejectsMissingFileWithoutMutation()
{
    removeTestWorkspace();
    const auto missingPath = testWorkspace() / "missing-recent.trackloom";

    trackloom::AppRecentProjects recent;
    recent.record(missingPath);

    trackloom::AppProjectSession session;
    session.createNewProject("Keep Current");
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host);
    trackloom::AppLoopPlaybackState loopState;

    const auto feedback = trackloom::openAppRecentProjectByNumber(
        session,
        playback,
        loopState,
        recent,
        1,
        testWorkspace() / "settings" / "recent-projects.txt");

    require(!feedback.success,
        "recent open should fail when the remembered project file is missing");
    require(feedback.kind == trackloom::AppRecentProjectOpenFeedbackKind::OpenFailed,
        "missing file recent open should expose an open-failed kind");
    require(session.project().name() == "Keep Current",
        "missing file recent open should keep the current project");
    require(recent.paths().size() == 1 && recent.paths()[0] == missingPath,
        "missing file recent open should not reorder the recent-project list");
}

void mainMenuDescribesFileAndPlaybackCommands()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Menu Snapshot");
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host);
    trackloom::AppRecentProjects recent;

    const auto menu = trackloom::describeAppMainMenu(session, playback, recent);

    require(menu.groups.size() == 6,
        "main menu should expose file, edit, track, clip, playback and tools menu groups");
    require(menu.groups[0].name == "文件",
        "first main menu group should be the file menu");
    require(menu.groups[1].name == "编辑",
        "second main menu group should be the edit menu");
    require(menu.groups[2].name == "轨道",
        "third main menu group should be the track menu");
    require(menu.groups[3].name == "片段",
        "fourth main menu group should be the clip menu");
    require(menu.groups[4].name == "播放",
        "fifth main menu group should be the playback menu");
    require(menu.groups[5].name == "工具",
        "sixth main menu group should be the tools menu");
    require(menu.groups[0].items.size() == 6,
        "file menu should include project commands, a separator and an empty recent-project row");
    require(menu.groups[0].items[0].commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::NewProject),
        "file menu should expose a stable command id for new project");
    require(menu.groups[0].items[2].label == "保存",
        "file menu should expose the save command label");
    require(menu.groups[0].items[2].enabled,
        "save stays enabled because the command can redirect unsaved projects to save-as feedback");
    require(menu.groups[0].items[4].separator,
        "file menu should separate regular file commands from recent projects");
    require(!menu.groups[0].items[5].enabled && menu.groups[0].items[5].commandId == 0,
        "empty recent-project menu row should be disabled and have no command id");
    require(menu.groups[1].items.size() == 2,
        "edit menu should expose undo and redo commands");
    require(menu.groups[1].items[0].commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::UndoProject),
        "edit menu should expose a stable command id for undo");
    require(menu.groups[1].items[0].label == "撤销",
        "edit menu should expose the undo command label");
    require(!menu.groups[1].items[0].enabled,
        "undo command should be disabled before there is undo history");
    require(menu.groups[1].items[1].commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::RedoProject),
        "edit menu should expose a stable command id for redo");
    require(!menu.groups[1].items[1].enabled,
        "redo command should be disabled before there is redo history");
    require(menu.groups[2].items.size() == 3,
        "track menu should expose the current no-selection track creation commands");
    require(menu.groups[2].items[0].commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::AddInstrumentTrack),
        "track menu should expose a stable command id for adding an instrument track");
    require(menu.groups[2].items[0].label == "添加乐器轨",
        "track menu should expose the add-instrument-track label");
    require(menu.groups[2].items[0].enabled,
        "add-instrument-track command should be enabled without a current selection");
    require(menu.groups[2].items[1].commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::AddAudioTrack),
        "track menu should expose a stable command id for adding an audio track");
    require(menu.groups[2].items[1].label == "添加音频轨",
        "track menu should expose the add-audio-track label");
    require(menu.groups[2].items[1].enabled,
        "add-audio-track command should be enabled without a current selection");
    require(menu.groups[2].items[2].commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::AddFolderTrack),
        "track menu should expose a stable command id for adding a folder track");
    require(menu.groups[2].items[2].label == "添加文件夹轨",
        "track menu should expose the add-folder-track label");
    require(menu.groups[2].items[2].enabled,
        "add-folder-track command should be enabled without a current selection");
    struct ExpectedClipMenuItem {
        trackloom::AppMainMenuCommand command;
        const char* label;
    };
    const ExpectedClipMenuItem expectedClipItems[] = {
        { trackloom::AppMainMenuCommand::RenameSelectedMidiClip, "重命名所选 MIDI 片段" },
        { trackloom::AppMainMenuCommand::DeleteSelectedMidiClip, "删除所选 MIDI 片段" },
        { trackloom::AppMainMenuCommand::DuplicateSelectedMidiClip, "复制所选 MIDI 片段" },
        { trackloom::AppMainMenuCommand::SplitSelectedMidiClip, "拆分所选 MIDI 片段" },
        { trackloom::AppMainMenuCommand::MoveSelectedMidiClipToTargetTrack, "移动所选 MIDI 片段到目标乐器轨" },
        { trackloom::AppMainMenuCommand::MoveSelectedMidiClipLeft, "左移所选 MIDI 片段" },
        { trackloom::AppMainMenuCommand::MoveSelectedMidiClipRight, "右移所选 MIDI 片段" },
        { trackloom::AppMainMenuCommand::TrimSelectedMidiClipEnd, "缩短所选 MIDI 片尾" },
        { trackloom::AppMainMenuCommand::ExtendSelectedMidiClipEnd, "延长所选 MIDI 片尾" },
        { trackloom::AppMainMenuCommand::TrimSelectedMidiClipStart, "缩短所选 MIDI 片头" },
        { trackloom::AppMainMenuCommand::ExtendSelectedMidiClipStart, "延长所选 MIDI 片头" },
        { trackloom::AppMainMenuCommand::RenameSelectedAudioClip, "重命名所选音频片段" },
        { trackloom::AppMainMenuCommand::DeleteSelectedAudioClip, "删除所选音频片段" },
        { trackloom::AppMainMenuCommand::DuplicateSelectedAudioClip, "复制所选音频片段" },
        { trackloom::AppMainMenuCommand::SplitSelectedAudioClip, "拆分所选音频片段" },
        { trackloom::AppMainMenuCommand::MoveSelectedAudioClipToTargetTrack, "移动所选音频片段到目标音频轨" },
        { trackloom::AppMainMenuCommand::MoveSelectedAudioClipLeft, "左移所选音频片段" },
        { trackloom::AppMainMenuCommand::MoveSelectedAudioClipRight, "右移所选音频片段" },
        { trackloom::AppMainMenuCommand::TrimSelectedAudioClipEnd, "缩短所选音频片尾" },
        { trackloom::AppMainMenuCommand::ExtendSelectedAudioClipEnd, "延长所选音频片尾" },
        { trackloom::AppMainMenuCommand::TrimSelectedAudioClipStart, "缩短所选音频片头" },
        { trackloom::AppMainMenuCommand::ExtendSelectedAudioClipStart, "延长所选音频片头" }
    };
    require(menu.groups[3].items.size() == sizeof(expectedClipItems) / sizeof(expectedClipItems[0]),
        "clip menu should expose the current selected-clip commands");
    for (std::size_t index = 0; index < sizeof(expectedClipItems) / sizeof(expectedClipItems[0]); ++index) {
        require(menu.groups[3].items[index].commandId
                == trackloom::appMainMenuCommandId(expectedClipItems[index].command),
            "clip menu should expose stable selected-clip command ids in order");
        require(menu.groups[3].items[index].label == expectedClipItems[index].label,
            "clip menu should expose selected-clip command labels in order");
        require(!menu.groups[3].items[index].enabled,
            "selected clip commands should be disabled without a selected clip");
    }
    require(menu.groups[4].items[0].label == "播放",
        "playback menu should expose the play command label");
    require(menu.groups[4].items[0].enabled,
        "play command should be enabled while playback is stopped");
    require(!menu.groups[4].items[1].enabled,
        "stop command should be disabled while playback is stopped");
    require(!menu.groups[4].items[2].enabled,
        "rewind command should be disabled before the playback head moves");
    require(menu.groups[5].items.size() == 3,
        "tools menu should expose the current utility commands");
    require(menu.groups[5].items[0].commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::OpenCommandPalette),
        "tools menu should expose a stable command id for opening the command palette");
    require(menu.groups[5].items[0].label == "命令面板...",
        "tools menu should expose the command palette label");
    require(menu.groups[5].items[0].enabled,
        "command palette command should be enabled because it only opens local UI state");
    require(menu.groups[5].items[1].commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::OpenShortcutStatus),
        "tools menu should expose a stable command id for opening shortcut status");
    require(menu.groups[5].items[1].label == "快捷键状态...",
        "tools menu should expose the shortcut status label");
    require(menu.groups[5].items[1].enabled,
        "shortcut status command should be enabled because it only reads local settings state");
}

void mainMenuReflectsUndoRedoHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Edit Menu History");
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host);
    trackloom::AppRecentProjects recent;

    const auto commandResult = session.executeProjectCommand(
        std::make_unique<trackloom::AddTrackCommand>("Lead", trackloom::TrackType::Instrument));
    require(commandResult.success,
        "edit menu history test should create undoable project history");

    const auto afterEdit = trackloom::describeAppMainMenu(session, playback, recent);
    const auto& editItemsAfterEdit = afterEdit.groups[1].items;
    require(editItemsAfterEdit[0].enabled,
        "undo command should be enabled after an undoable edit");
    require(!editItemsAfterEdit[1].enabled,
        "redo command should stay disabled until the user undoes an edit");

    require(session.undoProjectEdit(),
        "edit menu history test should create redo history");

    const auto afterUndo = trackloom::describeAppMainMenu(session, playback, recent);
    const auto& editItemsAfterUndo = afterUndo.groups[1].items;
    require(!editItemsAfterUndo[0].enabled,
        "undo command should be disabled after the only edit is undone");
    require(editItemsAfterUndo[1].enabled,
        "redo command should be enabled after undoing an edit");
}

void mainMenuReflectsSelectedTrackCommands()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Selected Track Menu");
    const auto first = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto second = session.editProject().createTrack("Pad", trackloom::TrackType::Instrument);
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host);
    trackloom::AppRecentProjects recent;

    trackloom::AppMainMenuSelection selection;
    selection.selectedInstrumentTrackId = second.id;
    selection.selectedAudioTrackId = audio.id;

    const auto menu = trackloom::describeAppMainMenu(session, playback, recent, selection);
    const auto& trackItems = menu.groups[2].items;

    require(trackItems.size() == 20,
        "track menu should include creation commands, selected-instrument commands and selected-audio commands");
    require(trackItems[3].separator && trackItems[12].separator,
        "track menu should separate creation commands from selection-dependent command groups");
    require(trackItems[4].commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::RenameSelectedInstrumentTrack),
        "track menu should expose a stable command id for selected instrument track rename");
    require(trackItems[4].label == "重命名所选乐器轨" && trackItems[4].enabled,
        "selected instrument track rename should be enabled for a valid selected instrument track");
    require(trackItems[5].commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::DeleteSelectedInstrumentTrack),
        "track menu should expose a stable command id for selected instrument track delete");
    require(trackItems[5].enabled,
        "selected instrument track delete should be enabled for a valid selected instrument track");
    require(trackItems[6].commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::MoveSelectedInstrumentTrackUp),
        "track menu should expose a stable command id for selected instrument track move-up");
    require(trackItems[6].enabled,
        "selected instrument track move-up should be enabled when the selected track is not first");
    require(trackItems[7].commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::MoveSelectedInstrumentTrackDown),
        "track menu should expose a stable command id for selected instrument track move-down");
    require(trackItems[7].enabled,
        "selected instrument track move-down should be enabled when the selected track is not last");
    require(trackItems[8].commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::ToggleSelectedInstrumentTrackMute),
        "track menu should expose a stable command id for selected instrument track mute toggle");
    require(trackItems[8].label == "切换所选乐器轨静音" && trackItems[8].enabled,
        "selected instrument track mute toggle should be enabled for a valid selected instrument track");
    require(trackItems[9].commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::ToggleSelectedInstrumentTrackSolo),
        "track menu should expose a stable command id for selected instrument track solo toggle");
    require(trackItems[9].enabled,
        "selected instrument track solo toggle should be enabled for a valid selected instrument track");
    require(trackItems[10].commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::ToggleSelectedInstrumentTrackDisabled),
        "track menu should expose a stable command id for selected instrument track disabled toggle");
    require(trackItems[10].enabled,
        "selected instrument track disabled toggle should be enabled for a valid selected instrument track");
    require(trackItems[11].commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::ToggleSelectedInstrumentTrackHidden),
        "track menu should expose a stable command id for selected instrument track hidden toggle");
    require(trackItems[11].enabled,
        "selected instrument track hidden toggle should be enabled for a valid selected instrument track");
    require(trackItems[13].commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::DeleteSelectedAudioTrack),
        "track menu should expose a stable command id for selected audio track delete");
    require(trackItems[13].label == "删除所选音频轨" && trackItems[13].enabled,
        "selected audio track delete should be enabled for a valid selected audio track");
    require(trackItems[14].commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::MoveSelectedAudioTrackUp),
        "track menu should expose a stable command id for selected audio track move-up");
    require(trackItems[14].enabled,
        "selected audio track move-up should be enabled when the selected track is not first");
    require(trackItems[15].commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::MoveSelectedAudioTrackDown),
        "track menu should expose a stable command id for selected audio track move-down");
    require(!trackItems[15].enabled,
        "selected audio track move-down should be disabled when the selected track is last");
    require(trackItems[16].commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::ToggleSelectedAudioTrackMute),
        "track menu should expose a stable command id for selected audio track mute toggle");
    require(trackItems[16].enabled,
        "selected audio track mute toggle should be enabled for a valid selected audio track");
    require(trackItems[17].commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::ToggleSelectedAudioTrackSolo),
        "track menu should expose a stable command id for selected audio track solo toggle");
    require(trackItems[17].enabled,
        "selected audio track solo toggle should be enabled for a valid selected audio track");
    require(trackItems[18].commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::ToggleSelectedAudioTrackDisabled),
        "track menu should expose a stable command id for selected audio track disabled toggle");
    require(trackItems[18].enabled,
        "selected audio track disabled toggle should be enabled for a valid selected audio track");
    require(trackItems[19].commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::ToggleSelectedAudioTrackHidden),
        "track menu should expose a stable command id for selected audio track hidden toggle");
    require(trackItems[19].enabled,
        "selected audio track hidden toggle should be enabled for a valid selected audio track");

    selection.selectedInstrumentTrackId = first.id;
    selection.selectedAudioTrackId = first.id;
    const auto boundaryMenu = trackloom::describeAppMainMenu(session, playback, recent, selection);
    const auto& boundaryTrackItems = boundaryMenu.groups[2].items;
    require(!boundaryTrackItems[6].enabled && boundaryTrackItems[7].enabled,
        "selected instrument track move-up should be disabled at the top boundary");
    require(boundaryTrackItems[8].enabled && boundaryTrackItems[11].enabled,
        "selected instrument track state toggles should stay enabled at move boundaries");
    require(!boundaryTrackItems[13].enabled
            && !boundaryTrackItems[14].enabled
            && !boundaryTrackItems[15].enabled
            && !boundaryTrackItems[16].enabled
            && !boundaryTrackItems[17].enabled
            && !boundaryTrackItems[18].enabled
            && !boundaryTrackItems[19].enabled,
        "selected audio track commands should be disabled when the selected id is not an audio track");
}

void mainMenuReflectsSelectedClipCommands()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Selected Clip Menu");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto audioTrack = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto targetInstrument = session.editProject().createTrack("Pad", trackloom::TrackType::Instrument);
    const auto targetAudioTrack = session.editProject().createTrack("Drums", trackloom::TrackType::Audio);
    const auto midiClip = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    const auto audioClip = trackloom::createDefaultAudioClipOnTrack(session, audioTrack.id);
    require(midiClip.success && audioClip.success,
        "selected clip menu test should create MIDI and audio clips");

    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host);
    trackloom::AppRecentProjects recent;
    trackloom::AppMainMenuSelection selection;
    selection.selectedInstrumentTrackId = targetInstrument.id;
    selection.selectedAudioTrackId = targetAudioTrack.id;
    selection.selectedMidiClipId = midiClip.clipId;
    selection.selectedAudioClipId = audioClip.clipId;

    const auto menu = trackloom::describeAppMainMenu(session, playback, recent, selection);
    const auto& clipItems = menu.groups[3].items;

    struct ExpectedClipMenuItem {
        trackloom::AppMainMenuCommand command;
        const char* label;
    };
    const ExpectedClipMenuItem expectedClipItems[] = {
        { trackloom::AppMainMenuCommand::RenameSelectedMidiClip, "重命名所选 MIDI 片段" },
        { trackloom::AppMainMenuCommand::DeleteSelectedMidiClip, "删除所选 MIDI 片段" },
        { trackloom::AppMainMenuCommand::DuplicateSelectedMidiClip, "复制所选 MIDI 片段" },
        { trackloom::AppMainMenuCommand::SplitSelectedMidiClip, "拆分所选 MIDI 片段" },
        { trackloom::AppMainMenuCommand::MoveSelectedMidiClipToTargetTrack, "移动所选 MIDI 片段到目标乐器轨" },
        { trackloom::AppMainMenuCommand::MoveSelectedMidiClipLeft, "左移所选 MIDI 片段" },
        { trackloom::AppMainMenuCommand::MoveSelectedMidiClipRight, "右移所选 MIDI 片段" },
        { trackloom::AppMainMenuCommand::TrimSelectedMidiClipEnd, "缩短所选 MIDI 片尾" },
        { trackloom::AppMainMenuCommand::ExtendSelectedMidiClipEnd, "延长所选 MIDI 片尾" },
        { trackloom::AppMainMenuCommand::TrimSelectedMidiClipStart, "缩短所选 MIDI 片头" },
        { trackloom::AppMainMenuCommand::ExtendSelectedMidiClipStart, "延长所选 MIDI 片头" },
        { trackloom::AppMainMenuCommand::RenameSelectedAudioClip, "重命名所选音频片段" },
        { trackloom::AppMainMenuCommand::DeleteSelectedAudioClip, "删除所选音频片段" },
        { trackloom::AppMainMenuCommand::DuplicateSelectedAudioClip, "复制所选音频片段" },
        { trackloom::AppMainMenuCommand::SplitSelectedAudioClip, "拆分所选音频片段" },
        { trackloom::AppMainMenuCommand::MoveSelectedAudioClipToTargetTrack, "移动所选音频片段到目标音频轨" },
        { trackloom::AppMainMenuCommand::MoveSelectedAudioClipLeft, "左移所选音频片段" },
        { trackloom::AppMainMenuCommand::MoveSelectedAudioClipRight, "右移所选音频片段" },
        { trackloom::AppMainMenuCommand::TrimSelectedAudioClipEnd, "缩短所选音频片尾" },
        { trackloom::AppMainMenuCommand::ExtendSelectedAudioClipEnd, "延长所选音频片尾" },
        { trackloom::AppMainMenuCommand::TrimSelectedAudioClipStart, "缩短所选音频片头" },
        { trackloom::AppMainMenuCommand::ExtendSelectedAudioClipStart, "延长所选音频片头" }
    };
    require(clipItems.size() == sizeof(expectedClipItems) / sizeof(expectedClipItems[0]),
        "clip menu should include selected MIDI and selected audio clip edit commands");
    for (std::size_t index = 0; index < sizeof(expectedClipItems) / sizeof(expectedClipItems[0]); ++index) {
        require(clipItems[index].commandId == trackloom::appMainMenuCommandId(expectedClipItems[index].command),
            "clip menu should expose stable selected-clip command ids in order");
        require(clipItems[index].label == expectedClipItems[index].label,
            "clip menu should expose selected-clip command labels in order");
        require(clipItems[index].enabled,
            "selected clip commands should be enabled when their clip and target selections are valid");
    }

    selection.selectedInstrumentTrackId = instrument.id;
    selection.selectedAudioTrackId = audioTrack.id;
    const auto sameTrackMenu = trackloom::describeAppMainMenu(session, playback, recent, selection);
    require(!sameTrackMenu.groups[3].items[4].enabled && !sameTrackMenu.groups[3].items[15].enabled,
        "selected clip move-to-target commands should be disabled when the target is already the clip owner");

    selection.selectedMidiClipId = audioClip.clipId;
    selection.selectedAudioClipId = midiClip.clipId;
    const auto invalidMenu = trackloom::describeAppMainMenu(session, playback, recent, selection);
    const auto& invalidClipItems = invalidMenu.groups[3].items;
    for (const auto& item : invalidClipItems) {
        require(!item.enabled,
            "selected clip edit commands should be disabled when selected clip ids have the wrong type");
    }
}

void mainMenuReflectsPlayingTransportState()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Playing Menu Snapshot");
    FakeRealtimePlaybackHost host;
    std::latch built(1);
    trackloom::AppPlaybackController playback(
        host,
        [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
            auto result = makeFakePreparedPlan(request);
            built.count_down();
            return result;
        });
    trackloom::AppRecentProjects recent;

    require(trackloom::startAppPlayback(playback, session, disabledLoopState).success,
        "playing menu test should start preparing playback");
    built.wait();
    pollPlaybackUntilWorkerSettles(playback, session);
    host.setRealtimePosition(512, 512);
    playback.poll(session, disabledLoopState);

    const auto menu = trackloom::describeAppMainMenu(session, playback, recent);
    const auto& playbackItems = menu.groups[4].items;
    require(!playbackItems[0].enabled,
        "play command should be disabled while playback is running");
    require(playbackItems[1].enabled,
        "stop command should be enabled while playback is running");
    require(playbackItems[2].enabled,
        "rewind command should be enabled after the host playback head moves");
}
void mainMenuListsRecentProjectsWithStableCommandIds()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Recent Menu Snapshot");
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host);
    trackloom::AppRecentProjects recent;
    const auto first = testWorkspace() / "first-menu.trackloom";
    const auto second = testWorkspace() / "second-menu.trackloom";
    recent.record(first);
    recent.record(second);

    const auto menu = trackloom::describeAppMainMenu(session, playback, recent);
    const auto& fileItems = menu.groups[0].items;

    require(fileItems.size() == 7,
        "file menu should append one row for each recent project after the separator");
    require(fileItems[5].label.find("second-menu.trackloom") != std::string::npos,
        "recent-project menu should keep newest project first");
    require(fileItems[5].commandId == trackloom::appMainMenuRecentProjectCommandId(1),
        "first recent-project row should use a stable command id derived from visible number");
    require(fileItems[6].commandId == trackloom::appMainMenuRecentProjectCommandId(2),
        "second recent-project row should use a stable command id derived from visible number");
    require(trackloom::appMainMenuRecentProjectNumberFromCommandId(fileItems[5].commandId).value_or(0) == 1,
        "recent-project command id should round-trip back to its visible number");
    require(!trackloom::appMainMenuRecentProjectNumberFromCommandId(
                trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SaveProject)).has_value(),
        "regular menu command ids should not be mistaken for recent-project ids");
}

void commandPaletteFlattensMenuCommandsWithoutSeparatorsOrInfoRows()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Palette Snapshot");
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host);
    trackloom::AppRecentProjects recent;

    const auto menu = trackloom::describeAppMainMenu(session, playback, recent);
    const auto palette = trackloom::describeAppCommandPalette(menu);

    require(palette.items.size() == 37,
        "command palette should include menu commands but skip separators and disabled info rows");
    require(palette.items[0].groupName == "文件" && palette.items[0].label == "新建工程",
        "command palette should preserve the file menu group and command label");
    require(palette.items[0].commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::NewProject),
        "command palette should preserve stable menu command ids");
    require(palette.items[4].groupName == "编辑" && palette.items[4].label == "撤销",
        "command palette should keep disabled edit commands visible for discoverability");
    require(!palette.items[4].enabled,
        "command palette should preserve disabled command state");
    require(palette.items[6].groupName == "轨道" && palette.items[6].label == "添加乐器轨",
        "command palette should include the current no-selection track creation commands");
    require(palette.items[9].groupName == "片段" && palette.items[9].label == "重命名所选 MIDI 片段",
        "command palette should include disabled selected-clip commands for discoverability");
    require(palette.items[11].groupName == "片段" && palette.items[11].label == "复制所选 MIDI 片段",
        "command palette should include disabled selected MIDI clip duplicate for discoverability");
    require(palette.items[12].groupName == "片段" && palette.items[12].label == "拆分所选 MIDI 片段",
        "command palette should include disabled selected MIDI clip split for discoverability");
    require(palette.items[13].groupName == "片段" && palette.items[13].label == "移动所选 MIDI 片段到目标乐器轨",
        "command palette should include disabled selected MIDI clip move-to-target for discoverability");
    require(palette.items[14].groupName == "片段" && palette.items[14].label == "左移所选 MIDI 片段",
        "command palette should include disabled selected MIDI clip move-left for discoverability");
    require(palette.items[16].groupName == "片段" && palette.items[16].label == "缩短所选 MIDI 片尾",
        "command palette should include disabled selected MIDI clip trim-end for discoverability");
    require(palette.items[17].groupName == "片段" && palette.items[17].label == "延长所选 MIDI 片尾",
        "command palette should include disabled selected MIDI clip extend-end for discoverability");
    require(palette.items[18].groupName == "片段" && palette.items[18].label == "缩短所选 MIDI 片头",
        "command palette should include disabled selected MIDI clip trim-start for discoverability");
    require(palette.items[19].groupName == "片段" && palette.items[19].label == "延长所选 MIDI 片头",
        "command palette should include disabled selected MIDI clip extend-start for discoverability");
    require(palette.items[31].groupName == "播放" && palette.items[31].label == "播放",
        "command palette should preserve playback commands after clip commands");
    require(palette.items[34].groupName == "工具" && palette.items[34].label == "命令面板...",
        "command palette should include the tools command for reopening itself by search");
    require(palette.items[35].groupName == "工具" && palette.items[35].label == "快捷键状态...",
        "command palette should include the shortcut status tool command for discoverability");
    require(palette.items[35].commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::OpenShortcutStatus),
        "command palette should preserve the shortcut status stable command id");
}

void commandPaletteIncludesRecentProjectsAndFiltersByQuery()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Palette Recent Snapshot");
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host);
    trackloom::AppRecentProjects recent;
    recent.record(testWorkspace() / "palette-first.trackloom");

    const auto menu = trackloom::describeAppMainMenu(session, playback, recent);
    const auto palette = trackloom::describeAppCommandPalette(menu);
    const auto recentCommands = trackloom::filterAppCommandPalette(palette, "palette-first");
    const auto trackCommands = trackloom::filterAppCommandPalette(palette, "轨道");
    const auto emptyInfoRows = trackloom::filterAppCommandPalette(palette, "暂无最近工程");

    require(recentCommands.items.size() == 1,
        "command palette search should find dynamic recent-project commands by label");
    require(recentCommands.items[0].commandId == trackloom::appMainMenuRecentProjectCommandId(1),
        "command palette should preserve recent-project dynamic command ids");
    require(trackCommands.items.size() == 3,
        "command palette search should find commands by their menu group name");
    require(emptyInfoRows.items.empty(),
        "command palette search should not expose disabled menu info rows as commands");
}

void commandPaletteSelectsFirstEnabledCommandForQuery()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Palette Select Snapshot");
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host);
    trackloom::AppRecentProjects recent;

    const auto menu = trackloom::describeAppMainMenu(session, playback, recent);
    const auto palette = trackloom::describeAppCommandPalette(menu);
    const auto selectedTrack = trackloom::selectFirstExecutableAppCommand(palette, "轨道");
    const auto selectedPlayback = trackloom::selectFirstExecutableAppCommand(palette, "播放");

    require(selectedTrack.has_value(),
        "command palette should select an executable command when a query matches enabled commands");
    require(selectedTrack->commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::AddInstrumentTrack),
        "command palette should select the first enabled command in filtered order");
    require(selectedTrack->label == "添加乐器轨" && selectedTrack->groupName == "轨道",
        "command palette selection should preserve the chosen command display fields");
    require(selectedPlayback.has_value()
            && selectedPlayback->commandId
                == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::PlayProject),
        "command palette should skip disabled playback commands and select the enabled play command");
}

void commandPaletteSelectionSkipsDisabledAndMissingMatches()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Palette Disabled Snapshot");
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host);
    trackloom::AppRecentProjects recent;

    const auto menu = trackloom::describeAppMainMenu(session, playback, recent);
    const auto palette = trackloom::describeAppCommandPalette(menu);
    const auto disabledEditCommand = trackloom::selectFirstExecutableAppCommand(palette, "撤销");
    const auto missingCommand = trackloom::selectFirstExecutableAppCommand(palette, "不存在的命令");

    require(!disabledEditCommand.has_value(),
        "command palette should not select a command when all matching commands are disabled");
    require(!missingCommand.has_value(),
        "command palette should return no selection when the query matches no command");
}

void commandPaletteSelectionReportsSelectedResultKind()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Palette Result Snapshot");
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host);
    trackloom::AppRecentProjects recent;

    const auto menu = trackloom::describeAppMainMenu(session, playback, recent);
    const auto palette = trackloom::describeAppCommandPalette(menu);
    const auto selectedPlayback = trackloom::selectAppCommandPaletteItem(palette, "播放");

    require(selectedPlayback.kind == trackloom::AppCommandPaletteSelectionResultKind::Selected,
        "command palette selection result should report when an executable command is selected");
    require(selectedPlayback.item.has_value()
            && selectedPlayback.item->commandId
                == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::PlayProject),
        "selected command palette result should include the executable command item");
}

void commandPaletteSelectionDistinguishesDisabledMatchesFromMissingMatches()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Palette Result Failure Snapshot");
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host);
    trackloom::AppRecentProjects recent;

    const auto menu = trackloom::describeAppMainMenu(session, playback, recent);
    const auto palette = trackloom::describeAppCommandPalette(menu);
    const auto disabledUndo = trackloom::selectAppCommandPaletteItem(palette, "撤销");
    const auto missingCommand = trackloom::selectAppCommandPaletteItem(palette, "不存在的命令");

    require(disabledUndo.kind == trackloom::AppCommandPaletteSelectionResultKind::OnlyDisabledMatches,
        "command palette selection should distinguish disabled matches from missing matches");
    require(!disabledUndo.item.has_value(),
        "disabled command palette matches should not be treated as executable selections");
    require(missingCommand.kind == trackloom::AppCommandPaletteSelectionResultKind::NoMatchingCommand,
        "command palette selection should report when a query matches no command at all");
    require(!missingCommand.item.has_value(),
        "missing command palette queries should not carry a stale selected item");
}

void commandPaletteActivationExecutesSelectedEnabledCommandThroughDispatcher()
{
    trackloom::AppCommandPaletteStatus palette;
    palette.items.push_back({
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::AddInstrumentTrack),
        true,
        "轨道",
        "添加乐器轨",
        {}
    });

    int addInstrumentTrackCalls = 0;
    trackloom::AppCommandHandlers handlers;
    handlers.addInstrumentTrack = [&] { ++addInstrumentTrackCalls; };

    const auto result = trackloom::activateAppCommandPaletteCommand(palette, "乐器", handlers);

    require(result.executed,
        "command palette activation should execute a selected enabled command");
    require(result.kind == trackloom::AppCommandPaletteActivationResultKind::Executed,
        "command palette activation should expose a stable executed result kind");
    require(result.dispatch.command == trackloom::AppCommandKind::AddInstrumentTrack,
        "command palette activation should execute through AppCommandDispatcher");
    require(addInstrumentTrackCalls == 1,
        "command palette activation should call the selected command handler exactly once");
}

void commandPaletteActivationDoesNotDispatchDisabledOrMissingCommands()
{
    trackloom::AppCommandPaletteStatus palette;
    palette.items.push_back({
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::UndoProject),
        false,
        "编辑",
        "撤销",
        "Ctrl+Z"
    });

    int undoCalls = 0;
    trackloom::AppCommandHandlers handlers;
    handlers.undoProject = [&] { ++undoCalls; };

    const auto disabled = trackloom::activateAppCommandPaletteCommand(palette, "撤销", handlers);
    const auto missing = trackloom::activateAppCommandPaletteCommand(palette, "不存在的命令", handlers);

    require(!disabled.executed
            && disabled.kind == trackloom::AppCommandPaletteActivationResultKind::OnlyDisabledMatches,
        "command palette activation should not dispatch when matching commands are disabled");
    require(!missing.executed
            && missing.kind == trackloom::AppCommandPaletteActivationResultKind::NoMatchingCommand,
        "command palette activation should not dispatch when no command matches the query");
    require(undoCalls == 0,
        "command palette activation should not call handlers for disabled or missing commands");
}

void commandPaletteActivationReportsDispatchFailure()
{
    trackloom::AppCommandPaletteStatus palette;
    palette.items.push_back({
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::PlayProject),
        true,
        "播放",
        "播放",
        {}
    });

    trackloom::AppCommandHandlers handlers;
    const auto result = trackloom::activateAppCommandPaletteCommand(palette, "播放", handlers);

    require(!result.executed,
        "command palette activation should report failure when the selected command lacks a handler");
    require(result.kind == trackloom::AppCommandPaletteActivationResultKind::DispatchFailed,
        "command palette activation should distinguish dispatcher failure from search failure");
    require(result.selection.kind == trackloom::AppCommandPaletteSelectionResultKind::Selected,
        "command palette activation should preserve the successful selection result");
    require(result.dispatch.kind == trackloom::AppCommandDispatchResultKind::MissingHandler,
        "command palette activation should expose the dispatcher failure reason");
}

void commandPaletteAddsShortcutLabelsForVisibleCommands()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Palette Shortcut Snapshot");
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host);
    trackloom::AppRecentProjects recent;

    const auto menu = trackloom::describeAppMainMenu(session, playback, recent);
    const auto palette = trackloom::addAppCommandPaletteShortcutLabels(
        trackloom::describeAppCommandPalette(menu),
        trackloom::defaultAppShortcutBindings());

    const auto saveProject = findPaletteItem(
        palette,
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SaveProject));
    const auto undoProject = findPaletteItem(
        palette,
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::UndoProject));
    const auto addInstrumentTrack = findPaletteItem(
        palette,
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::AddInstrumentTrack));
    const auto openCommandPalette = findPaletteItem(
        palette,
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::OpenCommandPalette));

    require(saveProject != nullptr && saveProject->shortcutLabel == "Ctrl+S",
        "command palette should show the registered shortcut for save");
    require(undoProject != nullptr && undoProject->shortcutLabel == "Ctrl+Z",
        "command palette should show shortcuts even when the command is currently disabled");
    require(addInstrumentTrack != nullptr && addInstrumentTrack->shortcutLabel.empty(),
        "command palette should leave commands without registered shortcuts unlabeled");
    require(openCommandPalette != nullptr && openCommandPalette->shortcutLabel == "Ctrl+K, Ctrl+Shift+P",
        "command palette should show both registered shortcuts for opening itself");
}

void commandPaletteUsesActiveShortcutBindingsForLabels()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Palette Active Shortcut Snapshot");
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host);
    trackloom::AppRecentProjects recent;
    const auto saveProjectId = trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SaveProject);

    const auto customized = trackloom::customizeAppShortcutBindings({
        {{ 'r', true, true, false }, saveProjectId}
    });
    const auto menu = trackloom::describeAppMainMenu(session, playback, recent);
    const auto palette = trackloom::addAppCommandPaletteShortcutLabels(
        trackloom::describeAppCommandPalette(menu),
        customized.bindings);

    const auto saveProject = findPaletteItem(palette, saveProjectId);
    const auto openProject = findPaletteItem(
        palette,
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::OpenProject));

    require(customized.conflicts.empty(),
        "accepted shortcut customization should be usable by command palette labels");
    require(saveProject != nullptr && saveProject->shortcutLabel == "Ctrl+Shift+R",
        "command palette should show the active custom shortcut instead of the old default");
    require(openProject != nullptr && openProject->shortcutLabel == "Ctrl+O",
        "command palette should keep unrelated default shortcut labels when using the active table");
}

void commandPaletteMergesMultipleShortcutLabelsForOneCommand()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Palette Shortcut Merge Snapshot");
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host);
    trackloom::AppRecentProjects recent;

    const auto menu = trackloom::describeAppMainMenu(session, playback, recent);
    const auto palette = trackloom::addAppCommandPaletteShortcutLabels(
        trackloom::describeAppCommandPalette(menu),
        trackloom::defaultAppShortcutBindings());
    const auto redoProject = findPaletteItem(
        palette,
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::RedoProject));

    require(redoProject != nullptr && redoProject->shortcutLabel == "Ctrl+Y, Ctrl+Shift+Z",
        "command palette should preserve every registered shortcut label for the same command");
}

void commandPaletteFiltersByShortcutLabel()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Palette Shortcut Search Snapshot");
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host);
    trackloom::AppRecentProjects recent;

    const auto menu = trackloom::describeAppMainMenu(session, playback, recent);
    const auto palette = trackloom::addAppCommandPaletteShortcutLabels(
        trackloom::describeAppCommandPalette(menu),
        trackloom::defaultAppShortcutBindings());

    const auto saveMatches = trackloom::filterAppCommandPalette(palette, "ctrl+s");
    const auto undoMatches = trackloom::filterAppCommandPalette(palette, "ctrl+z");

    require(findPaletteItem(
                saveMatches,
                trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SaveProject)) != nullptr,
        "command palette search should include enabled commands by displayed shortcut label");
    require(findPaletteItem(
                undoMatches,
                trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::UndoProject)) != nullptr,
        "command palette search should also include disabled commands by displayed shortcut label");
}

void commandPaletteFiltersByMergedShortcutLabel()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Palette Merged Shortcut Search Snapshot");
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host);
    trackloom::AppRecentProjects recent;
    session.executeProjectCommand(
        std::make_unique<trackloom::AddTrackCommand>("Undo seed", trackloom::TrackType::Instrument));
    session.undoProjectEdit();

    const auto menu = trackloom::describeAppMainMenu(session, playback, recent);
    const auto palette = trackloom::addAppCommandPaletteShortcutLabels(
        trackloom::describeAppCommandPalette(menu),
        trackloom::defaultAppShortcutBindings());

    const auto redoMatches = trackloom::filterAppCommandPalette(palette, "ctrl+shift+z");

    require(redoMatches.items.size() == 1
            && redoMatches.items.front().commandId == trackloom::appMainMenuCommandId(
                trackloom::AppMainMenuCommand::RedoProject),
        "command palette search should match commands by any shortcut inside a merged label");
}

void commandPaletteSessionOpensWithFirstEnabledCommandHighlighted()
{
    trackloom::AppCommandPaletteSession session;

    session.open(sampleCommandPaletteForSession());

    const auto& status = session.status();
    require(status.open,
        "command palette session should report open after opening");
    require(status.query.empty(),
        "command palette session should start with an empty query");
    require(status.filteredPalette.items.size() == 5,
        "command palette session should show every command before the user types");
    require(status.highlightedIndex.has_value() && status.highlightedIndex.value() == 1,
        "command palette session should highlight the first enabled command, skipping disabled items");
    require(status.filteredPalette.items.at(status.highlightedIndex.value()).commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SaveProject),
        "command palette session should expose the highlighted command through the filtered palette");
}

void commandPaletteSessionUpdatesQueryAndResetsHighlight()
{
    trackloom::AppCommandPaletteSession session;
    session.open(sampleCommandPaletteForSession());

    session.updateQuery("ctrl+z");

    const auto& disabledMatch = session.status();
    require(disabledMatch.query == "ctrl+z",
        "command palette session should keep the current query text");
    require(disabledMatch.filteredPalette.items.size() == 1
            && disabledMatch.filteredPalette.items.front().commandId == trackloom::appMainMenuCommandId(
                trackloom::AppMainMenuCommand::UndoProject),
        "command palette session should filter using the same shortcut-aware palette search");
    require(!disabledMatch.highlightedIndex.has_value(),
        "command palette session should not highlight a disabled-only match");

    session.updateQuery("ctrl+shift+s");

    const auto& enabledMatch = session.status();
    require(enabledMatch.filteredPalette.items.size() == 1
            && enabledMatch.filteredPalette.items.front().commandId == trackloom::appMainMenuCommandId(
                trackloom::AppMainMenuCommand::SaveProjectAs),
        "command palette session should refresh filtered commands when the query changes");
    require(enabledMatch.highlightedIndex.has_value() && enabledMatch.highlightedIndex.value() == 0,
        "command palette session should reset highlight to the first enabled filtered command");
}

void commandPaletteSessionMovesHighlightAcrossEnabledCommands()
{
    trackloom::AppCommandPaletteSession session;
    session.open(sampleCommandPaletteForSession());

    session.moveHighlightDown();
    const auto& afterDown = session.status();
    require(afterDown.highlightedIndex.has_value() && afterDown.highlightedIndex.value() == 2,
        "command palette session should move highlight down to the next enabled command");
    require(afterDown.filteredPalette.items.at(afterDown.highlightedIndex.value()).commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SaveProjectAs),
        "command palette session should skip disabled commands while moving down");

    session.moveHighlightDown();
    session.moveHighlightDown();
    const auto& wrappedDown = session.status();
    require(wrappedDown.highlightedIndex.has_value() && wrappedDown.highlightedIndex.value() == 1,
        "command palette session should wrap downward navigation to the first enabled command");

    session.moveHighlightUp();
    const auto& wrappedUp = session.status();
    require(wrappedUp.highlightedIndex.has_value() && wrappedUp.highlightedIndex.value() == 4,
        "command palette session should wrap upward navigation to the last enabled command");
}

void commandPaletteSessionPagesHighlightAcrossVisibleWindows()
{
    trackloom::AppCommandPaletteSession session;
    session.open(sampleLongCommandPaletteForSession());

    session.moveHighlightPageDown(6);
    const auto& firstPageDown = session.status();
    require(firstPageDown.highlightedIndex.has_value() && firstPageDown.highlightedIndex.value() == 7,
        "command palette page-down should move the highlight by one visible window");
    require(trackloom::firstVisibleAppCommandPaletteSessionRowIndex(
                trackloom::describeAppCommandPaletteSession(firstPageDown),
                6) == 2,
        "command palette visible window should scroll just enough to keep the page-down target visible");

    session.moveHighlightPageDown(6);
    const auto& secondPageDown = session.status();
    require(secondPageDown.highlightedIndex.has_value() && secondPageDown.highlightedIndex.value() == 11,
        "command palette page-down should clamp to the last enabled command at the bottom");
    require(trackloom::firstVisibleAppCommandPaletteSessionRowIndex(
                trackloom::describeAppCommandPaletteSession(secondPageDown),
                6) == 6,
        "command palette visible window should show the last page when the highlight reaches the bottom");

    session.moveHighlightPageUp(6);
    const auto& firstPageUp = session.status();
    require(firstPageUp.highlightedIndex.has_value() && firstPageUp.highlightedIndex.value() == 4,
        "command palette page-up should move the highlight upward by one visible window");

    session.moveHighlightPageUp(6);
    const auto& secondPageUp = session.status();
    require(secondPageUp.highlightedIndex.has_value() && secondPageUp.highlightedIndex.value() == 1,
        "command palette page-up should clamp to the first enabled command at the top");
}

void commandPaletteSessionPageNavigationSkipsDisabledTargetsAndInvalidCounts()
{
    trackloom::AppCommandPaletteSession session;
    session.open(sampleLongCommandPaletteForSession());

    session.moveHighlightPageDown(0);
    require(session.status().highlightedIndex.has_value() && session.status().highlightedIndex.value() == 1,
        "command palette page navigation should ignore a zero-row page size");

    session.moveHighlightPageDown(4);
    const auto& skippedDown = session.status();
    require(skippedDown.highlightedIndex.has_value() && skippedDown.highlightedIndex.value() == 6,
        "command palette page-down should move to the next enabled command when the target row is disabled");

    session.moveHighlightPageUp(1);
    const auto& skippedUp = session.status();
    require(skippedUp.highlightedIndex.has_value() && skippedUp.highlightedIndex.value() == 4,
        "command palette page-up should move to the previous enabled command when the target row is disabled");

    session.updateQuery("Ctrl+Z");
    session.moveHighlightPageDown(6);
    require(!session.status().highlightedIndex.has_value(),
        "command palette page navigation should keep disabled-only searches without a highlight");
    require(trackloom::firstVisibleAppCommandPaletteSessionRowIndex(
                trackloom::describeAppCommandPaletteSession(session.status()),
                6) == 0,
        "command palette visible window should stay at the top when no row is highlighted");
    require(trackloom::firstVisibleAppCommandPaletteSessionRowIndex(
                trackloom::describeAppCommandPaletteSession(session.status()),
                0) == 0,
        "command palette visible window should reject a zero visible row count");
}

void commandPaletteSessionWheelStepsMoveHighlightWithoutWrapping()
{
    trackloom::AppCommandPaletteSession session;
    session.open(sampleLongCommandPaletteForSession());

    session.moveHighlightByWheelSteps(2);
    require(session.status().highlightedIndex.has_value() && session.status().highlightedIndex.value() == 3,
        "command palette wheel-down steps should move through enabled commands");

    session.moveHighlightByWheelSteps(1);
    require(session.status().highlightedIndex.has_value() && session.status().highlightedIndex.value() == 4,
        "command palette wheel-down should move to the next enabled command before a disabled row");

    session.moveHighlightByWheelSteps(1);
    require(session.status().highlightedIndex.has_value() && session.status().highlightedIndex.value() == 6,
        "command palette wheel-down should skip disabled commands");

    session.moveHighlightByWheelSteps(20);
    require(session.status().highlightedIndex.has_value() && session.status().highlightedIndex.value() == 11,
        "command palette wheel-down should clamp to the last enabled command instead of wrapping");
    require(trackloom::describeAppCommandPaletteVisibleRowsRange(
                trackloom::describeVisibleAppCommandPaletteSessionRows(
                    trackloom::describeAppCommandPaletteSession(session.status()),
                    6)) == "↑ 7-12 / 12",
        "command palette wheel-down should let the visible window follow the bottom highlight");

    session.moveHighlightByWheelSteps(20);
    require(session.status().highlightedIndex.has_value() && session.status().highlightedIndex.value() == 11,
        "command palette wheel-down at the bottom should stay at the last enabled command");

    session.moveHighlightByWheelSteps(-20);
    require(session.status().highlightedIndex.has_value() && session.status().highlightedIndex.value() == 1,
        "command palette wheel-up should clamp to the first enabled command instead of wrapping");
    require(trackloom::describeAppCommandPaletteVisibleRowsRange(
                trackloom::describeVisibleAppCommandPaletteSessionRows(
                    trackloom::describeAppCommandPaletteSession(session.status()),
                    6)) == "1-6 / 12 ↓",
        "command palette wheel-up should return the visible window to the top");
}

void commandPaletteSessionWheelStepsHandleZeroDisabledOnlyAndClosedStates()
{
    trackloom::AppCommandPaletteSession session;
    session.open(sampleLongCommandPaletteForSession());

    session.moveHighlightByWheelSteps(0);
    require(session.status().highlightedIndex.has_value() && session.status().highlightedIndex.value() == 1,
        "command palette wheel navigation should ignore zero steps");

    session.updateQuery("Ctrl+Z");
    session.moveHighlightByWheelSteps(1);
    require(!session.status().highlightedIndex.has_value(),
        "command palette wheel navigation should not highlight disabled-only matches");

    session.close();
    session.moveHighlightByWheelSteps(1);
    require(!session.status().open && !session.status().highlightedIndex.has_value(),
        "command palette wheel navigation should leave a closed session closed and unhighlighted");
}

void commandPaletteSessionScrollsVisibleWindowWithoutMovingHighlight()
{
    trackloom::AppCommandPaletteSession session;
    session.open(sampleLongCommandPaletteForSession());

    session.scrollVisibleRowsByWheelSteps(3, 6);

    require(session.status().highlightedIndex.has_value() && session.status().highlightedIndex.value() == 1,
        "command palette visible-row scrolling should not change the highlighted command");

    const auto scrolledRows = trackloom::describeVisibleAppCommandPaletteSessionRows(
        trackloom::describeAppCommandPaletteSession(session.status()),
        6);
    require(scrolledRows.firstRowIndex == 3,
        "command palette visible-row scrolling should move the visible window independently");
    require(trackloom::describeAppCommandPaletteVisibleRowsRange(scrolledRows) == "↑ 4-9 / 12 ↓",
        "command palette visible-row scrolling should update the visible range hint");

    bool anyVisibleHighlight = false;
    for (const auto& row : scrolledRows.rows) {
        anyVisibleHighlight = anyVisibleHighlight || row.highlighted;
    }
    require(!anyVisibleHighlight,
        "command palette visible-row scrolling should allow the current highlight to be outside the visible window");
}

void commandPaletteSessionScrollWindowResetsWhenQueryOrKeyboardNavigationChangesHighlight()
{
    trackloom::AppCommandPaletteSession session;
    session.open(sampleLongCommandPaletteForSession());

    session.scrollVisibleRowsByWheelSteps(20, 6);
    auto bottomRows = trackloom::describeVisibleAppCommandPaletteSessionRows(
        trackloom::describeAppCommandPaletteSession(session.status()),
        6);
    require(bottomRows.firstRowIndex == 6,
        "command palette visible-row scrolling should clamp to the last possible visible window");

    session.updateQuery("播放");
    auto queriedRows = trackloom::describeVisibleAppCommandPaletteSessionRows(
        trackloom::describeAppCommandPaletteSession(session.status()),
        6);
    require(queriedRows.firstRowIndex == 0,
        "command palette query changes should reset the independent visible window");

    session.updateQuery("");
    session.scrollVisibleRowsByWheelSteps(20, 6);
    session.moveHighlightDown();
    auto keyboardRows = trackloom::describeVisibleAppCommandPaletteSessionRows(
        trackloom::describeAppCommandPaletteSession(session.status()),
        6);
    require(keyboardRows.firstRowIndex == 0,
        "command palette keyboard navigation should return to highlight-driven visible windows");
}

void commandPaletteSessionHighlightsEnabledCommandByIdForMouseHover()
{
    trackloom::AppCommandPaletteSession session;
    session.open(sampleLongCommandPaletteForSession());
    session.scrollVisibleRowsByWheelSteps(20, 6);

    const auto highlighted = session.highlightCommandById(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::AddAudioTrack));

    require(highlighted,
        "command palette hover highlighting should report when it finds an enabled command");
    require(session.status().highlightedIndex.has_value() && session.status().highlightedIndex.value() == 9,
        "command palette hover highlighting should move highlight to the hovered enabled command");

    const auto visibleRows = trackloom::describeVisibleAppCommandPaletteSessionRows(
        trackloom::describeAppCommandPaletteSession(session.status()),
        6);
    require(visibleRows.firstRowIndex == 6,
        "command palette hover highlighting should preserve the current independently scrolled window");

    bool addAudioTrackHighlighted = false;
    for (const auto& row : visibleRows.rows) {
        if (row.commandId == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::AddAudioTrack)) {
            addAudioTrackHighlighted = row.highlighted;
        }
    }
    require(addAudioTrackHighlighted,
        "command palette visible rows should mark the hovered enabled command as highlighted");
}

void commandPaletteSessionHoverHighlightRejectsDisabledMissingAndClosedCommands()
{
    trackloom::AppCommandPaletteSession session;
    session.open(sampleLongCommandPaletteForSession());

    const auto disabled = session.highlightCommandById(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::UndoProject));
    require(!disabled,
        "command palette hover highlighting should reject disabled commands");
    require(session.status().highlightedIndex.has_value() && session.status().highlightedIndex.value() == 1,
        "command palette hover highlighting should keep the old highlight when a disabled command is hovered");

    const auto missing = session.highlightCommandById(999999);
    require(!missing,
        "command palette hover highlighting should reject missing commands");
    require(session.status().highlightedIndex.has_value() && session.status().highlightedIndex.value() == 1,
        "command palette hover highlighting should keep the old highlight when the command is missing");

    session.close();
    const auto closed = session.highlightCommandById(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SaveProject));
    require(!closed && !session.status().highlightedIndex.has_value(),
        "command palette hover highlighting should reject closed sessions without reopening them");
}

void commandPaletteSessionMovesHighlightToFirstAndLastEnabledCommand()
{
    trackloom::AppCommandPaletteSession session;
    session.open(sampleLongCommandPaletteForSession());

    session.moveHighlightToLast();
    const auto& last = session.status();
    require(last.highlightedIndex.has_value() && last.highlightedIndex.value() == 11,
        "command palette End navigation should move to the last enabled command");
    require(trackloom::firstVisibleAppCommandPaletteSessionRowIndex(
                trackloom::describeAppCommandPaletteSession(last),
                6) == 6,
        "command palette visible window should scroll to the last page after End navigation");

    session.moveHighlightToFirst();
    const auto& first = session.status();
    require(first.highlightedIndex.has_value() && first.highlightedIndex.value() == 1,
        "command palette Home navigation should move to the first enabled command");
    require(trackloom::firstVisibleAppCommandPaletteSessionRowIndex(
                trackloom::describeAppCommandPaletteSession(first),
                6) == 0,
        "command palette visible window should return to the top after Home navigation");
}

void commandPaletteSessionBoundaryNavigationKeepsDisabledOnlySearchesUnhighlighted()
{
    trackloom::AppCommandPaletteSession session;
    session.open(sampleLongCommandPaletteForSession());
    session.updateQuery("Ctrl+Z");

    session.moveHighlightToLast();
    require(!session.status().highlightedIndex.has_value(),
        "command palette End navigation should not highlight disabled-only matches");

    session.moveHighlightToFirst();
    require(!session.status().highlightedIndex.has_value(),
        "command palette Home navigation should not highlight disabled-only matches");

    session.close();
    session.moveHighlightToFirst();
    session.moveHighlightToLast();
    require(!session.status().open && !session.status().highlightedIndex.has_value(),
        "command palette boundary navigation should leave a closed session closed and unhighlighted");
}

void commandPaletteSessionVisibleRowsDescribeWindowSliceForUi()
{
    trackloom::AppCommandPaletteSession session;
    session.open(sampleLongCommandPaletteForSession());

    session.moveHighlightPageDown(6);
    const auto firstPageDownRows = trackloom::describeVisibleAppCommandPaletteSessionRows(
        trackloom::describeAppCommandPaletteSession(session.status()),
        6);

    require(firstPageDownRows.firstRowIndex == 2,
        "command palette visible rows should start where the highlighted page-down row stays visible");
    require(firstPageDownRows.totalRowCount == 12,
        "command palette visible rows should report the full filtered result count");
    require(firstPageDownRows.rows.size() == 6,
        "command palette visible rows should return no more than the UI visible row count");
    require(firstPageDownRows.hasPreviousRows && firstPageDownRows.hasNextRows,
        "command palette visible rows should report when hidden rows exist above and below the slice");
    require(firstPageDownRows.rows.front().commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::OpenProject),
        "command palette visible rows should expose the first clipped row for the UI");
    require(firstPageDownRows.rows.back().commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::RewindProject)
            && firstPageDownRows.rows.back().highlighted,
        "command palette visible rows should preserve the highlighted row inside the clipped slice");

    session.moveHighlightToLast();
    const auto lastPageRows = trackloom::describeVisibleAppCommandPaletteSessionRows(
        trackloom::describeAppCommandPaletteSession(session.status()),
        6);

    require(lastPageRows.firstRowIndex == 6,
        "command palette visible rows should start at the last full page when the highlight is at the bottom");
    require(lastPageRows.rows.size() == 6,
        "command palette visible rows should keep the last page filled when enough rows remain");
    require(lastPageRows.hasPreviousRows && !lastPageRows.hasNextRows,
        "command palette visible rows should report that the last page has no rows below it");
    require(lastPageRows.rows.back().commandId
            == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::OpenCommandPalette)
            && lastPageRows.rows.back().highlighted,
        "command palette visible rows should include the final highlighted command on the last page");
}

void commandPaletteSessionVisibleRowsHandleClosedZeroAndDisabledOnlyStates()
{
    trackloom::AppCommandPaletteSession session;

    const auto closedRows = trackloom::describeVisibleAppCommandPaletteSessionRows(
        trackloom::describeAppCommandPaletteSession(session.status()),
        6);
    require(closedRows.totalRowCount == 0 && closedRows.firstRowIndex == 0 && closedRows.rows.empty(),
        "command palette visible rows should be empty for a closed session");
    require(!closedRows.hasPreviousRows && !closedRows.hasNextRows,
        "command palette visible rows should not report hidden rows for a closed session");

    session.open(sampleLongCommandPaletteForSession());
    const auto zeroVisibleRows = trackloom::describeVisibleAppCommandPaletteSessionRows(
        trackloom::describeAppCommandPaletteSession(session.status()),
        0);
    require(zeroVisibleRows.totalRowCount == 12 && zeroVisibleRows.rows.empty(),
        "command palette visible rows should keep the total count but return no slice for a zero visible row count");
    require(zeroVisibleRows.firstRowIndex == 0
            && !zeroVisibleRows.hasPreviousRows
            && !zeroVisibleRows.hasNextRows,
        "command palette visible rows should keep zero-row windows at the top with no scroll hints");

    session.updateQuery("Ctrl+Z");
    const auto disabledOnlyRows = trackloom::describeVisibleAppCommandPaletteSessionRows(
        trackloom::describeAppCommandPaletteSession(session.status()),
        6);
    require(disabledOnlyRows.totalRowCount == 1 && disabledOnlyRows.rows.size() == 1,
        "command palette visible rows should still expose disabled-only matches");
    require(disabledOnlyRows.firstRowIndex == 0
            && !disabledOnlyRows.hasPreviousRows
            && !disabledOnlyRows.hasNextRows,
        "command palette visible rows should not report scroll hints for one disabled-only row");
    require(!disabledOnlyRows.rows.front().enabled && !disabledOnlyRows.rows.front().highlighted,
        "command palette visible rows should preserve disabled-only row state without inventing a highlight");
}

void commandPaletteSessionVisibleRowsFormatRangeHintForUi()
{
    trackloom::AppCommandPaletteSession session;
    session.open(sampleLongCommandPaletteForSession());

    session.moveHighlightPageDown(6);
    const auto firstPageDownRows = trackloom::describeVisibleAppCommandPaletteSessionRows(
        trackloom::describeAppCommandPaletteSession(session.status()),
        6);
    require(trackloom::describeAppCommandPaletteVisibleRowsRange(firstPageDownRows) == "↑ 3-8 / 12 ↓",
        "command palette range text should show overflow indicators around a middle visible window");

    session.moveHighlightToLast();
    const auto lastPageRows = trackloom::describeVisibleAppCommandPaletteSessionRows(
        trackloom::describeAppCommandPaletteSession(session.status()),
        6);
    require(trackloom::describeAppCommandPaletteVisibleRowsRange(lastPageRows) == "↑ 7-12 / 12",
        "command palette range text should show only the previous-row indicator on the final visible window");

    session.updateQuery("Ctrl+Z");
    const auto disabledOnlyRows = trackloom::describeVisibleAppCommandPaletteSessionRows(
        trackloom::describeAppCommandPaletteSession(session.status()),
        6);
    require(trackloom::describeAppCommandPaletteVisibleRowsRange(disabledOnlyRows) == "1 / 1",
        "command palette range text should use a compact single-row format");

    const auto zeroVisibleRows = trackloom::describeVisibleAppCommandPaletteSessionRows(
        trackloom::describeAppCommandPaletteSession(session.status()),
        0);
    require(trackloom::describeAppCommandPaletteVisibleRowsRange(zeroVisibleRows).empty(),
        "command palette range text should be empty when no rows are currently visible");
}

void commandPaletteSessionClosesAndClearsState()
{
    trackloom::AppCommandPaletteSession session;
    session.open(sampleCommandPaletteForSession());
    session.updateQuery("保存");

    session.close();

    const auto& status = session.status();
    require(!status.open,
        "command palette session should report closed after closing");
    require(status.query.empty(),
        "command palette session should clear query text when closed");
    require(status.filteredPalette.items.empty(),
        "command palette session should clear filtered items when closed");
    require(!status.highlightedIndex.has_value(),
        "command palette session should clear highlight when closed");
}

void commandPaletteSessionActivationExecutesHighlightedCommand()
{
    trackloom::AppCommandPaletteSession session;
    session.open(sampleCommandPaletteForSession());
    session.moveHighlightDown();

    int saveProjectCalls = 0;
    int saveProjectAsCalls = 0;
    trackloom::AppCommandHandlers handlers;
    handlers.saveProject = [&] { ++saveProjectCalls; };
    handlers.saveProjectAs = [&] { ++saveProjectAsCalls; };

    const auto result = trackloom::activateHighlightedAppCommandPaletteCommand(session.status(), handlers);

    require(result.executed && result.kind == trackloom::AppCommandPaletteActivationResultKind::Executed,
        "command palette session activation should execute the highlighted enabled command");
    require(result.selection.item.has_value()
            && result.selection.item->commandId == trackloom::appMainMenuCommandId(
                trackloom::AppMainMenuCommand::SaveProjectAs),
        "command palette session activation should preserve the exact highlighted item");
    require(result.dispatch.command == trackloom::AppCommandKind::SaveProjectAs,
        "command palette session activation should dispatch the highlighted command id");
    require(saveProjectCalls == 0 && saveProjectAsCalls == 1,
        "command palette session activation should not fall back to the first matching command");
}

void commandPaletteSessionActivationRejectsClosedDisabledOrMissingHandler()
{
    trackloom::AppCommandHandlers handlers;
    int undoProjectCalls = 0;
    handlers.undoProject = [&] { ++undoProjectCalls; };

    const auto closed = trackloom::activateHighlightedAppCommandPaletteCommand(
        trackloom::AppCommandPaletteSessionStatus{},
        handlers);

    require(!closed.executed
            && closed.kind == trackloom::AppCommandPaletteActivationResultKind::NoMatchingCommand,
        "command palette session activation should not execute when the session has no highlighted command");

    trackloom::AppCommandPaletteSession disabledSession;
    disabledSession.open(sampleCommandPaletteForSession());
    disabledSession.updateQuery("ctrl+z");
    const auto disabled = trackloom::activateHighlightedAppCommandPaletteCommand(
        disabledSession.status(),
        handlers);

    require(!disabled.executed
            && disabled.kind == trackloom::AppCommandPaletteActivationResultKind::OnlyDisabledMatches,
        "command palette session activation should report disabled-only matches without dispatching");
    require(undoProjectCalls == 0,
        "command palette session activation should not dispatch disabled highlighted matches");

    trackloom::AppCommandPaletteSession missingHandlerSession;
    missingHandlerSession.open(sampleCommandPaletteForSession());
    const auto missingHandler = trackloom::activateHighlightedAppCommandPaletteCommand(
        missingHandlerSession.status(),
        trackloom::AppCommandHandlers{});

    require(!missingHandler.executed
            && missingHandler.kind == trackloom::AppCommandPaletteActivationResultKind::DispatchFailed,
        "command palette session activation should preserve dispatcher failures");
    require(missingHandler.selection.kind == trackloom::AppCommandPaletteSelectionResultKind::Selected,
        "command palette session activation should distinguish selected command dispatch failure from search failure");
}

void commandPaletteSessionRowActivationExecutesEnabledVisibleCommand()
{
    trackloom::AppCommandPaletteSession session;
    session.open(sampleCommandPaletteForSession());

    int saveProjectCalls = 0;
    int saveProjectAsCalls = 0;
    trackloom::AppCommandHandlers handlers;
    handlers.saveProject = [&] { ++saveProjectCalls; };
    handlers.saveProjectAs = [&] { ++saveProjectAsCalls; };

    const auto result = trackloom::activateAppCommandPaletteSessionRow(
        session.status(),
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SaveProjectAs),
        handlers);

    require(result.executed && result.kind == trackloom::AppCommandPaletteActivationResultKind::Executed,
        "command palette row activation should execute the enabled command represented by the clicked row");
    require(result.selection.item.has_value()
            && result.selection.item->commandId == trackloom::appMainMenuCommandId(
                trackloom::AppMainMenuCommand::SaveProjectAs),
        "command palette row activation should preserve the clicked command id instead of the highlighted command");
    require(result.dispatch.command == trackloom::AppCommandKind::SaveProjectAs,
        "command palette row activation should dispatch the clicked command id through the shared dispatcher");
    require(saveProjectCalls == 0 && saveProjectAsCalls == 1,
        "command palette row activation should not execute the highlighted command when a different row is clicked");
}

void commandPaletteSessionRowActivationRejectsClosedDisabledMissingOrMissingHandler()
{
    int undoProjectCalls = 0;
    trackloom::AppCommandHandlers handlers;
    handlers.undoProject = [&] { ++undoProjectCalls; };

    const auto closed = trackloom::activateAppCommandPaletteSessionRow(
        trackloom::AppCommandPaletteSessionStatus{},
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SaveProject),
        handlers);

    require(!closed.executed
            && closed.kind == trackloom::AppCommandPaletteActivationResultKind::NoMatchingCommand,
        "command palette row activation should reject clicks when the session is closed");

    trackloom::AppCommandPaletteSession disabledSession;
    disabledSession.open(sampleCommandPaletteForSession());
    const auto disabled = trackloom::activateAppCommandPaletteSessionRow(
        disabledSession.status(),
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::UndoProject),
        handlers);

    require(!disabled.executed
            && disabled.kind == trackloom::AppCommandPaletteActivationResultKind::OnlyDisabledMatches,
        "command palette row activation should report disabled rows without dispatching them");
    require(undoProjectCalls == 0,
        "command palette row activation should not call handlers for disabled commands");

    const auto missing = trackloom::activateAppCommandPaletteSessionRow(
        disabledSession.status(),
        999999,
        handlers);

    require(!missing.executed
            && missing.kind == trackloom::AppCommandPaletteActivationResultKind::NoMatchingCommand,
        "command palette row activation should reject stale row command ids that are no longer visible");

    trackloom::AppCommandPaletteSession missingHandlerSession;
    missingHandlerSession.open(sampleCommandPaletteForSession());
    const auto missingHandler = trackloom::activateAppCommandPaletteSessionRow(
        missingHandlerSession.status(),
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SaveProject),
        trackloom::AppCommandHandlers{});

    require(!missingHandler.executed
            && missingHandler.kind == trackloom::AppCommandPaletteActivationResultKind::DispatchFailed,
        "command palette row activation should preserve dispatcher failures for enabled clicked rows");
    require(missingHandler.selection.kind == trackloom::AppCommandPaletteSelectionResultKind::Selected,
        "command palette row activation should distinguish a selected row with no handler from a missing row");
}

void commandPaletteSessionDescriptionMarksRowsForUi()
{
    trackloom::AppCommandPaletteSession session;
    session.open(sampleCommandPaletteForSession());
    session.moveHighlightDown();

    const auto view = trackloom::describeAppCommandPaletteSession(session.status());

    require(view.open,
        "command palette session view should preserve the open state");
    require(view.query.empty(),
        "command palette session view should expose the current query text");
    require(view.rows.size() == 5,
        "command palette session view should expose one row for every filtered command");
    require(view.emptyMessage.empty(),
        "command palette session view should not show an empty message when rows exist");
    require(view.rows[0].label == "撤销" && !view.rows[0].enabled && !view.rows[0].highlighted,
        "command palette session view should expose disabled rows without highlighting them");
    require(view.rows[2].commandId == trackloom::appMainMenuCommandId(
                trackloom::AppMainMenuCommand::SaveProjectAs)
            && view.rows[2].highlighted
            && view.rows[2].shortcutLabel == "Ctrl+Shift+S",
        "command palette session view should mark the current highlighted row and preserve shortcut labels");
}

void commandPaletteSessionDescriptionReportsDisabledAndEmptyStates()
{
    trackloom::AppCommandPaletteSession session;
    session.open(sampleCommandPaletteForSession());

    session.updateQuery("ctrl+z");
    const auto disabledOnly = trackloom::describeAppCommandPaletteSession(session.status());

    require(disabledOnly.rows.size() == 1
            && disabledOnly.rows.front().commandId == trackloom::appMainMenuCommandId(
                trackloom::AppMainMenuCommand::UndoProject),
        "command palette session view should keep disabled-only matches visible");
    require(!disabledOnly.rows.front().enabled && !disabledOnly.rows.front().highlighted,
        "command palette session view should not mark disabled-only matches as highlighted");
    require(disabledOnly.emptyMessage.empty(),
        "command palette session view should not treat disabled-only matches as an empty result");

    session.updateQuery("definitely-not-a-command");
    const auto empty = trackloom::describeAppCommandPaletteSession(session.status());

    require(empty.open && empty.rows.empty(),
        "command palette session view should report open empty results when a query matches nothing");
    require(empty.emptyMessage == "没有匹配的命令",
        "command palette session view should provide one stable empty-result message for UI rendering");

    session.close();
    const auto closed = trackloom::describeAppCommandPaletteSession(session.status());

    require(!closed.open && closed.rows.empty() && closed.query.empty() && closed.emptyMessage.empty(),
        "command palette session view should be empty when the session is closed");
}

void commandPaletteSessionRowTextFormatsUiLabels()
{
    trackloom::AppCommandPaletteSessionRow saveRow;
    saveRow.enabled = true;
    saveRow.groupName = "文件";
    saveRow.label = "保存";
    saveRow.shortcutLabel = "Ctrl+S";

    trackloom::AppCommandPaletteSessionRow playRow;
    playRow.enabled = true;
    playRow.groupName = "播放";
    playRow.label = "播放";

    trackloom::AppCommandPaletteSessionRow disabledUndoRow;
    disabledUndoRow.enabled = false;
    disabledUndoRow.groupName = "编辑";
    disabledUndoRow.label = "撤销";
    disabledUndoRow.shortcutLabel = "Ctrl+Z";

    require(trackloom::describeAppCommandPaletteSessionRow(saveRow) == "文件 / 保存    Ctrl+S",
        "command palette row text should include group, command label and shortcut when present");
    require(trackloom::describeAppCommandPaletteSessionRow(playRow) == "播放 / 播放",
        "command palette row text should omit shortcut spacing when no shortcut is registered");
    require(trackloom::describeAppCommandPaletteSessionRow(disabledUndoRow) == "编辑 / 撤销    Ctrl+Z    不可用",
        "command palette row text should mark disabled commands without hiding them");
}

void commandDispatcherRunsOnlyTheSelectedMainMenuCommand()
{
    int newProjectCalls = 0;
    int saveProjectCalls = 0;
    int playProjectCalls = 0;
    int undoProjectCalls = 0;

    trackloom::AppCommandHandlers handlers;
    handlers.newProject = [&] { ++newProjectCalls; };
    handlers.saveProject = [&] { ++saveProjectCalls; };
    handlers.playProject = [&] { ++playProjectCalls; };
    handlers.undoProject = [&] { ++undoProjectCalls; };

    const auto result = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::UndoProject),
        handlers);

    require(result.executed,
        "command dispatcher should execute a known command when its handler exists");
    require(result.kind == trackloom::AppCommandDispatchResultKind::Executed,
        "executed command should expose a stable executed result kind");
    require(result.command == trackloom::AppCommandKind::UndoProject,
        "undo menu id should resolve to the undo project command kind");
    require(newProjectCalls == 0 && saveProjectCalls == 0 && playProjectCalls == 0 && undoProjectCalls == 1,
        "command dispatcher should run only the selected command handler");
}

void commandDispatcherRunsRedoMainMenuCommand()
{
    int redoProjectCalls = 0;

    trackloom::AppCommandHandlers handlers;
    handlers.redoProject = [&] { ++redoProjectCalls; };

    const auto result = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::RedoProject),
        handlers);

    require(result.executed,
        "command dispatcher should execute the redo command when its handler exists");
    require(result.command == trackloom::AppCommandKind::RedoProject,
        "redo menu id should resolve to the redo project command kind");
    require(redoProjectCalls == 1,
        "redo command should call the redo project handler exactly once");
}

void commandDispatcherRunsTrackCreationMenuCommands()
{
    int instrumentTrackCalls = 0;
    int audioTrackCalls = 0;
    int folderTrackCalls = 0;

    trackloom::AppCommandHandlers handlers;
    handlers.addInstrumentTrack = [&] { ++instrumentTrackCalls; };
    handlers.addAudioTrack = [&] { ++audioTrackCalls; };
    handlers.addFolderTrack = [&] { ++folderTrackCalls; };

    const auto instrumentResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::AddInstrumentTrack),
        handlers);
    const auto audioResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::AddAudioTrack),
        handlers);
    const auto folderResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::AddFolderTrack),
        handlers);

    require(instrumentResult.executed
            && instrumentResult.command == trackloom::AppCommandKind::AddInstrumentTrack,
        "track menu add-instrument command should dispatch to the instrument-track handler");
    require(audioResult.executed
            && audioResult.command == trackloom::AppCommandKind::AddAudioTrack,
        "track menu add-audio command should dispatch to the audio-track handler");
    require(folderResult.executed
            && folderResult.command == trackloom::AppCommandKind::AddFolderTrack,
        "track menu add-folder command should dispatch to the folder-track handler");
    require(instrumentTrackCalls == 1 && audioTrackCalls == 1 && folderTrackCalls == 1,
        "track creation menu commands should each call exactly their own handler once");
}

void commandDispatcherRunsSelectedTrackMenuCommands()
{
    int renameInstrumentTrackCalls = 0;
    int deleteInstrumentTrackCalls = 0;
    int moveInstrumentTrackUpCalls = 0;
    int moveInstrumentTrackDownCalls = 0;
    int toggleInstrumentTrackMuteCalls = 0;
    int toggleInstrumentTrackSoloCalls = 0;
    int toggleInstrumentTrackDisabledCalls = 0;
    int toggleInstrumentTrackHiddenCalls = 0;
    int deleteAudioTrackCalls = 0;
    int moveAudioTrackUpCalls = 0;
    int moveAudioTrackDownCalls = 0;
    int toggleAudioTrackMuteCalls = 0;
    int toggleAudioTrackSoloCalls = 0;
    int toggleAudioTrackDisabledCalls = 0;
    int toggleAudioTrackHiddenCalls = 0;

    trackloom::AppCommandHandlers handlers;
    handlers.renameSelectedInstrumentTrack = [&] { ++renameInstrumentTrackCalls; };
    handlers.deleteSelectedInstrumentTrack = [&] { ++deleteInstrumentTrackCalls; };
    handlers.moveSelectedInstrumentTrackUp = [&] { ++moveInstrumentTrackUpCalls; };
    handlers.moveSelectedInstrumentTrackDown = [&] { ++moveInstrumentTrackDownCalls; };
    handlers.toggleSelectedInstrumentTrackMute = [&] { ++toggleInstrumentTrackMuteCalls; };
    handlers.toggleSelectedInstrumentTrackSolo = [&] { ++toggleInstrumentTrackSoloCalls; };
    handlers.toggleSelectedInstrumentTrackDisabled = [&] { ++toggleInstrumentTrackDisabledCalls; };
    handlers.toggleSelectedInstrumentTrackHidden = [&] { ++toggleInstrumentTrackHiddenCalls; };
    handlers.deleteSelectedAudioTrack = [&] { ++deleteAudioTrackCalls; };
    handlers.moveSelectedAudioTrackUp = [&] { ++moveAudioTrackUpCalls; };
    handlers.moveSelectedAudioTrackDown = [&] { ++moveAudioTrackDownCalls; };
    handlers.toggleSelectedAudioTrackMute = [&] { ++toggleAudioTrackMuteCalls; };
    handlers.toggleSelectedAudioTrackSolo = [&] { ++toggleAudioTrackSoloCalls; };
    handlers.toggleSelectedAudioTrackDisabled = [&] { ++toggleAudioTrackDisabledCalls; };
    handlers.toggleSelectedAudioTrackHidden = [&] { ++toggleAudioTrackHiddenCalls; };

    const auto renameResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::RenameSelectedInstrumentTrack),
        handlers);
    const auto deleteInstrumentResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::DeleteSelectedInstrumentTrack),
        handlers);
    const auto moveUpResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::MoveSelectedInstrumentTrackUp),
        handlers);
    const auto moveDownResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::MoveSelectedInstrumentTrackDown),
        handlers);
    const auto toggleMuteResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::ToggleSelectedInstrumentTrackMute),
        handlers);
    const auto toggleSoloResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::ToggleSelectedInstrumentTrackSolo),
        handlers);
    const auto toggleDisabledResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::ToggleSelectedInstrumentTrackDisabled),
        handlers);
    const auto toggleHiddenResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::ToggleSelectedInstrumentTrackHidden),
        handlers);
    const auto deleteAudioResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::DeleteSelectedAudioTrack),
        handlers);
    const auto moveAudioUpResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::MoveSelectedAudioTrackUp),
        handlers);
    const auto moveAudioDownResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::MoveSelectedAudioTrackDown),
        handlers);
    const auto toggleAudioMuteResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::ToggleSelectedAudioTrackMute),
        handlers);
    const auto toggleAudioSoloResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::ToggleSelectedAudioTrackSolo),
        handlers);
    const auto toggleAudioDisabledResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::ToggleSelectedAudioTrackDisabled),
        handlers);
    const auto toggleAudioHiddenResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::ToggleSelectedAudioTrackHidden),
        handlers);

    require(renameResult.executed
            && renameResult.command == trackloom::AppCommandKind::RenameSelectedInstrumentTrack,
        "selected instrument track rename command should dispatch to the rename handler");
    require(deleteInstrumentResult.executed
            && deleteInstrumentResult.command == trackloom::AppCommandKind::DeleteSelectedInstrumentTrack,
        "selected instrument track delete command should dispatch to the delete handler");
    require(moveUpResult.executed
            && moveUpResult.command == trackloom::AppCommandKind::MoveSelectedInstrumentTrackUp,
        "selected instrument track move-up command should dispatch to the move-up handler");
    require(moveDownResult.executed
            && moveDownResult.command == trackloom::AppCommandKind::MoveSelectedInstrumentTrackDown,
        "selected instrument track move-down command should dispatch to the move-down handler");
    require(toggleMuteResult.executed
            && toggleMuteResult.command == trackloom::AppCommandKind::ToggleSelectedInstrumentTrackMute,
        "selected instrument track mute toggle command should dispatch to the mute handler");
    require(toggleSoloResult.executed
            && toggleSoloResult.command == trackloom::AppCommandKind::ToggleSelectedInstrumentTrackSolo,
        "selected instrument track solo toggle command should dispatch to the solo handler");
    require(toggleDisabledResult.executed
            && toggleDisabledResult.command == trackloom::AppCommandKind::ToggleSelectedInstrumentTrackDisabled,
        "selected instrument track disabled toggle command should dispatch to the disabled handler");
    require(toggleHiddenResult.executed
            && toggleHiddenResult.command == trackloom::AppCommandKind::ToggleSelectedInstrumentTrackHidden,
        "selected instrument track hidden toggle command should dispatch to the hidden handler");
    require(deleteAudioResult.executed
            && deleteAudioResult.command == trackloom::AppCommandKind::DeleteSelectedAudioTrack,
        "selected audio track delete command should dispatch to the delete-audio handler");
    require(moveAudioUpResult.executed
            && moveAudioUpResult.command == trackloom::AppCommandKind::MoveSelectedAudioTrackUp,
        "selected audio track move-up command should dispatch to the audio move-up handler");
    require(moveAudioDownResult.executed
            && moveAudioDownResult.command == trackloom::AppCommandKind::MoveSelectedAudioTrackDown,
        "selected audio track move-down command should dispatch to the audio move-down handler");
    require(toggleAudioMuteResult.executed
            && toggleAudioMuteResult.command == trackloom::AppCommandKind::ToggleSelectedAudioTrackMute,
        "selected audio track mute toggle command should dispatch to the audio mute handler");
    require(toggleAudioSoloResult.executed
            && toggleAudioSoloResult.command == trackloom::AppCommandKind::ToggleSelectedAudioTrackSolo,
        "selected audio track solo toggle command should dispatch to the audio solo handler");
    require(toggleAudioDisabledResult.executed
            && toggleAudioDisabledResult.command == trackloom::AppCommandKind::ToggleSelectedAudioTrackDisabled,
        "selected audio track disabled toggle command should dispatch to the audio disabled handler");
    require(toggleAudioHiddenResult.executed
            && toggleAudioHiddenResult.command == trackloom::AppCommandKind::ToggleSelectedAudioTrackHidden,
        "selected audio track hidden toggle command should dispatch to the audio hidden handler");
    require(renameInstrumentTrackCalls == 1
            && deleteInstrumentTrackCalls == 1
            && moveInstrumentTrackUpCalls == 1
            && moveInstrumentTrackDownCalls == 1
            && toggleInstrumentTrackMuteCalls == 1
            && toggleInstrumentTrackSoloCalls == 1
            && toggleInstrumentTrackDisabledCalls == 1
            && toggleInstrumentTrackHiddenCalls == 1
            && deleteAudioTrackCalls == 1
            && moveAudioTrackUpCalls == 1
            && moveAudioTrackDownCalls == 1
            && toggleAudioTrackMuteCalls == 1
            && toggleAudioTrackSoloCalls == 1
            && toggleAudioTrackDisabledCalls == 1
            && toggleAudioTrackHiddenCalls == 1,
        "selected track menu commands should each call exactly their own handler once");
}

void commandDispatcherRunsSelectedClipMenuCommands()
{
    int deleteMidiClipCalls = 0;
    int deleteAudioClipCalls = 0;
    int duplicateMidiClipCalls = 0;
    int duplicateAudioClipCalls = 0;
    int renameMidiClipCalls = 0;
    int renameAudioClipCalls = 0;
    int splitMidiClipCalls = 0;
    int splitAudioClipCalls = 0;
    int moveMidiClipToTargetTrackCalls = 0;
    int moveAudioClipToTargetTrackCalls = 0;
    int moveMidiClipLeftCalls = 0;
    int moveMidiClipRightCalls = 0;
    int trimMidiClipEndCalls = 0;
    int extendMidiClipEndCalls = 0;
    int trimMidiClipStartCalls = 0;
    int extendMidiClipStartCalls = 0;
    int moveAudioClipLeftCalls = 0;
    int moveAudioClipRightCalls = 0;
    int trimAudioClipEndCalls = 0;
    int extendAudioClipEndCalls = 0;
    int trimAudioClipStartCalls = 0;
    int extendAudioClipStartCalls = 0;

    trackloom::AppCommandHandlers handlers;
    handlers.deleteSelectedMidiClip = [&] { ++deleteMidiClipCalls; };
    handlers.deleteSelectedAudioClip = [&] { ++deleteAudioClipCalls; };
    handlers.duplicateSelectedMidiClip = [&] { ++duplicateMidiClipCalls; };
    handlers.duplicateSelectedAudioClip = [&] { ++duplicateAudioClipCalls; };
    handlers.renameSelectedMidiClip = [&] { ++renameMidiClipCalls; };
    handlers.renameSelectedAudioClip = [&] { ++renameAudioClipCalls; };
    handlers.splitSelectedMidiClip = [&] { ++splitMidiClipCalls; };
    handlers.splitSelectedAudioClip = [&] { ++splitAudioClipCalls; };
    handlers.moveSelectedMidiClipToTargetTrack = [&] { ++moveMidiClipToTargetTrackCalls; };
    handlers.moveSelectedAudioClipToTargetTrack = [&] { ++moveAudioClipToTargetTrackCalls; };
    handlers.moveSelectedMidiClipLeft = [&] { ++moveMidiClipLeftCalls; };
    handlers.moveSelectedMidiClipRight = [&] { ++moveMidiClipRightCalls; };
    handlers.trimSelectedMidiClipEnd = [&] { ++trimMidiClipEndCalls; };
    handlers.extendSelectedMidiClipEnd = [&] { ++extendMidiClipEndCalls; };
    handlers.trimSelectedMidiClipStart = [&] { ++trimMidiClipStartCalls; };
    handlers.extendSelectedMidiClipStart = [&] { ++extendMidiClipStartCalls; };
    handlers.moveSelectedAudioClipLeft = [&] { ++moveAudioClipLeftCalls; };
    handlers.moveSelectedAudioClipRight = [&] { ++moveAudioClipRightCalls; };
    handlers.trimSelectedAudioClipEnd = [&] { ++trimAudioClipEndCalls; };
    handlers.extendSelectedAudioClipEnd = [&] { ++extendAudioClipEndCalls; };
    handlers.trimSelectedAudioClipStart = [&] { ++trimAudioClipStartCalls; };
    handlers.extendSelectedAudioClipStart = [&] { ++extendAudioClipStartCalls; };

    const auto renameMidiClipResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::RenameSelectedMidiClip),
        handlers);
    const auto renameAudioClipResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::RenameSelectedAudioClip),
        handlers);
    const auto deleteMidiClipResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::DeleteSelectedMidiClip),
        handlers);
    const auto deleteAudioClipResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::DeleteSelectedAudioClip),
        handlers);
    const auto duplicateMidiClipResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::DuplicateSelectedMidiClip),
        handlers);
    const auto duplicateAudioClipResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::DuplicateSelectedAudioClip),
        handlers);
    const auto splitMidiClipResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SplitSelectedMidiClip),
        handlers);
    const auto splitAudioClipResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SplitSelectedAudioClip),
        handlers);
    const auto moveMidiClipToTargetTrackResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::MoveSelectedMidiClipToTargetTrack),
        handlers);
    const auto moveAudioClipToTargetTrackResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::MoveSelectedAudioClipToTargetTrack),
        handlers);
    const auto moveMidiClipLeftResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::MoveSelectedMidiClipLeft),
        handlers);
    const auto moveMidiClipRightResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::MoveSelectedMidiClipRight),
        handlers);
    const auto trimMidiClipEndResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::TrimSelectedMidiClipEnd),
        handlers);
    const auto extendMidiClipEndResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::ExtendSelectedMidiClipEnd),
        handlers);
    const auto trimMidiClipStartResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::TrimSelectedMidiClipStart),
        handlers);
    const auto extendMidiClipStartResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::ExtendSelectedMidiClipStart),
        handlers);
    const auto moveAudioClipLeftResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::MoveSelectedAudioClipLeft),
        handlers);
    const auto moveAudioClipRightResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::MoveSelectedAudioClipRight),
        handlers);
    const auto trimAudioClipEndResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::TrimSelectedAudioClipEnd),
        handlers);
    const auto extendAudioClipEndResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::ExtendSelectedAudioClipEnd),
        handlers);
    const auto trimAudioClipStartResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::TrimSelectedAudioClipStart),
        handlers);
    const auto extendAudioClipStartResult = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::ExtendSelectedAudioClipStart),
        handlers);

    require(renameMidiClipResult.executed
            && renameMidiClipResult.command == trackloom::AppCommandKind::RenameSelectedMidiClip,
        "selected MIDI clip rename command should dispatch to the MIDI clip rename handler");
    require(renameAudioClipResult.executed
            && renameAudioClipResult.command == trackloom::AppCommandKind::RenameSelectedAudioClip,
        "selected audio clip rename command should dispatch to the audio clip rename handler");
    require(deleteMidiClipResult.executed
            && deleteMidiClipResult.command == trackloom::AppCommandKind::DeleteSelectedMidiClip,
        "selected MIDI clip delete command should dispatch to the MIDI clip delete handler");
    require(deleteAudioClipResult.executed
            && deleteAudioClipResult.command == trackloom::AppCommandKind::DeleteSelectedAudioClip,
        "selected audio clip delete command should dispatch to the audio clip delete handler");
    require(duplicateMidiClipResult.executed
            && duplicateMidiClipResult.command == trackloom::AppCommandKind::DuplicateSelectedMidiClip,
        "selected MIDI clip duplicate command should dispatch to the MIDI clip duplicate handler");
    require(duplicateAudioClipResult.executed
            && duplicateAudioClipResult.command == trackloom::AppCommandKind::DuplicateSelectedAudioClip,
        "selected audio clip duplicate command should dispatch to the audio clip duplicate handler");
    require(splitMidiClipResult.executed
            && splitMidiClipResult.command == trackloom::AppCommandKind::SplitSelectedMidiClip,
        "selected MIDI clip split command should dispatch to the MIDI clip split handler");
    require(splitAudioClipResult.executed
            && splitAudioClipResult.command == trackloom::AppCommandKind::SplitSelectedAudioClip,
        "selected audio clip split command should dispatch to the audio clip split handler");
    require(moveMidiClipToTargetTrackResult.executed
            && moveMidiClipToTargetTrackResult.command == trackloom::AppCommandKind::MoveSelectedMidiClipToTargetTrack,
        "selected MIDI clip move-to-target command should dispatch to the MIDI clip move-to-target handler");
    require(moveAudioClipToTargetTrackResult.executed
            && moveAudioClipToTargetTrackResult.command == trackloom::AppCommandKind::MoveSelectedAudioClipToTargetTrack,
        "selected audio clip move-to-target command should dispatch to the audio clip move-to-target handler");
    require(moveMidiClipLeftResult.executed
            && moveMidiClipLeftResult.command == trackloom::AppCommandKind::MoveSelectedMidiClipLeft,
        "selected MIDI clip move-left command should dispatch to the MIDI clip move-left handler");
    require(moveMidiClipRightResult.executed
            && moveMidiClipRightResult.command == trackloom::AppCommandKind::MoveSelectedMidiClipRight,
        "selected MIDI clip move-right command should dispatch to the MIDI clip move-right handler");
    require(trimMidiClipEndResult.executed
            && trimMidiClipEndResult.command == trackloom::AppCommandKind::TrimSelectedMidiClipEnd,
        "selected MIDI clip trim-end command should dispatch to the MIDI clip trim-end handler");
    require(extendMidiClipEndResult.executed
            && extendMidiClipEndResult.command == trackloom::AppCommandKind::ExtendSelectedMidiClipEnd,
        "selected MIDI clip extend-end command should dispatch to the MIDI clip extend-end handler");
    require(trimMidiClipStartResult.executed
            && trimMidiClipStartResult.command == trackloom::AppCommandKind::TrimSelectedMidiClipStart,
        "selected MIDI clip trim-start command should dispatch to the MIDI clip trim-start handler");
    require(extendMidiClipStartResult.executed
            && extendMidiClipStartResult.command == trackloom::AppCommandKind::ExtendSelectedMidiClipStart,
        "selected MIDI clip extend-start command should dispatch to the MIDI clip extend-start handler");
    require(moveAudioClipLeftResult.executed
            && moveAudioClipLeftResult.command == trackloom::AppCommandKind::MoveSelectedAudioClipLeft,
        "selected audio clip move-left command should dispatch to the audio clip move-left handler");
    require(moveAudioClipRightResult.executed
            && moveAudioClipRightResult.command == trackloom::AppCommandKind::MoveSelectedAudioClipRight,
        "selected audio clip move-right command should dispatch to the audio clip move-right handler");
    require(trimAudioClipEndResult.executed
            && trimAudioClipEndResult.command == trackloom::AppCommandKind::TrimSelectedAudioClipEnd,
        "selected audio clip trim-end command should dispatch to the audio clip trim-end handler");
    require(extendAudioClipEndResult.executed
            && extendAudioClipEndResult.command == trackloom::AppCommandKind::ExtendSelectedAudioClipEnd,
        "selected audio clip extend-end command should dispatch to the audio clip extend-end handler");
    require(trimAudioClipStartResult.executed
            && trimAudioClipStartResult.command == trackloom::AppCommandKind::TrimSelectedAudioClipStart,
        "selected audio clip trim-start command should dispatch to the audio clip trim-start handler");
    require(extendAudioClipStartResult.executed
            && extendAudioClipStartResult.command == trackloom::AppCommandKind::ExtendSelectedAudioClipStart,
        "selected audio clip extend-start command should dispatch to the audio clip extend-start handler");
    require(renameMidiClipCalls == 1 && renameAudioClipCalls == 1
            && deleteMidiClipCalls == 1 && deleteAudioClipCalls == 1
            && duplicateMidiClipCalls == 1 && duplicateAudioClipCalls == 1
            && splitMidiClipCalls == 1 && splitAudioClipCalls == 1
            && moveMidiClipToTargetTrackCalls == 1 && moveAudioClipToTargetTrackCalls == 1
            && moveMidiClipLeftCalls == 1 && moveMidiClipRightCalls == 1
            && trimMidiClipEndCalls == 1 && extendMidiClipEndCalls == 1
            && trimMidiClipStartCalls == 1 && extendMidiClipStartCalls == 1
            && moveAudioClipLeftCalls == 1 && moveAudioClipRightCalls == 1
            && trimAudioClipEndCalls == 1 && extendAudioClipEndCalls == 1
            && trimAudioClipStartCalls == 1 && extendAudioClipStartCalls == 1,
        "selected clip menu commands should each call exactly their own handler once");
}

void commandDispatcherRunsCommandPaletteMenuCommand()
{
    int openCommandPaletteCalls = 0;

    trackloom::AppCommandHandlers handlers;
    handlers.openCommandPalette = [&] { ++openCommandPaletteCalls; };

    const auto result = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::OpenCommandPalette),
        handlers);

    require(result.executed,
        "command dispatcher should execute the command palette command when its handler exists");
    require(result.command == trackloom::AppCommandKind::OpenCommandPalette,
        "command palette menu id should resolve to the open-command-palette command kind");
    require(openCommandPaletteCalls == 1,
        "command palette command should call the open-command-palette handler exactly once");
}

void commandDispatcherRunsShortcutStatusMenuCommand()
{
    int openShortcutStatusCalls = 0;

    trackloom::AppCommandHandlers handlers;
    handlers.openShortcutStatus = [&] { ++openShortcutStatusCalls; };

    const auto result = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::OpenShortcutStatus),
        handlers);

    require(result.executed,
        "command dispatcher should execute the shortcut status command when its handler exists");
    require(result.command == trackloom::AppCommandKind::OpenShortcutStatus,
        "shortcut status menu id should resolve to the open-shortcut-status command kind");
    require(openShortcutStatusCalls == 1,
        "shortcut status command should call the open-shortcut-status handler exactly once");
}

void commandDispatcherRejectsShortcutStatusWithoutHandler()
{
    const auto result = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::OpenShortcutStatus),
        {});

    require(!result.executed,
        "shortcut status command should not be reported as executed without a handler");
    require(result.kind == trackloom::AppCommandDispatchResultKind::MissingHandler,
        "shortcut status command without a handler should report the stable missing-handler kind");
    require(result.command == trackloom::AppCommandKind::OpenShortcutStatus,
        "shortcut status missing-handler result should still identify the requested command");
}

void commandDispatcherPassesRecentProjectNumber()
{
    std::size_t openedNumber = 0;

    trackloom::AppCommandHandlers handlers;
    handlers.openRecentProject = [&](std::size_t number) { openedNumber = number; };

    const auto result = trackloom::dispatchAppCommand(
        trackloom::appMainMenuRecentProjectCommandId(3),
        handlers);

    require(result.executed,
        "recent project command should execute when the recent-project handler exists");
    require(result.command == trackloom::AppCommandKind::OpenRecentProject,
        "recent project menu id should resolve to the dynamic recent-project command kind");
    require(result.recentProjectNumber == 3,
        "recent project dispatch result should expose the visible recent-project number");
    require(openedNumber == 3,
        "recent project handler should receive the visible 1-based recent-project number");
}

void commandDispatcherRejectsUnknownOrUnboundCommands()
{
    bool saveCalled = false;

    trackloom::AppCommandHandlers handlers;
    handlers.saveProject = [&] { saveCalled = true; };

    const auto unknown = trackloom::dispatchAppCommand(42, handlers);
    const auto missingHandler = trackloom::dispatchAppCommand(
        trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::StopProject),
        handlers);

    require(!unknown.executed,
        "unknown command ids should not execute any handler");
    require(unknown.kind == trackloom::AppCommandDispatchResultKind::UnknownCommand,
        "unknown command ids should report a stable unknown-command result");
    require(!missingHandler.executed,
        "known command ids without a handler should not be reported as executed");
    require(missingHandler.kind == trackloom::AppCommandDispatchResultKind::MissingHandler,
        "known command ids without a callback should report a stable missing-handler result");
    require(!saveCalled,
        "rejecting unknown or unbound commands should not run unrelated handlers");
}

void commandShortcutsMapCommonFileKeysToMenuCommands()
{
    const auto newProject = trackloom::appCommandIdForShortcut({ 'n', true, false, false });
    const auto openProject = trackloom::appCommandIdForShortcut({ 'o', true, false, false });
    const auto saveProject = trackloom::appCommandIdForShortcut({ 's', true, false, false });
    const auto saveProjectAs = trackloom::appCommandIdForShortcut({ 's', true, true, false });

    require(newProject.has_value()
            && newProject.value() == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::NewProject),
        "Ctrl+N should map to the same new-project command id used by the file menu");
    require(openProject.has_value()
            && openProject.value() == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::OpenProject),
        "Ctrl+O should map to the same open-project command id used by the file menu");
    require(saveProject.has_value()
            && saveProject.value() == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SaveProject),
        "Ctrl+S should map to the same save command id used by the file menu");
    require(saveProjectAs.has_value()
            && saveProjectAs.value() == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SaveProjectAs),
        "Ctrl+Shift+S should map to the same save-as command id used by the file menu");
}

void commandShortcutsMapUndoRedoKeysToEditMenuCommands()
{
    const auto undoProject = trackloom::appCommandIdForShortcut({ 'z', true, false, false });
    const auto redoProject = trackloom::appCommandIdForShortcut({ 'y', true, false, false });
    const auto redoProjectAlternative = trackloom::appCommandIdForShortcut({ 'z', true, true, false });

    require(undoProject.has_value()
            && undoProject.value() == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::UndoProject),
        "Ctrl+Z should map to the same undo command id used by the edit menu");
    require(redoProject.has_value()
            && redoProject.value() == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::RedoProject),
        "Ctrl+Y should map to the same redo command id used by the edit menu");
    require(redoProjectAlternative.has_value()
            && redoProjectAlternative.value() == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::RedoProject),
        "Ctrl+Shift+Z should also map to redo for users who expect the common alternative redo shortcut");
}

void commandShortcutsMapCommandPaletteKeysToToolCommand()
{
    const auto openByCtrlK = trackloom::appCommandIdForShortcut({ 'k', true, false, false });
    const auto openByCtrlShiftP = trackloom::appCommandIdForShortcut({ 'p', true, true, false });

    require(openByCtrlK.has_value()
            && openByCtrlK.value() == trackloom::appMainMenuCommandId(
                trackloom::AppMainMenuCommand::OpenCommandPalette),
        "Ctrl+K should map to the same command palette command id used by the tools menu");
    require(openByCtrlShiftP.has_value()
            && openByCtrlShiftP.value() == trackloom::appMainMenuCommandId(
                trackloom::AppMainMenuCommand::OpenCommandPalette),
        "Ctrl+Shift+P should also open the command palette for common editor muscle memory");
}

void commandShortcutsIgnoreUnregisteredOrAmbiguousChords()
{
    require(!trackloom::appCommandIdForShortcut({ 's', false, false, false }).has_value(),
        "plain S should not trigger save without the primary modifier");
    require(!trackloom::appCommandIdForShortcut({ 's', true, false, true }).has_value(),
        "Ctrl+Alt+S should not accidentally trigger save");
    require(!trackloom::appCommandIdForShortcut({ 'x', true, false, false }).has_value(),
        "Ctrl+X is not registered in the first shortcut slice");
    require(!trackloom::appCommandIdForShortcut({ '\0', true, false, false }).has_value(),
        "empty shortcut characters should not map to commands");
}

void commandShortcutsAreSuppressedWhileCommandPaletteIsOpen()
{
    const auto saveProject = trackloom::appCommandIdForShortcut(
        { 's', true, false, false },
        trackloom::AppShortcutContext::MainWindow);
    const auto saveWhilePaletteOpen = trackloom::appCommandIdForShortcut(
        { 's', true, false, false },
        trackloom::AppShortcutContext::CommandPaletteOpen);
    const auto reopenPaletteWhileOpen = trackloom::appCommandIdForShortcut(
        { 'k', true, false, false },
        trackloom::AppShortcutContext::CommandPaletteOpen);

    require(saveProject.has_value()
            && saveProject.value() == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SaveProject),
        "shortcut context should keep normal main-window shortcuts registered");
    require(!saveWhilePaletteOpen.has_value(),
        "command palette context should suppress global save shortcuts while the search box is active");
    require(!reopenPaletteWhileOpen.has_value(),
        "command palette context should suppress global palette-open shortcuts instead of rebuilding the open session");
}

void commandShortcutsUseCustomBindingsWithContext()
{
    const auto saveProjectId = trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SaveProject);
    const auto customized = trackloom::customizeAppShortcutBindings({
        {{ 'r', true, true, false }, saveProjectId}
    });

    const auto oldSave = trackloom::appCommandIdForShortcut(
        { 's', true, false, false },
        trackloom::AppShortcutContext::MainWindow,
        customized.bindings);
    const auto newSave = trackloom::appCommandIdForShortcut(
        { 'r', true, true, false },
        trackloom::AppShortcutContext::MainWindow,
        customized.bindings);
    const auto newSaveWhilePaletteOpen = trackloom::appCommandIdForShortcut(
        { 'r', true, true, false },
        trackloom::AppShortcutContext::CommandPaletteOpen,
        customized.bindings);

    require(!oldSave.has_value(),
        "context-aware shortcut lookup should respect active bindings and remove replaced defaults");
    require(newSave.has_value() && newSave.value() == saveProjectId,
        "context-aware shortcut lookup should trigger accepted custom bindings in the main window");
    require(!newSaveWhilePaletteOpen.has_value(),
        "command palette context should suppress active custom shortcuts while the search box is active");
}

void commandShortcutCustomizationReplacesDefaultBinding()
{
    const auto saveProjectId = trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SaveProject);
    const auto openProjectId = trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::OpenProject);

    const auto customized = trackloom::customizeAppShortcutBindings({
        {{ 'r', true, true, false }, saveProjectId}
    });

    const auto oldSave = trackloom::appCommandIdForShortcut({ 's', true, false, false }, customized.bindings);
    const auto newSave = trackloom::appCommandIdForShortcut({ 'r', true, true, false }, customized.bindings);
    const auto openProject = trackloom::appCommandIdForShortcut({ 'o', true, false, false }, customized.bindings);

    require(customized.conflicts.empty(),
        "accepted shortcut customization should not report conflicts");
    require(!oldSave.has_value(),
        "accepted shortcut customization should remove the old binding for the same command");
    require(newSave.has_value() && newSave.value() == saveProjectId,
        "accepted shortcut customization should map the new chord to the requested command");
    require(openProject.has_value() && openProject.value() == openProjectId,
        "shortcut customization should keep unrelated default bindings active");
}

void commandShortcutCustomizationRejectsConflictingBinding()
{
    const auto saveProjectId = trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SaveProject);
    const auto openProjectId = trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::OpenProject);

    const auto customized = trackloom::customizeAppShortcutBindings({
        {{ 'o', true, false, false }, saveProjectId}
    });

    const auto openProject = trackloom::appCommandIdForShortcut({ 'o', true, false, false }, customized.bindings);
    const auto saveProject = trackloom::appCommandIdForShortcut({ 's', true, false, false }, customized.bindings);

    require(customized.conflicts.size() == 1,
        "conflicting shortcut customization should report exactly one rejected binding");
    require(customized.conflicts[0].existingCommandId == openProjectId,
        "shortcut conflict should identify the command that already owns the chord");
    require(customized.conflicts[0].requestedCommandId == saveProjectId,
        "shortcut conflict should identify the command that requested the chord");
    require(openProject.has_value() && openProject.value() == openProjectId,
        "conflicting shortcut customization should keep the original chord owner active");
    require(saveProject.has_value() && saveProject.value() == saveProjectId,
        "conflicting shortcut customization should keep the rejected command's old binding active");
}

void commandShortcutCustomizationAddsBindingForCommandWithoutDefault()
{
    const auto duplicateMidiClipId = trackloom::appMainMenuCommandId(
        trackloom::AppMainMenuCommand::DuplicateSelectedMidiClip);

    const auto customized = trackloom::customizeAppShortcutBindings({
        {{ 'd', true, true, false }, duplicateMidiClipId}
    });

    const auto duplicateMidiClip = trackloom::appCommandIdForShortcut(
        { 'd', true, true, false },
        customized.bindings);

    require(customized.conflicts.empty(),
        "shortcut customization should allow a non-conflicting binding for a command without a default chord");
    require(duplicateMidiClip.has_value() && duplicateMidiClip.value() == duplicateMidiClipId,
        "shortcut customization should add accepted bindings for commands that have no default shortcut");
}

void shortcutSettingsSaveAndLoadCustomBindings()
{
    removeTestWorkspace();
    const auto settingsPath = testWorkspace() / "settings" / "shortcuts.txt";
    const auto saveProjectId = trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SaveProject);

    const std::vector<trackloom::AppShortcutBinding> customBindings = {
        {{ 'r', true, true, false }, saveProjectId}
    };

    require(trackloom::saveAppShortcutCustomBindings(customBindings, settingsPath),
        "shortcut settings should save custom bindings to a local UTF-8 text file");
    require(std::filesystem::exists(settingsPath),
        "saving shortcut settings should create the settings file");

    const auto loadedBindings = trackloom::loadAppShortcutCustomBindings(settingsPath);
    const auto customized = trackloom::customizeAppShortcutBindings(loadedBindings);
    const auto oldSave = trackloom::appCommandIdForShortcut({ 's', true, false, false }, customized.bindings);
    const auto newSave = trackloom::appCommandIdForShortcut({ 'r', true, true, false }, customized.bindings);

    require(loadedBindings.size() == 1,
        "shortcut settings should reload the saved custom binding");
    require(loadedBindings[0].commandId == saveProjectId,
        "shortcut settings should preserve the saved command id");
    require(loadedBindings[0].chord.key == 'r'
            && loadedBindings[0].chord.primaryModifier
            && loadedBindings[0].chord.shift
            && !loadedBindings[0].chord.alt,
        "shortcut settings should preserve the saved shortcut chord");
    require(!oldSave.has_value(),
        "loaded shortcut customization should replace the command's old default binding");
    require(newSave.has_value() && newSave.value() == saveProjectId,
        "loaded shortcut customization should activate the saved custom binding");
}

void shortcutSettingsLoadMissingFileAsEmptyBindings()
{
    removeTestWorkspace();
    const auto missingPath = testWorkspace() / "missing" / "shortcuts.txt";

    const auto loadedBindings = trackloom::loadAppShortcutCustomBindings(missingPath);
    const auto customized = trackloom::loadAppShortcutCustomization(missingPath);
    const auto saveProject = trackloom::appCommandIdForShortcut({ 's', true, false, false }, customized.bindings);

    require(loadedBindings.empty(),
        "missing shortcut settings should load as an empty custom binding list");
    require(customized.conflicts.empty(),
        "missing shortcut settings should not invent shortcut conflicts");
    require(saveProject.has_value()
            && saveProject.value() == trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SaveProject),
        "missing shortcut settings should keep default shortcuts active");
}

void shortcutSettingsReportsLoadedConflictsThroughCustomization()
{
    removeTestWorkspace();
    const auto settingsPath = testWorkspace() / "settings" / "shortcut-conflict.txt";
    const auto saveProjectId = trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SaveProject);
    const auto openProjectId = trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::OpenProject);

    require(trackloom::saveAppShortcutCustomBindings({
                {{ 'o', true, false, false }, saveProjectId}
            },
                settingsPath),
        "conflicting shortcut settings should still save so load can report the conflict");

    const auto customized = trackloom::loadAppShortcutCustomization(settingsPath);
    const auto openProject = trackloom::appCommandIdForShortcut({ 'o', true, false, false }, customized.bindings);
    const auto saveProject = trackloom::appCommandIdForShortcut({ 's', true, false, false }, customized.bindings);

    require(customized.conflicts.size() == 1,
        "loaded shortcut settings should surface conflicts through the normal customization result");
    require(customized.conflicts[0].existingCommandId == openProjectId,
        "loaded shortcut conflict should identify the existing default command");
    require(customized.conflicts[0].requestedCommandId == saveProjectId,
        "loaded shortcut conflict should identify the requested custom command");
    require(openProject.has_value() && openProject.value() == openProjectId,
        "conflicting loaded shortcut should keep the original owner active");
    require(saveProject.has_value() && saveProject.value() == saveProjectId,
        "conflicting loaded shortcut should keep the rejected command's old default binding active");
}

void shortcutSettingsIgnoreMalformedOrUnsupportedRows()
{
    removeTestWorkspace();
    const auto settingsPath = testWorkspace() / "settings" / "shortcut-invalid.txt";
    std::filesystem::create_directories(settingsPath.parent_path());

    const auto duplicateMidiClipId = trackloom::appMainMenuCommandId(
        trackloom::AppMainMenuCommand::DuplicateSelectedMidiClip);
    std::ofstream output(settingsPath, std::ios::binary | std::ios::trunc);
    output << "# TrackLoom shortcut settings v1\n";
    output << "not-a-valid-row\n";
    output << duplicateMidiClipId << " d 0 1 0\n";
    output << duplicateMidiClipId << " d 1 1 0\n";
    output.close();

    const auto loadedBindings = trackloom::loadAppShortcutCustomBindings(settingsPath);
    const auto customized = trackloom::customizeAppShortcutBindings(loadedBindings);
    const auto duplicateMidiClip = trackloom::appCommandIdForShortcut(
        { 'd', true, true, false },
        customized.bindings);

    require(loadedBindings.size() == 1,
        "shortcut settings should ignore malformed rows and unsupported chords while keeping valid rows");
    require(duplicateMidiClip.has_value() && duplicateMidiClip.value() == duplicateMidiClipId,
        "shortcut settings should keep valid rows after ignoring invalid settings rows");
}

void shortcutStatusDescribesActiveRowsAndSummary()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Shortcut Status");
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host);
    trackloom::AppRecentProjects recent;
    const auto saveProjectId = trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SaveProject);
    const auto openProjectId = trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::OpenProject);
    const std::vector<trackloom::AppShortcutBinding> customBindings = {
        {{ 'r', true, true, false }, saveProjectId}
    };
    const auto customized = trackloom::customizeAppShortcutBindings(customBindings);

    const auto status = trackloom::describeAppShortcutStatus(
        trackloom::describeAppMainMenu(session, playback, recent),
        customized,
        customBindings);
    const auto saveProject = findShortcutStatusRow(status, saveProjectId);
    const auto openProject = findShortcutStatusRow(status, openProjectId);

    require(status.customBindingCount == 1,
        "shortcut status should report how many custom rows were loaded from settings");
    require(status.activeCustomBindingCount == 1,
        "shortcut status should count accepted custom shortcut rows separately from raw settings rows");
    require(status.conflicts.empty(),
        "shortcut status should expose an empty conflict list when custom bindings are accepted");
    require(status.summary.find("1 个自定义快捷键") != std::string::npos
            && status.summary.find("0 个冲突") != std::string::npos,
        "shortcut status summary should be ready for a settings surface to display");
    require(saveProject != nullptr && saveProject->shortcutLabel == "Ctrl+Shift+R",
        "shortcut status should show the active custom shortcut label for the affected command");
    require(saveProject != nullptr && saveProject->customized,
        "shortcut status should mark commands whose active shortcut came from user settings");
    require(openProject != nullptr && openProject->shortcutLabel == "Ctrl+O" && !openProject->customized,
        "shortcut status should keep unrelated default shortcuts visible and unmarked");
}

void shortcutStatusReportsConflictsWithMenuLabels()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Shortcut Conflict Status");
    FakeRealtimePlaybackHost host;
    trackloom::AppPlaybackController playback(host);
    trackloom::AppRecentProjects recent;
    const auto saveProjectId = trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::SaveProject);
    const auto openProjectId = trackloom::appMainMenuCommandId(trackloom::AppMainMenuCommand::OpenProject);
    const std::vector<trackloom::AppShortcutBinding> customBindings = {
        {{ 'o', true, false, false }, saveProjectId}
    };
    const auto customized = trackloom::customizeAppShortcutBindings(customBindings);

    const auto status = trackloom::describeAppShortcutStatus(
        trackloom::describeAppMainMenu(session, playback, recent),
        customized,
        customBindings);
    const auto saveProject = findShortcutStatusRow(status, saveProjectId);

    require(status.customBindingCount == 1,
        "shortcut status should keep the raw custom binding count even when a row conflicts");
    require(status.activeCustomBindingCount == 0,
        "conflicting custom bindings should not be counted as active custom shortcuts");
    require(status.conflicts.size() == 1,
        "shortcut status should expose conflicts for the future settings UI");
    require(status.summary.find("1 个冲突") != std::string::npos,
        "shortcut status summary should mention loaded shortcut conflicts");
    require(status.conflicts[0].shortcutLabel == "Ctrl+O",
        "shortcut conflict status should show the chord that caused the conflict");
    require(status.conflicts[0].existingCommandId == openProjectId
            && status.conflicts[0].existingCommandLabel == "打开工程...",
        "shortcut conflict status should name the command that already owns the chord");
    require(status.conflicts[0].requestedCommandId == saveProjectId
            && status.conflicts[0].requestedCommandLabel == "保存",
        "shortcut conflict status should name the command requested by user settings");
    require(saveProject != nullptr && saveProject->shortcutLabel == "Ctrl+S" && !saveProject->customized,
        "conflicting custom shortcuts should leave the rejected command on its default binding");
}

void trackActionCreatesDefaultInstrumentTrackAndMarksSessionDirty()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "track-action-create.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Track Action");
    require(session.saveAs(path).success,
        "track action create test should save the setup project before editing");

    const auto feedback = trackloom::createDefaultInstrumentTrack(session);

    require(feedback.success,
        "track action should create a default instrument track");
    require(feedback.kind == trackloom::AppTrackActionFeedbackKind::Success,
        "successful track create action should expose a stable success kind");
    require(!feedback.trackId.empty(),
        "successful track create action should expose the created track id");
    require(session.project().tracks().size() == 1,
        "track action should add exactly one track");
    require(session.project().tracks()[0].id == feedback.trackId,
        "track action feedback should point to the created track");
    require(session.project().tracks()[0].name == "Instrument 1",
        "first default instrument track should use the app-level starter name");
    require(session.project().tracks()[0].type == trackloom::TrackType::Instrument,
        "track action should create instrument tracks only");
    require(session.isDirty(),
        "track create action should mark the app session dirty");
    require(feedback.message.find("乐器轨") != std::string::npos,
        "successful track create feedback should describe the created instrument track");
}

void trackActionCreateCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Track Action History");

    const auto feedback = trackloom::createDefaultInstrumentTrack(session);

    require(feedback.success,
        "track action history test should create an instrument track");
    require(session.canUndoProjectEdit(),
        "track action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo track creation from the track action");
    require(session.project().tracks().empty(),
        "undoing track creation should remove the created track");
    require(session.canRedoProjectEdit(),
        "undoing track creation should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo track creation from the track action");
    require(session.project().tracks().size() == 1,
        "redoing track creation should restore the created track");
    require(session.project().tracks()[0].id == feedback.trackId,
        "redoing track creation should preserve the stable track id");
}

void trackActionNamesRepeatedDefaultInstrumentTracksByProjectOrder()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Repeated Tracks");

    const auto first = trackloom::createDefaultInstrumentTrack(session);
    const auto second = trackloom::createDefaultInstrumentTrack(session);

    require(first.success && second.success,
        "track action should create repeated default instrument tracks");
    require(session.project().tracks().size() == 2,
        "repeated track action should create two tracks");
    require(session.project().tracks()[0].name == "Instrument 1",
        "first default track should keep the first generated name");
    require(session.project().tracks()[1].name == "Instrument 2",
        "second default track should use the next generated name");
}

void trackActionCreatesDefaultAudioTrackAndMarksSessionDirty()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "track-action-create-audio.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Audio Track Action");
    require(session.saveAs(path).success,
        "audio track action create test should save the setup project before editing");

    const auto feedback = trackloom::createDefaultAudioTrack(session);

    require(feedback.success,
        "track action should create a default audio track");
    require(feedback.kind == trackloom::AppTrackActionFeedbackKind::Success,
        "successful audio track create action should expose a stable success kind");
    require(!feedback.trackId.empty(),
        "successful audio track create action should expose the created track id");
    require(session.project().tracks().size() == 1,
        "audio track create action should append one track to the project");
    require(session.project().tracks()[0].id == feedback.trackId,
        "audio track create feedback should point to the created track");
    require(session.project().tracks()[0].type == trackloom::TrackType::Audio,
        "audio track create action should create an audio track");
    require(session.project().tracks()[0].name == "Audio 1",
        "first default audio track should use the app-level starter name");
    require(session.isDirty(),
        "successful audio track create action should mark the app session dirty");
    require(feedback.message.find("音频轨") != std::string::npos,
        "successful audio track create feedback should describe the audio track creation");
}

void trackActionNamesRepeatedDefaultAudioTracksByProjectOrder()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Repeated Audio Tracks");

    const auto first = trackloom::createDefaultAudioTrack(session);
    const auto second = trackloom::createDefaultAudioTrack(session);

    require(first.success && second.success,
        "track action should create repeated default audio tracks");
    require(session.project().tracks().size() == 2,
        "repeated audio track action should create two tracks");
    require(session.project().tracks()[0].name == "Audio 1",
        "first default audio track should keep the first generated name");
    require(session.project().tracks()[1].name == "Audio 2",
        "second default audio track should use the next generated name");
}

void trackActionDeletesAudioTrackAndOwnedClips()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "track-action-delete-audio.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Delete Audio Track");
    const auto trackFeedback = trackloom::createDefaultAudioTrack(session);
    const auto clipFeedback = trackloom::createDefaultAudioClipOnTrack(session, trackFeedback.trackId);
    require(trackFeedback.success && clipFeedback.success,
        "audio track delete action test should create an audio track with one audio clip");
    require(session.saveAs(path).success,
        "audio track delete action test should save setup edits before deleting");

    const auto feedback = trackloom::deleteAudioTrackById(session, trackFeedback.trackId);

    require(feedback.success,
        "track action should delete the requested audio track");
    require(feedback.kind == trackloom::AppTrackActionFeedbackKind::Success,
        "successful audio track delete action should expose a stable success kind");
    require(feedback.trackId == trackFeedback.trackId,
        "audio track delete action should report the deleted track id");
    require(session.project().tracks().empty(),
        "audio track delete action should remove the target audio track");
    require(session.project().clips().empty(),
        "audio track delete action should remove clips owned by the deleted audio track");
    require(session.isDirty(),
        "successful audio track delete should mark the app session dirty");
    require(feedback.message.find("删除") != std::string::npos,
        "successful audio track delete feedback should describe the deletion");
}

void trackActionRejectsMissingAudioTrackDeleteWithoutDirtyingSession()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Missing Audio Track Delete");

    const auto feedback = trackloom::deleteAudioTrackById(session, "missing-track");

    require(!feedback.success,
        "audio track delete action should reject a missing track");
    require(feedback.kind == trackloom::AppTrackActionFeedbackKind::MissingTrack,
        "missing audio track delete action should expose a stable failure kind");
    require(session.project().tracks().empty(),
        "missing audio track delete action should not change tracks");
    require(!session.isDirty(),
        "missing audio track delete action should not dirty an unchanged session");
}

void trackActionRejectsNonAudioTrackDeleteWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "track-action-delete-instrument-as-audio.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Instrument Delete As Audio Track");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    require(session.saveAs(path).success,
        "non-audio track delete test should save setup edits before validation");

    const auto feedback = trackloom::deleteAudioTrackById(session, instrument.id);

    require(!feedback.success,
        "audio track delete action should reject instrument tracks");
    require(feedback.kind == trackloom::AppTrackActionFeedbackKind::IncompatibleTrackType,
        "non-audio track delete action should expose a stable failure kind");
    require(session.project().tracks().size() == 1 && session.project().tracks()[0].id == instrument.id,
        "non-audio track delete action should keep the instrument track unchanged");
    require(!session.isDirty(),
        "non-audio track delete action should not dirty an unchanged session");
}

void trackActionCreatesDefaultFolderTrackAndMarksSessionDirty()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "track-action-create-folder.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Folder Track Action");
    require(session.saveAs(path).success,
        "folder track action create test should save the setup project before editing");

    const auto feedback = trackloom::createDefaultFolderTrack(session);

    require(feedback.success,
        "track action should create a default folder track");
    require(feedback.kind == trackloom::AppTrackActionFeedbackKind::Success,
        "successful folder track create action should expose a stable success kind");
    require(!feedback.trackId.empty(),
        "successful folder track create action should expose the created track id");
    require(session.project().tracks().size() == 1,
        "folder track create action should append one track to the project");
    require(session.project().tracks()[0].id == feedback.trackId,
        "folder track create feedback should point to the created track");
    require(session.project().tracks()[0].type == trackloom::TrackType::Folder,
        "folder track create action should create a folder track");
    require(session.project().tracks()[0].name == "Folder 1",
        "first default folder track should use the app-level starter name");
    require(session.isDirty(),
        "successful folder track create action should mark the app session dirty");
    require(feedback.message.find("文件夹轨") != std::string::npos,
        "successful folder track create feedback should describe the folder track creation");
}

void trackActionNamesRepeatedDefaultFolderTracksByProjectOrder()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Repeated Folder Tracks");

    const auto first = trackloom::createDefaultFolderTrack(session);
    const auto second = trackloom::createDefaultFolderTrack(session);

    require(first.success && second.success,
        "track action should create repeated default folder tracks");
    require(session.project().tracks().size() == 2,
        "repeated folder track action should create two tracks");
    require(session.project().tracks()[0].name == "Folder 1",
        "first default folder track should keep the first generated name");
    require(session.project().tracks()[1].name == "Folder 2",
        "second default folder track should use the next generated name");
}

void trackActionDeletesInstrumentTrackAndOwnedClips()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "track-action-delete.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Delete Track");
    const auto trackFeedback = trackloom::createDefaultInstrumentTrack(session);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, trackFeedback.trackId);
    const auto noteFeedback = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    require(trackFeedback.success && clipFeedback.success && noteFeedback.success,
        "track delete action test should create a track with a MIDI clip and note");
    require(session.saveAs(path).success,
        "track delete action test should save setup edits before deleting");

    const auto feedback = trackloom::deleteInstrumentTrackById(session, trackFeedback.trackId);

    require(feedback.success,
        "track action should delete the requested instrument track");
    require(feedback.kind == trackloom::AppTrackActionFeedbackKind::Success,
        "successful track delete action should expose a stable success kind");
    require(feedback.trackId == trackFeedback.trackId,
        "track delete action should report the deleted track id");
    require(session.project().tracks().empty(),
        "track delete action should remove the target track");
    require(session.project().clips().empty(),
        "track delete action should remove clips owned by the deleted track");
    require(!session.project().findMidiNoteById(noteFeedback.noteId).has_value(),
        "track delete action should remove notes inside clips owned by the deleted track");
    require(session.isDirty(),
        "track delete action should mark the app session dirty");
    require(feedback.message.find("删除") != std::string::npos,
        "successful track delete feedback should describe the deletion");
}

void trackActionRejectsMissingTrackDeleteWithoutDirtyingSession()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Missing Track Delete");

    const auto feedback = trackloom::deleteInstrumentTrackById(session, "missing-track");

    require(!feedback.success,
        "track delete action should reject a missing track");
    require(feedback.kind == trackloom::AppTrackActionFeedbackKind::MissingTrack,
        "missing track delete action should expose a stable failure kind");
    require(session.project().tracks().empty(),
        "missing track delete action should not change tracks");
    require(!session.isDirty(),
        "missing track delete action should not dirty an unchanged session");
}

void trackActionRejectsNonInstrumentTrackDeleteWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "track-action-delete-audio.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Audio Track Delete");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    require(session.saveAs(path).success,
        "non-instrument track delete test should save setup edits before validation");

    const auto feedback = trackloom::deleteInstrumentTrackById(session, audio.id);

    require(!feedback.success,
        "instrument track delete action should reject audio tracks");
    require(feedback.kind == trackloom::AppTrackActionFeedbackKind::IncompatibleTrackType,
        "non-instrument track delete action should expose a stable failure kind");
    require(session.project().tracks().size() == 1 && session.project().tracks()[0].id == audio.id,
        "non-instrument track delete action should keep the audio track unchanged");
    require(!session.isDirty(),
        "non-instrument track delete action should not dirty an unchanged session");
}

void trackActionRenamesTrackAndMarksSessionDirty()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "track-action-rename.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Rename Track");
    const auto track = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    require(session.saveAs(path).success,
        "track rename action test should save setup edits before renaming");

    const auto feedback = trackloom::renameTrackById(session, track.id, "  Main Lead  ");

    require(feedback.success,
        "track rename action should rename an existing track");
    require(feedback.kind == trackloom::AppTrackActionFeedbackKind::Success,
        "successful track rename action should expose a stable success kind");
    require(feedback.trackId == track.id,
        "track rename action should report the renamed track id");
    require(session.project().findTrackById(track.id)->name == "Main Lead",
        "track rename action should trim outer whitespace before saving the name");
    require(session.isDirty(),
        "successful track rename should mark the app session dirty");

    const auto status = trackloom::describeAppTrackList(session.project());
    require(status.rows.size() == 1 && status.rows[0].name == "Main Lead",
        "track list status should expose the renamed track name");
}

void trackActionRejectsEmptyTrackNameWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "track-action-rename-empty.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Empty Rename");
    const auto track = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    require(session.saveAs(path).success,
        "empty track rename test should save setup edits before validation");

    const auto feedback = trackloom::renameTrackById(session, track.id, "   ");

    require(!feedback.success,
        "track rename action should reject a whitespace-only name");
    require(feedback.kind == trackloom::AppTrackActionFeedbackKind::EmptyName,
        "empty track rename action should expose a stable failure kind");
    require(session.project().findTrackById(track.id)->name == "Lead",
        "empty track rename action should keep the existing track name");
    require(!session.isDirty(),
        "empty track rename action should not dirty an unchanged session");
}

void trackActionRejectsMissingTrackRenameWithoutDirtyingSession()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Missing Rename");

    const auto feedback = trackloom::renameTrackById(session, "missing-track", "Lead");

    require(!feedback.success,
        "track rename action should reject a missing track");
    require(feedback.kind == trackloom::AppTrackActionFeedbackKind::MissingTrack,
        "missing track rename action should expose a stable failure kind");
    require(!session.isDirty(),
        "missing track rename action should not dirty an unchanged session");
}

void trackActionMovesInstrumentTrackUpAndDown()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "track-action-move.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Move Tracks");
    const auto first = session.editProject().createTrack("First", trackloom::TrackType::Instrument);
    const auto second = session.editProject().createTrack("Second", trackloom::TrackType::Instrument);
    const auto third = session.editProject().createTrack("Third", trackloom::TrackType::Instrument);
    require(session.saveAs(path).success,
        "track move action test should save setup edits before moving");

    const auto up = trackloom::moveInstrumentTrackUp(session, second.id);

    require(up.success,
        "track move action should move the selected instrument track up");
    require(up.kind == trackloom::AppTrackActionFeedbackKind::Success,
        "successful track move up should expose a stable success kind");
    require(up.trackId == second.id,
        "track move up feedback should report the moved track id");
    require(session.project().tracks()[0].id == second.id,
        "track move up should swap the selected track toward the top");
    require(session.project().tracks()[1].id == first.id,
        "track move up should shift the previous neighbor down");
    require(session.project().tracks()[2].id == third.id,
        "track move up should keep unrelated tracks in order");
    require(session.isDirty(),
        "successful track move up should mark the app session dirty");

    require(session.save().success,
        "track move action test should save after the first move before checking next dirty state");
    const auto down = trackloom::moveInstrumentTrackDown(session, second.id);

    require(down.success,
        "track move action should move the selected instrument track down");
    require(session.project().tracks()[0].id == first.id,
        "track move down should move the selected track below its next neighbor");
    require(session.project().tracks()[1].id == second.id,
        "track move down should keep the selected track id stable");
    require(session.isDirty(),
        "successful track move down should mark the app session dirty");
}

void trackActionMovesAudioTrackUpAndDown()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "audio-track-action-move.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Move Audio Tracks");
    const auto first = session.editProject().createTrack("First", trackloom::TrackType::Instrument);
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto third = session.editProject().createTrack("Third", trackloom::TrackType::Instrument);
    require(session.saveAs(path).success,
        "audio track move action test should save setup edits before moving");

    const auto up = trackloom::moveAudioTrackUp(session, audio.id);

    require(up.success,
        "audio track move action should move the selected audio track up");
    require(up.kind == trackloom::AppTrackActionFeedbackKind::Success,
        "successful audio track move up should expose a stable success kind");
    require(up.trackId == audio.id,
        "audio track move up feedback should report the moved track id");
    require(session.project().tracks()[0].id == audio.id,
        "audio track move up should swap the selected track toward the top");
    require(session.project().tracks()[1].id == first.id,
        "audio track move up should shift the previous neighbor down");
    require(session.project().tracks()[2].id == third.id,
        "audio track move up should keep unrelated tracks in order");
    require(session.isDirty(),
        "successful audio track move up should mark the app session dirty");

    require(session.save().success,
        "audio track move action test should save after the first move before checking next dirty state");
    const auto down = trackloom::moveAudioTrackDown(session, audio.id);

    require(down.success,
        "audio track move action should move the selected audio track down");
    require(session.project().tracks()[0].id == first.id,
        "audio track move down should move the selected track below its next neighbor");
    require(session.project().tracks()[1].id == audio.id,
        "audio track move down should keep the selected track id stable");
    require(session.isDirty(),
        "successful audio track move down should mark the app session dirty");
}

void trackActionRejectsMoveAtBoundariesWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "track-action-move-boundary.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Move Boundaries");
    const auto first = session.editProject().createTrack("First", trackloom::TrackType::Instrument);
    const auto second = session.editProject().createTrack("Second", trackloom::TrackType::Instrument);
    require(session.saveAs(path).success,
        "track boundary move test should save setup edits before validation");

    const auto top = trackloom::moveInstrumentTrackUp(session, first.id);
    const auto bottom = trackloom::moveInstrumentTrackDown(session, second.id);

    require(!top.success && !bottom.success,
        "track move action should reject top and bottom boundary moves");
    require(top.kind == trackloom::AppTrackActionFeedbackKind::AlreadyAtBoundary,
        "top boundary move should expose a stable boundary failure kind");
    require(bottom.kind == trackloom::AppTrackActionFeedbackKind::AlreadyAtBoundary,
        "bottom boundary move should expose a stable boundary failure kind");
    require(session.project().tracks()[0].id == first.id && session.project().tracks()[1].id == second.id,
        "failed boundary moves should keep track order unchanged");
    require(!session.isDirty(),
        "failed boundary moves should not dirty an unchanged session");
}

void trackActionRejectsInvalidMoveTargetsWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "track-action-move-invalid.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Invalid Move");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    require(session.saveAs(path).success,
        "invalid track move test should save setup edits before validation");

    const auto missing = trackloom::moveInstrumentTrackUp(session, "missing-track");
    const auto incompatible = trackloom::moveInstrumentTrackDown(session, audio.id);

    require(!missing.success,
        "track move action should reject a missing track");
    require(missing.kind == trackloom::AppTrackActionFeedbackKind::MissingTrack,
        "missing track move should expose a stable missing-track kind");
    require(!incompatible.success,
        "track move action should reject non-instrument tracks");
    require(incompatible.kind == trackloom::AppTrackActionFeedbackKind::IncompatibleTrackType,
        "non-instrument move should expose a stable incompatible-track kind");
    require(!session.isDirty(),
        "invalid track move actions should not dirty an unchanged session");
}

void trackStateActionTogglesMuteAndMarksSessionDirty()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "track-state-mute.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Mute Track");
    const auto track = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    require(session.saveAs(path).success,
        "track state mute test should save setup edits before toggling");

    const auto feedback = trackloom::toggleTrackMuted(session, track.id);

    require(feedback.success,
        "track state action should toggle mute on an existing track");
    require(feedback.kind == trackloom::AppTrackStateActionFeedbackKind::Success,
        "successful mute toggle should expose a stable success kind");
    require(feedback.target == trackloom::AppTrackStateActionTarget::Mute,
        "mute toggle feedback should expose the changed state target");
    require(feedback.trackId == track.id,
        "mute toggle feedback should report the changed track id");
    require(feedback.enabled,
        "first mute toggle should enable muted state");
    require(session.project().tracks()[0].playback.muted,
        "mute toggle should update the project playback state");
    require(session.isDirty(),
        "successful mute toggle should mark the app session dirty");
    require(feedback.message.find("静音") != std::string::npos,
        "mute toggle feedback should describe the visible action");
}

void trackStateActionMuteCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Mute Track History");
    const auto track = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);

    const auto feedback = trackloom::toggleTrackMuted(session, track.id);

    require(feedback.success,
        "track state history test should mute an existing track");
    require(session.project().tracks()[0].playback.muted,
        "mute history test should start from an enabled muted state");
    require(session.canUndoProjectEdit(),
        "mute track state action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo mute state changes from the track state action");
    require(!session.project().tracks()[0].playback.muted,
        "undoing mute state should restore the previous playback state");
    require(session.canRedoProjectEdit(),
        "undoing mute state should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo mute state changes from the track state action");
    require(session.project().tracks()[0].playback.muted,
        "redoing mute state should reapply the playback state change");
}

void trackStateActionTogglesPlaybackFlagsIndependently()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Track Playback Flags");
    const auto track = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);

    const auto mute = trackloom::toggleTrackMuted(session, track.id);
    const auto solo = trackloom::toggleTrackSoloed(session, track.id);
    const auto disabled = trackloom::toggleTrackDisabled(session, track.id);
    const auto unmute = trackloom::toggleTrackMuted(session, track.id);

    require(mute.success && solo.success && disabled.success && unmute.success,
        "track state playback toggles should all succeed on the same track");
    require(!session.project().tracks()[0].playback.muted,
        "second mute toggle should only clear muted state");
    require(session.project().tracks()[0].playback.soloed,
        "solo toggle should remain enabled after mute changes");
    require(session.project().tracks()[0].playback.disabled,
        "disabled toggle should remain enabled after mute changes");
}

void trackStateActionTogglesHiddenWithoutAffectingPlayback()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Track Hidden Flag");
    const auto track = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto mute = trackloom::toggleTrackMuted(session, track.id);

    const auto hidden = trackloom::toggleTrackHidden(session, track.id);

    require(mute.success && hidden.success,
        "hidden toggle test should set up playback and view state");
    require(hidden.target == trackloom::AppTrackStateActionTarget::Hide,
        "hidden toggle feedback should expose the changed state target");
    require(hidden.enabled,
        "first hidden toggle should enable hidden state");
    require(session.project().tracks()[0].view.hidden,
        "hidden toggle should update only the project view state");
    require(session.project().tracks()[0].playback.muted,
        "hidden toggle should not clear playback mute state");
}

void trackStateActionHiddenCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Hidden Track History");
    const auto track = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);

    const auto feedback = trackloom::toggleTrackHidden(session, track.id);

    require(feedback.success,
        "track state history test should hide an existing track");
    require(session.project().tracks()[0].view.hidden,
        "hidden history test should start from an enabled hidden state");
    require(session.canUndoProjectEdit(),
        "hidden track state action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo hidden state changes from the track state action");
    require(!session.project().tracks()[0].view.hidden,
        "undoing hidden state should restore the previous view state");
    require(session.canRedoProjectEdit(),
        "undoing hidden state should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo hidden state changes from the track state action");
    require(session.project().tracks()[0].view.hidden,
        "redoing hidden state should reapply the view state change");
}

void trackStateActionRejectsMissingTrackWithoutDirtyingSession()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Missing Track State");

    const auto feedback = trackloom::toggleTrackHidden(session, "missing-track");

    require(!feedback.success,
        "track state action should reject a missing track");
    require(feedback.kind == trackloom::AppTrackStateActionFeedbackKind::MissingTrack,
        "missing track state action should expose a stable failure kind");
    require(!session.isDirty(),
        "missing track state action should not dirty an unchanged session");
    require(!session.canUndoProjectEdit(),
        "missing track state action should not create undo history");
    require(!session.canRedoProjectEdit(),
        "missing track state action should not create redo history");
}

void trackStateActionUpdatesTrackListStatusLabels()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Track State Labels");
    const auto track = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);

    require(trackloom::toggleTrackMuted(session, track.id).success,
        "track state label test should enable muted state");
    require(trackloom::toggleTrackSoloed(session, track.id).success,
        "track state label test should enable soloed state");
    require(trackloom::toggleTrackDisabled(session, track.id).success,
        "track state label test should enable disabled state");
    require(trackloom::toggleTrackHidden(session, track.id).success,
        "track state label test should enable hidden state");

    const auto status = trackloom::describeAppTrackList(session.project());

    require(status.rows.size() == 1,
        "track list status should expose the toggled track");
    require(status.rows[0].stateLabels.size() == 4,
        "track list status should expose all toggled state labels");
    require(status.rows[0].summary.find("静音") != std::string::npos,
        "track list summary should include muted state after app toggle");
    require(status.rows[0].summary.find("独奏") != std::string::npos,
        "track list summary should include soloed state after app toggle");
    require(status.rows[0].summary.find("禁用") != std::string::npos,
        "track list summary should include disabled state after app toggle");
    require(status.rows[0].summary.find("隐藏") != std::string::npos,
        "track list summary should include hidden state after app toggle");
}

void playbackPreparationIsQueryableAndCancellableWithoutJoiningInHandlers()
{
    FakeRealtimePlaybackHost host;
    trackloom::AppProjectSession session;
    session.createNewProject("Async Preparation");
    std::latch builderEntered(1);
    std::latch releaseBuilder(1);
    std::latch builderFinished(1);

    trackloom::AppPlaybackController playback(
        host,
        [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token stopToken) {
            builderEntered.count_down();
            releaseBuilder.wait();
            trackloom::PreparedMidiPlaybackPlanBuildResult result;
            if (stopToken.stop_requested()) {
                result.failureReason = trackloom::PreparedMidiPlaybackPlanBuildFailureReason::Cancelled;
            } else {
                result = makeFakePreparedPlan(request);
            }
            builderFinished.count_down();
            return result;
        });

    const auto initial = playback.status();
    require(initial.state == trackloom::AppPlaybackState::Stopped && initial.canStart,
        "available stopped host should allow asynchronous playback start");
    const auto started = playback.start(session.capturePlaybackSnapshot());
    require(started.success, "asynchronous playback start request should be accepted");
    builderEntered.wait();
    require(playback.status().state == trackloom::AppPlaybackState::Preparing,
        "playback status should be immediately queryable while builder is blocked");
    require(!playback.status().canStart,
        "active preparation should disable a second start");

    const auto stopped = playback.stop();
    require(stopped.success, "stop during preparation should request cancellation");
    require(playback.status().state == trackloom::AppPlaybackState::Stopping,
        "stop during preparation should not wait for the worker to exit");
    releaseBuilder.count_down();
    builderFinished.wait();
    for (int attempt = 0;
         attempt < 1000 && playback.status().state == trackloom::AppPlaybackState::Stopping;
         ++attempt) {
        playback.poll(session, disabledLoopState);
        std::this_thread::yield();
    }

    const auto final = playback.status();
    require(final.state == trackloom::AppPlaybackState::Stopped,
        "cancelled preparation should become stopped only after worker completion is polled");
    require(final.failureReason == trackloom::AppPlaybackFailureReason::PreparationCancelled,
        "cancelled preparation should expose a stable failure reason");
    require(host.installCallCount == 0,
        "cancelled preparation must not install a playback plan");
}

void playbackWorkerBuildPublishesThroughIndependentCompletionState()
{
    auto completionState = std::make_shared<
        trackloom::detail::AppPlaybackPreparationCompletionState>();
    trackloom::PreparedMidiPlaybackPlanBuildRequest request {
        trackloom::Project {},
        48000.0,
        256,
        2,
        3,
        1536,
        trackloom::PlaybackLoopRange { 960, 3840 }
    };
    const trackloom::AppPlaybackPreparationKey key {
        4,
        7,
        1536,
        trackloom::PlaybackLoopRange { 960, 3840 }
    };
    trackloom::AppPreparedPlanBuildOperation build =
        [](trackloom::PreparedMidiPlaybackPlanBuildRequest buildRequest, std::stop_token) {
            return makeFakePreparedPlan(buildRequest);
        };
    std::stop_source stopSource;

    trackloom::detail::runAppPlaybackPreparationBuild(
        completionState,
        std::move(request),
        key,
        std::move(build),
        stopSource.get_token());

    require(completionState->completionPublished.load(std::memory_order_acquire),
        "independent worker completion state should publish after writing its mailbox");
    std::scoped_lock lock(completionState->mailboxMutex);
    require(completionState->mailbox.has_value()
            && completionState->mailbox->key == key
            && completionState->mailbox->result.plan != nullptr,
        "worker seam should publish the build result and accepted key without a controller");
}

void playbackPreparationInstallsAndTracksHostLifecycle()
{
    FakeRealtimePlaybackHost host;
    trackloom::AppProjectSession session;
    session.createNewProject("Lifecycle");
    std::latch built(1);
    trackloom::AppPlaybackController playback(
        host,
        [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
            auto result = makeFakePreparedPlan(request);
            built.count_down();
            return result;
        });

    require(trackloom::startAppPlayback(playback, session, disabledLoopState).success,
        "playback start action should accept asynchronous preparation");
    require(playback.status().state == trackloom::AppPlaybackState::Preparing,
        "accepted start should enter Preparing before poll installs the plan");
    built.wait();
    pollPlaybackUntilWorkerSettles(playback, session);
    require(playback.status().state == trackloom::AppPlaybackState::Playing,
        "completed preparation should install and enter Playing");
    require(host.installCallCount == 1,
        "completed preparation should install exactly one plan");
    require(!session.isDirty(), "runtime playback must not dirty the project");

    host.setRealtimePosition(512, 512);
    playback.poll(session, disabledLoopState);
    require(playback.currentSample() == 512 && playback.currentSeconds() > 0.0,
        "poll should expose the host playback position");
    require(trackloom::stopAppPlayback(playback).success,
        "playing stop action should request the host stop");
    require(playback.status().state == trackloom::AppPlaybackState::Stopping,
        "accepted host stop should enter Stopping");
    host.setRealtimeState(trackloom::RealtimePlaybackState::Stopped);
    playback.poll(session, disabledLoopState);
    require(playback.status().state == trackloom::AppPlaybackState::Stopped,
        "polled host stop completion should enter Stopped");
}

void playbackStatusAndMenuUseStructuredControllerState()
{
    FakeRealtimePlaybackHost host;
    trackloom::AppProjectSession session;
    session.createNewProject("Status");
    trackloom::AppPlaybackController playback(host);
    trackloom::AppRecentProjects recent;

    const auto stopped = playback.status();
    require(stopped.state == trackloom::AppPlaybackState::Stopped && stopped.canStart,
        "available stopped host should report a startable structured state");
    auto menu = trackloom::describeAppMainMenu(session, playback, recent);
    require(menu.groups[4].items[0].enabled && !menu.groups[4].items[1].enabled,
        "menu should enable Play from canStart and disable Stop while stopped");

    FakeRealtimePlaybackHost playingHost;
    std::latch built(1);
    trackloom::AppPlaybackController playing(
        playingHost,
        [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
            auto result = makeFakePreparedPlan(request);
            built.count_down();
            return result;
        });
    require(trackloom::toggleAppPlayback(playing, session, disabledLoopState).success,
        "toggle should start stopped playback");
    built.wait();
    pollPlaybackUntilWorkerSettles(playing, session);
    menu = trackloom::describeAppMainMenu(session, playing, recent);
    require(!menu.groups[4].items[0].enabled && menu.groups[4].items[1].enabled,
        "menu should disable Play and enable Stop while playing");
    require(playing.status().stateLabel == "播放中",
        "structured playing status should expose the playing label");
}

void playbackUnavailableHostAndFaultExposeStableReasons()
{
    FakeRealtimePlaybackHost unavailableHost;
    unavailableHost.setDeviceAvailable(false);
    trackloom::AppProjectSession session;
    trackloom::AppPlaybackController unavailable(unavailableHost);
    require(unavailable.status().state == trackloom::AppPlaybackState::Unavailable,
        "missing device should enter Unavailable");
    require(!unavailable.status().canStart,
        "missing device should disable start");
    const auto noDevice = startAppPlayback(unavailable, session, disabledLoopState);
    require(!noDevice.success
            && noDevice.failureReason == trackloom::AppPlaybackFailureReason::NoAudioDevice,
        "missing device start should expose NoAudioDevice");

    FakeRealtimePlaybackHost faultHost;
    trackloom::AppPlaybackController faulted(faultHost);
    faultHost.setRealtimeState(trackloom::RealtimePlaybackState::Faulted);
    faulted.poll(session, disabledLoopState);
    require(faulted.status().state == trackloom::AppPlaybackState::Faulted
            && faulted.status().failureReason == trackloom::AppPlaybackFailureReason::DeviceFault,
        "host fault should expose Faulted and DeviceFault");
    require(!faulted.status().canStart,
        "faulted host should disable start");
}

void playbackDropsStaleProjectAndDevicePreparationWithoutInstalling()
{
    {
        FakeRealtimePlaybackHost host;
        trackloom::AppProjectSession session;
        std::latch entered(1);
        std::latch release(1);
        trackloom::AppPlaybackController playback(
            host,
            [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
                entered.count_down();
                release.wait();
                return makeFakePreparedPlan(request);
            });
        require(playback.start(session.capturePlaybackSnapshot()).success,
            "stale project test should start preparation");
        entered.wait();
        session.editProject().rename("Edited During Preparation");
        release.count_down();
        pollPlaybackUntilWorkerSettles(playback, session);
        require(playback.status().failureReason == trackloom::AppPlaybackFailureReason::StalePreparation,
            "edited project should reject the old preparation with StalePreparation");
        require(host.installCallCount == 0,
            "stale project preparation must not call install");
    }
    {
        FakeRealtimePlaybackHost host;
        trackloom::AppProjectSession session;
        std::latch entered(1);
        std::latch release(1);
        trackloom::AppPlaybackController playback(
            host,
            [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
                entered.count_down();
                release.wait();
                return makeFakePreparedPlan(request);
            });
        require(playback.start(session.capturePlaybackSnapshot()).success,
            "stale device test should start preparation");
        entered.wait();
        host.setFormatGeneration(2);
        release.count_down();
        pollPlaybackUntilWorkerSettles(playback, session);
        require(playback.status().failureReason == trackloom::AppPlaybackFailureReason::StalePreparation,
            "changed device generation should reject old preparation");
        require(host.installCallCount == 0,
            "stale device preparation must not call install");
    }
}

void playbackPreparationAndHostFailuresUseStableReasons()
{
    trackloom::AppProjectSession session;
    {
        FakeRealtimePlaybackHost host;
        std::latch built(1);
        trackloom::AppPlaybackController playback(
            host,
            [&](trackloom::PreparedMidiPlaybackPlanBuildRequest, std::stop_token) {
                built.count_down();
                return trackloom::PreparedMidiPlaybackPlanBuildResult {
                    trackloom::PreparedMidiPlaybackPlanBuildFailureReason::InvalidLoopRange,
                    nullptr
                };
            });
        require(playback.start(session.capturePlaybackSnapshot()).success,
            "failed build test should accept asynchronous work");
        built.wait();
        pollPlaybackUntilWorkerSettles(playback, session);
        require(playback.status().state == trackloom::AppPlaybackState::Faulted
                && playback.status().failureReason == trackloom::AppPlaybackFailureReason::PreparationFailed,
            "builder failure should expose PreparationFailed");
        require(host.installCallCount == 0,
            "failed builder result must not call install");
    }
    {
        FakeRealtimePlaybackHost host;
        host.installResult = {
            false, trackloom::RealtimePlaybackHostFailureReason::DeviceStartFailed, "rejected"
        };
        std::latch built(1);
        trackloom::AppPlaybackController playback(
            host,
            [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
                auto result = makeFakePreparedPlan(request);
                built.count_down();
                return result;
            });
        require(playback.start(session.capturePlaybackSnapshot()).success,
            "host rejection test should accept asynchronous work");
        built.wait();
        pollPlaybackUntilWorkerSettles(playback, session);
        require(playback.status().state == trackloom::AppPlaybackState::Faulted
                && playback.status().failureReason == trackloom::AppPlaybackFailureReason::HostRejected,
            "install failure should expose HostRejected");
        require(host.installCallCount == 1,
            "host rejection should occur at the single install boundary");
    }
}

void playbackHostFaultDuringPreparationDiscardsCompletedPlan()
{
    FakeRealtimePlaybackHost host;
    trackloom::AppProjectSession session;
    std::latch entered(1);
    std::latch release(1);
    trackloom::AppPlaybackController playback(
        host,
        [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
            entered.count_down();
            release.wait();
            return makeFakePreparedPlan(request);
        });
    require(playback.start(session.capturePlaybackSnapshot()).success,
        "fault during preparation test should start preparation");
    entered.wait();
    host.setRealtimeState(trackloom::RealtimePlaybackState::Faulted);
    playback.poll(session, disabledLoopState);
    require(playback.status().state == trackloom::AppPlaybackState::Faulted
            && playback.status().failureReason == trackloom::AppPlaybackFailureReason::DeviceFault,
        "host fault during preparation should immediately expose DeviceFault");
    release.count_down();
    for (int attempt = 0; attempt < 10000 && host.installCallCount == 0; ++attempt) {
        playback.poll(session, disabledLoopState);
        std::this_thread::yield();
        if (playback.status().state == trackloom::AppPlaybackState::Faulted
            && playback.status().failureReason == trackloom::AppPlaybackFailureReason::DeviceFault) {
            continue;
        }
    }
    require(host.installCallCount == 0,
        "completed preparation after a host fault must not install a plan");
}

void playbackDeviceFaultDoesNotRecoverUntilItsPreparationWorkerJoins()
{
    FakeRealtimePlaybackHost host;
    trackloom::AppProjectSession session;
    std::latch entered(1);
    std::latch release(1);
    trackloom::AppPlaybackController playback(
        host,
        [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
            entered.count_down();
            release.wait();
            return makeFakePreparedPlan(request);
        });
    require(playback.start(session.capturePlaybackSnapshot()).success,
        "joinable-worker recovery test must start one blocked preparation");
    entered.wait();
    host.setRealtimeState(trackloom::RealtimePlaybackState::Faulted);
    playback.poll(session, disabledLoopState);
    require(playback.status().state == trackloom::AppPlaybackState::Faulted
            && playback.status().failureReason == trackloom::AppPlaybackFailureReason::DeviceFault,
        "a host fault while preparation is blocked must expose DeviceFault");

    host.setRealtimeState(trackloom::RealtimePlaybackState::Stopped);
    host.setDeviceAvailable(true);
    playback.poll(session, disabledLoopState);
    const auto beforeJoin = playback.status();
    if (beforeJoin.state != trackloom::AppPlaybackState::Faulted
        || beforeJoin.failureReason != trackloom::AppPlaybackFailureReason::DeviceFault) {
        release.count_down();
        throw std::runtime_error(
            "DeviceFault must remain while the recovered host still has a joinable preparation worker");
    }

    release.count_down();
    for (int attempt = 0;
         attempt < 10000
             && playback.status().state == trackloom::AppPlaybackState::Faulted;
         ++attempt) {
        playback.poll(session, disabledLoopState);
        std::this_thread::yield();
    }
    require(playback.status().state == trackloom::AppPlaybackState::Stopped
            && playback.status().failureReason == trackloom::AppPlaybackFailureReason::None,
        "DeviceFault may recover only after the released worker has been joined and discarded");
    require(host.installCallCount == 0,
        "a preparation released after DeviceFault must never install its stale plan");
}

void playbackTestToneAndActivePreparationDisableSecondStart()
{
    trackloom::AppProjectSession session;
    FakeRealtimePlaybackHost testToneHost;
    testToneHost.setRealtimeState(trackloom::RealtimePlaybackState::Playing);
    trackloom::AppPlaybackController testTone(testToneHost);
    require(!testTone.status().canStart,
        "host test tone playback should disable app playback start");
    const auto rejected = testTone.start(session.capturePlaybackSnapshot());
    require(!rejected.success
            && rejected.failureReason == trackloom::AppPlaybackFailureReason::HostRejected,
        "host test tone should reject start with HostRejected");

    FakeRealtimePlaybackHost host;
    std::latch entered(1);
    std::latch release(1);
    trackloom::AppPlaybackController preparing(
        host,
        [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
            entered.count_down();
            release.wait();
            return makeFakePreparedPlan(request);
        });
    require(preparing.start(session.capturePlaybackSnapshot()).success,
        "first preparation should be accepted");
    entered.wait();
    const auto second = preparing.start(session.capturePlaybackSnapshot());
    require(!second.success
            && second.failureReason == trackloom::AppPlaybackFailureReason::HostRejected,
        "second start should be rejected while preparation is active");
    release.count_down();
    pollPlaybackUntilWorkerSettles(preparing, session);
}

void playbackKeepsInstalledPlanDuringEditsAndRebuildsAfterStop()
{
    FakeRealtimePlaybackHost host;
    trackloom::AppProjectSession session;
    std::vector<std::string> builtProjectNames;
    std::latch firstBuilt(1);
    std::latch secondBuilt(1);
    trackloom::AppPlaybackController playback(
        host,
        [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
            builtProjectNames.push_back(request.projectSnapshot.name());
            auto result = makeFakePreparedPlan(request);
            if (builtProjectNames.size() == 1) {
                firstBuilt.count_down();
            } else {
                secondBuilt.count_down();
            }
            return result;
        });
    session.createNewProject("First Generation");
    require(playback.start(session.capturePlaybackSnapshot()).success,
        "first generation should start preparation");
    firstBuilt.wait();
    pollPlaybackUntilWorkerSettles(playback, session);
    require(playback.isPlaying(), "first generation should be playing");
    session.editProject().rename("Second Generation");
    playback.poll(session, disabledLoopState);
    require(host.installCallCount == 1 && playback.isPlaying(),
        "editing during playback should not replace the installed plan");
    require(playback.stop().success, "playing plan should accept stop");
    host.setRealtimeState(trackloom::RealtimePlaybackState::Stopped);
    playback.poll(session, disabledLoopState);
    require(playback.start(session.capturePlaybackSnapshot()).success,
        "stopped playback should accept rebuilding the edited project");
    secondBuilt.wait();
    pollPlaybackUntilWorkerSettles(playback, session);
    require(host.installCallCount == 2,
        "replay after stop should install one new plan");
    require(builtProjectNames.size() == 2
            && builtProjectNames[0] == "First Generation"
            && builtProjectNames[1] == "Second Generation",
        "replay should build from the new project generation snapshot");
}

void playbackRewindStopsThenPreparesAgainFromZero()
{
    FakeRealtimePlaybackHost host;
    trackloom::AppProjectSession session;
    session.createNewProject("Rewind");
    std::atomic<int> buildCount { 0 };
    trackloom::AppPlaybackController playback(
        host,
        [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
            ++buildCount;
            return makeFakePreparedPlan(request);
        });
    require(playback.start(session.capturePlaybackSnapshot()).success,
        "rewind test should start initial preparation");
    pollPlaybackUntilWorkerSettles(playback, session);
    host.setRealtimePosition(1024, 1024);
    playback.poll(session, disabledLoopState);

    require(playback.rewindToStart().success,
        "rewind while playing should request an orderly stop");
    require(playback.status().state == trackloom::AppPlaybackState::Stopping,
        "rewind while playing should first enter Stopping");
    require(host.stopCallCount == 1,
        "rewind while playing should request one host stop");
    host.setRealtimeState(trackloom::RealtimePlaybackState::Stopped);
    playback.poll(session, disabledLoopState);
    require(playback.status().state == trackloom::AppPlaybackState::Preparing,
        "host stop completion should begin fresh preparation for rewind");
    pollPlaybackUntilWorkerSettles(playback, session);
    require(playback.status().state == trackloom::AppPlaybackState::Playing,
        "rewind preparation should resume playback");
    require(playback.currentSample() == 0 && buildCount.load() == 2,
        "rewind should rebuild from sample zero exactly once");
}

void playbackPreparationUsesCurrentStoppedHostSampleAndRequestedLoop()
{
    FakeRealtimePlaybackHost host;
    trackloom::AppProjectSession session;
    std::int64_t capturedStart = -1;
    std::optional<trackloom::PlaybackLoopRange> capturedLoop;
    const trackloom::PlaybackLoopRange requestedLoop { 960, 3840 };
    trackloom::AppPlaybackController playback(
        host,
        [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
            capturedStart = request.playbackStartSample;
            capturedLoop = request.loopRange;
            return makeFakePreparedPlan(request);
        });
    host.setRealtimePosition(2048, 2048);
    require(playback.start(session.capturePlaybackSnapshot(), requestedLoop).success,
        "non-zero preparation should accept the current stopped position and loop");
    pollPlaybackUntilWorkerSettles(playback, session);
    require(capturedStart == 2048,
        "preparation key and build request should use the current stopped host sample");
    require(capturedLoop == requestedLoop,
        "preparation key and build request should use the requested loop");
}

void playbackCancelledPreparationCannotReplaceNewStartAndLoopIntent()
{
    FakeRealtimePlaybackHost host;
    host.setRealtimePosition(1024, 1024);
    trackloom::AppProjectSession session;
    std::latch firstEntered(1);
    std::latch releaseFirst(1);
    std::latch secondBuilt(1);
    std::vector<trackloom::PreparedMidiPlaybackPlanBuildRequest> requests;
    const trackloom::PlaybackLoopRange firstLoop { 960, 3840 };
    const trackloom::PlaybackLoopRange secondLoop { 1920, 7680 };
    trackloom::AppPlaybackController playback(
        host,
        [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request, std::stop_token) {
            requests.push_back(request);
            if (requests.size() == 1) {
                firstEntered.count_down();
                releaseFirst.wait();
            } else {
                secondBuilt.count_down();
            }
            return makeFakePreparedPlan(request);
        });

    require(playback.start(session.capturePlaybackSnapshot(), firstLoop).success,
        "first preparation should accept the old stopped sample and loop");
    firstEntered.wait();
    require(playback.stop().success,
        "first preparation should accept cancellation before its result is published");
    releaseFirst.count_down();
    pollPlaybackUntilWorkerSettles(playback, session);
    require(host.installCallCount == 0,
        "cancelled old preparation must not install before a replacement start");

    host.setRealtimePosition(4096, 4096);
    require(playback.start(session.capturePlaybackSnapshot(), secondLoop).success,
        "replacement preparation should accept the new stopped sample and loop");
    secondBuilt.wait();
    pollPlaybackUntilWorkerSettles(playback, session);

    require(requests.size() == 2,
        "cancel then restart should build exactly one request for each accepted intent");
    require(requests[0].playbackStartSample == 1024 && requests[0].loopRange == firstLoop,
        "first build should retain only the old accepted start and loop intent");
    require(requests[1].playbackStartSample == 4096 && requests[1].loopRange == secondLoop,
        "replacement build should use only the fresh start and new accepted loop intent");
    require(host.installCallCount == 1 && host.installedPlan != nullptr
            && host.installedPlan->playbackStartSample == 4096,
        "replacement should install only preparation B and never preparation A");
}
void midiClipActionCreatesDefaultClipOnInstrumentTrack()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "midi-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Clip Action");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    require(session.saveAs(path).success,
        "MIDI clip action test should save the setup project before editing");

    const auto feedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);

    require(feedback.success,
        "MIDI clip action should create a clip on an instrument track");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::Success,
        "successful MIDI clip action should expose a stable success kind");
    require(!feedback.clipId.empty(),
        "successful MIDI clip action should expose the created clip id");
    require(session.project().clips().size() == 1,
        "MIDI clip action should add exactly one clip");
    require(session.isDirty(),
        "MIDI clip action should mark the app session dirty");

    const auto clip = session.project().clips()[0];
    require(clip.id == feedback.clipId,
        "MIDI clip action feedback should point to the created clip");
    require(clip.trackId == instrument.id,
        "MIDI clip action should keep the clip on the requested track");
    require(clip.type == trackloom::ClipType::Midi,
        "MIDI clip action should create MIDI clips only");
    require(clip.startTick == 0,
        "first default MIDI clip should start at the beginning of the track");
    require(clip.lengthTick == trackloom::defaultAppMidiClipLengthTick,
        "default MIDI clip should use the app-level starter length");
    require(feedback.message.find("MIDI 片段") != std::string::npos,
        "successful MIDI clip feedback should describe the created MIDI clip");
}

void midiClipActionCreateCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Clip Create History");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);

    const auto feedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);

    require(feedback.success,
        "MIDI clip history test should create a default clip");
    require(session.project().clips().size() == 1,
        "MIDI clip history test should start with one created clip");
    require(session.canUndoProjectEdit(),
        "MIDI clip create action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo MIDI clip creation from the clip action");
    require(session.project().clips().empty(),
        "undoing MIDI clip creation should remove the created clip");
    require(session.canRedoProjectEdit(),
        "undoing MIDI clip creation should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo MIDI clip creation from the clip action");
    require(session.project().clips().size() == 1,
        "redoing MIDI clip creation should restore the created clip");
    require(session.project().clips()[0].id == feedback.clipId,
        "redoing MIDI clip creation should preserve the stable clip id");
}

void audioClipActionCreatesDefaultClipOnAudioTrack()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "audio-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Audio Clip Action");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    require(session.saveAs(path).success,
        "audio clip action test should save the setup project before editing");

    const auto feedback = trackloom::createDefaultAudioClipOnTrack(session, audio.id);

    require(feedback.success,
        "audio clip action should create a clip on an audio track");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::Success,
        "successful audio clip action should expose a stable success kind");
    require(!feedback.clipId.empty(),
        "successful audio clip action should expose the created clip id");
    require(session.project().clips().size() == 1,
        "audio clip action should add exactly one clip");
    require(session.isDirty(),
        "audio clip action should mark the app session dirty");

    const auto clip = session.project().clips()[0];
    require(clip.id == feedback.clipId,
        "audio clip action feedback should point to the created clip");
    require(clip.trackId == audio.id,
        "audio clip action should keep the clip on the requested track");
    require(clip.type == trackloom::ClipType::Audio,
        "audio clip action should create audio clips only");
    require(clip.startTick == 0,
        "first default audio clip should start at the beginning of the track");
    require(clip.lengthTick == trackloom::defaultAppAudioClipLengthTick,
        "default audio clip should use the app-level starter length");
    require(feedback.message.find("音频片段") != std::string::npos,
        "successful audio clip feedback should describe the created audio clip");
}

void audioClipActionCreateCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Audio Clip Create History");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    require(!session.canUndoProjectEdit(),
        "direct setup edits should not leave undo history before the audio clip create action");

    const auto feedback = trackloom::createDefaultAudioClipOnTrack(session, audio.id);

    require(feedback.success,
        "audio clip history test should create a default clip");
    require(session.project().clips().size() == 1,
        "audio clip history test should start with one created clip");
    require(session.canUndoProjectEdit(),
        "audio clip create action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo audio clip creation from the clip action");
    require(session.project().clips().empty(),
        "undoing audio clip creation should remove the created clip");
    require(session.canRedoProjectEdit(),
        "undoing audio clip creation should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo audio clip creation from the clip action");

    const auto restoredClip = session.project().findClipById(feedback.clipId);
    require(restoredClip.has_value(),
        "redoing audio clip creation should restore the same clip id");
    require(restoredClip->trackId == audio.id,
        "redoing audio clip creation should restore the requested audio track");
    require(restoredClip->type == trackloom::ClipType::Audio,
        "redoing audio clip creation should restore the audio clip type");
    require(restoredClip->startTick == 0 && restoredClip->lengthTick == trackloom::defaultAppAudioClipLengthTick,
        "redoing audio clip creation should restore the default timing");
}

void audioClipActionAppendsAfterExistingTrackClips()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Append Audio Clips");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);

    const auto first = trackloom::createDefaultAudioClipOnTrack(session, audio.id);
    const auto second = trackloom::createDefaultAudioClipOnTrack(session, audio.id);

    require(first.success && second.success,
        "audio clip action should create repeated default audio clips");
    require(session.project().clips().size() == 2,
        "audio clip action should keep both created clips");
    require(session.project().clips()[1].startTick == trackloom::defaultAppAudioClipLengthTick,
        "second default audio clip should append after the first audio clip");
}

void audioClipActionDuplicatesAudioClipAfterItself()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "duplicate-audio-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Duplicate Audio Clip");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clipFeedback = trackloom::createDefaultAudioClipOnTrack(session, audio.id);
    require(clipFeedback.success,
        "duplicate audio clip test should create a source audio clip");
    require(session.saveAs(path).success,
        "duplicate audio clip test should save setup edits before duplication");

    const auto feedback = trackloom::duplicateAudioClipAfterItself(session, clipFeedback.clipId);

    require(feedback.success,
        "audio clip action should duplicate the target audio clip");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::Success,
        "successful audio clip duplicate action should expose the stable success kind");
    require(feedback.clipId != clipFeedback.clipId,
        "audio clip duplicate action should report the new clip id");
    require(session.project().clips().size() == 2,
        "audio clip duplicate action should add one new clip");

    const auto source = session.project().findClipById(clipFeedback.clipId);
    const auto duplicate = session.project().findClipById(feedback.clipId);
    require(source.has_value() && duplicate.has_value(),
        "audio clip duplicate test should find source and duplicate clips");
    require(duplicate->trackId == source->trackId,
        "audio clip duplicate should stay on the source audio track");
    require(duplicate->type == trackloom::ClipType::Audio,
        "audio clip duplicate should keep the audio clip type");
    require(duplicate->startTick == source->startTick + source->lengthTick,
        "audio clip duplicate should start at the source clip end");
    require(duplicate->lengthTick == source->lengthTick,
        "audio clip duplicate should keep the source clip length");
    require(duplicate->midiNotes.empty(),
        "audio clip duplicate should not invent MIDI notes");
    require(session.isDirty(),
        "successful audio clip duplicate should mark the app session dirty");
    require(feedback.message.find("复制") != std::string::npos,
        "successful audio clip duplicate feedback should describe the copy");
}

void audioClipActionDuplicateCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Audio Clip Duplicate History");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audio.id,
        "Vocal Take",
        trackloom::ClipType::Audio,
        0,
        trackloom::defaultAppAudioClipLengthTick);
    require(clip.has_value(),
        "audio clip duplicate history test should create a source clip");
    require(!session.canUndoProjectEdit(),
        "direct setup edits should not leave undo history before the audio clip duplicate action");

    const auto feedback = trackloom::duplicateAudioClipAfterItself(session, clip->id);

    require(feedback.success,
        "audio clip duplicate history test should duplicate the source clip");
    require(session.project().clips().size() == 2,
        "audio clip duplicate history test should start with source and duplicate clips");
    const auto duplicatedClip = session.project().findClipById(feedback.clipId);
    require(duplicatedClip.has_value(),
        "audio clip duplicate history test should find the duplicate clip");
    require(duplicatedClip->startTick == clip->startTick + clip->lengthTick,
        "audio clip duplicate history test should place the duplicate after the source");
    require(session.canUndoProjectEdit(),
        "audio clip duplicate action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo audio clip duplication from the clip action");
    require(session.project().clips().size() == 1 && session.project().findClipById(clip->id).has_value(),
        "undoing audio clip duplication should keep only the source clip");
    require(!session.project().findClipById(feedback.clipId).has_value(),
        "undoing audio clip duplication should remove the duplicate id from the project");
    require(session.canRedoProjectEdit(),
        "undoing audio clip duplication should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo audio clip duplication from the clip action");

    const auto restoredDuplicate = session.project().findClipById(feedback.clipId);
    require(restoredDuplicate.has_value(),
        "redoing audio clip duplication should restore the same duplicate clip id");
    require(restoredDuplicate->trackId == audio.id
            && restoredDuplicate->type == trackloom::ClipType::Audio
            && restoredDuplicate->lengthTick == clip->lengthTick,
        "redoing audio clip duplication should restore the duplicate audio shell");
}

void audioClipActionSplitsAudioClipAtMidpoint()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "split-audio-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Split Audio Clip");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clipFeedback = trackloom::createDefaultAudioClipOnTrack(session, audio.id);
    require(clipFeedback.success,
        "split audio clip test should create a source audio clip");
    require(session.saveAs(path).success,
        "split audio clip test should save setup edits before splitting");

    const auto feedback = trackloom::splitAudioClipAtMidpoint(session, clipFeedback.clipId);

    require(feedback.success,
        "audio clip action should split the target audio clip at its midpoint");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::Success,
        "successful audio clip split action should expose the stable success kind");
    require(feedback.clipId != clipFeedback.clipId,
        "audio clip split action should report the new right-side clip id");
    require(session.project().clips().size() == 2,
        "audio clip split action should keep the shortened source and add one right-side clip");

    const auto left = session.project().findClipById(clipFeedback.clipId);
    const auto right = session.project().findClipById(feedback.clipId);
    require(left.has_value() && right.has_value(),
        "audio clip split test should find both split sides");
    require(left->trackId == audio.id && right->trackId == audio.id,
        "audio clip split should keep both sides on the source audio track");
    require(left->type == trackloom::ClipType::Audio && right->type == trackloom::ClipType::Audio,
        "audio clip split should keep both sides as audio clips");
    require(left->startTick == 0,
        "audio clip split should keep the left side at the original start");
    require(left->lengthTick == trackloom::defaultAppAudioClipLengthTick / 2,
        "audio clip split should shorten the left side to half the original length");
    require(right->startTick == trackloom::defaultAppAudioClipLengthTick / 2,
        "audio clip split should place the right side at the midpoint");
    require(right->lengthTick == trackloom::defaultAppAudioClipLengthTick - left->lengthTick,
        "audio clip split should preserve the full original duration across both sides");
    require(left->midiNotes.empty() && right->midiNotes.empty(),
        "audio clip split should not invent MIDI notes");
    require(session.isDirty(),
        "successful audio clip split should mark the app session dirty");
    require(feedback.message.find("拆分") != std::string::npos,
        "successful audio clip split feedback should describe the split");
}

void audioClipActionSplitCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Audio Clip Split History");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audio.id,
        "Vocal Take",
        trackloom::ClipType::Audio,
        0,
        trackloom::defaultAppAudioClipLengthTick);
    require(clip.has_value(),
        "audio clip split history test should create a source clip");
    require(!session.canUndoProjectEdit(),
        "direct setup edits should not leave undo history before the audio clip split action");

    const auto feedback = trackloom::splitAudioClipAtMidpoint(session, clip->id);

    require(feedback.success,
        "audio clip split history test should split the source clip");
    const auto leftAfterSplit = session.project().findClipById(clip->id);
    const auto rightAfterSplit = session.project().findClipById(feedback.clipId);
    require(leftAfterSplit.has_value() && rightAfterSplit.has_value(),
        "audio clip split history test should find both split sides");
    require(leftAfterSplit->lengthTick == trackloom::defaultAppAudioClipLengthTick / 2,
        "audio clip split history test should shorten the left clip");
    require(rightAfterSplit->startTick == leftAfterSplit->startTick + leftAfterSplit->lengthTick,
        "audio clip split history test should place the right clip after the left side");
    require(session.canUndoProjectEdit(),
        "audio clip split action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo audio clip splitting from the clip action");

    const auto restoredOriginal = session.project().findClipById(clip->id);
    require(session.project().clips().size() == 1 && restoredOriginal.has_value(),
        "undoing audio clip split should restore one original clip");
    require(!session.project().findClipById(feedback.clipId).has_value(),
        "undoing audio clip split should remove the right-side clip id");
    require(restoredOriginal->lengthTick == trackloom::defaultAppAudioClipLengthTick,
        "undoing audio clip split should restore the original clip length");
    require(session.canRedoProjectEdit(),
        "undoing audio clip split should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo audio clip splitting from the clip action");

    const auto redoLeft = session.project().findClipById(clip->id);
    const auto redoRight = session.project().findClipById(feedback.clipId);
    require(redoLeft.has_value() && redoRight.has_value(),
        "redoing audio clip split should restore both split sides");
    require(redoLeft->lengthTick == trackloom::defaultAppAudioClipLengthTick / 2,
        "redoing audio clip split should restore the shortened left clip");
    require(redoRight->type == trackloom::ClipType::Audio && redoRight->midiNotes.empty(),
        "redoing audio clip split should restore the same right-side audio shell");
}

void audioClipActionMovesAudioClipToAudioTrack()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "move-audio-clip-to-track-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Move Audio Clip To Track");
    const auto sourceTrack = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto targetTrack = session.editProject().createTrack("Harmony", trackloom::TrackType::Audio);
    const auto clipFeedback = trackloom::createDefaultAudioClipOnTrack(session, sourceTrack.id);
    require(clipFeedback.success,
        "move-to-track audio clip test should create a source audio clip");
    require(session.saveAs(path).success,
        "move-to-track audio clip test should save setup edits before moving");

    const auto feedback = trackloom::moveAudioClipToTrack(session, clipFeedback.clipId, targetTrack.id);

    const auto movedClip = session.project().findClipById(clipFeedback.clipId);
    require(feedback.success,
        "audio clip action should move the target clip to another audio track");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::Success,
        "successful audio clip move-to-track action should expose the stable success kind");
    require(feedback.clipId == clipFeedback.clipId,
        "audio clip move-to-track action should keep reporting the moved clip id");
    require(movedClip.has_value() && movedClip->trackId == targetTrack.id,
        "audio clip move-to-track action should update only the owning track id");
    require(movedClip->startTick == 0 && movedClip->lengthTick == trackloom::defaultAppAudioClipLengthTick,
        "audio clip move-to-track action should keep the clip timing unchanged");
    require(movedClip->type == trackloom::ClipType::Audio && movedClip->midiNotes.empty(),
        "audio clip move-to-track action should keep the clip as an empty audio shell");
    require(session.isDirty(),
        "successful audio clip move-to-track should mark the app session dirty");
    require(feedback.message.find("目标音频轨") != std::string::npos,
        "successful audio clip move-to-track feedback should describe the target-track move");
}

void audioClipActionMoveToTrackCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Audio Clip Track Move History");
    const auto sourceTrack = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto targetTrack = session.editProject().createTrack("Harmony", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        sourceTrack.id,
        "Vocal Take",
        trackloom::ClipType::Audio,
        0,
        trackloom::defaultAppAudioClipLengthTick);
    require(clip.has_value(),
        "audio clip track-move history test should create a source clip");
    require(!session.canUndoProjectEdit(),
        "direct setup edits should not leave undo history before the audio clip track-move action");

    const auto feedback = trackloom::moveAudioClipToTrack(session, clip->id, targetTrack.id);

    require(feedback.success,
        "audio clip track-move history test should move the source clip to the target audio track");
    const auto movedClip = session.project().findClipById(clip->id);
    require(movedClip.has_value() && movedClip->trackId == targetTrack.id,
        "audio clip track-move history test should update the owning track id");
    require(movedClip->startTick == 0 && movedClip->lengthTick == trackloom::defaultAppAudioClipLengthTick,
        "audio clip track-move history test should keep clip timing unchanged");
    require(session.canUndoProjectEdit(),
        "audio clip track-move action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo audio clip track movement from the clip action");

    const auto restoredClip = session.project().findClipById(clip->id);
    require(restoredClip.has_value() && restoredClip->trackId == sourceTrack.id,
        "undoing audio clip track movement should restore the source track id");
    require(restoredClip->type == trackloom::ClipType::Audio && restoredClip->midiNotes.empty(),
        "undoing audio clip track movement should keep the audio shell unchanged");
    require(session.canRedoProjectEdit(),
        "undoing audio clip track movement should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo audio clip track movement from the clip action");

    const auto redoneClip = session.project().findClipById(clip->id);
    require(redoneClip.has_value() && redoneClip->trackId == targetTrack.id,
        "redoing audio clip track movement should restore the target track id");
    require(redoneClip->startTick == 0 && redoneClip->lengthTick == trackloom::defaultAppAudioClipLengthTick,
        "redoing audio clip track movement should preserve the clip timing");
}

void audioClipActionRejectsMissingTrackWithoutDirtyingSession()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Missing Audio Track");

    const auto feedback = trackloom::createDefaultAudioClipOnTrack(session, "missing-track");

    require(!feedback.success,
        "audio clip action should reject a missing target track");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::MissingTrack,
        "missing audio track action should expose a stable failure kind");
    require(session.project().clips().empty(),
        "missing audio track action should not create clips");
    require(!session.isDirty(),
        "missing audio track action should not dirty an unchanged session");
}

void audioClipActionRejectsIncompatibleTrackWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "incompatible-audio-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Incompatible Audio Track");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    require(session.saveAs(path).success,
        "incompatible audio clip action test should save setup edits before validation");

    const auto feedback = trackloom::createDefaultAudioClipOnTrack(session, instrument.id);

    require(!feedback.success,
        "audio clip action should reject non-audio tracks");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::IncompatibleTrackType,
        "incompatible track audio clip action should expose a stable failure kind");
    require(session.project().clips().empty(),
        "incompatible track audio clip action should not create clips");
    require(!session.isDirty(),
        "incompatible track audio clip action should not dirty an unchanged session");
}

void audioClipActionDeletesAudioClip()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "delete-audio-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Delete Audio Clip");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clipFeedback = trackloom::createDefaultAudioClipOnTrack(session, audio.id);
    require(clipFeedback.success,
        "delete audio clip action test should create an audio clip");
    require(session.saveAs(path).success,
        "delete audio clip action test should save setup edits before deleting");

    const auto feedback = trackloom::deleteAudioClipById(session, clipFeedback.clipId);

    require(feedback.success,
        "audio clip action should delete the requested audio clip");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::Success,
        "successful audio clip delete action should expose the stable success kind");
    require(feedback.clipId == clipFeedback.clipId,
        "audio clip delete action should report the deleted clip id");
    require(session.project().clips().empty(),
        "audio clip delete action should remove the whole target clip");
    require(session.isDirty(),
        "audio clip delete action should mark the app session dirty");
    require(feedback.message.find("删除") != std::string::npos,
        "successful audio clip delete feedback should describe the deletion");
}

void audioClipActionDeleteCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Audio Clip Delete History");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audio.id,
        "Vocal Take",
        trackloom::ClipType::Audio,
        trackloom::Project::ticksPerQuarterNote,
        trackloom::defaultAppAudioClipLengthTick);
    require(clip.has_value(),
        "audio clip delete history test should create a source clip");
    require(!session.canUndoProjectEdit(),
        "direct setup edits should not leave undo history before the audio clip delete action");

    const auto feedback = trackloom::deleteAudioClipById(session, clip->id);

    require(feedback.success,
        "audio clip delete history test should delete the target clip");
    require(session.project().clips().empty(),
        "audio clip delete history test should start from a deleted clip state");
    require(session.canUndoProjectEdit(),
        "audio clip delete action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo audio clip deletion from the clip action");

    const auto restoredClip = session.project().findClipById(clip->id);
    require(restoredClip.has_value(),
        "undoing audio clip deletion should restore the deleted clip");
    require(restoredClip->name == "Vocal Take",
        "undoing audio clip deletion should restore the previous clip name");
    require(restoredClip->trackId == audio.id,
        "undoing audio clip deletion should restore the previous track");
    require(restoredClip->startTick == trackloom::Project::ticksPerQuarterNote
            && restoredClip->lengthTick == trackloom::defaultAppAudioClipLengthTick,
        "undoing audio clip deletion should restore the previous timing");
    require(session.canRedoProjectEdit(),
        "undoing audio clip deletion should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo audio clip deletion from the clip action");
    require(!session.project().findClipById(clip->id).has_value(),
        "redoing audio clip deletion should remove the clip again");
}

void audioClipActionRejectsMissingClipDeleteWithoutDirtyingSession()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Missing Audio Clip Delete");

    const auto feedback = trackloom::deleteAudioClipById(session, "missing-clip");

    require(!feedback.success,
        "audio clip delete action should reject a missing clip");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::MissingClip,
        "missing audio clip delete action should expose a stable failure kind");
    require(!session.isDirty(),
        "missing audio clip delete action should not dirty an unchanged session");
}

void audioClipActionRejectsMidiClipDeleteWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "midi-delete-audio-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Delete As Audio Clip");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto midi = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::Project::ticksPerQuarterNote);
    require(midi.has_value(),
        "MIDI-as-audio delete test should create a MIDI clip");
    require(session.saveAs(path).success,
        "MIDI-as-audio delete test should save setup edits before validation");

    const auto feedback = trackloom::deleteAudioClipById(session, midi->id);

    require(!feedback.success,
        "audio clip delete action should reject MIDI clips");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::IncompatibleClipType,
        "MIDI clip audio delete action should expose a stable failure kind");
    require(session.project().clips().size() == 1,
        "MIDI clip audio delete action should keep the original clip");
    require(!session.isDirty(),
        "MIDI clip audio delete action should not dirty an unchanged session");
}

void audioClipActionRejectsMissingClipDuplicateWithoutDirtyingSession()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Missing Audio Clip Duplicate");

    const auto feedback = trackloom::duplicateAudioClipAfterItself(session, "missing-clip");

    require(!feedback.success,
        "audio clip duplicate action should reject a missing clip");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::MissingClip,
        "missing audio clip duplicate should expose a stable failure kind");
    require(session.project().clips().empty(),
        "missing audio clip duplicate should not create clips");
    require(!session.isDirty(),
        "missing audio clip duplicate should not dirty an unchanged session");
}

void audioClipActionRejectsMidiClipDuplicateWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "midi-duplicate-audio-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Duplicate As Audio Clip");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto midi = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::Project::ticksPerQuarterNote);
    require(midi.has_value(),
        "MIDI-as-audio duplicate test should create a MIDI clip");
    require(session.saveAs(path).success,
        "MIDI-as-audio duplicate test should save setup edits before validation");

    const auto feedback = trackloom::duplicateAudioClipAfterItself(session, midi->id);

    require(!feedback.success,
        "audio clip duplicate action should reject MIDI clips");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::IncompatibleClipType,
        "MIDI clip audio duplicate should expose a stable failure kind");
    require(session.project().clips().size() == 1,
        "MIDI clip audio duplicate should keep only the original clip");
    require(!session.isDirty(),
        "MIDI clip audio duplicate should not dirty an unchanged session");
}

void audioClipActionRejectsMissingClipSplitWithoutDirtyingSession()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Missing Audio Clip Split");

    const auto feedback = trackloom::splitAudioClipAtMidpoint(session, "missing-clip");

    require(!feedback.success,
        "audio clip split action should reject a missing clip");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::MissingClip,
        "missing audio clip split should expose a stable failure kind");
    require(session.project().clips().empty(),
        "missing audio clip split should not create clips");
    require(!session.isDirty(),
        "missing audio clip split should not dirty an unchanged session");
}

void audioClipActionRejectsMidiClipSplitWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "midi-split-audio-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Split As Audio Clip");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto midi = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppAudioClipLengthTick);
    require(midi.has_value(),
        "MIDI-as-audio split test should create a MIDI clip");
    require(session.saveAs(path).success,
        "MIDI-as-audio split test should save setup edits before validation");

    const auto feedback = trackloom::splitAudioClipAtMidpoint(session, midi->id);

    require(!feedback.success,
        "audio clip split action should reject MIDI clips");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::IncompatibleClipType,
        "MIDI clip audio split should expose a stable failure kind");
    require(session.project().clips().size() == 1,
        "MIDI clip audio split should keep only the original clip");
    require(!session.isDirty(),
        "MIDI clip audio split should not dirty an unchanged session");
}

void audioClipActionRejectsTooShortClipSplitWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "short-split-audio-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Short Audio Clip Split");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audio.id,
        "Short audio",
        trackloom::ClipType::Audio,
        0,
        1);
    require(clip.has_value(),
        "too-short audio split test should create a one-tick audio clip");
    require(session.saveAs(path).success,
        "too-short audio split test should save setup edits before validation");

    const auto feedback = trackloom::splitAudioClipAtMidpoint(session, clip->id);

    require(!feedback.success,
        "audio clip split action should reject clips without a valid midpoint");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::SplitFailed,
        "too-short audio clip split should expose the stable split-failed kind");
    require(session.project().clips().size() == 1,
        "too-short audio clip split should not add a right-side clip");
    require(session.project().findClipById(clip->id)->lengthTick == 1,
        "too-short audio clip split should keep the source length unchanged");
    require(!session.isDirty(),
        "too-short audio clip split should not dirty an unchanged session");
}

void audioClipActionRejectsSameTrackMoveWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "same-track-move-audio-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Same Track Audio Move");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clipFeedback = trackloom::createDefaultAudioClipOnTrack(session, audio.id);
    require(clipFeedback.success,
        "same-track audio move test should create a source audio clip");
    require(session.saveAs(path).success,
        "same-track audio move test should save setup edits before validation");

    const auto feedback = trackloom::moveAudioClipToTrack(session, clipFeedback.clipId, audio.id);

    const auto sourceClip = session.project().findClipById(clipFeedback.clipId);
    require(!feedback.success,
        "audio clip move-to-track action should reject moving to the current track");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::MoveFailed,
        "same-track audio clip move should expose the stable move-failed kind");
    require(sourceClip.has_value() && sourceClip->trackId == audio.id,
        "same-track audio clip move should keep the source clip owner unchanged");
    require(!session.isDirty(),
        "same-track audio clip move should not dirty an unchanged session");
}

void audioClipActionRejectsMissingClipTrackMoveWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "missing-clip-track-move-audio-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Missing Audio Clip Track Move");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    require(session.saveAs(path).success,
        "missing source audio move test should save setup edits before validation");

    const auto feedback = trackloom::moveAudioClipToTrack(session, "missing-clip", audio.id);

    require(!feedback.success,
        "audio clip move-to-track action should reject a missing source clip");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::MissingClip,
        "missing source audio clip track move should expose a stable failure kind");
    require(session.project().clips().empty(),
        "missing source audio clip track move should not create clips");
    require(!session.isDirty(),
        "missing source audio clip track move should not dirty an unchanged session");
}

void audioClipActionRejectsMissingTargetTrackMoveWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "missing-target-track-move-audio-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Missing Target Audio Move");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clipFeedback = trackloom::createDefaultAudioClipOnTrack(session, audio.id);
    require(clipFeedback.success,
        "missing-target audio move test should create a source audio clip");
    require(session.saveAs(path).success,
        "missing-target audio move test should save setup edits before validation");

    const auto feedback = trackloom::moveAudioClipToTrack(session, clipFeedback.clipId, "missing-track");

    const auto sourceClip = session.project().findClipById(clipFeedback.clipId);
    require(!feedback.success,
        "audio clip move-to-track action should reject a missing target track");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::MissingTrack,
        "missing target track audio clip move should expose a stable failure kind");
    require(sourceClip.has_value() && sourceClip->trackId == audio.id,
        "missing target track audio clip move should keep the source clip owner unchanged");
    require(!session.isDirty(),
        "missing target track audio clip move should not dirty an unchanged session");
}

void audioClipActionRejectsInstrumentTargetTrackMoveWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "instrument-target-track-move-audio-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Instrument Target Audio Move");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultAudioClipOnTrack(session, audio.id);
    require(clipFeedback.success,
        "instrument-target audio move test should create a source audio clip");
    require(session.saveAs(path).success,
        "instrument-target audio move test should save setup edits before validation");

    const auto feedback = trackloom::moveAudioClipToTrack(session, clipFeedback.clipId, instrument.id);

    const auto sourceClip = session.project().findClipById(clipFeedback.clipId);
    require(!feedback.success,
        "audio clip move-to-track action should reject instrument target tracks");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::IncompatibleTrackType,
        "instrument target track audio clip move should expose a stable failure kind");
    require(sourceClip.has_value() && sourceClip->trackId == audio.id,
        "instrument target track audio clip move should keep the source clip owner unchanged");
    require(!session.isDirty(),
        "instrument target track audio clip move should not dirty an unchanged session");
}

void audioClipActionRejectsMidiClipTrackMoveWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "midi-clip-track-move-audio-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Clip Audio Track Move");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto midi = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppAudioClipLengthTick);
    require(midi.has_value(),
        "MIDI-as-audio track move test should create a MIDI clip");
    require(session.saveAs(path).success,
        "MIDI-as-audio track move test should save setup edits before validation");

    const auto feedback = trackloom::moveAudioClipToTrack(session, midi->id, audio.id);

    require(!feedback.success,
        "audio clip move-to-track action should reject MIDI clips");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::IncompatibleClipType,
        "MIDI clip audio track move should expose a stable failure kind");
    require(session.project().findClipById(midi->id)->trackId == instrument.id,
        "MIDI clip audio track move should keep the MIDI clip owner unchanged");
    require(!session.isDirty(),
        "MIDI clip audio track move should not dirty an unchanged session");
}

void audioClipActionRenamesAudioClipAndMarksSessionDirty()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "rename-audio-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Rename Audio Clip");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clipFeedback = trackloom::createDefaultAudioClipOnTrack(session, audio.id);
    require(clipFeedback.success,
        "rename audio clip test should create an audio clip");
    require(session.saveAs(path).success,
        "rename audio clip test should save setup edits before renaming");

    const auto feedback = trackloom::renameAudioClipById(session, clipFeedback.clipId, "  Verse Vocal  ");

    require(feedback.success,
        "audio clip action should rename the target audio clip");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::Success,
        "successful audio clip rename action should expose the stable success kind");
    require(feedback.clipId == clipFeedback.clipId,
        "audio clip rename action should report the renamed clip id");
    require(session.project().findClipById(clipFeedback.clipId)->name == "Verse Vocal",
        "audio clip rename action should trim surrounding whitespace before saving the name");
    require(session.isDirty(),
        "successful audio clip rename should mark the app session dirty");
    require(feedback.message.find("重命名") != std::string::npos,
        "successful audio clip rename feedback should describe the rename");
}

void audioClipActionRenameCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Audio Clip Rename History");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audio.id,
        "Raw Vocal",
        trackloom::ClipType::Audio,
        0,
        trackloom::defaultAppAudioClipLengthTick);
    require(clip.has_value(),
        "audio clip rename history test should create a source clip");
    require(!session.canUndoProjectEdit(),
        "direct setup edits should not leave undo history before the audio clip rename action");

    const auto feedback = trackloom::renameAudioClipById(session, clip->id, "  Verse Vocal  ");

    require(feedback.success,
        "audio clip rename history test should rename the target clip");
    require(session.project().findClipById(clip->id)->name == "Verse Vocal",
        "audio clip rename history test should start from the renamed clip");
    require(session.canUndoProjectEdit(),
        "audio clip rename action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo audio clip rename from the clip action");
    require(session.project().findClipById(clip->id)->name == "Raw Vocal",
        "undoing audio clip rename should restore the previous clip name");
    require(session.canRedoProjectEdit(),
        "undoing audio clip rename should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo audio clip rename from the clip action");
    require(session.project().findClipById(clip->id)->name == "Verse Vocal",
        "redoing audio clip rename should restore the renamed clip name");
}

void audioClipActionRejectsEmptyClipNameWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "rename-audio-clip-empty.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Empty Audio Clip Rename");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clipFeedback = trackloom::createDefaultAudioClipOnTrack(session, audio.id);
    require(clipFeedback.success,
        "empty audio clip rename test should create a source audio clip");
    require(session.saveAs(path).success,
        "empty audio clip rename test should save setup edits before validation");

    const auto feedback = trackloom::renameAudioClipById(session, clipFeedback.clipId, "   ");

    require(!feedback.success,
        "audio clip rename action should reject a whitespace-only name");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::EmptyName,
        "empty audio clip rename should expose a stable failure kind");
    require(session.project().findClipById(clipFeedback.clipId)->name != "   ",
        "empty audio clip rename should keep the existing clip name");
    require(!session.isDirty(),
        "empty audio clip rename should not dirty an unchanged session");
}

void audioClipActionRejectsMissingClipRenameWithoutDirtyingSession()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Missing Audio Clip Rename");

    const auto feedback = trackloom::renameAudioClipById(session, "missing-clip", "Verse");

    require(!feedback.success,
        "audio clip rename action should reject a missing clip");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::MissingClip,
        "missing audio clip rename should expose a stable failure kind");
    require(!session.isDirty(),
        "missing audio clip rename should not dirty an unchanged session");
}

void audioClipActionRejectsMidiClipRenameWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "midi-rename-audio-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Rename As Audio Clip");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto midi = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::Project::ticksPerQuarterNote);
    require(midi.has_value(),
        "MIDI-as-audio rename test should create a MIDI clip");
    require(session.saveAs(path).success,
        "MIDI-as-audio rename test should save setup edits before validation");

    const auto feedback = trackloom::renameAudioClipById(session, midi->id, "Verse");

    require(!feedback.success,
        "audio clip rename action should reject MIDI clips");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::IncompatibleClipType,
        "MIDI clip audio rename should expose a stable failure kind");
    require(session.project().findClipById(midi->id)->name == "Lead MIDI",
        "MIDI clip audio rename should keep the original clip name");
    require(!session.isDirty(),
        "MIDI clip audio rename should not dirty an unchanged session");
}

void audioClipActionMovesAudioClipRightOneBeat()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "move-audio-clip-right-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Move Audio Clip Right");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clipFeedback = trackloom::createDefaultAudioClipOnTrack(session, audio.id);
    require(clipFeedback.success,
        "move-right audio clip test should create a source audio clip");
    require(session.saveAs(path).success,
        "move-right audio clip test should save setup edits before moving");

    const auto feedback = trackloom::moveAudioClipRightOneBeat(session, clipFeedback.clipId);

    const auto movedClip = session.project().findClipById(clipFeedback.clipId);
    require(feedback.success,
        "audio clip action should move the target clip right by one beat");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::Success,
        "successful audio clip move-right action should expose the stable success kind");
    require(feedback.clipId == clipFeedback.clipId,
        "audio clip move-right action should keep reporting the moved clip id");
    require(movedClip.has_value() && movedClip->startTick == trackloom::Project::ticksPerQuarterNote,
        "audio clip move-right action should add one quarter-note tick span to the clip start");
    require(movedClip->lengthTick == trackloom::defaultAppAudioClipLengthTick,
        "audio clip move-right action should keep the clip length unchanged");
    require(session.isDirty(),
        "successful audio clip move-right should mark the app session dirty");
    require(feedback.message.find("右移") != std::string::npos,
        "successful audio clip move-right feedback should describe the direction");
}

void audioClipActionMovesAudioClipLeftOneBeat()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "move-audio-clip-left-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Move Audio Clip Left");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audio.id,
        "Verse Vocal",
        trackloom::ClipType::Audio,
        trackloom::Project::ticksPerQuarterNote,
        trackloom::defaultAppAudioClipLengthTick);
    require(clip.has_value(),
        "move-left audio clip test should create an audio clip after the timeline start");
    require(session.saveAs(path).success,
        "move-left audio clip test should save setup edits before moving");

    const auto feedback = trackloom::moveAudioClipLeftOneBeat(session, clip->id);

    const auto movedClip = session.project().findClipById(clip->id);
    require(feedback.success,
        "audio clip action should move the target clip left by one beat");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::Success,
        "successful audio clip move-left action should expose the stable success kind");
    require(movedClip.has_value() && movedClip->startTick == 0,
        "audio clip move-left action should subtract one quarter-note tick span from the clip start");
    require(movedClip->lengthTick == trackloom::defaultAppAudioClipLengthTick,
        "audio clip move-left action should keep the clip length unchanged");
    require(session.isDirty(),
        "successful audio clip move-left should mark the app session dirty");
}

void audioClipActionMoveCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Audio Clip Move History");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audio.id,
        "Verse Vocal",
        trackloom::ClipType::Audio,
        0,
        trackloom::defaultAppAudioClipLengthTick);
    require(clip.has_value(),
        "audio clip move history test should create a source clip");
    require(!session.canUndoProjectEdit(),
        "direct setup edits should not leave undo history before the audio clip move action");

    const auto feedback = trackloom::moveAudioClipRightOneBeat(session, clip->id);

    require(feedback.success,
        "audio clip move history test should move the clip right");
    const auto movedClip = session.project().findClipById(clip->id);
    require(movedClip.has_value()
            && movedClip->startTick == trackloom::Project::ticksPerQuarterNote
            && movedClip->lengthTick == trackloom::defaultAppAudioClipLengthTick,
        "audio clip move history test should update only the empty shell start");
    require(session.canUndoProjectEdit(),
        "audio clip move action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo audio clip movement from the clip action");

    const auto restoredClip = session.project().findClipById(clip->id);
    require(restoredClip.has_value()
            && restoredClip->startTick == 0
            && restoredClip->lengthTick == trackloom::defaultAppAudioClipLengthTick,
        "undoing audio clip movement should restore the original timing");
    require(session.canRedoProjectEdit(),
        "undoing audio clip movement should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo audio clip movement from the clip action");

    const auto redoneClip = session.project().findClipById(clip->id);
    require(redoneClip.has_value()
            && redoneClip->startTick == trackloom::Project::ticksPerQuarterNote
            && redoneClip->lengthTick == trackloom::defaultAppAudioClipLengthTick,
        "redoing audio clip movement should restore the moved timing");
}

void audioClipActionRejectsLeftMoveBeforeTimelineStartWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "move-audio-clip-before-start.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Move Audio Clip Before Start");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clipFeedback = trackloom::createDefaultAudioClipOnTrack(session, audio.id);
    require(clipFeedback.success,
        "audio clip boundary test should create a clip at the timeline start");
    require(session.saveAs(path).success,
        "audio clip boundary test should save setup edits before validation");

    const auto feedback = trackloom::moveAudioClipLeftOneBeat(session, clipFeedback.clipId);

    const auto unchangedClip = session.project().findClipById(clipFeedback.clipId);
    require(!feedback.success,
        "audio clip action should reject moving left before the timeline start");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::MoveFailed,
        "audio clip left-boundary failure should expose a stable move failure kind");
    require(unchangedClip.has_value() && unchangedClip->startTick == 0,
        "audio clip left-boundary failure should keep the clip start unchanged");
    require(!session.isDirty(),
        "audio clip left-boundary failure should not dirty an unchanged session");
}

void audioClipActionRejectsMissingClipMoveWithoutDirtyingSession()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Missing Audio Clip Move");

    const auto feedback = trackloom::moveAudioClipRightOneBeat(session, "missing-clip");

    require(!feedback.success,
        "audio clip move action should reject a missing clip");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::MissingClip,
        "missing audio clip move should expose a stable failure kind");
    require(!session.isDirty(),
        "missing audio clip move should not dirty an unchanged session");
}

void audioClipActionRejectsMidiClipMoveWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "midi-move-audio-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Move As Audio Clip");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto midi = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        trackloom::Project::ticksPerQuarterNote,
        trackloom::Project::ticksPerQuarterNote);
    require(midi.has_value(),
        "MIDI-as-audio move test should create a MIDI clip");
    require(session.saveAs(path).success,
        "MIDI-as-audio move test should save setup edits before validation");

    const auto feedback = trackloom::moveAudioClipLeftOneBeat(session, midi->id);

    const auto unchangedClip = session.project().findClipById(midi->id);
    require(!feedback.success,
        "audio clip move action should reject MIDI clips");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::IncompatibleClipType,
        "MIDI clip audio move should expose a stable failure kind");
    require(unchangedClip.has_value()
            && unchangedClip->startTick == trackloom::Project::ticksPerQuarterNote,
        "MIDI clip audio move should keep the original clip start");
    require(!session.isDirty(),
        "MIDI clip audio move should not dirty an unchanged session");
}

void audioClipActionTrimsAudioClipEndEarlierOneBeat()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "trim-audio-clip-end-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Trim Audio Clip End");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clipFeedback = trackloom::createDefaultAudioClipOnTrack(session, audio.id);
    require(clipFeedback.success,
        "trim-end audio clip test should create a source audio clip");
    require(session.saveAs(path).success,
        "trim-end audio clip test should save setup edits before trimming");

    const auto feedback = trackloom::trimAudioClipEndEarlierOneBeat(session, clipFeedback.clipId);

    const auto trimmedClip = session.project().findClipById(clipFeedback.clipId);
    require(feedback.success,
        "audio clip action should trim the target clip end earlier by one beat");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::Success,
        "successful audio clip trim-end action should expose the stable success kind");
    require(feedback.clipId == clipFeedback.clipId,
        "audio clip trim-end action should report the trimmed clip id");
    require(trimmedClip.has_value() && trimmedClip->startTick == 0,
        "audio clip trim-end action should keep the clip start unchanged");
    require(trimmedClip->lengthTick == trackloom::defaultAppAudioClipLengthTick - trackloom::Project::ticksPerQuarterNote,
        "audio clip trim-end action should shorten the clip by one quarter-note tick span");
    require(session.isDirty(),
        "successful audio clip trim-end should mark the app session dirty");
    require(feedback.message.find("缩短") != std::string::npos,
        "successful audio clip trim-end feedback should describe the action");
}

void audioClipActionTrimEndCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Audio Clip Trim End History");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audio.id,
        "Verse Vocal",
        trackloom::ClipType::Audio,
        0,
        trackloom::defaultAppAudioClipLengthTick);
    require(clip.has_value(),
        "audio clip trim-end history test should create a source clip");
    require(!session.canUndoProjectEdit(),
        "direct setup edits should not leave undo history before the audio clip trim-end action");

    const auto feedback = trackloom::trimAudioClipEndEarlierOneBeat(session, clip->id);

    require(feedback.success,
        "audio clip trim-end history test should trim the clip end");
    const auto trimmedClip = session.project().findClipById(clip->id);
    require(trimmedClip.has_value()
            && trimmedClip->startTick == 0
            && trimmedClip->lengthTick == trackloom::defaultAppAudioClipLengthTick - trackloom::Project::ticksPerQuarterNote,
        "audio clip trim-end history test should shorten only the empty shell length");
    require(session.canUndoProjectEdit(),
        "audio clip trim-end action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo audio clip end trimming from the clip action");

    const auto restoredClip = session.project().findClipById(clip->id);
    require(restoredClip.has_value()
            && restoredClip->startTick == 0
            && restoredClip->lengthTick == trackloom::defaultAppAudioClipLengthTick,
        "undoing audio clip end trimming should restore the original timing");
    require(session.canRedoProjectEdit(),
        "undoing audio clip end trimming should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo audio clip end trimming from the clip action");

    const auto redoneClip = session.project().findClipById(clip->id);
    require(redoneClip.has_value()
            && redoneClip->lengthTick == trackloom::defaultAppAudioClipLengthTick - trackloom::Project::ticksPerQuarterNote,
        "redoing audio clip end trimming should restore the shortened length");
}

void audioClipActionExtendsAudioClipEndLaterOneBeat()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "extend-audio-clip-end-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Extend Audio Clip End");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clipFeedback = trackloom::createDefaultAudioClipOnTrack(session, audio.id);
    require(clipFeedback.success,
        "extend-end audio clip test should create a source audio clip");
    require(session.saveAs(path).success,
        "extend-end audio clip test should save setup edits before extending");

    const auto feedback = trackloom::extendAudioClipEndLaterOneBeat(session, clipFeedback.clipId);

    const auto extendedClip = session.project().findClipById(clipFeedback.clipId);
    require(feedback.success,
        "audio clip action should extend the target clip end later by one beat");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::Success,
        "successful audio clip extend-end action should expose the stable success kind");
    require(feedback.clipId == clipFeedback.clipId,
        "audio clip extend-end action should report the extended clip id");
    require(extendedClip.has_value() && extendedClip->startTick == 0,
        "audio clip extend-end action should keep the clip start unchanged");
    require(extendedClip->lengthTick == trackloom::defaultAppAudioClipLengthTick + trackloom::Project::ticksPerQuarterNote,
        "audio clip extend-end action should lengthen the clip by one quarter-note tick span");
    require(session.isDirty(),
        "successful audio clip extend-end should mark the app session dirty");
    require(feedback.message.find("延长") != std::string::npos,
        "successful audio clip extend-end feedback should describe the action");
}

void audioClipActionExtendEndCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Audio Clip Extend End History");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audio.id,
        "Verse Vocal",
        trackloom::ClipType::Audio,
        0,
        trackloom::defaultAppAudioClipLengthTick);
    require(clip.has_value(),
        "audio clip extend-end history test should create a source clip");
    require(!session.canUndoProjectEdit(),
        "direct setup edits should not leave undo history before the audio clip extend-end action");

    const auto feedback = trackloom::extendAudioClipEndLaterOneBeat(session, clip->id);

    require(feedback.success,
        "audio clip extend-end history test should extend the clip end");
    const auto extendedClip = session.project().findClipById(clip->id);
    require(extendedClip.has_value()
            && extendedClip->startTick == 0
            && extendedClip->lengthTick == trackloom::defaultAppAudioClipLengthTick + trackloom::Project::ticksPerQuarterNote,
        "audio clip extend-end history test should lengthen only the empty shell");
    require(session.canUndoProjectEdit(),
        "audio clip extend-end action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo audio clip end extension from the clip action");

    const auto restoredClip = session.project().findClipById(clip->id);
    require(restoredClip.has_value()
            && restoredClip->startTick == 0
            && restoredClip->lengthTick == trackloom::defaultAppAudioClipLengthTick,
        "undoing audio clip end extension should restore the original timing");
    require(session.canRedoProjectEdit(),
        "undoing audio clip end extension should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo audio clip end extension from the clip action");

    const auto redoneClip = session.project().findClipById(clip->id);
    require(redoneClip.has_value()
            && redoneClip->lengthTick == trackloom::defaultAppAudioClipLengthTick + trackloom::Project::ticksPerQuarterNote,
        "redoing audio clip end extension should restore the extended length");
}

void audioClipActionTrimsAudioClipStartLaterOneBeat()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "trim-audio-clip-start-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Trim Audio Clip Start");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clipFeedback = trackloom::createDefaultAudioClipOnTrack(session, audio.id);
    require(clipFeedback.success,
        "trim-start audio clip test should create a source audio clip");
    require(session.saveAs(path).success,
        "trim-start audio clip test should save setup edits before trimming");

    const auto feedback = trackloom::trimAudioClipStartLaterOneBeat(session, clipFeedback.clipId);

    const auto trimmedClip = session.project().findClipById(clipFeedback.clipId);
    require(feedback.success,
        "audio clip action should trim the target clip start later by one beat");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::Success,
        "successful audio clip trim-start action should expose the stable success kind");
    require(feedback.clipId == clipFeedback.clipId,
        "audio clip trim-start action should report the trimmed clip id");
    require(trimmedClip.has_value()
            && trimmedClip->startTick == trackloom::Project::ticksPerQuarterNote,
        "audio clip trim-start action should move the empty shell start right by one beat");
    require(trimmedClip->lengthTick == trackloom::defaultAppAudioClipLengthTick - trackloom::Project::ticksPerQuarterNote,
        "audio clip trim-start action should keep the old end stable by shortening the shell");
    require(session.isDirty(),
        "successful audio clip trim-start should mark the app session dirty");
    require(feedback.message.find("片头") != std::string::npos,
        "successful audio clip trim-start feedback should describe the edited boundary");
}

void audioClipActionTrimStartCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Audio Clip Trim Start History");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audio.id,
        "Verse Vocal",
        trackloom::ClipType::Audio,
        0,
        trackloom::defaultAppAudioClipLengthTick);
    require(clip.has_value(),
        "audio clip trim-start history test should create a source clip");
    require(!session.canUndoProjectEdit(),
        "direct setup edits should not leave undo history before the audio clip trim-start action");

    const auto feedback = trackloom::trimAudioClipStartLaterOneBeat(session, clip->id);

    require(feedback.success,
        "audio clip trim-start history test should trim the clip start");
    const auto trimmedClip = session.project().findClipById(clip->id);
    require(trimmedClip.has_value()
            && trimmedClip->startTick == trackloom::Project::ticksPerQuarterNote
            && trimmedClip->lengthTick == trackloom::defaultAppAudioClipLengthTick - trackloom::Project::ticksPerQuarterNote,
        "audio clip trim-start history test should move the left boundary and keep the old end stable");
    require(session.canUndoProjectEdit(),
        "audio clip trim-start action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo audio clip start trimming from the clip action");

    const auto restoredClip = session.project().findClipById(clip->id);
    require(restoredClip.has_value()
            && restoredClip->startTick == 0
            && restoredClip->lengthTick == trackloom::defaultAppAudioClipLengthTick,
        "undoing audio clip start trimming should restore the original timing");
    require(session.canRedoProjectEdit(),
        "undoing audio clip start trimming should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo audio clip start trimming from the clip action");

    const auto redoneClip = session.project().findClipById(clip->id);
    require(redoneClip.has_value()
            && redoneClip->startTick == trackloom::Project::ticksPerQuarterNote
            && redoneClip->lengthTick == trackloom::defaultAppAudioClipLengthTick - trackloom::Project::ticksPerQuarterNote,
        "redoing audio clip start trimming should restore the moved left boundary");
}

void audioClipActionExtendsAudioClipStartEarlierOneBeat()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "extend-audio-clip-start-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Extend Audio Clip Start");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audio.id,
        "Verse Vocal",
        trackloom::ClipType::Audio,
        trackloom::Project::ticksPerQuarterNote,
        trackloom::defaultAppAudioClipLengthTick);
    require(clip.has_value(),
        "extend-start audio clip test should create an audio clip after the timeline start");
    require(session.saveAs(path).success,
        "extend-start audio clip test should save setup edits before extending");

    const auto feedback = trackloom::extendAudioClipStartEarlierOneBeat(session, clip->id);

    const auto extendedClip = session.project().findClipById(clip->id);
    require(feedback.success,
        "audio clip action should extend the target clip start earlier by one beat");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::Success,
        "successful audio clip extend-start action should expose the stable success kind");
    require(feedback.clipId == clip->id,
        "audio clip extend-start action should report the extended clip id");
    require(extendedClip.has_value() && extendedClip->startTick == 0,
        "audio clip extend-start action should move the empty shell start left by one beat");
    require(extendedClip->lengthTick == trackloom::defaultAppAudioClipLengthTick + trackloom::Project::ticksPerQuarterNote,
        "audio clip extend-start action should keep the old end stable by lengthening the shell");
    require(session.isDirty(),
        "successful audio clip extend-start should mark the app session dirty");
    require(feedback.message.find("片头") != std::string::npos,
        "successful audio clip extend-start feedback should describe the edited boundary");
}

void audioClipActionExtendStartCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Audio Clip Extend Start History");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audio.id,
        "Verse Vocal",
        trackloom::ClipType::Audio,
        trackloom::Project::ticksPerQuarterNote,
        trackloom::defaultAppAudioClipLengthTick);
    require(clip.has_value(),
        "audio clip extend-start history test should create a source clip after the timeline start");
    require(!session.canUndoProjectEdit(),
        "direct setup edits should not leave undo history before the audio clip extend-start action");

    const auto feedback = trackloom::extendAudioClipStartEarlierOneBeat(session, clip->id);

    require(feedback.success,
        "audio clip extend-start history test should extend the clip start");
    const auto extendedClip = session.project().findClipById(clip->id);
    require(extendedClip.has_value()
            && extendedClip->startTick == 0
            && extendedClip->lengthTick == trackloom::defaultAppAudioClipLengthTick + trackloom::Project::ticksPerQuarterNote,
        "audio clip extend-start history test should move the left boundary and keep the old end stable");
    require(session.canUndoProjectEdit(),
        "audio clip extend-start action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo audio clip start extension from the clip action");

    const auto restoredClip = session.project().findClipById(clip->id);
    require(restoredClip.has_value()
            && restoredClip->startTick == trackloom::Project::ticksPerQuarterNote
            && restoredClip->lengthTick == trackloom::defaultAppAudioClipLengthTick,
        "undoing audio clip start extension should restore the original timing");
    require(session.canRedoProjectEdit(),
        "undoing audio clip start extension should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo audio clip start extension from the clip action");

    const auto redoneClip = session.project().findClipById(clip->id);
    require(redoneClip.has_value()
            && redoneClip->startTick == 0
            && redoneClip->lengthTick == trackloom::defaultAppAudioClipLengthTick + trackloom::Project::ticksPerQuarterNote,
        "redoing audio clip start extension should restore the extended left boundary");
}

void audioClipActionRejectsTooShortClipEndTrimWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "short-trim-audio-clip-end-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Short Audio Clip Trim");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audio.id,
        "Short audio",
        trackloom::ClipType::Audio,
        0,
        trackloom::Project::ticksPerQuarterNote);
    require(clip.has_value(),
        "too-short audio trim-end test should create a one-beat audio clip");
    require(session.saveAs(path).success,
        "too-short audio trim-end test should save setup edits before validation");

    const auto feedback = trackloom::trimAudioClipEndEarlierOneBeat(session, clip->id);

    require(!feedback.success,
        "audio clip trim-end action should reject clips that cannot stay positive length");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::TrimFailed,
        "too-short audio clip trim-end should expose the stable trim-failed kind");
    require(session.project().findClipById(clip->id)->lengthTick == trackloom::Project::ticksPerQuarterNote,
        "too-short audio clip trim-end should keep the source clip length unchanged");
    require(!session.isDirty(),
        "too-short audio clip trim-end should not dirty an unchanged session");
}

void audioClipActionRejectsMissingClipEndTrimWithoutDirtyingSession()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Missing Audio Clip Trim");

    const auto feedback = trackloom::trimAudioClipEndEarlierOneBeat(session, "missing-clip");

    require(!feedback.success,
        "audio clip trim-end action should reject a missing clip");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::MissingClip,
        "missing audio clip trim-end should expose a stable failure kind");
    require(!session.isDirty(),
        "missing audio clip trim-end should not dirty an unchanged session");
}

void audioClipActionRejectsMidiClipEndTrimWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "midi-trim-audio-clip-end-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Clip Trim As Audio");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto midi = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppAudioClipLengthTick);
    require(midi.has_value(),
        "MIDI-as-audio trim-end test should create a MIDI clip");
    require(session.saveAs(path).success,
        "MIDI-as-audio trim-end test should save setup edits before validation");

    const auto feedback = trackloom::trimAudioClipEndEarlierOneBeat(session, midi->id);

    require(!feedback.success,
        "audio clip trim-end action should reject MIDI clips");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::IncompatibleClipType,
        "MIDI clip audio trim-end should expose a stable failure kind");
    require(session.project().findClipById(midi->id)->lengthTick == trackloom::defaultAppAudioClipLengthTick,
        "MIDI clip audio trim-end should keep the original clip length");
    require(!session.isDirty(),
        "MIDI clip audio trim-end should not dirty an unchanged session");
}

void audioClipActionRejectsMissingClipEndExtendWithoutDirtyingSession()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Missing Audio Clip Extend");

    const auto feedback = trackloom::extendAudioClipEndLaterOneBeat(session, "missing-clip");

    require(!feedback.success,
        "audio clip extend-end action should reject a missing clip");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::MissingClip,
        "missing audio clip extend-end should expose a stable failure kind");
    require(!session.isDirty(),
        "missing audio clip extend-end should not dirty an unchanged session");
}

void audioClipActionRejectsMidiClipEndExtendWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "midi-extend-audio-clip-end-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Clip Extend As Audio");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto midi = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppAudioClipLengthTick);
    require(midi.has_value(),
        "MIDI-as-audio extend-end test should create a MIDI clip");
    require(session.saveAs(path).success,
        "MIDI-as-audio extend-end test should save setup edits before validation");

    const auto feedback = trackloom::extendAudioClipEndLaterOneBeat(session, midi->id);

    require(!feedback.success,
        "audio clip extend-end action should reject MIDI clips");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::IncompatibleClipType,
        "MIDI clip audio extend-end should expose a stable failure kind");
    require(session.project().findClipById(midi->id)->lengthTick == trackloom::defaultAppAudioClipLengthTick,
        "MIDI clip audio extend-end should keep the original clip length");
    require(!session.isDirty(),
        "MIDI clip audio extend-end should not dirty an unchanged session");
}

void audioClipActionRejectsTooShortClipStartTrimWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "short-trim-audio-clip-start-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Short Audio Clip Start Trim");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audio.id,
        "Short audio",
        trackloom::ClipType::Audio,
        0,
        trackloom::Project::ticksPerQuarterNote);
    require(clip.has_value(),
        "too-short audio trim-start test should create a one-beat audio clip");
    require(session.saveAs(path).success,
        "too-short audio trim-start test should save setup edits before validation");

    const auto feedback = trackloom::trimAudioClipStartLaterOneBeat(session, clip->id);

    const auto unchangedClip = session.project().findClipById(clip->id);
    require(!feedback.success,
        "audio clip trim-start action should reject clips that cannot stay positive length");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::TrimFailed,
        "too-short audio clip trim-start should expose the stable trim-failed kind");
    require(unchangedClip.has_value()
            && unchangedClip->startTick == 0
            && unchangedClip->lengthTick == trackloom::Project::ticksPerQuarterNote,
        "too-short audio clip trim-start should keep the source clip timing unchanged");
    require(!session.isDirty(),
        "too-short audio clip trim-start should not dirty an unchanged session");
}

void audioClipActionRejectsStartExtendBeforeTimelineStartWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "extend-audio-clip-start-before-zero.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Audio Clip Start Extend Boundary");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clipFeedback = trackloom::createDefaultAudioClipOnTrack(session, audio.id);
    require(clipFeedback.success,
        "extend-start boundary test should create a clip at the timeline start");
    require(session.saveAs(path).success,
        "extend-start boundary test should save setup edits before validation");

    const auto feedback = trackloom::extendAudioClipStartEarlierOneBeat(session, clipFeedback.clipId);

    const auto unchangedClip = session.project().findClipById(clipFeedback.clipId);
    require(!feedback.success,
        "audio clip extend-start action should reject moving the shell before the timeline start");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::ExtendFailed,
        "audio clip extend-start boundary failure should expose a stable extend failure kind");
    require(unchangedClip.has_value()
            && unchangedClip->startTick == 0
            && unchangedClip->lengthTick == trackloom::defaultAppAudioClipLengthTick,
        "audio clip extend-start boundary failure should keep the source clip timing unchanged");
    require(!session.isDirty(),
        "audio clip extend-start boundary failure should not dirty an unchanged session");
}

void audioClipActionRejectsMissingClipStartTrimWithoutDirtyingSession()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Missing Audio Clip Start Trim");

    const auto feedback = trackloom::trimAudioClipStartLaterOneBeat(session, "missing-clip");

    require(!feedback.success,
        "audio clip trim-start action should reject a missing clip");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::MissingClip,
        "missing audio clip trim-start should expose a stable failure kind");
    require(!session.isDirty(),
        "missing audio clip trim-start should not dirty an unchanged session");
}

void audioClipActionRejectsMidiClipStartTrimWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "midi-trim-audio-clip-start-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Clip Start Trim As Audio");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto midi = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppAudioClipLengthTick);
    require(midi.has_value(),
        "MIDI-as-audio trim-start test should create a MIDI clip");
    require(session.saveAs(path).success,
        "MIDI-as-audio trim-start test should save setup edits before validation");

    const auto feedback = trackloom::trimAudioClipStartLaterOneBeat(session, midi->id);

    require(!feedback.success,
        "audio clip trim-start action should reject MIDI clips");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::IncompatibleClipType,
        "MIDI clip audio trim-start should expose a stable failure kind");
    require(session.project().findClipById(midi->id)->startTick == 0,
        "MIDI clip audio trim-start should keep the original clip start");
    require(!session.isDirty(),
        "MIDI clip audio trim-start should not dirty an unchanged session");
}

void audioClipActionRejectsMissingClipStartExtendWithoutDirtyingSession()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Missing Audio Clip Start Extend");

    const auto feedback = trackloom::extendAudioClipStartEarlierOneBeat(session, "missing-clip");

    require(!feedback.success,
        "audio clip extend-start action should reject a missing clip");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::MissingClip,
        "missing audio clip extend-start should expose a stable failure kind");
    require(!session.isDirty(),
        "missing audio clip extend-start should not dirty an unchanged session");
}

void audioClipActionRejectsMidiClipStartExtendWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "midi-extend-audio-clip-start-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Clip Start Extend As Audio");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto midi = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        trackloom::Project::ticksPerQuarterNote,
        trackloom::defaultAppAudioClipLengthTick);
    require(midi.has_value(),
        "MIDI-as-audio extend-start test should create a MIDI clip");
    require(session.saveAs(path).success,
        "MIDI-as-audio extend-start test should save setup edits before validation");

    const auto feedback = trackloom::extendAudioClipStartEarlierOneBeat(session, midi->id);

    require(!feedback.success,
        "audio clip extend-start action should reject MIDI clips");
    require(feedback.kind == trackloom::AppAudioClipActionFeedbackKind::IncompatibleClipType,
        "MIDI clip audio extend-start should expose a stable failure kind");
    require(session.project().findClipById(midi->id)->startTick == trackloom::Project::ticksPerQuarterNote,
        "MIDI clip audio extend-start should keep the original clip start");
    require(!session.isDirty(),
        "MIDI clip audio extend-start should not dirty an unchanged session");
}

void midiClipActionAppendsAfterExistingTrackClips()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Append Clips");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);

    const auto first = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    const auto second = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);

    require(first.success && second.success,
        "MIDI clip action should create repeated starter clips on the same track");
    require(session.project().clips().size() == 2,
        "repeated MIDI clip action should create two clips");
    require(session.project().clips()[1].startTick
            == session.project().clips()[0].startTick + session.project().clips()[0].lengthTick,
        "second starter MIDI clip should append after the first clip on that track");
}

void midiClipActionDuplicatesMidiClipAfterItself()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "duplicate-midi-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Duplicate Clip");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    const auto noteFeedback = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    require(clipFeedback.success && noteFeedback.success,
        "duplicate MIDI clip test should create a source clip with one note");
    require(session.saveAs(path).success,
        "duplicate MIDI clip test should save setup edits before duplication");

    const auto feedback = trackloom::duplicateMidiClipAfterItself(session, clipFeedback.clipId);

    require(feedback.success,
        "MIDI clip action should duplicate the target MIDI clip");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::Success,
        "successful MIDI clip duplicate action should expose the stable success kind");
    require(feedback.clipId != clipFeedback.clipId,
        "MIDI clip duplicate action should report the new clip id");
    require(session.project().clips().size() == 2,
        "MIDI clip duplicate action should add one new clip");

    const auto source = session.project().findClipById(clipFeedback.clipId);
    const auto duplicate = session.project().findClipById(feedback.clipId);
    require(source.has_value() && duplicate.has_value(),
        "MIDI clip duplicate test should find source and duplicate clips");
    require(duplicate->trackId == source->trackId,
        "MIDI clip duplicate should stay on the source track");
    require(duplicate->startTick == source->startTick + source->lengthTick,
        "MIDI clip duplicate should start at the source clip end");
    require(duplicate->lengthTick == source->lengthTick,
        "MIDI clip duplicate should keep the source clip length");
    require(duplicate->midiNotes.size() == 1,
        "MIDI clip duplicate should copy source MIDI notes");
    require(duplicate->midiNotes[0].id != source->midiNotes[0].id,
        "MIDI clip duplicate should allocate fresh note ids");
    require(session.isDirty(),
        "successful MIDI clip duplicate should mark the app session dirty");
}

void midiClipActionDuplicateCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Clip Duplicate History");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Loop",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiClipLengthTick);
    require(clip.has_value(),
        "MIDI clip duplicate history test should create a source clip");
    const auto note = session.editProject().createMidiNote(
        clip->id,
        0,
        trackloom::Project::ticksPerQuarterNote,
        60,
        100,
        1);
    require(note.has_value(),
        "MIDI clip duplicate history test should create a note inside the source clip");

    const auto feedback = trackloom::duplicateMidiClipAfterItself(session, clip->id);

    require(feedback.success,
        "MIDI clip duplicate history test should duplicate the source clip");
    require(session.project().clips().size() == 2,
        "MIDI clip duplicate history test should start with source and duplicate clips");
    const auto duplicatedClip = session.project().findClipById(feedback.clipId);
    require(duplicatedClip.has_value() && duplicatedClip->midiNotes.size() == 1,
        "MIDI clip duplicate history test should create a duplicate with copied notes");
    const auto duplicatedNoteId = duplicatedClip->midiNotes[0].id;
    require(duplicatedNoteId != note->id,
        "MIDI clip duplicate history test should allocate a fresh copied note id");
    require(session.canUndoProjectEdit(),
        "MIDI clip duplicate action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo MIDI clip duplication from the clip action");
    require(session.project().clips().size() == 1 && session.project().findClipById(clip->id).has_value(),
        "undoing MIDI clip duplication should remove only the duplicate clip");
    require(!session.project().findClipById(feedback.clipId).has_value(),
        "undoing MIDI clip duplication should remove the duplicate id from the project");
    require(session.canRedoProjectEdit(),
        "undoing MIDI clip duplication should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo MIDI clip duplication from the clip action");
    const auto restoredDuplicate = session.project().findClipById(feedback.clipId);
    require(restoredDuplicate.has_value(),
        "redoing MIDI clip duplication should restore the same duplicate clip id");
    require(restoredDuplicate->midiNotes.size() == 1 && restoredDuplicate->midiNotes[0].id == duplicatedNoteId,
        "redoing MIDI clip duplication should restore the same copied note id");
}

void midiClipActionRenamesMidiClipAndMarksSessionDirty()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "rename-midi-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Rename Clip");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    require(clipFeedback.success,
        "rename MIDI clip test should create a source MIDI clip");
    require(session.saveAs(path).success,
        "rename MIDI clip test should save setup edits before renaming");

    const auto feedback = trackloom::renameMidiClipById(session, clipFeedback.clipId, "  Verse Loop  ");

    require(feedback.success,
        "MIDI clip action should rename the target MIDI clip");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::Success,
        "successful MIDI clip rename action should expose the stable success kind");
    require(feedback.clipId == clipFeedback.clipId,
        "MIDI clip rename action should report the renamed clip id");
    require(session.project().findClipById(clipFeedback.clipId)->name == "Verse Loop",
        "MIDI clip rename action should trim outer whitespace before saving the name");
    require(session.isDirty(),
        "successful MIDI clip rename should mark the app session dirty");

    const auto status = trackloom::describeAppTimeline(session.project());
    require(status.rows.size() == 1 && status.rows[0].name == "Verse Loop",
        "timeline status should expose the renamed MIDI clip name");
}

void midiClipActionRenameCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Clip Rename History");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Loop",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiClipLengthTick);
    require(clip.has_value(),
        "MIDI clip rename history test should create a source clip");

    const auto feedback = trackloom::renameMidiClipById(session, clip->id, "  Verse Loop  ");

    require(feedback.success,
        "MIDI clip rename history test should rename the target clip");
    require(session.project().findClipById(clip->id)->name == "Verse Loop",
        "MIDI clip rename history test should start from the renamed clip");
    require(session.canUndoProjectEdit(),
        "MIDI clip rename action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo MIDI clip rename from the clip action");
    require(session.project().findClipById(clip->id)->name == "Loop",
        "undoing MIDI clip rename should restore the previous clip name");
    require(session.canRedoProjectEdit(),
        "undoing MIDI clip rename should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo MIDI clip rename from the clip action");
    require(session.project().findClipById(clip->id)->name == "Verse Loop",
        "redoing MIDI clip rename should restore the renamed clip name");
}

void midiClipActionSplitsMidiClipAtMidpoint()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "split-midi-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Split Clip");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    const auto rightNote = session.editProject().createMidiNote(
        clipFeedback.clipId,
        trackloom::defaultAppMidiClipLengthTick / 2,
        trackloom::Project::ticksPerQuarterNote,
        67,
        100,
        1);
    require(clipFeedback.success && rightNote.has_value(),
        "split MIDI clip test should create a source clip with a right-side note");
    require(session.saveAs(path).success,
        "split MIDI clip test should save setup edits before splitting");

    const auto feedback = trackloom::splitMidiClipAtMidpoint(session, clipFeedback.clipId);

    require(feedback.success,
        "MIDI clip action should split the target clip at its midpoint");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::Success,
        "successful MIDI clip split action should expose the stable success kind");
    require(feedback.clipId != clipFeedback.clipId,
        "MIDI clip split action should report the right-side clip id");
    require(session.project().clips().size() == 2,
        "MIDI clip split action should create one right-side clip");

    const auto left = session.project().findClipById(clipFeedback.clipId);
    const auto right = session.project().findClipById(feedback.clipId);
    require(left.has_value() && right.has_value(),
        "MIDI clip split test should find left and right clips");
    require(left->lengthTick == trackloom::defaultAppMidiClipLengthTick / 2,
        "MIDI clip split should shorten the original left clip");
    require(right->startTick == left->startTick + left->lengthTick,
        "MIDI clip split should place the right clip at the split tick");
    require(right->lengthTick == trackloom::defaultAppMidiClipLengthTick - left->lengthTick,
        "MIDI clip split should keep the remaining right-side length");
    require(right->midiNotes.size() == 1,
        "MIDI clip split should move right-side notes into the right clip");
    require(right->midiNotes[0].id == rightNote->id,
        "MIDI clip split should keep moved MIDI note ids stable");
    require(right->midiNotes[0].startTick == 0,
        "MIDI clip split should make moved note start relative to the right clip");
    require(session.isDirty(),
        "successful MIDI clip split should mark the app session dirty");
}

void midiClipActionSplitCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Clip Split History");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Loop",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiClipLengthTick);
    require(clip.has_value(),
        "MIDI clip split history test should create a source clip");

    const auto splitOffset = trackloom::defaultAppMidiClipLengthTick / 2;
    const auto leftNote = session.editProject().createMidiNote(
        clip->id,
        0,
        trackloom::Project::ticksPerQuarterNote,
        60,
        100,
        1);
    const auto rightNote = session.editProject().createMidiNote(
        clip->id,
        splitOffset,
        trackloom::Project::ticksPerQuarterNote,
        67,
        100,
        1);
    require(leftNote.has_value() && rightNote.has_value(),
        "MIDI clip split history test should create notes on both sides of the split");

    const auto feedback = trackloom::splitMidiClipAtMidpoint(session, clip->id);

    require(feedback.success,
        "MIDI clip split history test should split the source clip");
    const auto leftAfterSplit = session.project().findClipById(clip->id);
    const auto rightAfterSplit = session.project().findClipById(feedback.clipId);
    require(leftAfterSplit.has_value() && rightAfterSplit.has_value(),
        "MIDI clip split history test should find both split sides");
    require(leftAfterSplit->midiNotes.size() == 1 && leftAfterSplit->midiNotes[0].id == leftNote->id,
        "MIDI clip split history test should keep the left note on the source clip");
    require(rightAfterSplit->midiNotes.size() == 1 && rightAfterSplit->midiNotes[0].id == rightNote->id,
        "MIDI clip split history test should move the right note into the new clip");
    require(rightAfterSplit->midiNotes[0].startTick == 0,
        "MIDI clip split history test should rewrite right-side note time relative to the right clip");
    require(session.canUndoProjectEdit(),
        "MIDI clip split action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo MIDI clip splitting from the clip action");
    const auto restoredOriginal = session.project().findClipById(clip->id);
    require(session.project().clips().size() == 1 && restoredOriginal.has_value(),
        "undoing MIDI clip split should restore one original clip");
    require(!session.project().findClipById(feedback.clipId).has_value(),
        "undoing MIDI clip split should remove the right-side clip id");
    require(restoredOriginal->lengthTick == trackloom::defaultAppMidiClipLengthTick,
        "undoing MIDI clip split should restore the original clip length");
    require(restoredOriginal->midiNotes.size() == 2,
        "undoing MIDI clip split should restore both notes to the original clip");
    require(session.canRedoProjectEdit(),
        "undoing MIDI clip split should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo MIDI clip splitting from the clip action");
    const auto redoLeft = session.project().findClipById(clip->id);
    const auto redoRight = session.project().findClipById(feedback.clipId);
    require(redoLeft.has_value() && redoRight.has_value(),
        "redoing MIDI clip split should restore both split sides");
    require(redoLeft->midiNotes.size() == 1 && redoLeft->midiNotes[0].id == leftNote->id,
        "redoing MIDI clip split should restore the left note on the source clip");
    require(redoRight->midiNotes.size() == 1 && redoRight->midiNotes[0].id == rightNote->id,
        "redoing MIDI clip split should restore the same moved right note id");
    require(redoRight->midiNotes[0].startTick == 0,
        "redoing MIDI clip split should preserve the right note's relative start");
}

void midiClipActionMovesMidiClipRightOneBeat()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "move-midi-clip-right-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Move Clip Right");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    const auto noteFeedback = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    require(clipFeedback.success && noteFeedback.success,
        "move-right MIDI clip test should create a source clip with one note");
    require(session.saveAs(path).success,
        "move-right MIDI clip test should save setup edits before moving");

    const auto feedback = trackloom::moveMidiClipRightOneBeat(session, clipFeedback.clipId);

    const auto movedClip = session.project().findClipById(clipFeedback.clipId);
    require(feedback.success,
        "MIDI clip action should move the target clip right by one beat");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::Success,
        "successful MIDI clip move-right action should expose the stable success kind");
    require(feedback.clipId == clipFeedback.clipId,
        "MIDI clip move-right action should keep reporting the moved clip id");
    require(movedClip.has_value() && movedClip->startTick == trackloom::Project::ticksPerQuarterNote,
        "MIDI clip move-right action should add one quarter-note tick span to the clip start");
    require(movedClip->lengthTick == trackloom::defaultAppMidiClipLengthTick,
        "MIDI clip move-right action should keep the clip length unchanged");
    require(movedClip->midiNotes.size() == 1 && movedClip->midiNotes[0].startTick == 0,
        "MIDI clip move-right action should keep MIDI notes relative to the moved clip");
    require(session.isDirty(),
        "successful MIDI clip move-right should mark the app session dirty");
}

void midiClipActionMoveCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Clip Move History");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Loop",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiClipLengthTick);
    require(clip.has_value(),
        "MIDI clip move history test should create a source clip");
    const auto note = session.editProject().createMidiNote(
        clip->id,
        0,
        trackloom::Project::ticksPerQuarterNote,
        60,
        100,
        1);
    require(note.has_value(),
        "MIDI clip move history test should create a note inside the source clip");

    const auto feedback = trackloom::moveMidiClipRightOneBeat(session, clip->id);

    require(feedback.success,
        "MIDI clip move history test should move the source clip right");
    const auto movedClip = session.project().findClipById(clip->id);
    require(movedClip.has_value() && movedClip->startTick == trackloom::Project::ticksPerQuarterNote,
        "MIDI clip move history test should move the clip start by one beat");
    require(movedClip->lengthTick == trackloom::defaultAppMidiClipLengthTick,
        "MIDI clip move history test should keep clip length unchanged");
    require(movedClip->midiNotes.size() == 1 && movedClip->midiNotes[0].id == note->id,
        "MIDI clip move history test should keep the source note id inside the moved clip");
    require(movedClip->midiNotes[0].startTick == 0,
        "MIDI clip move history test should keep MIDI note time relative to the moved clip");
    require(session.canUndoProjectEdit(),
        "MIDI clip move action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo MIDI clip movement from the clip action");
    const auto restoredClip = session.project().findClipById(clip->id);
    require(restoredClip.has_value() && restoredClip->startTick == 0,
        "undoing MIDI clip movement should restore the original clip start");
    require(restoredClip->lengthTick == trackloom::defaultAppMidiClipLengthTick,
        "undoing MIDI clip movement should keep the original clip length");
    require(restoredClip->midiNotes.size() == 1 && restoredClip->midiNotes[0].id == note->id,
        "undoing MIDI clip movement should keep the original note id");
    require(session.canRedoProjectEdit(),
        "undoing MIDI clip movement should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo MIDI clip movement from the clip action");
    const auto redoneClip = session.project().findClipById(clip->id);
    require(redoneClip.has_value() && redoneClip->startTick == trackloom::Project::ticksPerQuarterNote,
        "redoing MIDI clip movement should restore the moved start");
    require(redoneClip->midiNotes.size() == 1 && redoneClip->midiNotes[0].id == note->id,
        "redoing MIDI clip movement should preserve the same note id");
    require(redoneClip->midiNotes[0].startTick == 0,
        "redoing MIDI clip movement should keep note timing relative to the moved clip");
}

void midiClipActionMovesMidiClipLeftOneBeat()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "move-midi-clip-left-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Move Clip Left");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Verse",
        trackloom::ClipType::Midi,
        trackloom::Project::ticksPerQuarterNote,
        trackloom::defaultAppMidiClipLengthTick);
    require(clip.has_value(),
        "move-left MIDI clip test should create a clip after the timeline start");
    require(session.saveAs(path).success,
        "move-left MIDI clip test should save setup edits before moving");

    const auto feedback = trackloom::moveMidiClipLeftOneBeat(session, clip->id);

    const auto movedClip = session.project().findClipById(clip->id);
    require(feedback.success,
        "MIDI clip action should move the target clip left by one beat");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::Success,
        "successful MIDI clip move-left action should expose the stable success kind");
    require(movedClip.has_value() && movedClip->startTick == 0,
        "MIDI clip move-left action should subtract one quarter-note tick span from the clip start");
    require(movedClip->lengthTick == trackloom::defaultAppMidiClipLengthTick,
        "MIDI clip move-left action should keep the clip length unchanged");
    require(session.isDirty(),
        "successful MIDI clip move-left should mark the app session dirty");
}

void midiClipActionMovesMidiClipToInstrumentTrack()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "move-midi-clip-to-track-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Move Clip To Track");
    const auto sourceTrack = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto targetTrack = session.editProject().createTrack("Pad", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, sourceTrack.id);
    const auto noteFeedback = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    require(clipFeedback.success && noteFeedback.success,
        "move-to-track MIDI clip test should create a source clip with one note");
    require(session.saveAs(path).success,
        "move-to-track MIDI clip test should save setup edits before moving");

    const auto feedback = trackloom::moveMidiClipToTrack(session, clipFeedback.clipId, targetTrack.id);

    const auto movedClip = session.project().findClipById(clipFeedback.clipId);
    require(feedback.success,
        "MIDI clip action should move the target clip to another instrument track");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::Success,
        "successful MIDI clip move-to-track action should expose the stable success kind");
    require(feedback.clipId == clipFeedback.clipId,
        "MIDI clip move-to-track action should keep reporting the moved clip id");
    require(movedClip.has_value() && movedClip->trackId == targetTrack.id,
        "MIDI clip move-to-track action should update only the owning track id");
    require(movedClip->startTick == 0 && movedClip->lengthTick == trackloom::defaultAppMidiClipLengthTick,
        "MIDI clip move-to-track action should keep the clip timing unchanged");
    require(movedClip->midiNotes.size() == 1 && movedClip->midiNotes[0].startTick == 0,
        "MIDI clip move-to-track action should keep MIDI notes relative to the same clip");
    require(session.isDirty(),
        "successful MIDI clip move-to-track should mark the app session dirty");
}

void midiClipActionMoveToTrackCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Clip Track Move History");
    const auto sourceTrack = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto targetTrack = session.editProject().createTrack("Pad", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        sourceTrack.id,
        "Loop",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiClipLengthTick);
    require(clip.has_value(),
        "MIDI clip track-move history test should create a source clip");
    const auto note = session.editProject().createMidiNote(
        clip->id,
        0,
        trackloom::Project::ticksPerQuarterNote,
        60,
        100,
        1);
    require(note.has_value(),
        "MIDI clip track-move history test should create a note inside the source clip");

    const auto feedback = trackloom::moveMidiClipToTrack(session, clip->id, targetTrack.id);

    require(feedback.success,
        "MIDI clip track-move history test should move the source clip to the target track");
    const auto movedClip = session.project().findClipById(clip->id);
    require(movedClip.has_value() && movedClip->trackId == targetTrack.id,
        "MIDI clip track-move history test should update the owning track id");
    require(movedClip->startTick == 0 && movedClip->lengthTick == trackloom::defaultAppMidiClipLengthTick,
        "MIDI clip track-move history test should keep clip timing unchanged");
    require(movedClip->midiNotes.size() == 1 && movedClip->midiNotes[0].id == note->id,
        "MIDI clip track-move history test should keep the source note id inside the moved clip");
    require(session.canUndoProjectEdit(),
        "MIDI clip track-move action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo MIDI clip track movement from the clip action");
    const auto restoredClip = session.project().findClipById(clip->id);
    require(restoredClip.has_value() && restoredClip->trackId == sourceTrack.id,
        "undoing MIDI clip track movement should restore the source track id");
    require(restoredClip->midiNotes.size() == 1 && restoredClip->midiNotes[0].id == note->id,
        "undoing MIDI clip track movement should keep the original note id");
    require(session.canRedoProjectEdit(),
        "undoing MIDI clip track movement should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo MIDI clip track movement from the clip action");
    const auto redoneClip = session.project().findClipById(clip->id);
    require(redoneClip.has_value() && redoneClip->trackId == targetTrack.id,
        "redoing MIDI clip track movement should restore the target track id");
    require(redoneClip->midiNotes.size() == 1 && redoneClip->midiNotes[0].id == note->id,
        "redoing MIDI clip track movement should preserve the same note id");
}

void midiClipActionTrimsMidiClipEndEarlierOneBeat()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "trim-midi-clip-end-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Trim Clip End");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    const auto noteFeedback = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    require(clipFeedback.success && noteFeedback.success,
        "trim-end MIDI clip test should create a source clip with one note");
    require(session.saveAs(path).success,
        "trim-end MIDI clip test should save setup edits before trimming");

    const auto feedback = trackloom::trimMidiClipEndEarlierOneBeat(session, clipFeedback.clipId);

    const auto trimmedClip = session.project().findClipById(clipFeedback.clipId);
    require(feedback.success,
        "MIDI clip action should trim the target clip end earlier by one beat");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::Success,
        "successful MIDI clip trim-end action should expose the stable success kind");
    require(feedback.clipId == clipFeedback.clipId,
        "MIDI clip trim-end action should report the trimmed clip id");
    require(trimmedClip.has_value() && trimmedClip->startTick == 0,
        "MIDI clip trim-end action should keep the clip start unchanged");
    require(trimmedClip->lengthTick == trackloom::defaultAppMidiClipLengthTick - trackloom::Project::ticksPerQuarterNote,
        "MIDI clip trim-end action should shorten the clip by one quarter-note tick span");
    require(trimmedClip->midiNotes.size() == 1 && trimmedClip->midiNotes[0].startTick == 0,
        "MIDI clip trim-end action should keep MIDI notes relative to the same clip");
    require(session.isDirty(),
        "successful MIDI clip trim-end should mark the app session dirty");
}

void midiClipActionTrimEndCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Clip Trim End History");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Loop",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiClipLengthTick);
    require(clip.has_value(),
        "MIDI clip trim-end history test should create a source clip");
    const auto note = session.editProject().createMidiNote(
        clip->id,
        0,
        trackloom::Project::ticksPerQuarterNote,
        60,
        100,
        1);
    require(note.has_value(),
        "MIDI clip trim-end history test should create a note that remains after trimming");

    const auto feedback = trackloom::trimMidiClipEndEarlierOneBeat(session, clip->id);

    require(feedback.success,
        "MIDI clip trim-end history test should trim the clip end");
    const auto trimmedClip = session.project().findClipById(clip->id);
    require(trimmedClip.has_value()
            && trimmedClip->lengthTick == trackloom::defaultAppMidiClipLengthTick - trackloom::Project::ticksPerQuarterNote,
        "MIDI clip trim-end history test should shorten the clip by one beat");
    require(trimmedClip->midiNotes.size() == 1 && trimmedClip->midiNotes[0].id == note->id,
        "MIDI clip trim-end history test should preserve the source note id");
    require(session.canUndoProjectEdit(),
        "MIDI clip trim-end action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo MIDI clip trim-end from the clip action");
    const auto restoredClip = session.project().findClipById(clip->id);
    require(restoredClip.has_value() && restoredClip->lengthTick == trackloom::defaultAppMidiClipLengthTick,
        "undoing MIDI clip trim-end should restore the original clip length");
    require(restoredClip->midiNotes.size() == 1 && restoredClip->midiNotes[0].id == note->id,
        "undoing MIDI clip trim-end should keep the original note id");
    require(session.canRedoProjectEdit(),
        "undoing MIDI clip trim-end should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo MIDI clip trim-end from the clip action");
    const auto redoneClip = session.project().findClipById(clip->id);
    require(redoneClip.has_value()
            && redoneClip->lengthTick == trackloom::defaultAppMidiClipLengthTick - trackloom::Project::ticksPerQuarterNote,
        "redoing MIDI clip trim-end should restore the trimmed length");
    require(redoneClip->midiNotes.size() == 1 && redoneClip->midiNotes[0].id == note->id,
        "redoing MIDI clip trim-end should preserve the same note id");
}

void midiClipActionExtendsMidiClipEndLaterOneBeat()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "extend-midi-clip-end-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Extend Clip End");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    const auto noteFeedback = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    require(clipFeedback.success && noteFeedback.success,
        "extend-end MIDI clip test should create a source clip with one note");
    require(session.saveAs(path).success,
        "extend-end MIDI clip test should save setup edits before extending");

    const auto feedback = trackloom::extendMidiClipEndLaterOneBeat(session, clipFeedback.clipId);

    const auto extendedClip = session.project().findClipById(clipFeedback.clipId);
    require(feedback.success,
        "MIDI clip action should extend the target clip end later by one beat");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::Success,
        "successful MIDI clip extend-end action should expose the stable success kind");
    require(feedback.clipId == clipFeedback.clipId,
        "MIDI clip extend-end action should report the extended clip id");
    require(extendedClip.has_value() && extendedClip->startTick == 0,
        "MIDI clip extend-end action should keep the clip start unchanged");
    require(extendedClip->lengthTick == trackloom::defaultAppMidiClipLengthTick + trackloom::Project::ticksPerQuarterNote,
        "MIDI clip extend-end action should lengthen the clip by one quarter-note tick span");
    require(extendedClip->midiNotes.size() == 1 && extendedClip->midiNotes[0].startTick == 0,
        "MIDI clip extend-end action should keep MIDI notes relative to the same clip");
    require(session.isDirty(),
        "successful MIDI clip extend-end should mark the app session dirty");
}

void midiClipActionExtendEndCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Clip Extend End History");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Loop",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiClipLengthTick);
    require(clip.has_value(),
        "MIDI clip extend-end history test should create a source clip");
    const auto note = session.editProject().createMidiNote(
        clip->id,
        0,
        trackloom::Project::ticksPerQuarterNote,
        60,
        100,
        1);
    require(note.has_value(),
        "MIDI clip extend-end history test should create a note inside the source clip");

    const auto feedback = trackloom::extendMidiClipEndLaterOneBeat(session, clip->id);

    require(feedback.success,
        "MIDI clip extend-end history test should extend the clip end");
    const auto extendedClip = session.project().findClipById(clip->id);
    require(extendedClip.has_value()
            && extendedClip->lengthTick == trackloom::defaultAppMidiClipLengthTick + trackloom::Project::ticksPerQuarterNote,
        "MIDI clip extend-end history test should lengthen the clip by one beat");
    require(extendedClip->midiNotes.size() == 1 && extendedClip->midiNotes[0].id == note->id,
        "MIDI clip extend-end history test should preserve the source note id");
    require(session.canUndoProjectEdit(),
        "MIDI clip extend-end action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo MIDI clip extend-end from the clip action");
    const auto restoredClip = session.project().findClipById(clip->id);
    require(restoredClip.has_value() && restoredClip->lengthTick == trackloom::defaultAppMidiClipLengthTick,
        "undoing MIDI clip extend-end should restore the original clip length");
    require(restoredClip->midiNotes.size() == 1 && restoredClip->midiNotes[0].id == note->id,
        "undoing MIDI clip extend-end should keep the original note id");
    require(session.canRedoProjectEdit(),
        "undoing MIDI clip extend-end should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo MIDI clip extend-end from the clip action");
    const auto redoneClip = session.project().findClipById(clip->id);
    require(redoneClip.has_value()
            && redoneClip->lengthTick == trackloom::defaultAppMidiClipLengthTick + trackloom::Project::ticksPerQuarterNote,
        "redoing MIDI clip extend-end should restore the extended length");
    require(redoneClip->midiNotes.size() == 1 && redoneClip->midiNotes[0].id == note->id,
        "redoing MIDI clip extend-end should preserve the same note id");
}

void midiClipActionTrimsMidiClipStartLaterOneBeat()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "trim-midi-clip-start-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Trim Clip Start");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    const auto note = session.editProject().createMidiNote(
        clipFeedback.clipId,
        trackloom::Project::ticksPerQuarterNote,
        trackloom::Project::ticksPerQuarterNote,
        64,
        100,
        1);
    require(clipFeedback.success && note.has_value(),
        "trim-start MIDI clip test should create a source clip with a note after the trim boundary");
    require(session.saveAs(path).success,
        "trim-start MIDI clip test should save setup edits before trimming");

    const auto feedback = trackloom::trimMidiClipStartLaterOneBeat(session, clipFeedback.clipId);

    const auto trimmedClip = session.project().findClipById(clipFeedback.clipId);
    require(feedback.success,
        "MIDI clip action should trim the target clip start later by one beat");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::Success,
        "successful MIDI clip trim-start action should expose the stable success kind");
    require(feedback.clipId == clipFeedback.clipId,
        "MIDI clip trim-start action should report the trimmed clip id");
    require(trimmedClip.has_value() && trimmedClip->startTick == trackloom::Project::ticksPerQuarterNote,
        "MIDI clip trim-start action should move the clip start right by one beat");
    require(trimmedClip->lengthTick == trackloom::defaultAppMidiClipLengthTick - trackloom::Project::ticksPerQuarterNote,
        "MIDI clip trim-start action should shorten the clip by one beat");
    require(trimmedClip->midiNotes.size() == 1 && trimmedClip->midiNotes[0].startTick == 0,
        "MIDI clip trim-start action should shift kept notes left so their absolute time stays unchanged");
    require(session.isDirty(),
        "successful MIDI clip trim-start should mark the app session dirty");
}

void midiClipActionExtendsMidiClipStartEarlierOneBeat()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "extend-midi-clip-start-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Extend Clip Start");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Verse",
        trackloom::ClipType::Midi,
        trackloom::Project::ticksPerQuarterNote,
        trackloom::defaultAppMidiClipLengthTick);
    require(clip.has_value(),
        "extend-start MIDI clip test should create a clip after the timeline start");
    const auto note = session.editProject().createMidiNote(
        clip->id,
        0,
        trackloom::Project::ticksPerQuarterNote,
        64,
        100,
        1);
    require(note.has_value(),
        "extend-start MIDI clip test should create a note at the current clip start");
    require(session.saveAs(path).success,
        "extend-start MIDI clip test should save setup edits before extending");

    const auto feedback = trackloom::extendMidiClipStartEarlierOneBeat(session, clip->id);

    const auto extendedClip = session.project().findClipById(clip->id);
    require(feedback.success,
        "MIDI clip action should extend the target clip start earlier by one beat");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::Success,
        "successful MIDI clip extend-start action should expose the stable success kind");
    require(feedback.clipId == clip->id,
        "MIDI clip extend-start action should report the extended clip id");
    require(extendedClip.has_value() && extendedClip->startTick == 0,
        "MIDI clip extend-start action should move the clip start left by one beat");
    require(extendedClip->lengthTick == trackloom::defaultAppMidiClipLengthTick + trackloom::Project::ticksPerQuarterNote,
        "MIDI clip extend-start action should lengthen the clip by one beat");
    require(extendedClip->midiNotes.size() == 1
            && extendedClip->midiNotes[0].startTick == trackloom::Project::ticksPerQuarterNote,
        "MIDI clip extend-start action should shift kept notes right so their absolute time stays unchanged");
    require(session.isDirty(),
        "successful MIDI clip extend-start should mark the app session dirty");
}

void midiClipActionTrimStartCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Clip Trim Start History");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Loop",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiClipLengthTick);
    require(clip.has_value(),
        "MIDI clip trim-start history test should create a source clip");
    const auto note = session.editProject().createMidiNote(
        clip->id,
        trackloom::Project::ticksPerQuarterNote,
        trackloom::Project::ticksPerQuarterNote,
        64,
        100,
        1);
    require(note.has_value(),
        "MIDI clip trim-start history test should create a note after the trim boundary");
    require(!session.canUndoProjectEdit(),
        "direct setup edits should not leave undo history before the trim-start action");

    const auto feedback = trackloom::trimMidiClipStartLaterOneBeat(session, clip->id);

    require(feedback.success,
        "MIDI clip trim-start history test should trim the clip start");
    const auto trimmedClip = session.project().findClipById(clip->id);
    require(trimmedClip.has_value()
            && trimmedClip->startTick == trackloom::Project::ticksPerQuarterNote
            && trimmedClip->lengthTick == trackloom::defaultAppMidiClipLengthTick - trackloom::Project::ticksPerQuarterNote,
        "MIDI clip trim-start history test should move the left edge right by one beat");
    require(trimmedClip->midiNotes.size() == 1
            && trimmedClip->midiNotes[0].id == note->id
            && trimmedClip->midiNotes[0].startTick == 0,
        "MIDI clip trim-start history test should preserve the note id and shift the note left");
    require(session.canUndoProjectEdit(),
        "MIDI clip trim-start action should enter the app session undo history once");
    require(session.undoProjectEdit(),
        "app session should undo MIDI clip trim-start from the clip action");
    const auto restoredClip = session.project().findClipById(clip->id);
    require(restoredClip.has_value()
            && restoredClip->startTick == 0
            && restoredClip->lengthTick == trackloom::defaultAppMidiClipLengthTick,
        "undoing MIDI clip trim-start should restore the original clip timing");
    require(restoredClip->midiNotes.size() == 1
            && restoredClip->midiNotes[0].id == note->id
            && restoredClip->midiNotes[0].startTick == trackloom::Project::ticksPerQuarterNote,
        "undoing MIDI clip trim-start should restore the original note timing");
    require(session.canRedoProjectEdit(),
        "undoing MIDI clip trim-start should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo MIDI clip trim-start from the clip action");
    const auto redoneClip = session.project().findClipById(clip->id);
    require(redoneClip.has_value()
            && redoneClip->startTick == trackloom::Project::ticksPerQuarterNote
            && redoneClip->lengthTick == trackloom::defaultAppMidiClipLengthTick - trackloom::Project::ticksPerQuarterNote,
        "redoing MIDI clip trim-start should restore the trimmed clip timing");
    require(redoneClip->midiNotes.size() == 1
            && redoneClip->midiNotes[0].id == note->id
            && redoneClip->midiNotes[0].startTick == 0,
        "redoing MIDI clip trim-start should restore the shifted note timing");
}

void midiClipActionExtendStartCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Clip Extend Start History");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Loop",
        trackloom::ClipType::Midi,
        trackloom::Project::ticksPerQuarterNote,
        trackloom::defaultAppMidiClipLengthTick);
    require(clip.has_value(),
        "MIDI clip extend-start history test should create a source clip after the timeline start");
    const auto note = session.editProject().createMidiNote(
        clip->id,
        0,
        trackloom::Project::ticksPerQuarterNote,
        64,
        100,
        1);
    require(note.has_value(),
        "MIDI clip extend-start history test should create a note at the clip start");
    require(!session.canUndoProjectEdit(),
        "direct setup edits should not leave undo history before the extend-start action");

    const auto feedback = trackloom::extendMidiClipStartEarlierOneBeat(session, clip->id);

    require(feedback.success,
        "MIDI clip extend-start history test should extend the clip start");
    const auto extendedClip = session.project().findClipById(clip->id);
    require(extendedClip.has_value()
            && extendedClip->startTick == 0
            && extendedClip->lengthTick == trackloom::defaultAppMidiClipLengthTick + trackloom::Project::ticksPerQuarterNote,
        "MIDI clip extend-start history test should move the left edge left by one beat");
    require(extendedClip->midiNotes.size() == 1
            && extendedClip->midiNotes[0].id == note->id
            && extendedClip->midiNotes[0].startTick == trackloom::Project::ticksPerQuarterNote,
        "MIDI clip extend-start history test should preserve the note id and shift the note right");
    require(session.canUndoProjectEdit(),
        "MIDI clip extend-start action should enter the app session undo history once");
    require(session.undoProjectEdit(),
        "app session should undo MIDI clip extend-start from the clip action");
    const auto restoredClip = session.project().findClipById(clip->id);
    require(restoredClip.has_value()
            && restoredClip->startTick == trackloom::Project::ticksPerQuarterNote
            && restoredClip->lengthTick == trackloom::defaultAppMidiClipLengthTick,
        "undoing MIDI clip extend-start should restore the original clip timing");
    require(restoredClip->midiNotes.size() == 1
            && restoredClip->midiNotes[0].id == note->id
            && restoredClip->midiNotes[0].startTick == 0,
        "undoing MIDI clip extend-start should restore the original note timing");
    require(session.canRedoProjectEdit(),
        "undoing MIDI clip extend-start should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo MIDI clip extend-start from the clip action");
    const auto redoneClip = session.project().findClipById(clip->id);
    require(redoneClip.has_value()
            && redoneClip->startTick == 0
            && redoneClip->lengthTick == trackloom::defaultAppMidiClipLengthTick + trackloom::Project::ticksPerQuarterNote,
        "redoing MIDI clip extend-start should restore the extended clip timing");
    require(redoneClip->midiNotes.size() == 1
            && redoneClip->midiNotes[0].id == note->id
            && redoneClip->midiNotes[0].startTick == trackloom::Project::ticksPerQuarterNote,
        "redoing MIDI clip extend-start should restore the shifted note timing");
}

void midiClipActionRejectsEmptyClipNameWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "rename-midi-clip-empty.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Empty Clip Rename");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    require(clipFeedback.success,
        "empty MIDI clip rename test should create a source MIDI clip");
    require(session.saveAs(path).success,
        "empty MIDI clip rename test should save setup edits before validation");

    const auto feedback = trackloom::renameMidiClipById(session, clipFeedback.clipId, "   ");

    require(!feedback.success,
        "MIDI clip rename action should reject a whitespace-only name");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::EmptyName,
        "empty MIDI clip rename should expose a stable failure kind");
    require(session.project().findClipById(clipFeedback.clipId)->name != "   ",
        "empty MIDI clip rename should keep the existing clip name");
    require(!session.isDirty(),
        "empty MIDI clip rename should not dirty an unchanged session");
}

void midiClipActionRejectsMissingTrackWithoutDirtyingSession()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Missing Track");

    const auto feedback = trackloom::createDefaultMidiClipOnTrack(session, "missing-track");

    require(!feedback.success,
        "MIDI clip action should reject a missing target track");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::MissingTrack,
        "missing track MIDI clip action should expose a stable failure kind");
    require(session.project().clips().empty(),
        "missing track MIDI clip action should not create clips");
    require(!session.isDirty(),
        "missing track MIDI clip action should not dirty an unchanged session");
}

void midiClipActionRejectsIncompatibleTrackWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "incompatible-midi-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Incompatible Track");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    require(session.saveAs(path).success,
        "incompatible MIDI clip action test should save setup edits before validation");

    const auto feedback = trackloom::createDefaultMidiClipOnTrack(session, audio.id);

    require(!feedback.success,
        "MIDI clip action should reject non-instrument tracks");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::IncompatibleTrackType,
        "incompatible track MIDI clip action should expose a stable failure kind");
    require(session.project().clips().empty(),
        "incompatible track MIDI clip action should not create clips");
    require(!session.isDirty(),
        "incompatible track MIDI clip action should not dirty an unchanged session");
}

void midiClipActionRejectsMissingClipDuplicateWithoutDirtyingSession()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Missing Clip Duplicate");

    const auto feedback = trackloom::duplicateMidiClipAfterItself(session, "missing-clip");

    require(!feedback.success,
        "MIDI clip duplicate action should reject a missing clip");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::MissingClip,
        "missing clip duplicate action should expose a stable failure kind");
    require(!session.isDirty(),
        "missing clip duplicate action should not dirty an unchanged session");
}

void midiClipActionRejectsAudioClipDuplicateWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "audio-duplicate-midi-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Audio Clip Duplicate");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audio.id,
        "Vocal clip",
        trackloom::ClipType::Audio,
        0,
        trackloom::Project::ticksPerQuarterNote);
    require(clip.has_value(),
        "audio clip duplicate test should create an audio clip");
    require(session.saveAs(path).success,
        "audio clip duplicate test should save setup edits before validation");

    const auto feedback = trackloom::duplicateMidiClipAfterItself(session, clip->id);

    require(!feedback.success,
        "MIDI clip duplicate action should reject audio clips");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::IncompatibleClipType,
        "audio clip duplicate action should expose a stable failure kind");
    require(session.project().clips().size() == 1 && session.project().clips()[0].id == clip->id,
        "audio clip duplicate action should keep the audio clip unchanged");
    require(!session.isDirty(),
        "audio clip duplicate action should not dirty an unchanged session");
}

void midiClipActionRejectsMissingClipRenameWithoutDirtyingSession()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Missing Clip Rename");

    const auto feedback = trackloom::renameMidiClipById(session, "missing-clip", "Verse");

    require(!feedback.success,
        "MIDI clip rename action should reject a missing clip");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::MissingClip,
        "missing clip rename should expose a stable failure kind");
    require(!session.isDirty(),
        "missing clip rename should not dirty an unchanged session");
}

void midiClipActionRejectsAudioClipRenameWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "audio-rename-midi-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Audio Clip Rename");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audio.id,
        "Vocal clip",
        trackloom::ClipType::Audio,
        0,
        trackloom::Project::ticksPerQuarterNote);
    require(clip.has_value(),
        "audio clip rename test should create an audio clip");
    require(session.saveAs(path).success,
        "audio clip rename test should save setup edits before validation");

    const auto feedback = trackloom::renameMidiClipById(session, clip->id, "Verse");

    require(!feedback.success,
        "MIDI clip rename action should reject audio clips");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::IncompatibleClipType,
        "audio clip rename should expose a stable failure kind");
    require(session.project().findClipById(clip->id)->name == "Vocal clip",
        "audio clip rename should keep the audio clip name unchanged");
    require(!session.isDirty(),
        "audio clip rename should not dirty an unchanged session");
}

void midiClipActionRejectsMissingClipSplitWithoutDirtyingSession()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Missing Clip Split");

    const auto feedback = trackloom::splitMidiClipAtMidpoint(session, "missing-clip");

    require(!feedback.success,
        "MIDI clip split action should reject a missing clip");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::MissingClip,
        "missing clip split should expose a stable failure kind");
    require(!session.isDirty(),
        "missing clip split should not dirty an unchanged session");
}

void midiClipActionRejectsAudioClipSplitWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "audio-split-midi-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Audio Clip Split");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audio.id,
        "Vocal clip",
        trackloom::ClipType::Audio,
        0,
        trackloom::Project::ticksPerQuarterNote);
    require(clip.has_value(),
        "audio clip split test should create an audio clip");
    require(session.saveAs(path).success,
        "audio clip split test should save setup edits before validation");

    const auto feedback = trackloom::splitMidiClipAtMidpoint(session, clip->id);

    require(!feedback.success,
        "MIDI clip split action should reject audio clips");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::IncompatibleClipType,
        "audio clip split should expose a stable failure kind");
    require(session.project().clips().size() == 1 && session.project().clips()[0].id == clip->id,
        "audio clip split should keep the audio clip unchanged");
    require(!session.isDirty(),
        "audio clip split should not dirty an unchanged session");
}

void midiClipActionRejectsTooShortMidiClipSplitWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "short-split-midi-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Short Clip Split");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Short clip",
        trackloom::ClipType::Midi,
        0,
        1);
    require(clip.has_value(),
        "too-short MIDI clip split test should create a one-tick MIDI clip");
    require(session.saveAs(path).success,
        "too-short MIDI clip split test should save setup edits before validation");

    const auto feedback = trackloom::splitMidiClipAtMidpoint(session, clip->id);

    require(!feedback.success,
        "MIDI clip split action should reject a clip without a valid midpoint");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::SplitFailed,
        "too-short MIDI clip split should expose the stable split-failed kind");
    require(session.project().clips().size() == 1 && session.project().clips()[0].lengthTick == 1,
        "too-short MIDI clip split should keep the source clip unchanged");
    require(!session.isDirty(),
        "too-short MIDI clip split should not dirty an unchanged session");
}

void midiClipActionRejectsCrossingNoteSplitWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "crossing-note-split-midi-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Crossing Note Split");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    const auto crossingNote = session.editProject().createMidiNote(
        clipFeedback.clipId,
        trackloom::defaultAppMidiClipLengthTick / 2 - 120,
        240,
        64,
        100,
        1);
    require(clipFeedback.success && crossingNote.has_value(),
        "crossing note split test should create a note that crosses the midpoint");
    require(session.saveAs(path).success,
        "crossing note split test should save setup edits before validation");

    const auto feedback = trackloom::splitMidiClipAtMidpoint(session, clipFeedback.clipId);

    require(!feedback.success,
        "MIDI clip split action should reject notes crossing the split midpoint");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::SplitFailed,
        "crossing note split should expose a stable split-failed kind");
    require(session.project().clips().size() == 1,
        "crossing note split should not add a right-side clip");
    require(session.project().findClipById(clipFeedback.clipId)->lengthTick == trackloom::defaultAppMidiClipLengthTick,
        "crossing note split should keep the source clip length unchanged");
    require(!session.isDirty(),
        "crossing note split should not dirty an unchanged session");
}

void midiClipActionRejectsLeftMoveBeforeTimelineStartWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "left-boundary-move-midi-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Left Boundary Move");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    require(clipFeedback.success,
        "left-boundary move test should create a source MIDI clip at the timeline start");
    require(session.saveAs(path).success,
        "left-boundary move test should save setup edits before validation");

    const auto feedback = trackloom::moveMidiClipLeftOneBeat(session, clipFeedback.clipId);

    require(!feedback.success,
        "MIDI clip move-left action should reject moves before the timeline start");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::MoveFailed,
        "left-boundary MIDI clip move should expose the stable move-failed kind");
    require(session.project().findClipById(clipFeedback.clipId)->startTick == 0,
        "left-boundary MIDI clip move should keep the source clip start unchanged");
    require(!session.isDirty(),
        "left-boundary MIDI clip move should not dirty an unchanged session");
}

void midiClipActionRejectsMissingClipMoveWithoutDirtyingSession()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Missing Clip Move");

    const auto feedback = trackloom::moveMidiClipRightOneBeat(session, "missing-clip");

    require(!feedback.success,
        "MIDI clip move action should reject a missing clip");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::MissingClip,
        "missing clip move should expose a stable failure kind");
    require(!session.isDirty(),
        "missing clip move should not dirty an unchanged session");
}

void midiClipActionRejectsAudioClipMoveWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "audio-move-midi-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Audio Clip Move");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audio.id,
        "Vocal clip",
        trackloom::ClipType::Audio,
        trackloom::Project::ticksPerQuarterNote,
        trackloom::Project::ticksPerQuarterNote);
    require(clip.has_value(),
        "audio clip move test should create an audio clip");
    require(session.saveAs(path).success,
        "audio clip move test should save setup edits before validation");

    const auto feedback = trackloom::moveMidiClipLeftOneBeat(session, clip->id);

    require(!feedback.success,
        "MIDI clip move action should reject audio clips");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::IncompatibleClipType,
        "audio clip move should expose a stable failure kind");
    require(session.project().findClipById(clip->id)->startTick == trackloom::Project::ticksPerQuarterNote,
        "audio clip move should keep the audio clip unchanged");
    require(!session.isDirty(),
        "audio clip move should not dirty an unchanged session");
}

void midiClipActionRejectsSameTrackMoveWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "same-track-move-midi-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Same Track Move");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    require(clipFeedback.success,
        "same-track move test should create a source MIDI clip");
    require(session.saveAs(path).success,
        "same-track move test should save setup edits before validation");

    const auto feedback = trackloom::moveMidiClipToTrack(session, clipFeedback.clipId, instrument.id);

    const auto sourceClip = session.project().findClipById(clipFeedback.clipId);
    require(!feedback.success,
        "MIDI clip move-to-track action should reject moving to the current track");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::MoveFailed,
        "same-track MIDI clip move should expose the stable move-failed kind");
    require(sourceClip.has_value() && sourceClip->trackId == instrument.id,
        "same-track MIDI clip move should keep the source clip owner unchanged");
    require(!session.isDirty(),
        "same-track MIDI clip move should not dirty an unchanged session");
}

void midiClipActionRejectsMissingTargetTrackMoveWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "missing-target-track-move-midi-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Missing Target Track Move");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    require(clipFeedback.success,
        "missing-target track move test should create a source MIDI clip");
    require(session.saveAs(path).success,
        "missing-target track move test should save setup edits before validation");

    const auto feedback = trackloom::moveMidiClipToTrack(session, clipFeedback.clipId, "missing-track");

    const auto sourceClip = session.project().findClipById(clipFeedback.clipId);
    require(!feedback.success,
        "MIDI clip move-to-track action should reject a missing target track");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::MissingTrack,
        "missing target track MIDI clip move should expose a stable failure kind");
    require(sourceClip.has_value() && sourceClip->trackId == instrument.id,
        "missing target track MIDI clip move should keep the source clip owner unchanged");
    require(!session.isDirty(),
        "missing target track MIDI clip move should not dirty an unchanged session");
}

void midiClipActionRejectsAudioTargetTrackMoveWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "audio-target-track-move-midi-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Audio Target Track Move");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    require(clipFeedback.success,
        "audio-target move test should create a source MIDI clip");
    require(session.saveAs(path).success,
        "audio-target move test should save setup edits before validation");

    const auto feedback = trackloom::moveMidiClipToTrack(session, clipFeedback.clipId, audio.id);

    const auto sourceClip = session.project().findClipById(clipFeedback.clipId);
    require(!feedback.success,
        "MIDI clip move-to-track action should reject audio target tracks");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::IncompatibleTrackType,
        "audio target track MIDI clip move should expose a stable failure kind");
    require(sourceClip.has_value() && sourceClip->trackId == instrument.id,
        "audio target track MIDI clip move should keep the source clip owner unchanged");
    require(!session.isDirty(),
        "audio target track MIDI clip move should not dirty an unchanged session");
}

void midiClipActionRejectsAudioClipTrackMoveWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "audio-clip-track-move-midi-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Audio Clip Track Move");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        audio.id,
        "Vocal clip",
        trackloom::ClipType::Audio,
        0,
        trackloom::defaultAppMidiClipLengthTick);
    require(clip.has_value(),
        "audio clip track move test should create an audio clip");
    require(session.saveAs(path).success,
        "audio clip track move test should save setup edits before validation");

    const auto feedback = trackloom::moveMidiClipToTrack(session, clip->id, instrument.id);

    require(!feedback.success,
        "MIDI clip move-to-track action should reject audio clips");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::IncompatibleClipType,
        "audio clip track move should expose a stable failure kind");
    require(session.project().findClipById(clip->id)->trackId == audio.id,
        "audio clip track move should keep the audio clip owner unchanged");
    require(!session.isDirty(),
        "audio clip track move should not dirty an unchanged session");
}

void midiClipActionRejectsTooShortClipEndTrimWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "short-trim-midi-clip-end-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Short Clip Trim");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Short clip",
        trackloom::ClipType::Midi,
        0,
        trackloom::Project::ticksPerQuarterNote);
    require(clip.has_value(),
        "too-short trim-end test should create a one-beat MIDI clip");
    require(session.saveAs(path).success,
        "too-short trim-end test should save setup edits before validation");

    const auto feedback = trackloom::trimMidiClipEndEarlierOneBeat(session, clip->id);

    require(!feedback.success,
        "MIDI clip trim-end action should reject clips that cannot stay positive length");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::TrimFailed,
        "too-short MIDI clip trim-end should expose the stable trim-failed kind");
    require(session.project().findClipById(clip->id)->lengthTick == trackloom::Project::ticksPerQuarterNote,
        "too-short MIDI clip trim-end should keep the source clip length unchanged");
    require(!session.isDirty(),
        "too-short MIDI clip trim-end should not dirty an unchanged session");
}

void midiClipActionRejectsMissingClipEndTrimWithoutDirtyingSession()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Missing Clip Trim");

    const auto feedback = trackloom::trimMidiClipEndEarlierOneBeat(session, "missing-clip");

    require(!feedback.success,
        "MIDI clip trim-end action should reject a missing clip");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::MissingClip,
        "missing clip trim-end should expose a stable failure kind");
    require(!session.isDirty(),
        "missing clip trim-end should not dirty an unchanged session");
}

void midiClipActionRejectsAudioClipEndTrimWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "audio-trim-midi-clip-end-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Audio Clip Trim");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audio.id,
        "Vocal clip",
        trackloom::ClipType::Audio,
        0,
        trackloom::defaultAppMidiClipLengthTick);
    require(clip.has_value(),
        "audio clip trim-end test should create an audio clip");
    require(session.saveAs(path).success,
        "audio clip trim-end test should save setup edits before validation");

    const auto feedback = trackloom::trimMidiClipEndEarlierOneBeat(session, clip->id);

    require(!feedback.success,
        "MIDI clip trim-end action should reject audio clips");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::IncompatibleClipType,
        "audio clip trim-end should expose a stable failure kind");
    require(session.project().findClipById(clip->id)->lengthTick == trackloom::defaultAppMidiClipLengthTick,
        "audio clip trim-end should keep the audio clip unchanged");
    require(!session.isDirty(),
        "audio clip trim-end should not dirty an unchanged session");
}

void midiClipActionRejectsClipEndTrimThatWouldDropNotesWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "note-boundary-trim-midi-clip-end-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Note Boundary Trim");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    const auto endNote = session.editProject().createMidiNote(
        clipFeedback.clipId,
        trackloom::defaultAppMidiClipLengthTick - 120,
        120,
        72,
        100,
        1);
    require(clipFeedback.success && endNote.has_value(),
        "note-boundary trim-end test should create a note at the current clip end");
    require(session.saveAs(path).success,
        "note-boundary trim-end test should save setup edits before validation");

    const auto feedback = trackloom::trimMidiClipEndEarlierOneBeat(session, clipFeedback.clipId);

    const auto sourceClip = session.project().findClipById(clipFeedback.clipId);
    require(!feedback.success,
        "MIDI clip trim-end action should reject trimming away existing notes");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::TrimFailed,
        "note-boundary MIDI clip trim-end should expose the stable trim-failed kind");
    require(sourceClip.has_value() && sourceClip->lengthTick == trackloom::defaultAppMidiClipLengthTick,
        "note-boundary MIDI clip trim-end should keep the source clip length unchanged");
    require(!session.isDirty(),
        "note-boundary MIDI clip trim-end should not dirty an unchanged session");
}

void midiClipActionRejectsClipStartTrimThatWouldDropNotesWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "note-boundary-trim-midi-clip-start-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Start Note Boundary Trim");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    const auto startNote = session.editProject().createMidiNote(
        clipFeedback.clipId,
        0,
        trackloom::Project::ticksPerQuarterNote,
        72,
        100,
        1);
    require(clipFeedback.success && startNote.has_value(),
        "note-boundary trim-start test should create a note at the current clip start");
    require(session.saveAs(path).success,
        "note-boundary trim-start test should save setup edits before validation");

    const auto feedback = trackloom::trimMidiClipStartLaterOneBeat(session, clipFeedback.clipId);

    const auto sourceClip = session.project().findClipById(clipFeedback.clipId);
    require(!feedback.success,
        "MIDI clip trim-start action should reject trimming away existing notes");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::TrimFailed,
        "note-boundary MIDI clip trim-start should expose the stable trim-failed kind");
    require(sourceClip.has_value()
            && sourceClip->startTick == 0
            && sourceClip->lengthTick == trackloom::defaultAppMidiClipLengthTick
            && sourceClip->midiNotes[0].startTick == 0,
        "note-boundary MIDI clip trim-start should keep the source clip and note unchanged");
    require(!session.isDirty(),
        "note-boundary MIDI clip trim-start should not dirty an unchanged session");
}

void midiClipActionRejectsMissingClipEndExtendWithoutDirtyingSession()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Missing Clip Extend");

    const auto feedback = trackloom::extendMidiClipEndLaterOneBeat(session, "missing-clip");

    require(!feedback.success,
        "MIDI clip extend-end action should reject a missing clip");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::MissingClip,
        "missing clip extend-end should expose a stable failure kind");
    require(!session.isDirty(),
        "missing clip extend-end should not dirty an unchanged session");
}

void midiClipActionRejectsAudioClipEndExtendWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "audio-extend-midi-clip-end-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Audio Clip Extend");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audio.id,
        "Vocal clip",
        trackloom::ClipType::Audio,
        0,
        trackloom::defaultAppMidiClipLengthTick);
    require(clip.has_value(),
        "audio clip extend-end test should create an audio clip");
    require(session.saveAs(path).success,
        "audio clip extend-end test should save setup edits before validation");

    const auto feedback = trackloom::extendMidiClipEndLaterOneBeat(session, clip->id);

    require(!feedback.success,
        "MIDI clip extend-end action should reject audio clips");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::IncompatibleClipType,
        "audio clip extend-end should expose a stable failure kind");
    require(session.project().findClipById(clip->id)->lengthTick == trackloom::defaultAppMidiClipLengthTick,
        "audio clip extend-end should keep the audio clip unchanged");
    require(!session.isDirty(),
        "audio clip extend-end should not dirty an unchanged session");
}

void midiClipActionDeletesMidiClipAndItsNotes()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "delete-midi-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Delete Clip");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    const auto noteFeedback = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    require(clipFeedback.success && noteFeedback.success,
        "delete MIDI clip action test should create a MIDI clip with a note");
    require(session.saveAs(path).success,
        "delete MIDI clip action test should save setup edits before deleting");

    const auto feedback = trackloom::deleteMidiClipById(session, clipFeedback.clipId);

    require(feedback.success,
        "MIDI clip action should delete the requested MIDI clip");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::Success,
        "successful MIDI clip delete action should expose the stable success kind");
    require(feedback.clipId == clipFeedback.clipId,
        "MIDI clip delete action should report the deleted clip id");
    require(session.project().clips().empty(),
        "MIDI clip delete action should remove the whole target clip");
    require(!session.project().findMidiNoteById(noteFeedback.noteId).has_value(),
        "MIDI clip delete action should remove notes stored inside the deleted clip");
    require(session.isDirty(),
        "MIDI clip delete action should mark the app session dirty");
    require(feedback.message.find("删除") != std::string::npos,
        "successful MIDI clip delete feedback should describe the deletion");
}

void midiClipActionDeleteCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Clip Delete History");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Loop",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiClipLengthTick);
    require(clip.has_value(),
        "MIDI clip delete history test should create a source clip");
    const auto note = session.editProject().createMidiNote(
        clip->id,
        0,
        trackloom::Project::ticksPerQuarterNote,
        60,
        100,
        1);
    require(note.has_value(),
        "MIDI clip delete history test should create a note inside the source clip");

    const auto feedback = trackloom::deleteMidiClipById(session, clip->id);

    require(feedback.success,
        "MIDI clip delete history test should delete the target clip");
    require(session.project().clips().empty(),
        "MIDI clip delete history test should start from a deleted clip state");
    require(session.canUndoProjectEdit(),
        "MIDI clip delete action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo MIDI clip deletion from the clip action");
    const auto restoredClip = session.project().findClipById(clip->id);
    require(restoredClip.has_value(),
        "undoing MIDI clip deletion should restore the deleted clip");
    require(restoredClip->midiNotes.size() == 1 && restoredClip->midiNotes[0].id == note->id,
        "undoing MIDI clip deletion should restore notes stored inside the clip");
    require(session.canRedoProjectEdit(),
        "undoing MIDI clip deletion should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo MIDI clip deletion from the clip action");
    require(session.project().clips().empty(),
        "redoing MIDI clip deletion should remove the clip again");
    require(!session.project().findMidiNoteById(note->id).has_value(),
        "redoing MIDI clip deletion should remove the restored note again");
}

void midiClipActionRejectsMissingClipDeleteWithoutDirtyingSession()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Missing Clip Delete");

    const auto feedback = trackloom::deleteMidiClipById(session, "missing-clip");

    require(!feedback.success,
        "MIDI clip delete action should reject a missing clip");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::MissingClip,
        "missing clip MIDI clip delete action should expose a stable failure kind");
    require(session.project().clips().empty(),
        "missing clip MIDI clip delete action should not change clips");
    require(!session.isDirty(),
        "missing clip MIDI clip delete action should not dirty an unchanged session");
}

void midiClipActionRejectsAudioClipDeleteWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "audio-delete-midi-clip-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Audio Clip Delete");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audio.id,
        "Vocal clip",
        trackloom::ClipType::Audio,
        0,
        trackloom::Project::ticksPerQuarterNote);
    require(clip.has_value(),
        "audio MIDI clip delete test should create an audio clip");
    require(session.saveAs(path).success,
        "audio MIDI clip delete test should save setup edits before validation");

    const auto feedback = trackloom::deleteMidiClipById(session, clip->id);

    require(!feedback.success,
        "MIDI clip delete action should reject audio clips");
    require(feedback.kind == trackloom::AppMidiClipActionFeedbackKind::IncompatibleClipType,
        "audio clip MIDI clip delete action should expose a stable failure kind");
    require(session.project().clips().size() == 1 && session.project().clips()[0].id == clip->id,
        "audio clip MIDI clip delete action should keep the audio clip unchanged");
    require(!session.isDirty(),
        "audio clip MIDI clip delete action should not dirty an unchanged session");
}

void midiNoteActionCreatesDefaultNoteInMidiClip()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "midi-note-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Note Action");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    require(clipFeedback.success,
        "MIDI note action test should create a target MIDI clip");
    require(session.saveAs(path).success,
        "MIDI note action test should save the setup project before editing");

    const auto feedback = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);

    require(feedback.success,
        "MIDI note action should create a note in a MIDI clip");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::Success,
        "successful MIDI note action should expose a stable success kind");
    require(!feedback.noteId.empty(),
        "successful MIDI note action should expose the created note id");
    require(session.isDirty(),
        "MIDI note action should mark the app session dirty");

    const auto clip = session.project().clips()[0];
    require(clip.midiNotes.size() == 1,
        "MIDI note action should add exactly one note");
    require(clip.midiNotes[0].id == feedback.noteId,
        "MIDI note action feedback should point to the created note");
    require(clip.midiNotes[0].startTick == 0,
        "first default MIDI note should start at the beginning of the clip");
    require(clip.midiNotes[0].lengthTick == trackloom::defaultAppMidiNoteLengthTick,
        "default MIDI note should use the app-level starter note length");
    require(clip.midiNotes[0].noteNumber == trackloom::defaultAppMidiNoteNumber,
        "default MIDI note should use the app-level starter pitch");
    require(clip.midiNotes[0].velocity == trackloom::defaultAppMidiNoteVelocity,
        "default MIDI note should use the app-level starter velocity");
    require(clip.midiNotes[0].channel == trackloom::defaultAppMidiNoteChannel,
        "default MIDI note should use the app-level starter channel");
    require(feedback.message.find("音符") != std::string::npos,
        "successful MIDI note feedback should describe the created note");
}

void midiNoteActionAppendsAfterExistingNotes()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Append Notes");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);

    const auto first = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    const auto second = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);

    require(first.success && second.success,
        "MIDI note action should create repeated starter notes in the same clip");
    require(session.project().clips()[0].midiNotes.size() == 2,
        "repeated MIDI note action should create two notes");
    require(session.project().clips()[0].midiNotes[1].startTick
            == session.project().clips()[0].midiNotes[0].startTick
                + session.project().clips()[0].midiNotes[0].lengthTick,
        "second starter MIDI note should append after the first note in that clip");
}

void midiNoteActionCreateCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Note Create History");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiClipLengthTick);
    require(clip.has_value(),
        "MIDI note create history test should create a target MIDI clip");
    require(!session.canUndoProjectEdit(),
        "direct setup edits should not leave undo history before the MIDI note create action");

    const auto feedback = trackloom::createDefaultMidiNoteInClip(session, clip->id);

    require(feedback.success,
        "MIDI note create history test should add a default note");
    const auto createdClip = session.project().findClipById(clip->id);
    require(createdClip.has_value()
            && createdClip->midiNotes.size() == 1
            && createdClip->midiNotes[0].id == feedback.noteId,
        "MIDI note create history test should find the created note id");
    require(session.canUndoProjectEdit(),
        "MIDI note create action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo MIDI note creation from the note action");

    const auto emptyClip = session.project().findClipById(clip->id);
    require(emptyClip.has_value() && emptyClip->midiNotes.empty(),
        "undoing MIDI note creation should remove the created note");
    require(session.canRedoProjectEdit(),
        "undoing MIDI note creation should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo MIDI note creation from the note action");

    const auto redoneClip = session.project().findClipById(clip->id);
    require(redoneClip.has_value()
            && redoneClip->midiNotes.size() == 1
            && redoneClip->midiNotes[0].id == feedback.noteId,
        "redoing MIDI note creation should restore the same note id");
    require(redoneClip->midiNotes[0].startTick == 0
            && redoneClip->midiNotes[0].lengthTick == trackloom::defaultAppMidiNoteLengthTick
            && redoneClip->midiNotes[0].noteNumber == trackloom::defaultAppMidiNoteNumber
            && redoneClip->midiNotes[0].velocity == trackloom::defaultAppMidiNoteVelocity
            && redoneClip->midiNotes[0].channel == trackloom::defaultAppMidiNoteChannel,
        "redoing MIDI note creation should restore the default note values");
}

void midiNoteActionDuplicatesLastNoteAfterItself()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "duplicate-midi-note-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Duplicate Note");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    const auto first = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    const auto second = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    require(first.success && second.success,
        "duplicate MIDI note test should create two source notes");
    require(session.editProject().setMidiNotePitch(second.noteId, trackloom::defaultAppMidiNoteNumber + 5),
        "duplicate MIDI note test should customize the copied note pitch");
    require(session.editProject().setMidiNoteVelocity(second.noteId, trackloom::defaultAppMidiNoteVelocity - 20),
        "duplicate MIDI note test should customize the copied note velocity");
    require(session.editProject().setMidiNoteTiming(
                second.noteId,
                trackloom::defaultAppMidiNoteLengthTick,
                trackloom::defaultAppMidiNoteLengthTick / 2),
        "duplicate MIDI note test should customize the copied note timing");
    require(session.saveAs(path).success,
        "duplicate MIDI note test should save setup edits before duplicating");

    const auto feedback = trackloom::duplicateLastMidiNoteInClip(session, clipFeedback.clipId);

    const auto clip = session.project().findClipById(clipFeedback.clipId);
    require(feedback.success,
        "MIDI note duplicate action should copy the last note");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::Success,
        "successful MIDI note duplicate should expose the stable success kind");
    require(feedback.noteId != second.noteId && !feedback.noteId.empty(),
        "MIDI note duplicate should report the new note id, not the source note id");
    require(clip.has_value() && clip->midiNotes.size() == 3,
        "MIDI note duplicate should append one copied note to the clip");

    const auto& sourceNote = clip->midiNotes[1];
    const auto& copiedNote = clip->midiNotes[2];
    require(sourceNote.id == second.noteId,
        "duplicate MIDI note test should still find the source note");
    require(copiedNote.id == feedback.noteId,
        "MIDI note duplicate feedback should point to the copied note");
    require(copiedNote.startTick == sourceNote.startTick + sourceNote.lengthTick,
        "MIDI note duplicate should place the copied note immediately after the source note");
    require(copiedNote.lengthTick == sourceNote.lengthTick,
        "MIDI note duplicate should preserve the source note length");
    require(copiedNote.noteNumber == sourceNote.noteNumber,
        "MIDI note duplicate should preserve the source note pitch");
    require(copiedNote.velocity == sourceNote.velocity,
        "MIDI note duplicate should preserve the source note velocity");
    require(copiedNote.channel == sourceNote.channel,
        "MIDI note duplicate should preserve the source note channel");
    require(session.isDirty(),
        "successful MIDI note duplicate should mark the app session dirty");
}

void midiNoteActionDuplicateCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Note Duplicate History");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiClipLengthTick);
    require(clip.has_value(),
        "MIDI note duplicate history test should create a target MIDI clip");
    const auto first = session.editProject().createMidiNote(
        clip->id,
        0,
        trackloom::defaultAppMidiNoteLengthTick,
        trackloom::defaultAppMidiNoteNumber,
        trackloom::defaultAppMidiNoteVelocity,
        trackloom::defaultAppMidiNoteChannel);
    const auto second = session.editProject().createMidiNote(
        clip->id,
        trackloom::defaultAppMidiNoteLengthTick,
        trackloom::defaultAppMidiNoteLengthTick / 2,
        trackloom::defaultAppMidiNoteNumber + 5,
        trackloom::defaultAppMidiNoteVelocity - 20,
        trackloom::defaultAppMidiNoteChannel);
    require(first.has_value() && second.has_value(),
        "MIDI note duplicate history test should create source notes");
    require(!session.canUndoProjectEdit(),
        "direct setup edits should not leave undo history before the MIDI note duplicate action");

    const auto feedback = trackloom::duplicateLastMidiNoteInClip(session, clip->id);

    require(feedback.success,
        "MIDI note duplicate history test should copy the last note");
    const auto duplicatedClip = session.project().findClipById(clip->id);
    require(duplicatedClip.has_value() && duplicatedClip->midiNotes.size() == 3,
        "MIDI note duplicate history test should append the duplicate note");
    const auto duplicatedNote = duplicatedClip->midiNotes[2];
    require(duplicatedNote.id == feedback.noteId
            && duplicatedNote.id != second->id
            && duplicatedNote.startTick == second->startTick + second->lengthTick
            && duplicatedNote.lengthTick == second->lengthTick
            && duplicatedNote.noteNumber == second->noteNumber
            && duplicatedNote.velocity == second->velocity
            && duplicatedNote.channel == second->channel,
        "MIDI note duplicate history test should preserve source note values with a new id");
    require(session.canUndoProjectEdit(),
        "MIDI note duplicate action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo MIDI note duplication from the note action");

    const auto restoredClip = session.project().findClipById(clip->id);
    require(restoredClip.has_value()
            && restoredClip->midiNotes.size() == 2
            && restoredClip->midiNotes[0].id == first->id
            && restoredClip->midiNotes[1].id == second->id,
        "undoing MIDI note duplication should restore only the original notes");
    require(session.canRedoProjectEdit(),
        "undoing MIDI note duplication should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo MIDI note duplication from the note action");

    const auto redoneClip = session.project().findClipById(clip->id);
    require(redoneClip.has_value()
            && redoneClip->midiNotes.size() == 3
            && redoneClip->midiNotes[2].id == feedback.noteId,
        "redoing MIDI note duplication should restore the same duplicate note id");
}

void midiNoteActionRejectsDuplicateBeyondClipWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "duplicate-midi-note-boundary.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Duplicate Boundary");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiNoteLengthTick);
    require(clip.has_value(),
        "duplicate boundary test should create a MIDI clip");
    const auto note = session.editProject().createMidiNote(
        clip->id,
        0,
        trackloom::defaultAppMidiNoteLengthTick,
        trackloom::defaultAppMidiNoteNumber,
        trackloom::defaultAppMidiNoteVelocity,
        trackloom::defaultAppMidiNoteChannel);
    require(note.has_value(),
        "duplicate boundary test should create a note ending at the clip boundary");
    require(session.saveAs(path).success,
        "duplicate boundary test should save setup edits before validation");

    const auto feedback = trackloom::duplicateLastMidiNoteInClip(session, clip->id);

    const auto sourceClip = session.project().findClipById(clip->id);
    require(!feedback.success,
        "MIDI note duplicate should reject copied notes beyond the clip boundary");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::ClipFull,
        "duplicate boundary rejection should expose a stable clip-full kind");
    require(sourceClip.has_value() && sourceClip->midiNotes.size() == 1,
        "duplicate boundary rejection should keep the clip notes unchanged");
    require(!session.isDirty(),
        "duplicate boundary rejection should not dirty an unchanged session");
}

void midiNoteActionRejectsEmptyClipDuplicateWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "empty-duplicate-midi-note-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Empty Duplicate");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    require(clipFeedback.success,
        "empty MIDI note duplicate test should create an empty MIDI clip");
    require(session.saveAs(path).success,
        "empty MIDI note duplicate test should save setup edits before validation");

    const auto feedback = trackloom::duplicateLastMidiNoteInClip(session, clipFeedback.clipId);

    require(!feedback.success,
        "MIDI note duplicate action should reject an empty MIDI clip");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::EmptyClip,
        "empty MIDI note duplicate action should expose a stable empty-clip failure kind");
    require(session.project().clips()[0].midiNotes.empty(),
        "empty MIDI note duplicate action should keep the clip unchanged");
    require(!session.isDirty(),
        "empty MIDI note duplicate action should not dirty an unchanged session");
}

void midiNoteActionRejectsMissingClipDuplicateWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "missing-duplicate-midi-note-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Missing Duplicate Clip");
    require(session.saveAs(path).success,
        "missing MIDI note duplicate test should save setup edits before validation");

    const auto feedback = trackloom::duplicateLastMidiNoteInClip(session, "missing-clip-id");

    require(!feedback.success,
        "MIDI note duplicate action should reject a missing clip");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::MissingClip,
        "missing MIDI note duplicate action should expose a stable missing-clip failure kind");
    require(!session.isDirty(),
        "missing MIDI note duplicate action should not dirty an unchanged session");
}

void midiNoteActionRejectsAudioClipDuplicateWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "audio-duplicate-midi-note-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Audio Duplicate Clip");
    const auto audioTrack = session.editProject().createTrack("Audio", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audioTrack.id,
        "Audio Clip",
        trackloom::ClipType::Audio,
        0,
        trackloom::defaultAppMidiNoteLengthTick);
    require(clip.has_value(),
        "audio MIDI note duplicate test should create an audio clip");
    require(session.saveAs(path).success,
        "audio MIDI note duplicate test should save setup edits before validation");

    const auto feedback = trackloom::duplicateLastMidiNoteInClip(session, clip->id);

    require(!feedback.success,
        "MIDI note duplicate action should reject an audio clip");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::IncompatibleClipType,
        "audio MIDI note duplicate action should expose a stable incompatible-clip failure kind");
    require(!session.isDirty(),
        "audio MIDI note duplicate action should not dirty an unchanged session");
}

void midiNoteActionRejectsMissingClipWithoutDirtyingSession()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Missing Clip");

    const auto feedback = trackloom::createDefaultMidiNoteInClip(session, "missing-clip");

    require(!feedback.success,
        "MIDI note action should reject a missing target clip");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::MissingClip,
        "missing clip MIDI note action should expose a stable failure kind");
    require(!session.isDirty(),
        "missing clip MIDI note action should not dirty an unchanged session");
}

void midiNoteActionRejectsAudioClipWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "audio-note-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Audio Clip");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audio.id,
        "Vocal clip",
        trackloom::ClipType::Audio,
        0,
        trackloom::Project::ticksPerQuarterNote);
    require(clip.has_value(),
        "audio MIDI note action test should create an audio clip");
    require(session.saveAs(path).success,
        "audio MIDI note action test should save setup edits before validation");

    const auto feedback = trackloom::createDefaultMidiNoteInClip(session, clip->id);

    require(!feedback.success,
        "MIDI note action should reject audio clips");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::IncompatibleClipType,
        "audio clip MIDI note action should expose a stable failure kind");
    require(session.project().clips()[0].midiNotes.empty(),
        "audio clip MIDI note action should not create notes");
    require(!session.isDirty(),
        "audio clip MIDI note action should not dirty an unchanged session");
}

void midiNoteActionRejectsFullClipWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "full-note-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Full Clip");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Short MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiNoteLengthTick);
    require(clip.has_value(),
        "full MIDI note action test should create a short MIDI clip");
    require(session.editProject().createMidiNote(
                clip->id,
                0,
                trackloom::defaultAppMidiNoteLengthTick,
                trackloom::defaultAppMidiNoteNumber,
                trackloom::defaultAppMidiNoteVelocity,
                trackloom::defaultAppMidiNoteChannel)
            .has_value(),
        "full MIDI note action test should fill the short MIDI clip");
    require(session.saveAs(path).success,
        "full MIDI note action test should save setup edits before validation");

    const auto feedback = trackloom::createDefaultMidiNoteInClip(session, clip->id);

    require(!feedback.success,
        "MIDI note action should reject a clip with no room for another starter note");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::ClipFull,
        "full clip MIDI note action should expose a stable failure kind");
    require(session.project().clips()[0].midiNotes.size() == 1,
        "full clip MIDI note action should keep existing notes unchanged");
    require(!session.isDirty(),
        "full clip MIDI note action should not dirty an unchanged session");
}

void midiNoteActionDeletesLastNoteInMidiClip()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "delete-midi-note-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Delete Note");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    const auto first = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    const auto second = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    require(first.success && second.success,
        "delete MIDI note action test should create two notes");
    require(session.saveAs(path).success,
        "delete MIDI note action test should save setup edits before deleting");

    const auto feedback = trackloom::deleteLastMidiNoteInClip(session, clipFeedback.clipId);

    require(feedback.success,
        "MIDI note action should delete the last note in a MIDI clip");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::Success,
        "successful MIDI note delete action should expose the stable success kind");
    require(feedback.noteId == second.noteId,
        "MIDI note delete action should report the deleted note id");
    require(session.project().clips()[0].midiNotes.size() == 1,
        "MIDI note delete action should remove one note");
    require(session.project().clips()[0].midiNotes[0].id == first.noteId,
        "MIDI note delete action should keep the earlier note");
    require(session.isDirty(),
        "MIDI note delete action should mark the app session dirty");
    require(feedback.message.find("删除") != std::string::npos,
        "successful MIDI note delete feedback should describe the deletion");
}

void midiNoteActionDeleteCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Note Delete History");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiClipLengthTick);
    require(clip.has_value(),
        "MIDI note delete history test should create a target MIDI clip");
    const auto first = session.editProject().createMidiNote(
        clip->id,
        0,
        trackloom::defaultAppMidiNoteLengthTick,
        trackloom::defaultAppMidiNoteNumber,
        trackloom::defaultAppMidiNoteVelocity,
        trackloom::defaultAppMidiNoteChannel);
    const auto second = session.editProject().createMidiNote(
        clip->id,
        trackloom::defaultAppMidiNoteLengthTick,
        trackloom::defaultAppMidiNoteLengthTick,
        trackloom::defaultAppMidiNoteNumber + 7,
        trackloom::defaultAppMidiNoteVelocity - 10,
        trackloom::defaultAppMidiNoteChannel);
    require(first.has_value() && second.has_value(),
        "MIDI note delete history test should create source notes");
    require(!session.canUndoProjectEdit(),
        "direct setup edits should not leave undo history before the MIDI note delete action");

    const auto feedback = trackloom::deleteLastMidiNoteInClip(session, clip->id);

    require(feedback.success,
        "MIDI note delete history test should delete the last note");
    const auto afterDeleteClip = session.project().findClipById(clip->id);
    require(afterDeleteClip.has_value()
            && afterDeleteClip->midiNotes.size() == 1
            && afterDeleteClip->midiNotes[0].id == first->id,
        "MIDI note delete history test should keep only the earlier note");
    require(session.canUndoProjectEdit(),
        "MIDI note delete action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo MIDI note deletion from the note action");

    const auto restoredClip = session.project().findClipById(clip->id);
    require(restoredClip.has_value()
            && restoredClip->midiNotes.size() == 2
            && restoredClip->midiNotes[1].id == second->id
            && restoredClip->midiNotes[1].noteNumber == second->noteNumber
            && restoredClip->midiNotes[1].velocity == second->velocity,
        "undoing MIDI note deletion should restore the removed note and its values");
    require(session.canRedoProjectEdit(),
        "undoing MIDI note deletion should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo MIDI note deletion from the note action");

    const auto redoneClip = session.project().findClipById(clip->id);
    require(redoneClip.has_value()
            && redoneClip->midiNotes.size() == 1
            && redoneClip->midiNotes[0].id == first->id,
        "redoing MIDI note deletion should remove the same last note again");
}

void midiNoteActionRaisesLastNotePitchOneSemitone()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "raise-midi-note-pitch-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Raise Note Pitch");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    const auto first = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    const auto second = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    require(first.success && second.success,
        "raise MIDI note pitch test should create two notes");
    require(session.saveAs(path).success,
        "raise MIDI note pitch test should save setup edits before transposing");

    const auto feedback = trackloom::raiseLastMidiNotePitchInClip(session, clipFeedback.clipId);

    const auto clip = session.project().findClipById(clipFeedback.clipId);
    require(feedback.success,
        "MIDI note pitch action should raise the last note by one semitone");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::Success,
        "successful MIDI note pitch raise should expose the stable success kind");
    require(feedback.noteId == second.noteId,
        "MIDI note pitch raise should report the changed note id");
    require(clip.has_value() && clip->midiNotes.size() == 2,
        "MIDI note pitch raise should keep all notes in the clip");
    require(clip->midiNotes[0].noteNumber == trackloom::defaultAppMidiNoteNumber,
        "MIDI note pitch raise should keep earlier notes unchanged");
    require(clip->midiNotes[1].noteNumber == trackloom::defaultAppMidiNoteNumber + 1,
        "MIDI note pitch raise should increase only the last note pitch by one");
    require(session.isDirty(),
        "successful MIDI note pitch raise should mark the app session dirty");
}

void midiNoteActionLowersLastNotePitchOneSemitone()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "lower-midi-note-pitch-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Lower Note Pitch");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    const auto first = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    const auto second = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    require(first.success && second.success,
        "lower MIDI note pitch test should create two notes");
    require(session.saveAs(path).success,
        "lower MIDI note pitch test should save setup edits before transposing");

    const auto feedback = trackloom::lowerLastMidiNotePitchInClip(session, clipFeedback.clipId);

    const auto clip = session.project().findClipById(clipFeedback.clipId);
    require(feedback.success,
        "MIDI note pitch action should lower the last note by one semitone");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::Success,
        "successful MIDI note pitch lower should expose the stable success kind");
    require(feedback.noteId == second.noteId,
        "MIDI note pitch lower should report the changed note id");
    require(clip.has_value() && clip->midiNotes.size() == 2,
        "MIDI note pitch lower should keep all notes in the clip");
    require(clip->midiNotes[0].noteNumber == trackloom::defaultAppMidiNoteNumber,
        "MIDI note pitch lower should keep earlier notes unchanged");
    require(clip->midiNotes[1].noteNumber == trackloom::defaultAppMidiNoteNumber - 1,
        "MIDI note pitch lower should decrease only the last note pitch by one");
    require(session.isDirty(),
        "successful MIDI note pitch lower should mark the app session dirty");
}

void midiNoteActionPitchCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Note Pitch History");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiClipLengthTick);
    require(clip.has_value(),
        "MIDI note pitch history test should create a target MIDI clip");
    const auto first = session.editProject().createMidiNote(
        clip->id,
        0,
        trackloom::defaultAppMidiNoteLengthTick,
        trackloom::defaultAppMidiNoteNumber,
        trackloom::defaultAppMidiNoteVelocity,
        trackloom::defaultAppMidiNoteChannel);
    const auto second = session.editProject().createMidiNote(
        clip->id,
        trackloom::defaultAppMidiNoteLengthTick,
        trackloom::defaultAppMidiNoteLengthTick,
        trackloom::defaultAppMidiNoteNumber + 4,
        trackloom::defaultAppMidiNoteVelocity,
        trackloom::defaultAppMidiNoteChannel);
    require(first.has_value() && second.has_value(),
        "MIDI note pitch history test should create source notes");
    require(!session.canUndoProjectEdit(),
        "direct setup edits should not leave undo history before the MIDI note pitch action");

    const auto feedback = trackloom::raiseLastMidiNotePitchInClip(session, clip->id);

    require(feedback.success,
        "MIDI note pitch history test should raise the last note");
    const auto raisedClip = session.project().findClipById(clip->id);
    require(raisedClip.has_value()
            && raisedClip->midiNotes[1].id == second->id
            && raisedClip->midiNotes[1].noteNumber == second->noteNumber + 1,
        "MIDI note pitch history test should change only the target note pitch");
    require(session.canUndoProjectEdit(),
        "MIDI note pitch action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo MIDI note pitch changes from the note action");

    const auto restoredClip = session.project().findClipById(clip->id);
    require(restoredClip.has_value()
            && restoredClip->midiNotes[1].id == second->id
            && restoredClip->midiNotes[1].noteNumber == second->noteNumber,
        "undoing MIDI note pitch should restore the original pitch");
    require(session.canRedoProjectEdit(),
        "undoing MIDI note pitch should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo MIDI note pitch changes from the note action");

    const auto redoneClip = session.project().findClipById(clip->id);
    require(redoneClip.has_value()
            && redoneClip->midiNotes[1].id == second->id
            && redoneClip->midiNotes[1].noteNumber == second->noteNumber + 1,
        "redoing MIDI note pitch should reapply the raised pitch");
}

void midiNoteActionRejectsPitchRaiseAboveMidiRangeWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "raise-midi-note-pitch-boundary.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Raise Pitch Boundary");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiNoteLengthTick);
    require(clip.has_value(),
        "raise pitch boundary test should create a MIDI clip");
    const auto note = session.editProject().createMidiNote(
        clip->id,
        0,
        trackloom::defaultAppMidiNoteLengthTick,
        127,
        trackloom::defaultAppMidiNoteVelocity,
        trackloom::defaultAppMidiNoteChannel);
    require(note.has_value(),
        "raise pitch boundary test should create a top-range MIDI note");
    require(session.saveAs(path).success,
        "raise pitch boundary test should save setup edits before validation");

    const auto feedback = trackloom::raiseLastMidiNotePitchInClip(session, clip->id);

    const auto sourceClip = session.project().findClipById(clip->id);
    require(!feedback.success,
        "MIDI note pitch raise should reject pitches above 127");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::PitchFailed,
        "top-range pitch raise should expose a stable pitch-failed kind");
    require(sourceClip.has_value() && sourceClip->midiNotes[0].noteNumber == 127,
        "top-range pitch raise should keep the note pitch unchanged");
    require(!session.isDirty(),
        "top-range pitch raise should not dirty an unchanged session");
}

void midiNoteActionRejectsPitchLowerBelowMidiRangeWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "lower-midi-note-pitch-boundary.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Lower Pitch Boundary");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiNoteLengthTick);
    require(clip.has_value(),
        "lower pitch boundary test should create a MIDI clip");
    const auto note = session.editProject().createMidiNote(
        clip->id,
        0,
        trackloom::defaultAppMidiNoteLengthTick,
        0,
        trackloom::defaultAppMidiNoteVelocity,
        trackloom::defaultAppMidiNoteChannel);
    require(note.has_value(),
        "lower pitch boundary test should create a bottom-range MIDI note");
    require(session.saveAs(path).success,
        "lower pitch boundary test should save setup edits before validation");

    const auto feedback = trackloom::lowerLastMidiNotePitchInClip(session, clip->id);

    const auto sourceClip = session.project().findClipById(clip->id);
    require(!feedback.success,
        "MIDI note pitch lower should reject pitches below 0");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::PitchFailed,
        "bottom-range pitch lower should expose a stable pitch-failed kind");
    require(sourceClip.has_value() && sourceClip->midiNotes[0].noteNumber == 0,
        "bottom-range pitch lower should keep the note pitch unchanged");
    require(!session.isDirty(),
        "bottom-range pitch lower should not dirty an unchanged session");
}

void midiNoteActionRejectsEmptyClipPitchWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "empty-pitch-midi-note-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Empty Pitch");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    require(clipFeedback.success,
        "empty MIDI note pitch test should create an empty MIDI clip");
    require(session.saveAs(path).success,
        "empty MIDI note pitch test should save setup edits before validation");

    const auto feedback = trackloom::raiseLastMidiNotePitchInClip(session, clipFeedback.clipId);

    require(!feedback.success,
        "MIDI note pitch action should reject an empty MIDI clip");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::EmptyClip,
        "empty MIDI note pitch action should expose a stable empty-clip failure kind");
    require(session.project().clips()[0].midiNotes.empty(),
        "empty MIDI note pitch action should keep the clip unchanged");
    require(!session.isDirty(),
        "empty MIDI note pitch action should not dirty an unchanged session");
}

void midiNoteActionIncreasesLastNoteVelocityByStep()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "increase-midi-note-velocity-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Increase Note Velocity");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    const auto first = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    const auto second = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    require(first.success && second.success,
        "increase MIDI note velocity test should create two notes");
    require(session.saveAs(path).success,
        "increase MIDI note velocity test should save setup edits before changing velocity");

    const auto feedback = trackloom::increaseLastMidiNoteVelocityInClip(session, clipFeedback.clipId);

    const auto clip = session.project().findClipById(clipFeedback.clipId);
    require(feedback.success,
        "MIDI note velocity action should increase the last note velocity");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::Success,
        "successful MIDI note velocity increase should expose the stable success kind");
    require(feedback.noteId == second.noteId,
        "MIDI note velocity increase should report the changed note id");
    require(clip.has_value() && clip->midiNotes.size() == 2,
        "MIDI note velocity increase should keep all notes in the clip");
    require(clip->midiNotes[0].velocity == trackloom::defaultAppMidiNoteVelocity,
        "MIDI note velocity increase should keep earlier notes unchanged");
    require(clip->midiNotes[1].velocity == trackloom::defaultAppMidiNoteVelocity + trackloom::defaultAppMidiNoteVelocityStep,
        "MIDI note velocity increase should affect only the last note by the app-level step");
    require(session.isDirty(),
        "successful MIDI note velocity increase should mark the app session dirty");
}

void midiNoteActionDecreasesLastNoteVelocityByStep()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "decrease-midi-note-velocity-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Decrease Note Velocity");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    const auto first = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    const auto second = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    require(first.success && second.success,
        "decrease MIDI note velocity test should create two notes");
    require(session.saveAs(path).success,
        "decrease MIDI note velocity test should save setup edits before changing velocity");

    const auto feedback = trackloom::decreaseLastMidiNoteVelocityInClip(session, clipFeedback.clipId);

    const auto clip = session.project().findClipById(clipFeedback.clipId);
    require(feedback.success,
        "MIDI note velocity action should decrease the last note velocity");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::Success,
        "successful MIDI note velocity decrease should expose the stable success kind");
    require(feedback.noteId == second.noteId,
        "MIDI note velocity decrease should report the changed note id");
    require(clip.has_value() && clip->midiNotes.size() == 2,
        "MIDI note velocity decrease should keep all notes in the clip");
    require(clip->midiNotes[0].velocity == trackloom::defaultAppMidiNoteVelocity,
        "MIDI note velocity decrease should keep earlier notes unchanged");
    require(clip->midiNotes[1].velocity == trackloom::defaultAppMidiNoteVelocity - trackloom::defaultAppMidiNoteVelocityStep,
        "MIDI note velocity decrease should affect only the last note by the app-level step");
    require(session.isDirty(),
        "successful MIDI note velocity decrease should mark the app session dirty");
}

void midiNoteActionVelocityCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Note Velocity History");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiClipLengthTick);
    require(clip.has_value(),
        "MIDI note velocity history test should create a target MIDI clip");
    const auto first = session.editProject().createMidiNote(
        clip->id,
        0,
        trackloom::defaultAppMidiNoteLengthTick,
        trackloom::defaultAppMidiNoteNumber,
        trackloom::defaultAppMidiNoteVelocity,
        trackloom::defaultAppMidiNoteChannel);
    const auto second = session.editProject().createMidiNote(
        clip->id,
        trackloom::defaultAppMidiNoteLengthTick,
        trackloom::defaultAppMidiNoteLengthTick,
        trackloom::defaultAppMidiNoteNumber,
        trackloom::defaultAppMidiNoteVelocity - 20,
        trackloom::defaultAppMidiNoteChannel);
    require(first.has_value() && second.has_value(),
        "MIDI note velocity history test should create source notes");
    require(!session.canUndoProjectEdit(),
        "direct setup edits should not leave undo history before the MIDI note velocity action");

    const auto feedback = trackloom::increaseLastMidiNoteVelocityInClip(session, clip->id);

    require(feedback.success,
        "MIDI note velocity history test should increase the last note velocity");
    const auto changedClip = session.project().findClipById(clip->id);
    require(changedClip.has_value()
            && changedClip->midiNotes[1].id == second->id
            && changedClip->midiNotes[1].velocity == second->velocity + trackloom::defaultAppMidiNoteVelocityStep,
        "MIDI note velocity history test should change only the target note velocity");
    require(session.canUndoProjectEdit(),
        "MIDI note velocity action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo MIDI note velocity changes from the note action");

    const auto restoredClip = session.project().findClipById(clip->id);
    require(restoredClip.has_value()
            && restoredClip->midiNotes[1].id == second->id
            && restoredClip->midiNotes[1].velocity == second->velocity,
        "undoing MIDI note velocity should restore the original velocity");
    require(session.canRedoProjectEdit(),
        "undoing MIDI note velocity should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo MIDI note velocity changes from the note action");

    const auto redoneClip = session.project().findClipById(clip->id);
    require(redoneClip.has_value()
            && redoneClip->midiNotes[1].id == second->id
            && redoneClip->midiNotes[1].velocity == second->velocity + trackloom::defaultAppMidiNoteVelocityStep,
        "redoing MIDI note velocity should reapply the changed velocity");
}

void midiNoteActionRejectsVelocityIncreaseAboveMidiRangeWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "increase-midi-note-velocity-boundary.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Increase Velocity Boundary");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiNoteLengthTick);
    require(clip.has_value(),
        "increase velocity boundary test should create a MIDI clip");
    const auto note = session.editProject().createMidiNote(
        clip->id,
        0,
        trackloom::defaultAppMidiNoteLengthTick,
        trackloom::defaultAppMidiNoteNumber,
        127,
        trackloom::defaultAppMidiNoteChannel);
    require(note.has_value(),
        "increase velocity boundary test should create a top-velocity MIDI note");
    require(session.saveAs(path).success,
        "increase velocity boundary test should save setup edits before validation");

    const auto feedback = trackloom::increaseLastMidiNoteVelocityInClip(session, clip->id);

    const auto sourceClip = session.project().findClipById(clip->id);
    require(!feedback.success,
        "MIDI note velocity increase should reject velocities above 127");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::VelocityFailed,
        "top-range velocity increase should expose a stable velocity-failed kind");
    require(sourceClip.has_value() && sourceClip->midiNotes[0].velocity == 127,
        "top-range velocity increase should keep the note velocity unchanged");
    require(!session.isDirty(),
        "top-range velocity increase should not dirty an unchanged session");
}

void midiNoteActionRejectsVelocityDecreaseBelowMidiRangeWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "decrease-midi-note-velocity-boundary.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Decrease Velocity Boundary");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiNoteLengthTick);
    require(clip.has_value(),
        "decrease velocity boundary test should create a MIDI clip");
    const auto note = session.editProject().createMidiNote(
        clip->id,
        0,
        trackloom::defaultAppMidiNoteLengthTick,
        trackloom::defaultAppMidiNoteNumber,
        1,
        trackloom::defaultAppMidiNoteChannel);
    require(note.has_value(),
        "decrease velocity boundary test should create a bottom-velocity MIDI note");
    require(session.saveAs(path).success,
        "decrease velocity boundary test should save setup edits before validation");

    const auto feedback = trackloom::decreaseLastMidiNoteVelocityInClip(session, clip->id);

    const auto sourceClip = session.project().findClipById(clip->id);
    require(!feedback.success,
        "MIDI note velocity decrease should reject velocities below 1");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::VelocityFailed,
        "bottom-range velocity decrease should expose a stable velocity-failed kind");
    require(sourceClip.has_value() && sourceClip->midiNotes[0].velocity == 1,
        "bottom-range velocity decrease should keep the note velocity unchanged");
    require(!session.isDirty(),
        "bottom-range velocity decrease should not dirty an unchanged session");
}

void midiNoteActionRejectsEmptyClipVelocityWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "empty-velocity-midi-note-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Empty Velocity");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    require(clipFeedback.success,
        "empty MIDI note velocity test should create an empty MIDI clip");
    require(session.saveAs(path).success,
        "empty MIDI note velocity test should save setup edits before validation");

    const auto feedback = trackloom::increaseLastMidiNoteVelocityInClip(session, clipFeedback.clipId);

    require(!feedback.success,
        "MIDI note velocity action should reject an empty MIDI clip");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::EmptyClip,
        "empty MIDI note velocity action should expose a stable empty-clip failure kind");
    require(session.project().clips()[0].midiNotes.empty(),
        "empty MIDI note velocity action should keep the clip unchanged");
    require(!session.isDirty(),
        "empty MIDI note velocity action should not dirty an unchanged session");
}

void midiNoteActionRejectsMissingClipVelocityWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "missing-velocity-midi-note-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Missing Velocity Clip");
    require(session.saveAs(path).success,
        "missing MIDI note velocity test should save setup edits before validation");

    const auto feedback = trackloom::increaseLastMidiNoteVelocityInClip(session, "missing-clip-id");

    require(!feedback.success,
        "MIDI note velocity action should reject a missing clip");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::MissingClip,
        "missing MIDI note velocity action should expose a stable missing-clip failure kind");
    require(!session.isDirty(),
        "missing MIDI note velocity action should not dirty an unchanged session");
}

void midiNoteActionRejectsAudioClipVelocityWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "audio-velocity-midi-note-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Audio Velocity Clip");
    const auto audioTrack = session.editProject().createTrack("Audio", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audioTrack.id,
        "Audio Clip",
        trackloom::ClipType::Audio,
        0,
        trackloom::defaultAppMidiNoteLengthTick);
    require(clip.has_value(),
        "audio MIDI note velocity test should create an audio clip");
    require(session.saveAs(path).success,
        "audio MIDI note velocity test should save setup edits before validation");

    const auto feedback = trackloom::increaseLastMidiNoteVelocityInClip(session, clip->id);

    require(!feedback.success,
        "MIDI note velocity action should reject an audio clip");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::IncompatibleClipType,
        "audio MIDI note velocity action should expose a stable incompatible-clip failure kind");
    require(!session.isDirty(),
        "audio MIDI note velocity action should not dirty an unchanged session");
}

void midiNoteActionLengthensLastNoteByStep()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "lengthen-midi-note-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Lengthen Note");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    const auto first = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    const auto second = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    require(first.success && second.success,
        "lengthen MIDI note test should create two notes");
    require(session.saveAs(path).success,
        "lengthen MIDI note test should save setup edits before changing length");

    const auto feedback = trackloom::lengthenLastMidiNoteInClip(session, clipFeedback.clipId);

    const auto clip = session.project().findClipById(clipFeedback.clipId);
    require(feedback.success,
        "MIDI note length action should lengthen the last note");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::Success,
        "successful MIDI note lengthen should expose the stable success kind");
    require(feedback.noteId == second.noteId,
        "MIDI note lengthen should report the changed note id");
    require(clip.has_value() && clip->midiNotes.size() == 2,
        "MIDI note lengthen should keep all notes in the clip");
    require(clip->midiNotes[0].lengthTick == trackloom::defaultAppMidiNoteLengthTick,
        "MIDI note lengthen should keep earlier notes unchanged");
    require(clip->midiNotes[1].lengthTick == trackloom::defaultAppMidiNoteLengthTick + trackloom::defaultAppMidiNoteLengthStepTick,
        "MIDI note lengthen should affect only the last note by the app-level step");
    require(session.isDirty(),
        "successful MIDI note lengthen should mark the app session dirty");
}

void midiNoteActionShortensLastNoteByStep()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "shorten-midi-note-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Shorten Note");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    const auto first = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    const auto second = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    require(first.success && second.success,
        "shorten MIDI note test should create two notes");
    require(session.saveAs(path).success,
        "shorten MIDI note test should save setup edits before changing length");

    const auto feedback = trackloom::shortenLastMidiNoteInClip(session, clipFeedback.clipId);

    const auto clip = session.project().findClipById(clipFeedback.clipId);
    require(feedback.success,
        "MIDI note length action should shorten the last note");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::Success,
        "successful MIDI note shorten should expose the stable success kind");
    require(feedback.noteId == second.noteId,
        "MIDI note shorten should report the changed note id");
    require(clip.has_value() && clip->midiNotes.size() == 2,
        "MIDI note shorten should keep all notes in the clip");
    require(clip->midiNotes[0].lengthTick == trackloom::defaultAppMidiNoteLengthTick,
        "MIDI note shorten should keep earlier notes unchanged");
    require(clip->midiNotes[1].lengthTick == trackloom::defaultAppMidiNoteLengthTick - trackloom::defaultAppMidiNoteLengthStepTick,
        "MIDI note shorten should affect only the last note by the app-level step");
    require(session.isDirty(),
        "successful MIDI note shorten should mark the app session dirty");
}

void midiNoteActionLengthCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Note Length History");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiClipLengthTick);
    require(clip.has_value(),
        "MIDI note length history test should create a target MIDI clip");
    const auto first = session.editProject().createMidiNote(
        clip->id,
        0,
        trackloom::defaultAppMidiNoteLengthTick,
        trackloom::defaultAppMidiNoteNumber,
        trackloom::defaultAppMidiNoteVelocity,
        trackloom::defaultAppMidiNoteChannel);
    const auto second = session.editProject().createMidiNote(
        clip->id,
        trackloom::defaultAppMidiNoteLengthTick,
        trackloom::defaultAppMidiNoteLengthTick,
        trackloom::defaultAppMidiNoteNumber,
        trackloom::defaultAppMidiNoteVelocity,
        trackloom::defaultAppMidiNoteChannel);
    require(first.has_value() && second.has_value(),
        "MIDI note length history test should create source notes");
    require(!session.canUndoProjectEdit(),
        "direct setup edits should not leave undo history before the MIDI note length action");

    const auto feedback = trackloom::lengthenLastMidiNoteInClip(session, clip->id);

    require(feedback.success,
        "MIDI note length history test should lengthen the last note");
    const auto changedClip = session.project().findClipById(clip->id);
    require(changedClip.has_value()
            && changedClip->midiNotes[1].id == second->id
            && changedClip->midiNotes[1].startTick == second->startTick
            && changedClip->midiNotes[1].lengthTick == second->lengthTick + trackloom::defaultAppMidiNoteLengthStepTick,
        "MIDI note length history test should change only the target note length");
    require(session.canUndoProjectEdit(),
        "MIDI note length action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo MIDI note length changes from the note action");

    const auto restoredClip = session.project().findClipById(clip->id);
    require(restoredClip.has_value()
            && restoredClip->midiNotes[1].id == second->id
            && restoredClip->midiNotes[1].startTick == second->startTick
            && restoredClip->midiNotes[1].lengthTick == second->lengthTick,
        "undoing MIDI note length should restore the original timing");
    require(session.canRedoProjectEdit(),
        "undoing MIDI note length should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo MIDI note length changes from the note action");

    const auto redoneClip = session.project().findClipById(clip->id);
    require(redoneClip.has_value()
            && redoneClip->midiNotes[1].id == second->id
            && redoneClip->midiNotes[1].lengthTick == second->lengthTick + trackloom::defaultAppMidiNoteLengthStepTick,
        "redoing MIDI note length should reapply the changed length");
}

void midiNoteActionRejectsLengthenBeyondClipWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "lengthen-midi-note-boundary.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Lengthen Boundary");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiNoteLengthTick);
    require(clip.has_value(),
        "lengthen boundary test should create a MIDI clip");
    const auto note = session.editProject().createMidiNote(
        clip->id,
        0,
        trackloom::defaultAppMidiNoteLengthTick,
        trackloom::defaultAppMidiNoteNumber,
        trackloom::defaultAppMidiNoteVelocity,
        trackloom::defaultAppMidiNoteChannel);
    require(note.has_value(),
        "lengthen boundary test should create a note ending at the clip boundary");
    require(session.saveAs(path).success,
        "lengthen boundary test should save setup edits before validation");

    const auto feedback = trackloom::lengthenLastMidiNoteInClip(session, clip->id);

    const auto sourceClip = session.project().findClipById(clip->id);
    require(!feedback.success,
        "MIDI note lengthen should reject note end beyond the clip boundary");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::LengthFailed,
        "lengthen boundary rejection should expose a stable length-failed kind");
    require(sourceClip.has_value() && sourceClip->midiNotes[0].lengthTick == trackloom::defaultAppMidiNoteLengthTick,
        "lengthen boundary rejection should keep the note length unchanged");
    require(!session.isDirty(),
        "lengthen boundary rejection should not dirty an unchanged session");
}

void midiNoteActionRejectsShortenBelowMinimumWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "shorten-midi-note-boundary.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Shorten Boundary");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiNoteLengthTick);
    require(clip.has_value(),
        "shorten boundary test should create a MIDI clip");
    const auto note = session.editProject().createMidiNote(
        clip->id,
        0,
        trackloom::defaultAppMidiNoteLengthStepTick,
        trackloom::defaultAppMidiNoteNumber,
        trackloom::defaultAppMidiNoteVelocity,
        trackloom::defaultAppMidiNoteChannel);
    require(note.has_value(),
        "shorten boundary test should create a minimum-length MIDI note");
    require(session.saveAs(path).success,
        "shorten boundary test should save setup edits before validation");

    const auto feedback = trackloom::shortenLastMidiNoteInClip(session, clip->id);

    const auto sourceClip = session.project().findClipById(clip->id);
    require(!feedback.success,
        "MIDI note shorten should reject lengths below the app minimum");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::LengthFailed,
        "shorten boundary rejection should expose a stable length-failed kind");
    require(sourceClip.has_value() && sourceClip->midiNotes[0].lengthTick == trackloom::defaultAppMidiNoteLengthStepTick,
        "shorten boundary rejection should keep the note length unchanged");
    require(!session.isDirty(),
        "shorten boundary rejection should not dirty an unchanged session");
}

void midiNoteActionRejectsEmptyClipLengthWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "empty-length-midi-note-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Empty Length");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    require(clipFeedback.success,
        "empty MIDI note length test should create an empty MIDI clip");
    require(session.saveAs(path).success,
        "empty MIDI note length test should save setup edits before validation");

    const auto feedback = trackloom::lengthenLastMidiNoteInClip(session, clipFeedback.clipId);

    require(!feedback.success,
        "MIDI note length action should reject an empty MIDI clip");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::EmptyClip,
        "empty MIDI note length action should expose a stable empty-clip failure kind");
    require(session.project().clips()[0].midiNotes.empty(),
        "empty MIDI note length action should keep the clip unchanged");
    require(!session.isDirty(),
        "empty MIDI note length action should not dirty an unchanged session");
}

void midiNoteActionRejectsMissingClipLengthWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "missing-length-midi-note-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Missing Length Clip");
    require(session.saveAs(path).success,
        "missing MIDI note length test should save setup edits before validation");

    const auto feedback = trackloom::lengthenLastMidiNoteInClip(session, "missing-clip-id");

    require(!feedback.success,
        "MIDI note length action should reject a missing clip");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::MissingClip,
        "missing MIDI note length action should expose a stable missing-clip failure kind");
    require(!session.isDirty(),
        "missing MIDI note length action should not dirty an unchanged session");
}

void midiNoteActionRejectsAudioClipLengthWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "audio-length-midi-note-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Audio Length Clip");
    const auto audioTrack = session.editProject().createTrack("Audio", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audioTrack.id,
        "Audio Clip",
        trackloom::ClipType::Audio,
        0,
        trackloom::defaultAppMidiNoteLengthTick);
    require(clip.has_value(),
        "audio MIDI note length test should create an audio clip");
    require(session.saveAs(path).success,
        "audio MIDI note length test should save setup edits before validation");

    const auto feedback = trackloom::lengthenLastMidiNoteInClip(session, clip->id);

    require(!feedback.success,
        "MIDI note length action should reject an audio clip");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::IncompatibleClipType,
        "audio MIDI note length action should expose a stable incompatible-clip failure kind");
    require(!session.isDirty(),
        "audio MIDI note length action should not dirty an unchanged session");
}

void midiNoteActionMovesLastNoteStartEarlierByStep()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "move-earlier-midi-note-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Move Note Earlier");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    const auto first = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    const auto second = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    require(first.success && second.success,
        "move earlier MIDI note test should create two notes");
    require(session.saveAs(path).success,
        "move earlier MIDI note test should save setup edits before changing timing");

    const auto feedback = trackloom::moveLastMidiNoteStartEarlierInClip(session, clipFeedback.clipId);

    const auto clip = session.project().findClipById(clipFeedback.clipId);
    require(feedback.success,
        "MIDI note timing action should move the last note earlier");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::Success,
        "successful MIDI note timing move should expose the stable success kind");
    require(feedback.noteId == second.noteId,
        "MIDI note timing move should report the changed note id");
    require(clip.has_value() && clip->midiNotes.size() == 2,
        "MIDI note timing move should keep all notes in the clip");
    require(clip->midiNotes[0].startTick == 0,
        "MIDI note timing move should keep earlier notes unchanged");
    require(clip->midiNotes[1].startTick
            == trackloom::defaultAppMidiNoteLengthTick - trackloom::defaultAppMidiNoteLengthStepTick,
        "MIDI note timing move earlier should subtract one app-level step from the last note start");
    require(clip->midiNotes[1].lengthTick == trackloom::defaultAppMidiNoteLengthTick,
        "MIDI note timing move earlier should keep the last note length unchanged");
    require(session.isDirty(),
        "successful MIDI note timing move should mark the app session dirty");
}

void midiNoteActionMovesLastNoteStartLaterByStep()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "move-later-midi-note-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Move Note Later");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    const auto first = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    const auto second = trackloom::createDefaultMidiNoteInClip(session, clipFeedback.clipId);
    require(first.success && second.success,
        "move later MIDI note test should create two notes");
    require(session.saveAs(path).success,
        "move later MIDI note test should save setup edits before changing timing");

    const auto feedback = trackloom::moveLastMidiNoteStartLaterInClip(session, clipFeedback.clipId);

    const auto clip = session.project().findClipById(clipFeedback.clipId);
    require(feedback.success,
        "MIDI note timing action should move the last note later");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::Success,
        "successful MIDI note timing move should expose the stable success kind");
    require(feedback.noteId == second.noteId,
        "MIDI note timing move should report the changed note id");
    require(clip.has_value() && clip->midiNotes.size() == 2,
        "MIDI note timing move should keep all notes in the clip");
    require(clip->midiNotes[0].startTick == 0,
        "MIDI note timing move should keep earlier notes unchanged");
    require(clip->midiNotes[1].startTick
            == trackloom::defaultAppMidiNoteLengthTick + trackloom::defaultAppMidiNoteLengthStepTick,
        "MIDI note timing move later should add one app-level step to the last note start");
    require(clip->midiNotes[1].lengthTick == trackloom::defaultAppMidiNoteLengthTick,
        "MIDI note timing move later should keep the last note length unchanged");
    require(session.isDirty(),
        "successful MIDI note timing move should mark the app session dirty");
}

void midiNoteActionStartCanBeUndoneAndRedoneThroughSessionHistory()
{
    trackloom::AppProjectSession session;
    session.createNewProject("MIDI Note Start History");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiClipLengthTick);
    require(clip.has_value(),
        "MIDI note start history test should create a target MIDI clip");
    const auto first = session.editProject().createMidiNote(
        clip->id,
        0,
        trackloom::defaultAppMidiNoteLengthTick,
        trackloom::defaultAppMidiNoteNumber,
        trackloom::defaultAppMidiNoteVelocity,
        trackloom::defaultAppMidiNoteChannel);
    const auto second = session.editProject().createMidiNote(
        clip->id,
        trackloom::defaultAppMidiNoteLengthTick,
        trackloom::defaultAppMidiNoteLengthTick,
        trackloom::defaultAppMidiNoteNumber,
        trackloom::defaultAppMidiNoteVelocity,
        trackloom::defaultAppMidiNoteChannel);
    require(first.has_value() && second.has_value(),
        "MIDI note start history test should create source notes");
    require(!session.canUndoProjectEdit(),
        "direct setup edits should not leave undo history before the MIDI note start action");

    const auto feedback = trackloom::moveLastMidiNoteStartLaterInClip(session, clip->id);

    require(feedback.success,
        "MIDI note start history test should move the last note later");
    const auto changedClip = session.project().findClipById(clip->id);
    require(changedClip.has_value()
            && changedClip->midiNotes[1].id == second->id
            && changedClip->midiNotes[1].startTick == second->startTick + trackloom::defaultAppMidiNoteLengthStepTick
            && changedClip->midiNotes[1].lengthTick == second->lengthTick,
        "MIDI note start history test should move only the target note start");
    require(session.canUndoProjectEdit(),
        "MIDI note start action should enter the app session undo history");
    require(session.undoProjectEdit(),
        "app session should undo MIDI note start changes from the note action");

    const auto restoredClip = session.project().findClipById(clip->id);
    require(restoredClip.has_value()
            && restoredClip->midiNotes[1].id == second->id
            && restoredClip->midiNotes[1].startTick == second->startTick
            && restoredClip->midiNotes[1].lengthTick == second->lengthTick,
        "undoing MIDI note start should restore the original timing");
    require(session.canRedoProjectEdit(),
        "undoing MIDI note start should make redo available");
    require(session.redoProjectEdit(),
        "app session should redo MIDI note start changes from the note action");

    const auto redoneClip = session.project().findClipById(clip->id);
    require(redoneClip.has_value()
            && redoneClip->midiNotes[1].id == second->id
            && redoneClip->midiNotes[1].startTick == second->startTick + trackloom::defaultAppMidiNoteLengthStepTick,
        "redoing MIDI note start should reapply the moved start");
}

void midiNoteActionRejectsMoveEarlierBeforeClipStartWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "move-earlier-midi-note-boundary.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Move Earlier Boundary");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiNoteLengthTick);
    require(clip.has_value(),
        "move earlier boundary test should create a MIDI clip");
    const auto note = session.editProject().createMidiNote(
        clip->id,
        0,
        trackloom::defaultAppMidiNoteLengthTick,
        trackloom::defaultAppMidiNoteNumber,
        trackloom::defaultAppMidiNoteVelocity,
        trackloom::defaultAppMidiNoteChannel);
    require(note.has_value(),
        "move earlier boundary test should create a note at the clip start");
    require(session.saveAs(path).success,
        "move earlier boundary test should save setup edits before validation");

    const auto feedback = trackloom::moveLastMidiNoteStartEarlierInClip(session, clip->id);

    const auto sourceClip = session.project().findClipById(clip->id);
    require(!feedback.success,
        "MIDI note timing move earlier should reject negative note starts");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::TimingFailed,
        "move earlier boundary rejection should expose a stable timing-failed kind");
    require(sourceClip.has_value() && sourceClip->midiNotes[0].startTick == 0,
        "move earlier boundary rejection should keep the note start unchanged");
    require(!session.isDirty(),
        "move earlier boundary rejection should not dirty an unchanged session");
}

void midiNoteActionRejectsMoveLaterBeyondClipEndWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "move-later-midi-note-boundary.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Move Later Boundary");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = session.editProject().createClip(
        instrument.id,
        "Lead MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::defaultAppMidiNoteLengthTick);
    require(clip.has_value(),
        "move later boundary test should create a MIDI clip");
    const auto note = session.editProject().createMidiNote(
        clip->id,
        0,
        trackloom::defaultAppMidiNoteLengthTick,
        trackloom::defaultAppMidiNoteNumber,
        trackloom::defaultAppMidiNoteVelocity,
        trackloom::defaultAppMidiNoteChannel);
    require(note.has_value(),
        "move later boundary test should create a note ending at the clip boundary");
    require(session.saveAs(path).success,
        "move later boundary test should save setup edits before validation");

    const auto feedback = trackloom::moveLastMidiNoteStartLaterInClip(session, clip->id);

    const auto sourceClip = session.project().findClipById(clip->id);
    require(!feedback.success,
        "MIDI note timing move later should reject note ends beyond the clip boundary");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::TimingFailed,
        "move later boundary rejection should expose a stable timing-failed kind");
    require(sourceClip.has_value() && sourceClip->midiNotes[0].startTick == 0,
        "move later boundary rejection should keep the note start unchanged");
    require(!session.isDirty(),
        "move later boundary rejection should not dirty an unchanged session");
}

void midiNoteActionRejectsEmptyClipTimingWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "empty-timing-midi-note-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Empty Timing");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    require(clipFeedback.success,
        "empty MIDI note timing test should create an empty MIDI clip");
    require(session.saveAs(path).success,
        "empty MIDI note timing test should save setup edits before validation");

    const auto feedback = trackloom::moveLastMidiNoteStartLaterInClip(session, clipFeedback.clipId);

    require(!feedback.success,
        "MIDI note timing action should reject an empty MIDI clip");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::EmptyClip,
        "empty MIDI note timing action should expose a stable empty-clip failure kind");
    require(session.project().clips()[0].midiNotes.empty(),
        "empty MIDI note timing action should keep the clip unchanged");
    require(!session.isDirty(),
        "empty MIDI note timing action should not dirty an unchanged session");
}

void midiNoteActionRejectsMissingClipTimingWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "missing-timing-midi-note-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Missing Timing Clip");
    require(session.saveAs(path).success,
        "missing MIDI note timing test should save setup edits before validation");

    const auto feedback = trackloom::moveLastMidiNoteStartLaterInClip(session, "missing-clip-id");

    require(!feedback.success,
        "MIDI note timing action should reject a missing clip");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::MissingClip,
        "missing MIDI note timing action should expose a stable missing-clip failure kind");
    require(!session.isDirty(),
        "missing MIDI note timing action should not dirty an unchanged session");
}

void midiNoteActionRejectsAudioClipTimingWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "audio-timing-midi-note-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Audio Timing Clip");
    const auto audioTrack = session.editProject().createTrack("Audio", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audioTrack.id,
        "Audio Clip",
        trackloom::ClipType::Audio,
        0,
        trackloom::defaultAppMidiNoteLengthTick);
    require(clip.has_value(),
        "audio MIDI note timing test should create an audio clip");
    require(session.saveAs(path).success,
        "audio MIDI note timing test should save setup edits before validation");

    const auto feedback = trackloom::moveLastMidiNoteStartLaterInClip(session, clip->id);

    require(!feedback.success,
        "MIDI note timing action should reject an audio clip");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::IncompatibleClipType,
        "audio MIDI note timing action should expose a stable incompatible-clip failure kind");
    require(!session.isDirty(),
        "audio MIDI note timing action should not dirty an unchanged session");
}

void midiNoteActionRejectsEmptyClipDeleteWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "empty-delete-midi-note-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Empty Delete");
    const auto instrument = session.editProject().createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clipFeedback = trackloom::createDefaultMidiClipOnTrack(session, instrument.id);
    require(clipFeedback.success,
        "empty MIDI note delete test should create an empty MIDI clip");
    require(session.saveAs(path).success,
        "empty MIDI note delete test should save setup edits before validation");

    const auto feedback = trackloom::deleteLastMidiNoteInClip(session, clipFeedback.clipId);

    require(!feedback.success,
        "MIDI note delete action should reject an empty MIDI clip");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::EmptyClip,
        "empty MIDI note delete action should expose a stable failure kind");
    require(session.project().clips()[0].midiNotes.empty(),
        "empty MIDI note delete action should keep the clip unchanged");
    require(!session.isDirty(),
        "empty MIDI note delete action should not dirty an unchanged session");
}

void midiNoteActionRejectsMissingClipDeleteWithoutDirtyingSession()
{
    trackloom::AppProjectSession session;
    session.createNewProject("Missing Delete");

    const auto feedback = trackloom::deleteLastMidiNoteInClip(session, "missing-clip");

    require(!feedback.success,
        "MIDI note delete action should reject a missing target clip");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::MissingClip,
        "missing clip MIDI note delete action should expose a stable failure kind");
    require(!session.isDirty(),
        "missing clip MIDI note delete action should not dirty an unchanged session");
}

void midiNoteActionRejectsAudioClipDeleteWithoutDirtyingSession()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "audio-delete-midi-note-action.trackloom-test";

    trackloom::AppProjectSession session;
    session.createNewProject("Audio Delete");
    const auto audio = session.editProject().createTrack("Vocal", trackloom::TrackType::Audio);
    const auto clip = session.editProject().createClip(
        audio.id,
        "Vocal clip",
        trackloom::ClipType::Audio,
        0,
        trackloom::Project::ticksPerQuarterNote);
    require(clip.has_value(),
        "audio MIDI note delete test should create an audio clip");
    require(session.saveAs(path).success,
        "audio MIDI note delete test should save setup edits before validation");

    const auto feedback = trackloom::deleteLastMidiNoteInClip(session, clip->id);

    require(!feedback.success,
        "MIDI note delete action should reject audio clips");
    require(feedback.kind == trackloom::AppMidiNoteActionFeedbackKind::IncompatibleClipType,
        "audio clip MIDI note delete action should expose a stable failure kind");
    require(session.project().clips()[0].midiNotes.empty(),
        "audio clip MIDI note delete action should not create or delete notes");
    require(!session.isDirty(),
        "audio clip MIDI note delete action should not dirty an unchanged session");
}

void timelineStatusDescribesEmptyProject()
{
    const trackloom::Project project("Empty Timeline");

    const auto status = trackloom::describeAppTimeline(project);

    require(status.rows.empty(),
        "empty timeline status should not expose phantom clip rows");
    require(status.emptyMessage.find("暂无片段") != std::string::npos,
        "empty timeline status should guide the user to create timeline clips");
}

void timelineStatusDescribesClipRowsWithTrackNames()
{
    trackloom::Project project("Timeline");
    const auto instrument = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(
        instrument.id,
        "Lead MIDI 1",
        trackloom::ClipType::Midi,
        trackloom::Project::ticksPerQuarterNote,
        trackloom::Project::ticksPerQuarterNote * 2);

    require(clip.has_value(),
        "timeline status test should create a MIDI clip");
    require(project.createMidiNote(
                clip->id,
                0,
                trackloom::Project::ticksPerQuarterNote,
                64,
                100,
                1)
            .has_value(),
        "timeline status test should create a MIDI note inside the clip");

    const auto status = trackloom::describeAppTimeline(project);

    require(status.rows.size() == 1,
        "timeline status should expose one row per project clip");
    require(status.rows[0].number == 1 && status.rows[0].clipId == clip->id,
        "timeline status should keep project clip order and clip id");
    require(status.rows[0].trackId == instrument.id && status.rows[0].trackName == "Lead",
        "timeline status should resolve the owning track name");
    require(status.rows[0].typeLabel == "MIDI",
        "timeline status should label MIDI clips clearly");
    require(status.rows[0].startTick == trackloom::Project::ticksPerQuarterNote,
        "timeline status should expose the clip start tick");
    require(status.rows[0].lengthTick == trackloom::Project::ticksPerQuarterNote * 2,
        "timeline status should expose the clip length tick");
    require(status.rows[0].noteCount == 1,
        "timeline status should count MIDI notes inside a MIDI clip");
    require(status.rows[0].summary.find("Lead") != std::string::npos,
        "timeline row summary should include the owning track name");
    require(status.rows[0].summary.find("长度") != std::string::npos,
        "timeline row summary should include the clip length");
}

void timelineStatusDescribesLastMidiNoteDetails()
{
    trackloom::Project project("Timeline Note Details");
    const auto instrument = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(
        instrument.id,
        "Lead MIDI 1",
        trackloom::ClipType::Midi,
        0,
        trackloom::Project::ticksPerQuarterNote * 4);

    require(clip.has_value(),
        "timeline last-note status test should create a MIDI clip");
    require(project.createMidiNote(
                clip->id,
                0,
                trackloom::Project::ticksPerQuarterNote,
                60,
                90,
                1)
            .has_value(),
        "timeline last-note status test should create the first MIDI note");
    require(project.createMidiNote(
                clip->id,
                trackloom::Project::ticksPerQuarterNote,
                trackloom::Project::ticksPerQuarterNote / 2,
                67,
                88,
                1)
            .has_value(),
        "timeline last-note status test should create the final MIDI note");

    const auto status = trackloom::describeAppTimeline(project);

    require(status.rows.size() == 1,
        "timeline last-note status should expose the MIDI clip row");
    require(status.rows[0].hasLastMidiNote,
        "timeline MIDI row should mark that it has last-note details");
    require(status.rows[0].lastMidiNoteStartTick == trackloom::Project::ticksPerQuarterNote,
        "timeline MIDI row should expose the last note start tick");
    require(status.rows[0].lastMidiNoteLengthTick == trackloom::Project::ticksPerQuarterNote / 2,
        "timeline MIDI row should expose the last note length tick");
    require(status.rows[0].lastMidiNoteNumber == 67,
        "timeline MIDI row should expose the last note pitch");
    require(status.rows[0].lastMidiNoteVelocity == 88,
        "timeline MIDI row should expose the last note velocity");
    require(status.rows[0].summary.find("末尾音符") != std::string::npos,
        "timeline MIDI row summary should include a last-note label");
    require(status.rows[0].summary.find("音符起点 960") != std::string::npos,
        "timeline MIDI row summary should include the last note start tick");
    require(status.rows[0].summary.find("音高 67") != std::string::npos,
        "timeline MIDI row summary should include the last note pitch");
    require(status.rows[0].summary.find("力度 88") != std::string::npos,
        "timeline MIDI row summary should include the last note velocity");
}

void timelineStatusOmitsLastMidiNoteDetailsForEmptyMidiClips()
{
    trackloom::Project project("Empty MIDI Details");
    const auto instrument = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(
        instrument.id,
        "Empty MIDI",
        trackloom::ClipType::Midi,
        0,
        trackloom::Project::ticksPerQuarterNote);

    require(clip.has_value(),
        "empty MIDI detail status test should create a MIDI clip");

    const auto status = trackloom::describeAppTimeline(project);

    require(status.rows.size() == 1,
        "empty MIDI detail status should expose the MIDI clip row");
    require(!status.rows[0].hasLastMidiNote,
        "empty MIDI row should not expose last-note details");
    require(status.rows[0].summary.find("末尾音符") == std::string::npos,
        "empty MIDI row summary should not claim a last note exists");
}

void trackListStatusDescribesEmptyProject()
{
    const trackloom::Project project("Empty");

    const auto status = trackloom::describeAppTrackList(project);

    require(status.rows.empty(),
        "empty track list status should not expose phantom rows");
    require(status.emptyMessage.find("暂无轨道") != std::string::npos,
        "empty track list status should guide the user to create a track");
}

void trackListStatusDescribesTrackRowsInProjectOrder()
{
    trackloom::Project project("Tracks");
    const auto instrument = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto audio = project.createTrack("Vocal", trackloom::TrackType::Audio);
    const auto folder = project.createTrack("Group", trackloom::TrackType::Folder);
    const auto clip = project.createClip(
        instrument.id,
        "Lead clip",
        trackloom::ClipType::Midi,
        0,
        trackloom::Project::ticksPerQuarterNote);

    require(clip.has_value(), "track list test should create a MIDI clip on the instrument track");

    const auto status = trackloom::describeAppTrackList(project);

    require(status.rows.size() == 3,
        "track list status should expose one row per project track");
    require(status.rows[0].number == 1 && status.rows[0].trackId == instrument.id,
        "track list status should keep project track order for the first row");
    require(status.rows[0].name == "Lead",
        "track list status should expose the track name");
    require(status.rows[0].typeLabel == "乐器轨",
        "track list status should label instrument tracks in user-facing Chinese");
    require(status.rows[0].clipCount == 1,
        "track list status should count clips that belong to a track");
    require(status.rows[0].summary.find("1 个片段") != std::string::npos,
        "track list row summary should include the clip count");
    require(status.rows[1].trackId == audio.id && status.rows[1].typeLabel == "音频轨",
        "track list status should label audio tracks");
    require(status.rows[2].trackId == folder.id && status.rows[2].typeLabel == "文件夹",
        "track list status should label folder tracks");
}

void trackListStatusDescribesTrackPlaybackAndViewFlags()
{
    trackloom::Project project("Track flags");
    const auto track = project.createTrack("Muted Lead", trackloom::TrackType::Instrument);

    trackloom::TrackPlaybackState playback;
    playback.muted = true;
    playback.soloed = true;
    playback.disabled = true;
    require(project.setTrackPlaybackState(track.id, playback),
        "track list flag test should set playback state");

    trackloom::TrackViewState view;
    view.hidden = true;
    require(project.setTrackViewState(track.id, view),
        "track list flag test should set view state");

    const auto status = trackloom::describeAppTrackList(project);

    require(status.rows.size() == 1,
        "track list flag status should expose the flagged track");
    require(status.rows[0].stateLabels.size() == 4,
        "track list status should expose muted, soloed, disabled and hidden labels");
    require(status.rows[0].summary.find("静音") != std::string::npos,
        "track list summary should include muted state");
    require(status.rows[0].summary.find("独奏") != std::string::npos,
        "track list summary should include solo state");
    require(status.rows[0].summary.find("禁用") != std::string::npos,
        "track list summary should include disabled state");
    require(status.rows[0].summary.find("隐藏") != std::string::npos,
        "track list summary should include hidden state");
}

}

int main()
{
    configureTestFailureOutput();
    missingAudioSettingsUseDocumentedDefaults();
    audioSettingsSaveAndLoadUseTheDocumentedFiveLineFormat();
    audioSettingsSavePreservesUnicodeSettingsPaths();
    invalidAudioSettingsLoadAsDefaultsAndCannotOverwriteSavedSettings();
    audioSettingsCommandAppearsInToolsAndDispatches();
    appInfoExposesStableDesktopIdentity();
    projectPlaybackGenerationTracksOnlyPossibleContentChanges();
    projectPlaybackGenerationTracksCommandsFilesAndIndependentSnapshots();
    projectSessionTracksNewProjectAndDirtyState();
    projectSessionSavesAndOpensProjectFile();
    projectSessionKeepsCurrentProjectWhenOpenFails();
    projectSessionRejectsSaveWithoutPath();
    projectSessionRunsCoreCommandsThroughUndoRedoHistory();
    projectSessionDoesNotDirtyOrRecordFailedCoreCommands();
    projectSessionRejectsNullCoreCommandWithoutMutation();
    projectSessionClearsCommandHistoryWhenCreatingNewProject();
    projectSessionDirectEditClearsCommandHistory();
    projectStatusDescribesUnsavedDirtyProject();
    projectStatusDescribesSavedCleanProject();
    projectFileActionAddsDefaultTrackLoomExtension();
    projectFileActionFeedbackExplainsSaveWithoutPath();
    projectFileActionFeedbackDescribesCanceledOpen();
    projectFileActionFeedbackDescribesSuccessfulSaveAs();
    projectFileActionFeedbackWarnsAfterSuccessfulSave();
    projectFileActionWarningPresentationKeepsCompleteUnicodeDetails();
    projectFileActionOrdinaryPresentationsDoNotRequestWarningDetails();
    projectSessionSaveAsPropagatesRecoveryWarningToFileActionFeedback();
    projectFileActionFeedbackDeduplicatesExactStructuredRecoveryPaths();
    projectSessionNullSaveOperationFallsBackToCoreSave();
    recentProjectsKeepNewestUniquePathsWithinLimit();
    recentProjectsSaveAndLoadUtf8TextFile();
    recentProjectsLoadMissingFileAsEmptyList();
    recentProjectsStatusDescribesEmptyAndStoredProjects();
    recentProjectsRecordAndSaveUpdatesMemoryAndSettingsFile();
    recentProjectsRecordAndSaveKeepsMemoryWhenSettingsCannotSave();
    recentProjectsOpenByNumberLoadsProjectAndPromotesSelection();
    recentProjectsOpenByNumberRejectsDirtySessionWithoutMutation();
    recentProjectsOpenByNumberRejectsMissingSelection();
    recentProjectsOpenByNumberRejectsMissingFileWithoutMutation();
    mainMenuDescribesFileAndPlaybackCommands();
    mainMenuReflectsUndoRedoHistory();
    mainMenuReflectsSelectedTrackCommands();
    mainMenuReflectsSelectedClipCommands();
    mainMenuReflectsPlayingTransportState();
    mainMenuListsRecentProjectsWithStableCommandIds();
    commandPaletteFlattensMenuCommandsWithoutSeparatorsOrInfoRows();
    commandPaletteIncludesRecentProjectsAndFiltersByQuery();
    commandPaletteSelectsFirstEnabledCommandForQuery();
    commandPaletteSelectionSkipsDisabledAndMissingMatches();
    commandPaletteSelectionReportsSelectedResultKind();
    commandPaletteSelectionDistinguishesDisabledMatchesFromMissingMatches();
    commandPaletteActivationExecutesSelectedEnabledCommandThroughDispatcher();
    commandPaletteActivationDoesNotDispatchDisabledOrMissingCommands();
    commandPaletteActivationReportsDispatchFailure();
    commandPaletteAddsShortcutLabelsForVisibleCommands();
    commandPaletteUsesActiveShortcutBindingsForLabels();
    commandPaletteMergesMultipleShortcutLabelsForOneCommand();
    commandPaletteFiltersByShortcutLabel();
    commandPaletteFiltersByMergedShortcutLabel();
    commandPaletteSessionOpensWithFirstEnabledCommandHighlighted();
    commandPaletteSessionUpdatesQueryAndResetsHighlight();
    commandPaletteSessionMovesHighlightAcrossEnabledCommands();
    commandPaletteSessionPagesHighlightAcrossVisibleWindows();
    commandPaletteSessionPageNavigationSkipsDisabledTargetsAndInvalidCounts();
    commandPaletteSessionWheelStepsMoveHighlightWithoutWrapping();
    commandPaletteSessionWheelStepsHandleZeroDisabledOnlyAndClosedStates();
    commandPaletteSessionScrollsVisibleWindowWithoutMovingHighlight();
    commandPaletteSessionScrollWindowResetsWhenQueryOrKeyboardNavigationChangesHighlight();
    commandPaletteSessionHighlightsEnabledCommandByIdForMouseHover();
    commandPaletteSessionHoverHighlightRejectsDisabledMissingAndClosedCommands();
    commandPaletteSessionMovesHighlightToFirstAndLastEnabledCommand();
    commandPaletteSessionBoundaryNavigationKeepsDisabledOnlySearchesUnhighlighted();
    commandPaletteSessionVisibleRowsDescribeWindowSliceForUi();
    commandPaletteSessionVisibleRowsHandleClosedZeroAndDisabledOnlyStates();
    commandPaletteSessionVisibleRowsFormatRangeHintForUi();
    commandPaletteSessionClosesAndClearsState();
    commandPaletteSessionActivationExecutesHighlightedCommand();
    commandPaletteSessionActivationRejectsClosedDisabledOrMissingHandler();
    commandPaletteSessionRowActivationExecutesEnabledVisibleCommand();
    commandPaletteSessionRowActivationRejectsClosedDisabledMissingOrMissingHandler();
    commandPaletteSessionDescriptionMarksRowsForUi();
    commandPaletteSessionDescriptionReportsDisabledAndEmptyStates();
    commandPaletteSessionRowTextFormatsUiLabels();
    commandDispatcherRunsOnlyTheSelectedMainMenuCommand();
    commandDispatcherRunsRedoMainMenuCommand();
    commandDispatcherRunsTrackCreationMenuCommands();
    commandDispatcherRunsSelectedTrackMenuCommands();
    commandDispatcherRunsSelectedClipMenuCommands();
    commandDispatcherRunsCommandPaletteMenuCommand();
    commandDispatcherRunsShortcutStatusMenuCommand();
    commandDispatcherRejectsShortcutStatusWithoutHandler();
    commandDispatcherPassesRecentProjectNumber();
    commandDispatcherRejectsUnknownOrUnboundCommands();
    commandShortcutsMapCommonFileKeysToMenuCommands();
    commandShortcutsMapUndoRedoKeysToEditMenuCommands();
    commandShortcutsMapCommandPaletteKeysToToolCommand();
    commandShortcutsIgnoreUnregisteredOrAmbiguousChords();
    commandShortcutsAreSuppressedWhileCommandPaletteIsOpen();
    commandShortcutsUseCustomBindingsWithContext();
    commandShortcutCustomizationReplacesDefaultBinding();
    commandShortcutCustomizationRejectsConflictingBinding();
    commandShortcutCustomizationAddsBindingForCommandWithoutDefault();
    shortcutSettingsSaveAndLoadCustomBindings();
    shortcutSettingsLoadMissingFileAsEmptyBindings();
    shortcutSettingsReportsLoadedConflictsThroughCustomization();
    shortcutSettingsIgnoreMalformedOrUnsupportedRows();
    shortcutStatusDescribesActiveRowsAndSummary();
    shortcutStatusReportsConflictsWithMenuLabels();
    trackActionCreatesDefaultInstrumentTrackAndMarksSessionDirty();
    trackActionCreateCanBeUndoneAndRedoneThroughSessionHistory();
    trackActionNamesRepeatedDefaultInstrumentTracksByProjectOrder();
    trackActionCreatesDefaultAudioTrackAndMarksSessionDirty();
    trackActionNamesRepeatedDefaultAudioTracksByProjectOrder();
    trackActionDeletesAudioTrackAndOwnedClips();
    trackActionRejectsMissingAudioTrackDeleteWithoutDirtyingSession();
    trackActionRejectsNonAudioTrackDeleteWithoutDirtyingSession();
    trackActionCreatesDefaultFolderTrackAndMarksSessionDirty();
    trackActionNamesRepeatedDefaultFolderTracksByProjectOrder();
    trackActionDeletesInstrumentTrackAndOwnedClips();
    trackActionRejectsMissingTrackDeleteWithoutDirtyingSession();
    trackActionRejectsNonInstrumentTrackDeleteWithoutDirtyingSession();
    trackActionRenamesTrackAndMarksSessionDirty();
    trackActionRejectsEmptyTrackNameWithoutDirtyingSession();
    trackActionRejectsMissingTrackRenameWithoutDirtyingSession();
    trackActionMovesInstrumentTrackUpAndDown();
    trackActionMovesAudioTrackUpAndDown();
    trackActionRejectsMoveAtBoundariesWithoutDirtyingSession();
    trackActionRejectsInvalidMoveTargetsWithoutDirtyingSession();
    trackStateActionTogglesMuteAndMarksSessionDirty();
    trackStateActionMuteCanBeUndoneAndRedoneThroughSessionHistory();
    trackStateActionTogglesPlaybackFlagsIndependently();
    trackStateActionTogglesHiddenWithoutAffectingPlayback();
    trackStateActionHiddenCanBeUndoneAndRedoneThroughSessionHistory();
    trackStateActionRejectsMissingTrackWithoutDirtyingSession();
    trackStateActionUpdatesTrackListStatusLabels();
    playbackPreparationIsQueryableAndCancellableWithoutJoiningInHandlers();
    playbackWorkerBuildPublishesThroughIndependentCompletionState();
    playbackPreparationInstallsAndTracksHostLifecycle();
    playbackStatusAndMenuUseStructuredControllerState();
    playbackUnavailableHostAndFaultExposeStableReasons();
    playbackDropsStaleProjectAndDevicePreparationWithoutInstalling();
    playbackPreparationAndHostFailuresUseStableReasons();
    playbackHostFaultDuringPreparationDiscardsCompletedPlan();
    playbackDeviceFaultDoesNotRecoverUntilItsPreparationWorkerJoins();
    playbackTestToneAndActivePreparationDisableSecondStart();
    playbackKeepsInstalledPlanDuringEditsAndRebuildsAfterStop();
    playbackRewindStopsThenPreparesAgainFromZero();
    playbackPreparationUsesCurrentStoppedHostSampleAndRequestedLoop();
    playbackCancelledPreparationCannotReplaceNewStartAndLoopIntent();
    audioClipActionCreatesDefaultClipOnAudioTrack();
    audioClipActionCreateCanBeUndoneAndRedoneThroughSessionHistory();
    audioClipActionAppendsAfterExistingTrackClips();
    audioClipActionDuplicatesAudioClipAfterItself();
    audioClipActionDuplicateCanBeUndoneAndRedoneThroughSessionHistory();
    audioClipActionSplitsAudioClipAtMidpoint();
    audioClipActionSplitCanBeUndoneAndRedoneThroughSessionHistory();
    audioClipActionMovesAudioClipToAudioTrack();
    audioClipActionMoveToTrackCanBeUndoneAndRedoneThroughSessionHistory();
    audioClipActionRejectsMissingTrackWithoutDirtyingSession();
    audioClipActionRejectsIncompatibleTrackWithoutDirtyingSession();
    audioClipActionDeletesAudioClip();
    audioClipActionDeleteCanBeUndoneAndRedoneThroughSessionHistory();
    audioClipActionRejectsMissingClipDeleteWithoutDirtyingSession();
    audioClipActionRejectsMidiClipDeleteWithoutDirtyingSession();
    audioClipActionRejectsMissingClipDuplicateWithoutDirtyingSession();
    audioClipActionRejectsMidiClipDuplicateWithoutDirtyingSession();
    audioClipActionRejectsMissingClipSplitWithoutDirtyingSession();
    audioClipActionRejectsMidiClipSplitWithoutDirtyingSession();
    audioClipActionRejectsTooShortClipSplitWithoutDirtyingSession();
    audioClipActionRejectsSameTrackMoveWithoutDirtyingSession();
    audioClipActionRejectsMissingClipTrackMoveWithoutDirtyingSession();
    audioClipActionRejectsMissingTargetTrackMoveWithoutDirtyingSession();
    audioClipActionRejectsInstrumentTargetTrackMoveWithoutDirtyingSession();
    audioClipActionRejectsMidiClipTrackMoveWithoutDirtyingSession();
    audioClipActionRenamesAudioClipAndMarksSessionDirty();
    audioClipActionRenameCanBeUndoneAndRedoneThroughSessionHistory();
    audioClipActionRejectsEmptyClipNameWithoutDirtyingSession();
    audioClipActionRejectsMissingClipRenameWithoutDirtyingSession();
    audioClipActionRejectsMidiClipRenameWithoutDirtyingSession();
    audioClipActionMovesAudioClipRightOneBeat();
    audioClipActionMovesAudioClipLeftOneBeat();
    audioClipActionMoveCanBeUndoneAndRedoneThroughSessionHistory();
    audioClipActionRejectsLeftMoveBeforeTimelineStartWithoutDirtyingSession();
    audioClipActionRejectsMissingClipMoveWithoutDirtyingSession();
    audioClipActionRejectsMidiClipMoveWithoutDirtyingSession();
    audioClipActionTrimsAudioClipEndEarlierOneBeat();
    audioClipActionTrimEndCanBeUndoneAndRedoneThroughSessionHistory();
    audioClipActionExtendsAudioClipEndLaterOneBeat();
    audioClipActionExtendEndCanBeUndoneAndRedoneThroughSessionHistory();
    audioClipActionTrimsAudioClipStartLaterOneBeat();
    audioClipActionTrimStartCanBeUndoneAndRedoneThroughSessionHistory();
    audioClipActionExtendsAudioClipStartEarlierOneBeat();
    audioClipActionExtendStartCanBeUndoneAndRedoneThroughSessionHistory();
    audioClipActionRejectsTooShortClipEndTrimWithoutDirtyingSession();
    audioClipActionRejectsMissingClipEndTrimWithoutDirtyingSession();
    audioClipActionRejectsMidiClipEndTrimWithoutDirtyingSession();
    audioClipActionRejectsMissingClipEndExtendWithoutDirtyingSession();
    audioClipActionRejectsMidiClipEndExtendWithoutDirtyingSession();
    audioClipActionRejectsTooShortClipStartTrimWithoutDirtyingSession();
    audioClipActionRejectsStartExtendBeforeTimelineStartWithoutDirtyingSession();
    audioClipActionRejectsMissingClipStartTrimWithoutDirtyingSession();
    audioClipActionRejectsMidiClipStartTrimWithoutDirtyingSession();
    audioClipActionRejectsMissingClipStartExtendWithoutDirtyingSession();
    audioClipActionRejectsMidiClipStartExtendWithoutDirtyingSession();
    midiClipActionCreatesDefaultClipOnInstrumentTrack();
    midiClipActionCreateCanBeUndoneAndRedoneThroughSessionHistory();
    midiClipActionAppendsAfterExistingTrackClips();
    midiClipActionDuplicatesMidiClipAfterItself();
    midiClipActionDuplicateCanBeUndoneAndRedoneThroughSessionHistory();
    midiClipActionRenamesMidiClipAndMarksSessionDirty();
    midiClipActionRenameCanBeUndoneAndRedoneThroughSessionHistory();
    midiClipActionSplitsMidiClipAtMidpoint();
    midiClipActionSplitCanBeUndoneAndRedoneThroughSessionHistory();
    midiClipActionMovesMidiClipRightOneBeat();
    midiClipActionMoveCanBeUndoneAndRedoneThroughSessionHistory();
    midiClipActionMovesMidiClipLeftOneBeat();
    midiClipActionMovesMidiClipToInstrumentTrack();
    midiClipActionMoveToTrackCanBeUndoneAndRedoneThroughSessionHistory();
    midiClipActionTrimsMidiClipEndEarlierOneBeat();
    midiClipActionTrimEndCanBeUndoneAndRedoneThroughSessionHistory();
    midiClipActionExtendsMidiClipEndLaterOneBeat();
    midiClipActionExtendEndCanBeUndoneAndRedoneThroughSessionHistory();
    midiClipActionTrimsMidiClipStartLaterOneBeat();
    midiClipActionExtendsMidiClipStartEarlierOneBeat();
    midiClipActionTrimStartCanBeUndoneAndRedoneThroughSessionHistory();
    midiClipActionExtendStartCanBeUndoneAndRedoneThroughSessionHistory();
    midiClipActionRejectsEmptyClipNameWithoutDirtyingSession();
    midiClipActionRejectsMissingTrackWithoutDirtyingSession();
    midiClipActionRejectsIncompatibleTrackWithoutDirtyingSession();
    midiClipActionRejectsMissingClipDuplicateWithoutDirtyingSession();
    midiClipActionRejectsAudioClipDuplicateWithoutDirtyingSession();
    midiClipActionRejectsMissingClipRenameWithoutDirtyingSession();
    midiClipActionRejectsAudioClipRenameWithoutDirtyingSession();
    midiClipActionRejectsMissingClipSplitWithoutDirtyingSession();
    midiClipActionRejectsAudioClipSplitWithoutDirtyingSession();
    midiClipActionRejectsTooShortMidiClipSplitWithoutDirtyingSession();
    midiClipActionRejectsCrossingNoteSplitWithoutDirtyingSession();
    midiClipActionRejectsLeftMoveBeforeTimelineStartWithoutDirtyingSession();
    midiClipActionRejectsMissingClipMoveWithoutDirtyingSession();
    midiClipActionRejectsAudioClipMoveWithoutDirtyingSession();
    midiClipActionRejectsSameTrackMoveWithoutDirtyingSession();
    midiClipActionRejectsMissingTargetTrackMoveWithoutDirtyingSession();
    midiClipActionRejectsAudioTargetTrackMoveWithoutDirtyingSession();
    midiClipActionRejectsAudioClipTrackMoveWithoutDirtyingSession();
    midiClipActionRejectsTooShortClipEndTrimWithoutDirtyingSession();
    midiClipActionRejectsMissingClipEndTrimWithoutDirtyingSession();
    midiClipActionRejectsAudioClipEndTrimWithoutDirtyingSession();
    midiClipActionRejectsClipEndTrimThatWouldDropNotesWithoutDirtyingSession();
    midiClipActionRejectsClipStartTrimThatWouldDropNotesWithoutDirtyingSession();
    midiClipActionRejectsMissingClipEndExtendWithoutDirtyingSession();
    midiClipActionRejectsAudioClipEndExtendWithoutDirtyingSession();
    midiClipActionDeletesMidiClipAndItsNotes();
    midiClipActionDeleteCanBeUndoneAndRedoneThroughSessionHistory();
    midiClipActionRejectsMissingClipDeleteWithoutDirtyingSession();
    midiClipActionRejectsAudioClipDeleteWithoutDirtyingSession();
    midiNoteActionCreatesDefaultNoteInMidiClip();
    midiNoteActionAppendsAfterExistingNotes();
    midiNoteActionCreateCanBeUndoneAndRedoneThroughSessionHistory();
    midiNoteActionDuplicatesLastNoteAfterItself();
    midiNoteActionDuplicateCanBeUndoneAndRedoneThroughSessionHistory();
    midiNoteActionRejectsDuplicateBeyondClipWithoutDirtyingSession();
    midiNoteActionRejectsEmptyClipDuplicateWithoutDirtyingSession();
    midiNoteActionRejectsMissingClipDuplicateWithoutDirtyingSession();
    midiNoteActionRejectsAudioClipDuplicateWithoutDirtyingSession();
    midiNoteActionRejectsMissingClipWithoutDirtyingSession();
    midiNoteActionRejectsAudioClipWithoutDirtyingSession();
    midiNoteActionRejectsFullClipWithoutDirtyingSession();
    midiNoteActionDeletesLastNoteInMidiClip();
    midiNoteActionDeleteCanBeUndoneAndRedoneThroughSessionHistory();
    midiNoteActionRaisesLastNotePitchOneSemitone();
    midiNoteActionLowersLastNotePitchOneSemitone();
    midiNoteActionPitchCanBeUndoneAndRedoneThroughSessionHistory();
    midiNoteActionRejectsPitchRaiseAboveMidiRangeWithoutDirtyingSession();
    midiNoteActionRejectsPitchLowerBelowMidiRangeWithoutDirtyingSession();
    midiNoteActionRejectsEmptyClipPitchWithoutDirtyingSession();
    midiNoteActionIncreasesLastNoteVelocityByStep();
    midiNoteActionDecreasesLastNoteVelocityByStep();
    midiNoteActionVelocityCanBeUndoneAndRedoneThroughSessionHistory();
    midiNoteActionRejectsVelocityIncreaseAboveMidiRangeWithoutDirtyingSession();
    midiNoteActionRejectsVelocityDecreaseBelowMidiRangeWithoutDirtyingSession();
    midiNoteActionRejectsEmptyClipVelocityWithoutDirtyingSession();
    midiNoteActionRejectsMissingClipVelocityWithoutDirtyingSession();
    midiNoteActionRejectsAudioClipVelocityWithoutDirtyingSession();
    midiNoteActionLengthensLastNoteByStep();
    midiNoteActionShortensLastNoteByStep();
    midiNoteActionLengthCanBeUndoneAndRedoneThroughSessionHistory();
    midiNoteActionRejectsLengthenBeyondClipWithoutDirtyingSession();
    midiNoteActionRejectsShortenBelowMinimumWithoutDirtyingSession();
    midiNoteActionRejectsEmptyClipLengthWithoutDirtyingSession();
    midiNoteActionRejectsMissingClipLengthWithoutDirtyingSession();
    midiNoteActionRejectsAudioClipLengthWithoutDirtyingSession();
    midiNoteActionMovesLastNoteStartEarlierByStep();
    midiNoteActionMovesLastNoteStartLaterByStep();
    midiNoteActionStartCanBeUndoneAndRedoneThroughSessionHistory();
    midiNoteActionRejectsMoveEarlierBeforeClipStartWithoutDirtyingSession();
    midiNoteActionRejectsMoveLaterBeyondClipEndWithoutDirtyingSession();
    midiNoteActionRejectsEmptyClipTimingWithoutDirtyingSession();
    midiNoteActionRejectsMissingClipTimingWithoutDirtyingSession();
    midiNoteActionRejectsAudioClipTimingWithoutDirtyingSession();
    midiNoteActionRejectsEmptyClipDeleteWithoutDirtyingSession();
    midiNoteActionRejectsMissingClipDeleteWithoutDirtyingSession();
    midiNoteActionRejectsAudioClipDeleteWithoutDirtyingSession();
    timelineStatusDescribesEmptyProject();
    timelineStatusDescribesClipRowsWithTrackNames();
    timelineStatusDescribesLastMidiNoteDetails();
    timelineStatusOmitsLastMidiNoteDetailsForEmptyMidiClips();
    trackListStatusDescribesEmptyProject();
    trackListStatusDescribesTrackRowsInProjectOrder();
    trackListStatusDescribesTrackPlaybackAndViewFlags();
    return 0;
}
