#include "TimelineLoopEditorComponent.h"
#include "support/TestFailureOutput.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << message << '\n';
        throw std::runtime_error(message);
    }
}

trackloom::AppTimelineCanvasStatus fixtureStatus()
{
    trackloom::AppTimelineCanvasStatus status;
    status.visibleRange = { 0, 7680 };
    status.tracks = {
        { "track-midi", trackloom::TrackType::Instrument, "MIDI" },
        { "track-audio", trackloom::TrackType::Audio, "Audio" }
    };
    status.clips = {
        { "midi-1", "track-midi", trackloom::ClipType::Midi, "MIDI clip", 960, 3840 },
        { "audio-1", "track-audio", trackloom::ClipType::Audio, "Audio clip", 4800, 6720 }
    };
    status.playbackLoopRange = trackloom::PlaybackLoopRange { 1920, 5760 };
    status.measureBoundaryTicks = { 0, 3840, 7680 };
    return status;
}

juce::MouseEvent mouseEvent(
    juce::Component& component,
    juce::Point<float> position,
    juce::Point<float> mouseDownPosition,
    bool wasDragged = false)
{
    return {
        juce::Desktop::getInstance().getMainMouseSource(),
        position,
        juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),
        juce::MouseInputSource::defaultPressure,
        juce::MouseInputSource::defaultOrientation,
        juce::MouseInputSource::defaultRotation,
        juce::MouseInputSource::defaultTiltX,
        juce::MouseInputSource::defaultTiltY,
        &component,
        &component,
        juce::Time::getCurrentTime(),
        mouseDownPosition,
        juce::Time::getCurrentTime(),
        1,
        wasDragged
    };
}

void geometryPaintsTheTimelineBandsAndExposesMonotonicClipAndHandleBounds()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    trackloom::TimelineLoopEditorComponent component;
    component.setSize(960, 360);
    component.setTimelineStatus(fixtureStatus());

    require(component.getComponentID() == trackloom::timelineLoopEditorComponentId,
        "timeline component must retain its stable component ID");
    const auto midi = component.clipBounds("midi-1");
    const auto audio = component.clipBounds("audio-1");
    const auto loopStart = component.loopHandleBounds(trackloom::AppLoopBoundaryEdge::Start);
    const auto loopEnd = component.loopHandleBounds(trackloom::AppLoopBoundaryEdge::End);
    require(midi.has_value() && audio.has_value() && loopStart.has_value() && loopEnd.has_value(),
        "fixed snapshot must expose MIDI, audio, and both loop handle bounds");
    require(midi->getWidth() > 0.0f && audio->getWidth() > 0.0f
            && midi->getX() < midi->getRight() && audio->getX() < audio->getRight(),
        "MIDI and audio clips must have non-empty monotonic lane geometry");
    require(midi->getCentreY() < audio->getCentreY(),
        "track rows must use one shared increasing vertical geometry");
    require(loopStart->getWidth() > 0.0f && loopEnd->getWidth() > 0.0f
            && loopStart->getX() < loopEnd->getX(),
        "loop handles must be non-empty and follow their loop boundary order");

    juce::Image image(juce::Image::ARGB, 960, 360, true);
    {
        juce::Graphics graphics(image);
        component.paint(graphics);
    }
    require(image.getPixelAt(8, 70).getAlpha() != 0,
        "paint must fill the fixed left track header");
    require(image.getPixelAt(170, 70).getAlpha() != 0,
        "paint must fill the right track lane beside the header");
    require(image.getPixelAt(170, 10).getAlpha() != 0,
        "paint must fill the top ruler");
    require(image.getPixelAt(170, 35).getAlpha() != 0,
        "paint must fill the loop band below the ruler");
}

void clicksSelectOnlyMidiClips()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    std::vector<std::string> selected;
    trackloom::TimelineLoopEditorComponent component({
        [&](std::string clipId) { selected.push_back(std::move(clipId)); }, {}, {}, {}
    });
    component.setSize(960, 360);
    component.setTimelineStatus(fixtureStatus());
    const auto midi = *component.clipBounds("midi-1");
    const auto audio = *component.clipBounds("audio-1");

    component.mouseDown(mouseEvent(component, midi.getCentre(), midi.getCentre()));
    component.mouseUp(mouseEvent(component, midi.getCentre(), midi.getCentre()));
    component.mouseDown(mouseEvent(component, audio.getCentre(), audio.getCentre()));
    component.mouseUp(mouseEvent(component, audio.getCentre(), audio.getCentre()));

    require(selected == std::vector<std::string> { "midi-1" },
        "audio clicks must not send a MIDI selection callback or overwrite a MIDI selection");
}

