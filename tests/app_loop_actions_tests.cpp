#include "AppLoopActions.h"
#include "AppTimelineCanvasStatus.h"
#include "support/TestFailureOutput.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << message << '\n';
        throw std::runtime_error(message);
    }
}

struct ClipFixtures {
    trackloom::AppProjectSession session;
    trackloom::TimelineClip midi;
    trackloom::TimelineClip audio;
};

ClipFixtures createClipFixtures()
{
    ClipFixtures fixtures;
    auto& project = fixtures.session.editProject();
    const auto instrument = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto audioTrack = project.createTrack("Vocal", trackloom::TrackType::Audio);
    fixtures.midi = *project.createClip(
        instrument.id, "Verse MIDI", trackloom::ClipType::Midi, 960, 3840);
    fixtures.audio = *project.createClip(
        audioTrack.id, "Voice", trackloom::ClipType::Audio, 1920, 960);
    return fixtures;
}

void requireSessionMetadataUnchanged(
    const trackloom::AppProjectSession& session,
    bool dirty,
    std::uint64_t generation,
    bool canUndo,
    bool canRedo,
    const std::string& message)
{
    require(session.isDirty() == dirty, message + ": dirty");
    require(session.projectEditGeneration() == generation, message + ": generation");
    require(session.canUndoProjectEdit() == canUndo, message + ": undo");
    require(session.canRedoProjectEdit() == canRedo, message + ": redo");
}

void selectedMidiClipValidationDoesNotTouchSessionBeforeCommand()
{
    auto fixtures = createClipFixtures();
    trackloom::AppLoopPlaybackState state;
    const auto dirty = fixtures.session.isDirty();
    const auto generation = fixtures.session.projectEditGeneration();

    const auto empty = trackloom::setAppPlaybackLoopFromSelectedMidiClip(
        fixtures.session, "", state);
    require(!empty.success && empty.kind == trackloom::AppLoopActionFeedbackKind::MissingSelection,
        "empty selection must be rejected with the stable kind");
    requireSessionMetadataUnchanged(fixtures.session, dirty, generation, false, false,
        "empty selection must not mutate session metadata");

    const auto missing = trackloom::setAppPlaybackLoopFromSelectedMidiClip(
        fixtures.session, "missing", state);
    require(!missing.success && missing.kind == trackloom::AppLoopActionFeedbackKind::MissingClip,
        "missing selected clip must be rejected with the stable kind");
    requireSessionMetadataUnchanged(fixtures.session, dirty, generation, false, false,
        "missing selected clip must not mutate session metadata");

    const auto audio = trackloom::setAppPlaybackLoopFromSelectedMidiClip(
        fixtures.session, fixtures.audio.id, state);
    require(!audio.success && audio.kind == trackloom::AppLoopActionFeedbackKind::IncompatibleClipType,
        "audio selection must be rejected with the stable kind");
    requireSessionMetadataUnchanged(fixtures.session, dirty, generation, false, false,
        "audio selection must not mutate session metadata");
}

void selectedMidiClipOverflowDoesNotWrapOrCreateHistory()
{
    trackloom::AppProjectSession session;
    auto& project = session.editProject();
    const auto instrument = project.createTrack("Lead", trackloom::TrackType::Instrument);
    const auto clip = project.createClip(
        instrument.id, "Near Maximum", trackloom::ClipType::Midi,
        std::numeric_limits<std::int64_t>::max(), 1);
    require(clip.has_value(), "fixture must permit the core-valid clip timing used for overflow defense");
    const auto dirty = session.isDirty();
    const auto generation = session.projectEditGeneration();
    trackloom::AppLoopPlaybackState state;

    const auto result = trackloom::setAppPlaybackLoopFromSelectedMidiClip(session, clip->id, state);

    require(!result.success && result.kind == trackloom::AppLoopActionFeedbackKind::ClipEndOverflow,
        "selected MIDI clip end overflow must fail before creating a loop command");
    require(!session.project().playbackLoopRange().has_value() && !state.enabled(),
        "overflow must preserve the absent loop and disabled session state");
    requireSessionMetadataUnchanged(session, dirty, generation, false, false,
        "overflow must not change session metadata or history");
}

