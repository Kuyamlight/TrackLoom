#include "AppTimelineCanvasStatus.h"
#include "support/TestFailureOutput.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << message << '\n';
        throw std::runtime_error(message);
    }
}

void requireTicks(
    const std::vector<std::int64_t>& actual,
    const std::vector<std::int64_t>& expected,
    const std::string& message)
{
    require(actual == expected, message);
}

void requireEmptyStatus(
    const trackloom::AppTimelineCanvasStatus& status,
    const std::string& message)
{
    require(status.visibleRange == trackloom::AppTimelineVisibleTickRange {}, message + ": visible range");
    require(status.tracks.empty(), message + ": tracks");
    require(status.clips.empty(), message + ": clips");
    require(!status.playbackLoopRange.has_value(), message + ": loop range");
    require(status.measureBoundaryTicks.empty(), message + ": measure boundaries");
}

void defaultFourFourProducesHandCalculatedClosedBoundaries()
{
    const trackloom::Project project("Four Four");

    const auto result = trackloom::buildAppTimelineCanvasStatus(project, { 0, 7680 });

    require(result.success, "default 4/4 canvas should build");
    require(result.failureReason == trackloom::AppTimelineCanvasFailureReason::None,
        "successful 4/4 canvas should report no failure");
    requireTicks(result.status.measureBoundaryTicks, { 0, 3840, 7680 },
        "4/4 boundaries should include both ends of the closed visible range");
}

void visibleRangeBeginningInsideMeasureOnlyReturnsVisibleBoundaries()
{
    const trackloom::Project project("Mid Measure");

    const auto result = trackloom::buildAppTimelineCanvasStatus(project, { 1000, 5000 });

    require(result.success, "mid-measure visible range should build");
    requireTicks(result.status.measureBoundaryTicks, { 3840 },
        "builder must not leak the preceding or following off-screen boundary");
}

void nonAlignedTimeSignatureEventStartsANewMeasureOrigin()
{
    trackloom::Project project("Meter Change");
    require(project.createTimeSignatureEvent(5000, 3, 4).has_value(),
        "non-aligned 3/4 event should be accepted by the fixture");

    const auto result = trackloom::buildAppTimelineCanvasStatus(project, { 3500, 11000 });

    require(result.success, "meter-changing canvas should build");
    requireTicks(result.status.measureBoundaryTicks, { 3840, 5000, 7880, 10760 },
        "the 3/4 event at tick 5000 must reset the 2880-tick measure origin");
}

void canvasPreservesStableTrackClipIdentityTypesAndLoopRange()
{
    trackloom::Project project("Identity");
    require(project.insertExistingTrack({
        "track-midi-stable", "Lead", trackloom::TrackType::Instrument, {}, {}, {} }),
        "instrument track fixture should insert");
    require(project.insertExistingTrack({
        "track-audio-stable", "Voice", trackloom::TrackType::Audio, {}, {}, {} }),
        "audio track fixture should insert");
    require(project.insertExistingTrack({
        "track-folder-stable", "Songs", trackloom::TrackType::Folder, {}, {}, {} }),
        "folder track fixture should insert");
    require(project.insertExistingClip({
        "clip-midi-stable", "track-midi-stable", "Verse MIDI",
        trackloom::ClipType::Midi, 960, 1920, {} }),
        "MIDI clip fixture should insert");
    require(project.insertExistingClip({
        "clip-audio-stable", "track-audio-stable", "Voice Take",
        trackloom::ClipType::Audio, 4000, 1000, {} }),
        "audio clip fixture should insert");
    const trackloom::PlaybackLoopRange expectedLoop { 960, 2880 };
    require(project.setPlaybackLoopRange(expectedLoop), "loop range fixture should set");

    const auto result = trackloom::buildAppTimelineCanvasStatus(project, { 0, 5000 });

    require(result.success, "identity canvas should build");
    require(result.status.visibleRange == trackloom::AppTimelineVisibleTickRange { 0, 5000 },
        "canvas should retain the explicit visible range");
    require(result.status.tracks.size() == 3, "canvas should contain all three project tracks");
    require(result.status.tracks[0].trackId == "track-midi-stable"
            && result.status.tracks[0].type == trackloom::TrackType::Instrument
            && result.status.tracks[0].name == "Lead",
        "instrument row should preserve stable identity, type, and name");
    require(result.status.tracks[1].trackId == "track-audio-stable"
            && result.status.tracks[1].type == trackloom::TrackType::Audio
            && result.status.tracks[1].name == "Voice",
        "audio row should preserve stable identity, type, and name");
    require(result.status.tracks[2].trackId == "track-folder-stable"
            && result.status.tracks[2].type == trackloom::TrackType::Folder
            && result.status.tracks[2].name == "Songs",
        "folder row should preserve stable identity, type, and name");
    require(result.status.clips.size() == 2, "canvas should contain both project clips");
    require(result.status.clips[0].clipId == "clip-midi-stable"
            && result.status.clips[0].trackId == "track-midi-stable"
            && result.status.clips[0].type == trackloom::ClipType::Midi
            && result.status.clips[0].name == "Verse MIDI"
            && result.status.clips[0].startTick == 960
            && result.status.clips[0].endTick == 2880,
        "MIDI row should preserve identity and derive its half-open end tick");
    require(result.status.clips[1].clipId == "clip-audio-stable"
            && result.status.clips[1].trackId == "track-audio-stable"
            && result.status.clips[1].type == trackloom::ClipType::Audio
            && result.status.clips[1].name == "Voice Take"
            && result.status.clips[1].startTick == 4000
            && result.status.clips[1].endTick == 5000,
        "audio row should preserve identity and derive its half-open end tick");
    require(result.status.playbackLoopRange == expectedLoop,
        "canvas should expose the one project playback loop range");
}

