#include "AppProjectFileActions.h"
#include "AppRecentProjects.h"
#include "AppMidiClipActions.h"
#include "AppMidiNoteActions.h"
#include "AppPlaybackActions.h"
#include "AppProjectSession.h"
#include "AppProjectStatus.h"
#include "AppTimelineStatus.h"
#include "AppTrackActions.h"
#include "AppTrackListStatus.h"
#include "TrackLoomAppInfo.h"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
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

std::filesystem::path testWorkspace()
{
    return std::filesystem::temp_directory_path() / "trackloom_app_session_tests";
}

void removeTestWorkspace()
{
    std::error_code ignoredError;
    std::filesystem::remove_all(testWorkspace(), ignoredError);
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

    // 编号 2 打开当前列表里的第二个工程；打开后它应移动到最近列表最前。
    const auto feedback = trackloom::openAppRecentProjectByNumber(
        session,
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

    const auto feedback = trackloom::openAppRecentProjectByNumber(
        session,
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

    const auto feedback = trackloom::openAppRecentProjectByNumber(
        session,
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

    const auto feedback = trackloom::openAppRecentProjectByNumber(
        session,
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

void playbackActionStartsTransportWithoutDirtyingProject()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "playback-start-action.trackloom-test";

    trackloom::AppProjectSession session;
    trackloom::AppPlaybackController playback;
    session.createNewProject("Playback Start");
    require(session.saveAs(path).success,
        "playback start action test should save setup edits before runtime control");

    const auto feedback = trackloom::startAppPlayback(playback, session.project());

    require(feedback.success,
        "playback action should start the app transport");
    require(feedback.kind == trackloom::AppPlaybackActionFeedbackKind::Success,
        "successful playback start action should expose a stable success kind");
    require(playback.isPrepared(),
        "playback start action should prepare the runtime playback session");
    require(playback.isPlaying(),
        "playback start action should put the app transport into playing state");
    require(playback.currentSample() == 0,
        "playback start action should not move the playback position by itself");
    require(!session.isDirty(),
        "playback start action should not dirty the project session");
    require(feedback.message.find("播放") != std::string::npos,
        "successful playback start feedback should describe playback");
}

void playbackActionStopsTransportWithoutDirtyingProject()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "playback-stop-action.trackloom-test";

    trackloom::AppProjectSession session;
    trackloom::AppPlaybackController playback;
    session.createNewProject("Playback Stop");
    require(session.saveAs(path).success,
        "playback stop action test should save setup edits before runtime control");
    require(trackloom::startAppPlayback(playback, session.project()).success,
        "playback stop action test should start playback before stopping");

    const auto feedback = trackloom::stopAppPlayback(playback, session.project());

    require(feedback.success,
        "playback action should stop the app transport");
    require(feedback.kind == trackloom::AppPlaybackActionFeedbackKind::Success,
        "successful playback stop action should expose a stable success kind");
    require(playback.isPrepared(),
        "playback stop action should keep the runtime playback session prepared");
    require(!playback.isPlaying(),
        "playback stop action should put the app transport into stopped state");
    require(!session.isDirty(),
        "playback stop action should not dirty the project session");
    require(feedback.message.find("停止") != std::string::npos,
        "successful playback stop feedback should describe the stop action");
}

void playbackActionStopsCleanlyBeforeStart()
{
    trackloom::AppProjectSession session;
    trackloom::AppPlaybackController playback;
    session.createNewProject("Stop Before Start");

    const auto feedback = trackloom::stopAppPlayback(playback, session.project());

    require(feedback.success,
        "playback stop action should accept an already stopped runtime");
    require(playback.isPrepared(),
        "playback stop action should prepare runtime state before using the core safe stop command");
    require(!playback.isPlaying(),
        "playback stop action should leave the app transport stopped");
    require(!session.isDirty(),
        "playback stop action before start should not dirty the project session");
}

void playbackToggleStartsStoppedTransportWithoutDirtyingProject()
{
    // 从停止态切换到播放态时，只应改变运行态播放控制，不能把工程标记为已修改。
    removeTestWorkspace();
    const auto path = testWorkspace() / "playback-toggle-start.trackloom-test";

    trackloom::AppProjectSession session;
    trackloom::AppPlaybackController playback;
    session.createNewProject("Toggle Start");
    require(session.saveAs(path).success,
        "toggle start test should save setup edits before runtime control");

    const auto feedback = trackloom::toggleAppPlayback(playback, session.project());

    require(feedback.success,
        "toggle playback should start a stopped runtime");
    require(feedback.kind == trackloom::AppPlaybackActionFeedbackKind::Success,
        "successful toggle start should expose the stable success kind");
    require(playback.isPrepared(),
        "toggle playback from stopped should prepare playback runtime");
    require(playback.isPlaying(),
        "toggle playback from stopped should start playback");
    require(playback.currentSample() == 0,
        "toggle playback from stopped should not move the playback position by itself");
    require(!session.isDirty(),
        "toggle playback from stopped should not dirty the project session");
}

void playbackToggleStopsPlayingTransportWithoutDirtyingProject()
{
    // 从播放态切换到停止态时，必须复用标准停止路径，并保持工程 dirty 状态不变。
    removeTestWorkspace();
    const auto path = testWorkspace() / "playback-toggle-stop.trackloom-test";

    trackloom::AppProjectSession session;
    trackloom::AppPlaybackController playback;
    session.createNewProject("Toggle Stop");
    require(session.saveAs(path).success,
        "toggle stop test should save setup edits before runtime control");
    require(trackloom::startAppPlayback(playback, session.project()).success,
        "toggle stop test should start playback before toggling");

    const auto feedback = trackloom::toggleAppPlayback(playback, session.project());

    require(feedback.success,
        "toggle playback should stop a playing runtime");
    require(feedback.kind == trackloom::AppPlaybackActionFeedbackKind::Success,
        "successful toggle stop should expose the stable success kind");
    require(playback.isPrepared(),
        "toggle playback from playing should keep playback runtime prepared");
    require(!playback.isPlaying(),
        "toggle playback from playing should stop playback");
    require(!session.isDirty(),
        "toggle playback from playing should not dirty the project session");
}

void playbackToggleStopsAfterUiAdvanceAndKeepsPosition()
{
    // 播放头已经推进后再停止，不应偷偷回到开头；回到开头由单独的 rewind 动作负责。
    trackloom::AppProjectSession session;
    trackloom::AppPlaybackController playback;
    session.createNewProject("Toggle After Advance");
    require(trackloom::startAppPlayback(playback, session.project()).success,
        "toggle after advance test should start playback before advancing");
    require(trackloom::advanceAppPlaybackForUiTick(playback, session.project()).success,
        "toggle after advance test should advance playback before toggling");

    const auto sampleBeforeToggle = playback.currentSample();
    const auto feedback = trackloom::toggleAppPlayback(playback, session.project());

    require(feedback.success,
        "toggle playback should stop after playback has advanced");
    require(!playback.isPlaying(),
        "toggle playback after advance should stop playback");
    require(playback.currentSample() == sampleBeforeToggle,
        "toggle playback after advance should keep the current playback position");
    require(!session.isDirty(),
        "toggle playback after advance should not dirty the project session");
}

void playbackStatusDescribesStoppedAndPlayingStates()
{
    trackloom::AppProjectSession session;
    trackloom::AppPlaybackController playback;
    session.createNewProject("Playback Status");

    const auto stopped = trackloom::describeAppPlayback(playback);
    require(!stopped.playing,
        "fresh playback status should start stopped");
    require(stopped.stateLabel == "已停止",
        "fresh playback status should expose a stopped label");

    require(trackloom::startAppPlayback(playback, session.project()).success,
        "playback status test should start playback");
    const auto playing = trackloom::describeAppPlayback(playback);

    require(playing.prepared,
        "playing status should report prepared runtime state");
    require(playing.playing,
        "playing status should report playing runtime state");
    require(playing.currentSample == 0,
        "playing status should expose the current sample");
    require(playing.stateLabel == "播放中",
        "playing status should expose a playing label");
    require(playing.summary.find("播放中") != std::string::npos,
        "playing status summary should include the visible playback state");
}

void playbackUiTickAdvancesPlayingTransportWithoutDirtyingProject()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "playback-ui-tick.trackloom-test";

    trackloom::AppProjectSession session;
    trackloom::AppPlaybackController playback;
    session.createNewProject("Playback UI Tick");
    require(session.saveAs(path).success,
        "playback UI tick test should save setup edits before runtime control");
    require(trackloom::startAppPlayback(playback, session.project()).success,
        "playback UI tick test should start playback before advancing");

    const auto feedback = trackloom::advanceAppPlaybackForUiTick(playback, session.project());

    require(feedback.success,
        "playback UI tick should advance a playing runtime");
    require(feedback.kind == trackloom::AppPlaybackActionFeedbackKind::Success,
        "successful playback UI tick should expose a stable success kind");
    require(playback.currentSample() == trackloom::defaultAppPlaybackUiBlockFrames,
        "playback UI tick should advance the transport by one app UI block");
    require(playback.currentSeconds() > 0.0,
        "playback UI tick should make the visible playback seconds advance");
    require(!session.isDirty(),
        "playback UI tick should not dirty the project session");

    const auto status = trackloom::describeAppPlayback(playback);
    require(status.currentSample == trackloom::defaultAppPlaybackUiBlockFrames,
        "playback status should report the advanced sample position");
    require(status.summary.find("秒") != std::string::npos,
        "playback status summary should include a human-readable seconds value");
}

void playbackUiTickSkipsStoppedTransportWithoutPreparingRuntime()
{
    trackloom::AppProjectSession session;
    trackloom::AppPlaybackController playback;
    session.createNewProject("Stopped UI Tick");

    const auto feedback = trackloom::advanceAppPlaybackForUiTick(playback, session.project());

    require(feedback.success,
        "playback UI tick should treat stopped playback as a harmless no-op");
    require(feedback.kind == trackloom::AppPlaybackActionFeedbackKind::NoOp,
        "stopped playback UI tick should expose a stable no-op kind");
    require(!playback.isPrepared(),
        "stopped playback UI tick should not prepare playback until the user starts playback");
    require(playback.currentSample() == 0,
        "stopped playback UI tick should keep the playback position unchanged");
    require(!session.isDirty(),
        "stopped playback UI tick should not dirty the project session");
}

void playbackUiTickSkipsAfterStopAndKeepsPosition()
{
    trackloom::AppProjectSession session;
    trackloom::AppPlaybackController playback;
    session.createNewProject("Tick After Stop");
    require(trackloom::startAppPlayback(playback, session.project()).success,
        "tick after stop test should start playback before advancing");
    require(trackloom::advanceAppPlaybackForUiTick(playback, session.project()).success,
        "tick after stop test should advance once before stopping");
    require(trackloom::stopAppPlayback(playback, session.project()).success,
        "tick after stop test should stop playback before the no-op tick");

    const auto sampleAfterStop = playback.currentSample();
    const auto feedback = trackloom::advanceAppPlaybackForUiTick(playback, session.project());

    require(feedback.success,
        "playback UI tick after stop should be a harmless no-op");
    require(feedback.kind == trackloom::AppPlaybackActionFeedbackKind::NoOp,
        "playback UI tick after stop should expose a stable no-op kind");
    require(playback.currentSample() == sampleAfterStop,
        "playback UI tick after stop should keep the stopped position");
}

void playbackRewindReturnsPlayingTransportToStartWithoutDirtyingProject()
{
    removeTestWorkspace();
    const auto path = testWorkspace() / "playback-rewind-playing.trackloom-test";

    trackloom::AppProjectSession session;
    trackloom::AppPlaybackController playback;
    session.createNewProject("Rewind Playing");
    require(session.saveAs(path).success,
        "playing rewind test should save setup edits before runtime control");
    require(trackloom::startAppPlayback(playback, session.project()).success,
        "playing rewind test should start playback before advancing");
    require(trackloom::advanceAppPlaybackForUiTick(playback, session.project()).success,
        "playing rewind test should advance playback before rewinding");

    const auto feedback = trackloom::rewindAppPlaybackToStart(playback, session.project());

    require(feedback.success,
        "rewind action should move a playing transport back to the beginning");
    require(feedback.kind == trackloom::AppPlaybackActionFeedbackKind::Success,
        "successful rewind action should expose a stable success kind");
    require(playback.isPlaying(),
        "rewind while playing should keep playback running");
    require(playback.currentSample() == 0,
        "rewind while playing should move the playback position to sample zero");
    require(!session.isDirty(),
        "rewind while playing should not dirty the project session");
    require(feedback.message.find("开头") != std::string::npos,
        "successful rewind feedback should describe returning to the beginning");
}

void playbackRewindReturnsStoppedTransportToStartWithoutDirtyingProject()
{
    trackloom::AppProjectSession session;
    trackloom::AppPlaybackController playback;
    session.createNewProject("Rewind Stopped");
    require(trackloom::startAppPlayback(playback, session.project()).success,
        "stopped rewind test should start playback before advancing");
    require(trackloom::advanceAppPlaybackForUiTick(playback, session.project()).success,
        "stopped rewind test should advance playback before stopping");
    require(trackloom::stopAppPlayback(playback, session.project()).success,
        "stopped rewind test should stop playback before rewinding");

    const auto feedback = trackloom::rewindAppPlaybackToStart(playback, session.project());

    require(feedback.success,
        "rewind action should move a stopped transport back to the beginning");
    require(!playback.isPlaying(),
        "rewind while stopped should keep playback stopped");
    require(playback.currentSample() == 0,
        "rewind while stopped should move the playback position to sample zero");
    require(!session.isDirty(),
        "rewind while stopped should not dirty the project session");
}

void playbackRewindPreparesFreshRuntimeWithoutStartingPlayback()
{
    trackloom::AppProjectSession session;
    trackloom::AppPlaybackController playback;
    session.createNewProject("Fresh Rewind");

    const auto feedback = trackloom::rewindAppPlaybackToStart(playback, session.project());

    require(feedback.success,
        "rewind action should prepare a fresh runtime so safe seek can run");
    require(playback.isPrepared(),
        "rewind action on a fresh runtime should prepare playback state");
    require(!playback.isPlaying(),
        "rewind action on a fresh runtime should not start playback");
    require(playback.currentSample() == 0,
        "rewind action on a fresh runtime should keep the playback position at sample zero");
    require(!session.isDirty(),
        "rewind action on a fresh runtime should not dirty the project session");
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
    require(status.emptyMessage.find("暂无 MIDI 片段") != std::string::npos,
        "empty timeline status should guide the user to create MIDI clips");
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
    appInfoExposesStableDesktopIdentity();
    projectSessionTracksNewProjectAndDirtyState();
    projectSessionSavesAndOpensProjectFile();
    projectSessionKeepsCurrentProjectWhenOpenFails();
    projectSessionRejectsSaveWithoutPath();
    projectStatusDescribesUnsavedDirtyProject();
    projectStatusDescribesSavedCleanProject();
    projectFileActionAddsDefaultTrackLoomExtension();
    projectFileActionFeedbackExplainsSaveWithoutPath();
    projectFileActionFeedbackDescribesCanceledOpen();
    projectFileActionFeedbackDescribesSuccessfulSaveAs();
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
    trackActionCreatesDefaultInstrumentTrackAndMarksSessionDirty();
    trackActionNamesRepeatedDefaultInstrumentTracksByProjectOrder();
    trackActionDeletesInstrumentTrackAndOwnedClips();
    trackActionRejectsMissingTrackDeleteWithoutDirtyingSession();
    trackActionRejectsNonInstrumentTrackDeleteWithoutDirtyingSession();
    playbackActionStartsTransportWithoutDirtyingProject();
    playbackActionStopsTransportWithoutDirtyingProject();
    playbackActionStopsCleanlyBeforeStart();
    playbackToggleStartsStoppedTransportWithoutDirtyingProject();
    playbackToggleStopsPlayingTransportWithoutDirtyingProject();
    playbackToggleStopsAfterUiAdvanceAndKeepsPosition();
    playbackStatusDescribesStoppedAndPlayingStates();
    playbackUiTickAdvancesPlayingTransportWithoutDirtyingProject();
    playbackUiTickSkipsStoppedTransportWithoutPreparingRuntime();
    playbackUiTickSkipsAfterStopAndKeepsPosition();
    playbackRewindReturnsPlayingTransportToStartWithoutDirtyingProject();
    playbackRewindReturnsStoppedTransportToStartWithoutDirtyingProject();
    playbackRewindPreparesFreshRuntimeWithoutStartingPlayback();
    midiClipActionCreatesDefaultClipOnInstrumentTrack();
    midiClipActionAppendsAfterExistingTrackClips();
    midiClipActionRejectsMissingTrackWithoutDirtyingSession();
    midiClipActionRejectsIncompatibleTrackWithoutDirtyingSession();
    midiClipActionDeletesMidiClipAndItsNotes();
    midiClipActionRejectsMissingClipDeleteWithoutDirtyingSession();
    midiClipActionRejectsAudioClipDeleteWithoutDirtyingSession();
    midiNoteActionCreatesDefaultNoteInMidiClip();
    midiNoteActionAppendsAfterExistingNotes();
    midiNoteActionRejectsMissingClipWithoutDirtyingSession();
    midiNoteActionRejectsAudioClipWithoutDirtyingSession();
    midiNoteActionRejectsFullClipWithoutDirtyingSession();
    midiNoteActionDeletesLastNoteInMidiClip();
    midiNoteActionRejectsEmptyClipDeleteWithoutDirtyingSession();
    midiNoteActionRejectsMissingClipDeleteWithoutDirtyingSession();
    midiNoteActionRejectsAudioClipDeleteWithoutDirtyingSession();
    timelineStatusDescribesEmptyProject();
    timelineStatusDescribesClipRowsWithTrackNames();
    trackListStatusDescribesEmptyProject();
    trackListStatusDescribesTrackRowsInProjectOrder();
    trackListStatusDescribesTrackPlaybackAndViewFlags();
    return 0;
}