void setCommitAndToggleRespectSessionOnlyNoOpSemantics()
{
    auto fixtures = createClipFixtures();
    trackloom::AppLoopPlaybackState state;
    const auto set = trackloom::setAppPlaybackLoopFromSelectedMidiClip(
        fixtures.session, fixtures.midi.id, state);
    const trackloom::PlaybackLoopRange expected { 960, 4800 };
    require(set.success && set.kind == trackloom::AppLoopActionFeedbackKind::Success,
        "setting the selected MIDI clip must execute the loop command");
    require(fixtures.session.project().playbackLoopRange() == expected && state.enabled(),
        "setting the selected MIDI clip must persist its half-open range and enable the session");
    require(fixtures.session.canUndoProjectEdit(), "setting a new range must create one undo entry");
    state.reconcile(fixtures.session.project());
    require(state.enabled(), "reconcile must preserve an enabled session while a project range exists");

    require(fixtures.session.undoProjectEdit(), "fixture must undo the set command");
    require(!fixtures.session.project().playbackLoopRange().has_value(), "undo must clear the project loop");
    state.reconcile(fixtures.session.project());
    require(!state.enabled(), "reconcile must disable the session after undo removes the range");
    require(fixtures.session.redoProjectEdit(), "fixture must redo the set command");
    state.reconcile(fixtures.session.project());
    require(fixtures.session.project().playbackLoopRange() == expected && !state.enabled(),
        "redo must restore the range without silently re-enabling session playback");

    const auto dirty = fixtures.session.isDirty();
    const auto generation = fixtures.session.projectEditGeneration();
    const auto enabled = trackloom::commitAppPlaybackLoopRange(fixtures.session, expected, state);
    require(enabled.success && enabled.kind == trackloom::AppLoopActionFeedbackKind::SessionOnlyEnabled,
        "committing an equal project range while disabled must only enable the session");
    require(state.enabled(), "equal-range session-only commit must enable playback");
    requireSessionMetadataUnchanged(fixtures.session, dirty, generation, true, false,
        "equal-range session-only commit must not change dirty, generation, or history");

    const auto noOp = trackloom::commitAppPlaybackLoopRange(fixtures.session, expected, state);
    require(noOp.success && noOp.kind == trackloom::AppLoopActionFeedbackKind::NoOp,
        "committing an equal range while already enabled must be a no-op");
    requireSessionMetadataUnchanged(fixtures.session, dirty, generation, true, false,
        "equal-range no-op must not change dirty, generation, or history");

    const auto toggledOff = trackloom::toggleAppLoopPlaybackEnabled(fixtures.session.project(), state);
    const auto toggledOn = trackloom::toggleAppLoopPlaybackEnabled(fixtures.session.project(), state);
    require(toggledOff.success && toggledOn.success && state.enabled(),
        "loop toggle round trip must change only session-enabled state");
    requireSessionMetadataUnchanged(fixtures.session, dirty, generation, true, false,
        "loop toggle round trip must not change dirty, generation, or history");

    trackloom::Project noRangeProject("No Range");
    trackloom::AppLoopPlaybackState noRangeState;
    const auto noRange = trackloom::toggleAppLoopPlaybackEnabled(noRangeProject, noRangeState);
    require(!noRange.success && noRange.kind == trackloom::AppLoopActionFeedbackKind::MissingLoopRange,
        "toggle must not enable a session without a project range");
}

void clearUndoRedoAndReconcileKeepProjectAndSessionStateSeparate()
{
    auto fixtures = createClipFixtures();
    trackloom::AppLoopPlaybackState state;
    require(trackloom::setAppPlaybackLoopFromSelectedMidiClip(fixtures.session, fixtures.midi.id, state).success,
        "fixture must set a loop range first");
    const auto cleared = trackloom::clearAppPlaybackLoopRange(fixtures.session, state);
    require(cleared.success && !fixtures.session.project().playbackLoopRange().has_value() && !state.enabled(),
        "clear must remove the project range and disable the session");

    require(fixtures.session.undoProjectEdit(), "clear command must undo");
    state.reconcile(fixtures.session.project());
    require(fixtures.session.project().playbackLoopRange().has_value() && !state.enabled(),
        "undoing clear must restore only project data and preserve disabled session state");
    require(fixtures.session.redoProjectEdit(), "clear command must redo");
    state.reconcile(fixtures.session.project());
    require(!fixtures.session.project().playbackLoopRange().has_value() && !state.enabled(),
        "redoing clear must remove the range and leave session state disabled");

    const auto dirty = fixtures.session.isDirty();
    const auto generation = fixtures.session.projectEditGeneration();
    const auto clearNoOp = trackloom::clearAppPlaybackLoopRange(fixtures.session, state);
    require(clearNoOp.success && clearNoOp.kind == trackloom::AppLoopActionFeedbackKind::NoOp,
        "clearing an already absent loop must not attempt a core command");
    requireSessionMetadataUnchanged(fixtures.session, dirty, generation, true, false,
        "clear no-op must preserve session metadata and redo history");
}