void invalidVisibleRangesFailWithoutPartialCanvas()
{
    trackloom::Project project("Invalid Visible");
    project.createTrack("Existing Track", trackloom::TrackType::Instrument);

    const auto negative = trackloom::buildAppTimelineCanvasStatus(project, { -1, 100 });
    require(!negative.success, "negative visible start should fail");
    require(negative.failureReason == trackloom::AppTimelineCanvasFailureReason::InvalidVisibleRange,
        "negative visible start should use the stable failure reason");
    requireEmptyStatus(negative.status, "negative visible start must not return a partial canvas");

    const auto reversed = trackloom::buildAppTimelineCanvasStatus(project, { 101, 100 });
    require(!reversed.success, "reversed visible range should fail");
    require(reversed.failureReason == trackloom::AppTimelineCanvasFailureReason::InvalidVisibleRange,
        "reversed visible range should use the stable failure reason");
    requireEmptyStatus(reversed.status, "reversed visible range must not return a partial canvas");
}

void clipEndOverflowFailsWithoutPartialCanvas()
{
    trackloom::Project project("Overflow Clip");
    require(project.insertExistingTrack({
        "track-overflow", "Overflow", trackloom::TrackType::Instrument, {}, {}, {} }),
        "overflow track fixture should insert");
    require(project.insertExistingClip({
        "clip-overflow", "track-overflow", "Overflow Clip", trackloom::ClipType::Midi,
        std::numeric_limits<std::int64_t>::max() - 2, 3, {} }),
        "core fixture intentionally permits individually valid clip timing values");

    const auto result = trackloom::buildAppTimelineCanvasStatus(project, { 0, 0 });

    require(!result.success, "clip end overflow should fail the canvas build");
    require(result.failureReason == trackloom::AppTimelineCanvasFailureReason::ClipEndOverflow,
        "clip end overflow should use the stable failure reason");
    requireEmptyStatus(result.status, "clip overflow must discard already collected tracks and clips");
}

