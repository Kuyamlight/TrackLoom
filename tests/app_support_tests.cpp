#include "AppProjectFileActions.h"
#include "AppProjectSession.h"
#include "AppProjectStatus.h"
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
    trackListStatusDescribesEmptyProject();
    trackListStatusDescribesTrackRowsInProjectOrder();
    trackListStatusDescribesTrackPlaybackAndViewFlags();
    return 0;
}