void loopDragPreviewsUntilMouseUpAndCommitsOnceOnlyWhenChanged()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    int previewCalls = 0;
    int commitCalls = 0;
    std::vector<std::int64_t> candidates;
    trackloom::PlaybackLoopRange committed;
    trackloom::TimelineLoopEditorComponent component({
        {},
        [&](trackloom::AppLoopBoundaryEdge edge, std::int64_t candidate) {
            ++previewCalls;
            candidates.push_back(candidate);
            return edge == trackloom::AppLoopBoundaryEdge::Start
                ? std::optional<trackloom::PlaybackLoopRange>({ candidate, 5760 })
                : std::optional<trackloom::PlaybackLoopRange>({ 1920, candidate });
        },
        [&](trackloom::PlaybackLoopRange range) { ++commitCalls; committed = range; },
        {}
    });
    component.setSize(960, 360);
    component.setTimelineStatus(fixtureStatus());
    const auto handle = *component.loopHandleBounds(trackloom::AppLoopBoundaryEdge::Start);
    const auto down = handle.getCentre();
    const juce::Point<float> firstDrag { 410.0f, down.y };
    const juce::Point<float> secondDrag { 460.0f, down.y };

    component.mouseDown(mouseEvent(component, down, down));
    component.mouseDrag(mouseEvent(component, firstDrag, down, true));
    component.mouseDrag(mouseEvent(component, secondDrag, down, true));
    require(previewCalls == 2 && commitCalls == 0,
        "multiple loop drags must only synchronously preview and never commit before mouse-up");
    require(candidates.size() == 2 && candidates[0] < candidates[1],
        "pixel drag positions must be converted to increasing raw candidate ticks");
    require(component.previewLoopRange() == std::optional<trackloom::PlaybackLoopRange>({ candidates[1], 5760 }),
        "the component must retain the snapped preview returned by the application callback");

    component.mouseUp(mouseEvent(component, secondDrag, down, true));
    require(commitCalls == 1 && committed == trackloom::PlaybackLoopRange { candidates[1], 5760 },
        "mouse-up must commit exactly the returned preview once when it differs from the saved range");
}

void unchangedLoopReleaseAndCaptureLossDoNotCommit()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    int commitCalls = 0;
    trackloom::TimelineLoopEditorComponent component({
        {},
        [](trackloom::AppLoopBoundaryEdge, std::int64_t) {
            return std::optional<trackloom::PlaybackLoopRange>({ 1920, 5760 });
        },
        [&](trackloom::PlaybackLoopRange) { ++commitCalls; },
        {}
    });
    component.setSize(960, 360);
    component.setTimelineStatus(fixtureStatus());
    const auto handle = *component.loopHandleBounds(trackloom::AppLoopBoundaryEdge::Start);
    const auto down = handle.getCentre();

    component.mouseDown(mouseEvent(component, down, down));
    component.mouseDrag(mouseEvent(component, down, down, true));
    component.mouseUp(mouseEvent(component, down, down, true));
    require(commitCalls == 0,
        "a preview equal to the stored loop must not create a duplicate commit");

    component.mouseDown(mouseEvent(component, down, down));
    component.mouseDrag(mouseEvent(component, { 460.0f, down.y }, down, true));
    component.mouseCaptureLost();
    component.mouseUp(mouseEvent(component, { 460.0f, down.y }, down, true));
    require(commitCalls == 0 && !component.previewLoopRange().has_value(),
        "capture loss must cancel the drag preview and suppress the later mouse-up commit");
}

void wheelAndInvalidSnapshotKeepTheVisibleRangeLegal()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    int rangeChanges = 0;
    trackloom::TimelineLoopEditorComponent component({
        {}, {}, {}, [&](trackloom::AppTimelineVisibleTickRange) { ++rangeChanges; }
    });
    component.setSize(960, 360);
    auto invalid = fixtureStatus();
    invalid.visibleRange = { 90, 90 };
    component.setTimelineStatus(invalid);
    const auto fallback = component.visibleTickRange();
    require(fallback.startTick >= 0 && fallback.endTick > fallback.startTick,
        "invalid snapshot range must be normalized before geometry or input uses it");

    const auto point = juce::Point<float> { 500.0f, 100.0f };
    component.mouseWheelMove(mouseEvent(component, point, point), { 1.0f, 1.0f, false, false, false });
    const auto changed = component.visibleTickRange();
    require(rangeChanges == 1 && changed.startTick >= 0 && changed.endTick > changed.startTick,
        "wheel zoom and scroll must publish one valid visible tick range without Project access");

    auto huge = fixtureStatus();
    huge.visibleRange = { 0, std::numeric_limits<std::int64_t>::max() };
    component.setTimelineStatus(huge);
    component.mouseWheelMove(mouseEvent(component, point, point), { 0.0f, -1.0f, false, false, false });
    const auto afterHugeZoom = component.visibleTickRange();
    require(afterHugeZoom.startTick >= 0 && afterHugeZoom.endTick > afterHugeZoom.startTick,
        "wheel zoom must saturate an enormous valid range instead of overflowing it invalid");
}

}

int main()
{
    trackloom::test::configureTestFailureOutput();
    try {
        geometryPaintsTheTimelineBandsAndExposesMonotonicClipAndHandleBounds();
        clicksSelectOnlyMidiClips();
        loopDragPreviewsUntilMouseUpAndCommitsOnceOnlyWhenChanged();
        unchangedLoopReleaseAndCaptureLossDoNotCommit();
        wheelAndInvalidSnapshotKeepTheVisibleRangeLegal();
    } catch (const std::exception& error) {
        std::cerr << "D1 JUCE loop editor test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "D1 JUCE loop editor tests passed\n";
    return 0;
}