void previewsSnapBySharedNeighborsAndRejectCrossingRanges()
{
    trackloom::Project tieProject("Tie");
    require(tieProject.setPlaybackLoopRange(trackloom::PlaybackLoopRange { 0, 11520 }), "tie fixture range must be valid");

    const auto equalDistance = trackloom::previewAppPlaybackLoopBoundaryDrag(
        tieProject, trackloom::AppLoopBoundaryEdge::Start, 5760);
    require(equalDistance.success && equalDistance.range == trackloom::PlaybackLoopRange { 3840, 11520 },
        "equidistant snapping must choose the lower measure boundary");

    trackloom::Project project("Snap");
    require(project.setPlaybackLoopRange(trackloom::PlaybackLoopRange { 3840, 7680 }), "fixture range must be valid");

    const auto endCrossing = trackloom::previewAppPlaybackLoopBoundaryDrag(
        project, trackloom::AppLoopBoundaryEdge::End, 2000);
    require(!endCrossing.success && !endCrossing.range.has_value(),
        "dragging end across fixed start must reject every crossing snap candidate");

    const auto startCrossing = trackloom::previewAppPlaybackLoopBoundaryDrag(
        project, trackloom::AppLoopBoundaryEdge::Start, 8000);
    require(!startCrossing.success && !startCrossing.range.has_value(),
        "dragging start across fixed end must reject every crossing snap candidate");

    const auto negative = trackloom::previewAppPlaybackLoopBoundaryDrag(
        project, trackloom::AppLoopBoundaryEdge::Start, -1);
    require(!negative.success && negative.kind == trackloom::AppLoopActionFeedbackKind::InvalidLoopRange
            && !negative.range.has_value(),
        "negative drag candidates must fail without a preview range");

    const auto neighbor = trackloom::findAppTimelineMeasureBoundaryNeighbors(project, 6000);
    const auto shared = trackloom::previewAppPlaybackLoopBoundaryDrag(
        project, trackloom::AppLoopBoundaryEdge::Start, 6000);
    require(neighbor.success && shared.success && shared.range->startTick == *neighbor.neighbors.atOrBeforeTick,
        "drag preview must agree with the Task 4 neighbor primitive");
}

void previewsHandleFinalRepresentableBoundaryWithoutOverflow()
{
    constexpr std::int64_t lastBoundary = 9223372036854773760LL;
    trackloom::Project project("Maximum");
    require(project.setPlaybackLoopRange(trackloom::PlaybackLoopRange { lastBoundary - 3840, lastBoundary }),
        "near-maximum fixture range must be valid");

    const auto exact = trackloom::previewAppPlaybackLoopBoundaryDrag(
        project, trackloom::AppLoopBoundaryEdge::End, lastBoundary);
    require(exact.success && exact.range == trackloom::PlaybackLoopRange { lastBoundary - 3840, lastBoundary },
        "the final representable measure boundary must remain a valid drag candidate");

    const auto after = trackloom::previewAppPlaybackLoopBoundaryDrag(
        project, trackloom::AppLoopBoundaryEdge::End, std::numeric_limits<std::int64_t>::max());
    require(after.success && after.range == trackloom::PlaybackLoopRange { lastBoundary - 3840, lastBoundary },
        "a missing next boundary must not overflow and must retain the lower neighbor");
}

void commitRejectsInvalidRangeAndEffectiveRangeTracksSessionState()
{
    trackloom::AppProjectSession session;
    trackloom::AppLoopPlaybackState state;
    const auto invalid = trackloom::commitAppPlaybackLoopRange(session, { 960, 960 }, state);
    require(!invalid.success && invalid.kind == trackloom::AppLoopActionFeedbackKind::InvalidLoopRange,
        "invalid range must be rejected before issuing a core command");
    require(!trackloom::effectiveAppPlaybackLoopRange(session.project(), state).has_value(),
        "disabled session must not expose a project loop as effective playback state");

    require(session.editProject().setPlaybackLoopRange(trackloom::PlaybackLoopRange { 0, 3840 }),
        "fixture must establish a project loop directly");
    require(!trackloom::effectiveAppPlaybackLoopRange(session.project(), state).has_value(),
        "project loop remains ineffective until session playback is enabled");
    require(state.setEnabled(session.project(), true), "state may enable only when a project loop exists");
    require(trackloom::effectiveAppPlaybackLoopRange(session.project(), state)
            == trackloom::PlaybackLoopRange { 0, 3840 },
        "effective range must expose the persisted range only while session is enabled");
    state.resetForProjectReplacement();
    require(!state.enabled(), "project replacement must reset session-only loop state");
}

}

int main()
{
    trackloom::test::configureTestFailureOutput();

    try {
        selectedMidiClipValidationDoesNotTouchSessionBeforeCommand();
        selectedMidiClipOverflowDoesNotWrapOrCreateHistory();
        setCommitAndToggleRespectSessionOnlyNoOpSemantics();
        clearUndoRedoAndReconcileKeepProjectAndSessionStateSeparate();
        previewsSnapBySharedNeighborsAndRejectCrossingRanges();
        previewsHandleFinalRepresentableBoundaryWithoutOverflow();
        commitRejectsInvalidRangeAndEffectiveRangeTracksSessionState();
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "All app loop action tests passed.\n";
    return 0;
}