void neighborSearchRejectsNegativeCandidateAndMatchesExactBoundary()
{
    const trackloom::Project project("Neighbors");

    const auto negative = trackloom::findAppTimelineMeasureBoundaryNeighbors(project, -1);
    require(!negative.success, "negative boundary candidate should fail");
    require(negative.failureReason == trackloom::AppTimelineCanvasFailureReason::InvalidCandidateTick,
        "negative boundary candidate should use the stable failure reason");
    require(!negative.neighbors.atOrBeforeTick.has_value()
            && !negative.neighbors.atOrAfterTick.has_value(),
        "failed neighbor search must not return candidates");

    const auto exact = trackloom::findAppTimelineMeasureBoundaryNeighbors(project, 3840);
    require(exact.success, "exact boundary candidate should succeed");
    require(exact.failureReason == trackloom::AppTimelineCanvasFailureReason::None,
        "successful neighbor search should report no failure");
    require(exact.neighbors.atOrBeforeTick == 3840
            && exact.neighbors.atOrAfterTick == 3840,
        "exact measure boundary should be both nearest neighbors");
}

void finalRepresentableBoundaryHasNoOverflowingUpperNeighbor()
{
    constexpr std::int64_t lastFourFourBoundary = 9223372036854773760LL;
    const trackloom::Project project("Maximum Tick");

    const auto exact = trackloom::findAppTimelineMeasureBoundaryNeighbors(
        project, lastFourFourBoundary);
    require(exact.success, "last representable 4/4 boundary should be searchable");
    require(exact.neighbors.atOrBeforeTick == lastFourFourBoundary
            && exact.neighbors.atOrAfterTick == lastFourFourBoundary,
        "the last representable boundary should match itself exactly");

    const auto after = trackloom::findAppTimelineMeasureBoundaryNeighbors(
        project, std::numeric_limits<std::int64_t>::max());
    require(after.success, "candidate after the final representable boundary should still succeed");
    require(after.neighbors.atOrBeforeTick == lastFourFourBoundary,
        "search should retain the final representable lower boundary");
    require(!after.neighbors.atOrAfterTick.has_value(),
        "search must omit rather than overflow the unrepresentable next boundary");

    const auto canvas = trackloom::buildAppTimelineCanvasStatus(
        project, { lastFourFourBoundary, std::numeric_limits<std::int64_t>::max() });
    require(canvas.success, "maximum visible range should build without overflowing");
    requireTicks(canvas.status.measureBoundaryTicks, { lastFourFourBoundary },
        "maximum canvas should contain only the final representable boundary");
}

void canvasAndNeighborSearchShareMeterChangeBoundarySemantics()
{
    trackloom::Project project("Shared Semantics");
    require(project.createTimeSignatureEvent(5000, 3, 4).has_value(),
        "shared-semantics meter fixture should insert");

    const auto canvas = trackloom::buildAppTimelineCanvasStatus(project, { 4000, 9000 });
    require(canvas.success, "shared-semantics canvas should build");
    requireTicks(canvas.status.measureBoundaryTicks, { 5000, 7880 },
        "canvas should expose the hand-calculated visible meter boundaries");

    const auto between = trackloom::findAppTimelineMeasureBoundaryNeighbors(project, 6000);
    require(between.success, "neighbor search inside changed meter should succeed");
    require(between.neighbors.atOrBeforeTick == 5000
            && between.neighbors.atOrAfterTick == 7880,
        "neighbor search must agree with canvas boundaries around the same candidate");

    for (const auto boundary : canvas.status.measureBoundaryTicks) {
        const auto exact = trackloom::findAppTimelineMeasureBoundaryNeighbors(project, boundary);
        require(exact.success
                && exact.neighbors.atOrBeforeTick == boundary
                && exact.neighbors.atOrAfterTick == boundary,
            "every canvas boundary must be exact according to the shared neighbor query");
    }
}

}

int main()
{
    trackloom::test::configureTestFailureOutput();

    try {
        defaultFourFourProducesHandCalculatedClosedBoundaries();
        visibleRangeBeginningInsideMeasureOnlyReturnsVisibleBoundaries();
        nonAlignedTimeSignatureEventStartsANewMeasureOrigin();
        canvasPreservesStableTrackClipIdentityTypesAndLoopRange();
        invalidVisibleRangesFailWithoutPartialCanvas();
        clipEndOverflowFailsWithoutPartialCanvas();
        neighborSearchRejectsNegativeCandidateAndMatchesExactBoundary();
        finalRepresentableBoundaryHasNoOverflowingUpperNeighbor();
        canvasAndNeighborSearchShareMeterChangeBoundarySemantics();
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "All app timeline canvas tests passed.\n";
    return 0;
}
