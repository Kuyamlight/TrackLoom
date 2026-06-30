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
#include "AppTrackStateActions.h"
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
    trackActionCreatesDefaultAudioTrackAndMarksSessionDirty();
    trackActionNamesRepeatedDefaultAudioTracksByProjectOrder();
    trackActionCreatesDefaultFolderTrackAndMarksSessionDirty();
    trackActionNamesRepeatedDefaultFolderTracksByProjectOrder();
    trackActionDeletesInstrumentTrackAndOwnedClips();
    trackActionRejectsMissingTrackDeleteWithoutDirtyingSession();
    trackActionRejectsNonInstrumentTrackDeleteWithoutDirtyingSession();
    trackActionRenamesTrackAndMarksSessionDirty();
    trackActionRejectsEmptyTrackNameWithoutDirtyingSession();
    trackActionRejectsMissingTrackRenameWithoutDirtyingSession();
    trackActionMovesInstrumentTrackUpAndDown();
    trackActionRejectsMoveAtBoundariesWithoutDirtyingSession();
    trackActionRejectsInvalidMoveTargetsWithoutDirtyingSession();
    trackStateActionTogglesMuteAndMarksSessionDirty();
    trackStateActionTogglesPlaybackFlagsIndependently();
    trackStateActionTogglesHiddenWithoutAffectingPlayback();
    trackStateActionRejectsMissingTrackWithoutDirtyingSession();
    trackStateActionUpdatesTrackListStatusLabels();
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
    midiClipActionDuplicatesMidiClipAfterItself();
    midiClipActionRenamesMidiClipAndMarksSessionDirty();
    midiClipActionSplitsMidiClipAtMidpoint();
    midiClipActionMovesMidiClipRightOneBeat();
    midiClipActionMovesMidiClipLeftOneBeat();
    midiClipActionMovesMidiClipToInstrumentTrack();
    midiClipActionTrimsMidiClipEndEarlierOneBeat();
    midiClipActionExtendsMidiClipEndLaterOneBeat();
    midiClipActionTrimsMidiClipStartLaterOneBeat();
    midiClipActionExtendsMidiClipStartEarlierOneBeat();
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
    midiClipActionRejectsMissingClipDeleteWithoutDirtyingSession();
    midiClipActionRejectsAudioClipDeleteWithoutDirtyingSession();
    midiNoteActionCreatesDefaultNoteInMidiClip();
    midiNoteActionAppendsAfterExistingNotes();
    midiNoteActionRejectsMissingClipWithoutDirtyingSession();
    midiNoteActionRejectsAudioClipWithoutDirtyingSession();
    midiNoteActionRejectsFullClipWithoutDirtyingSession();
    midiNoteActionDeletesLastNoteInMidiClip();
    midiNoteActionRaisesLastNotePitchOneSemitone();
    midiNoteActionLowersLastNotePitchOneSemitone();
    midiNoteActionRejectsPitchRaiseAboveMidiRangeWithoutDirtyingSession();
    midiNoteActionRejectsPitchLowerBelowMidiRangeWithoutDirtyingSession();
    midiNoteActionRejectsEmptyClipPitchWithoutDirtyingSession();
    midiNoteActionIncreasesLastNoteVelocityByStep();
    midiNoteActionDecreasesLastNoteVelocityByStep();
    midiNoteActionRejectsVelocityIncreaseAboveMidiRangeWithoutDirtyingSession();
    midiNoteActionRejectsVelocityDecreaseBelowMidiRangeWithoutDirtyingSession();
    midiNoteActionRejectsEmptyClipVelocityWithoutDirtyingSession();
    midiNoteActionRejectsMissingClipVelocityWithoutDirtyingSession();
    midiNoteActionRejectsAudioClipVelocityWithoutDirtyingSession();
    midiNoteActionLengthensLastNoteByStep();
    midiNoteActionShortensLastNoteByStep();
    midiNoteActionRejectsLengthenBeyondClipWithoutDirtyingSession();
    midiNoteActionRejectsShortenBelowMinimumWithoutDirtyingSession();
    midiNoteActionRejectsEmptyClipLengthWithoutDirtyingSession();
    midiNoteActionRejectsMissingClipLengthWithoutDirtyingSession();
    midiNoteActionRejectsAudioClipLengthWithoutDirtyingSession();
    midiNoteActionMovesLastNoteStartEarlierByStep();
    midiNoteActionMovesLastNoteStartLaterByStep();
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
    trackListStatusDescribesEmptyProject();
    trackListStatusDescribesTrackRowsInProjectOrder();
    trackListStatusDescribesTrackPlaybackAndViewFlags();
    return 0;
}
