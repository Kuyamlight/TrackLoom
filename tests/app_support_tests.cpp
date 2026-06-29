#include "AppProjectFileActions.h"
#include "AppMidiClipActions.h"
#include "AppMidiNoteActions.h"
#include "AppProjectSession.h"
#include "AppProjectStatus.h"
#include "AppTimelineStatus.h"
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
    midiClipActionCreatesDefaultClipOnInstrumentTrack();
    midiClipActionAppendsAfterExistingTrackClips();
    midiClipActionRejectsMissingTrackWithoutDirtyingSession();
    midiClipActionRejectsIncompatibleTrackWithoutDirtyingSession();
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
