#include "TimelineLoopEditorComponent.h"
#include "TrackLoomMainComponent.h"
#include "AppMainMenu.h"
#include "AppMidiClipActions.h"
#include "AppProjectSession.h"
#include "support/TestFailureOutput.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
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

void requireBounds(
    const std::optional<juce::Rectangle<float>>& actual,
    float x,
    float y,
    float width,
    float height,
    const std::string& message)
{
    require(actual.has_value(), message + " must exist");
    require(actual->getX() == x && actual->getY() == y
            && actual->getWidth() == width && actual->getHeight() == height,
        message + " must match its hand-derived bounds");
}

void requireColour(
    juce::Colour actual,
    juce::Colour expected,
    const std::string& message)
{
    require(actual.getARGB() == expected.getARGB(),
        message + " (actual ARGB=" + std::to_string(actual.getARGB()) + ")");
}

void requireRectangle(
    juce::Rectangle<int> actual,
    int x,
    int y,
    int width,
    int height,
    const std::string& message)
{
    require(actual == juce::Rectangle<int>(x, y, width, height),
        message + " must match its hand-derived main-coordinate bounds");
}

std::unique_ptr<trackloom::JuceAudioHost> makeHeadlessHost()
{
    return std::make_unique<trackloom::JuceAudioHost>(
        []() -> std::unique_ptr<juce::AudioIODeviceType> { return {}; });
}

juce::Component* findDescendantWithId(juce::Component& component, const char* id)
{
    if (component.getComponentID() == id) {
        return &component;
    }
    for (int index = 0; index < component.getNumChildComponents(); ++index) {
        if (auto* child = findDescendantWithId(*component.getChildComponent(index), id)) {
            return child;
        }
    }
    return nullptr;
}

juce::TextButton* findDescendantTextButtonWithText(juce::Component& component, const char* text)
{
    if (auto* button = dynamic_cast<juce::TextButton*>(&component);
        button != nullptr && button->getButtonText() == text) {
        return button;
    }
    for (int index = 0; index < component.getNumChildComponents(); ++index) {
        if (auto* child = findDescendantTextButtonWithText(*component.getChildComponent(index), text)) {
            return child;
        }
    }
    return nullptr;
}

juce::Rectangle<int> boundsInMain(juce::Component& main, juce::Component& child)
{
    return main.getLocalArea(&child, child.getLocalBounds());
}

juce::Colour paintedTimelinePixel(
    trackloom::TimelineLoopEditorComponent& timeline,
    juce::Point<int> point)
{
    juce::Image image(juce::Image::ARGB, timeline.getWidth(), timeline.getHeight(), true);
    {
        juce::Graphics graphics(image);
        timeline.paint(graphics);
    }
    return image.getPixelAt(point.x, point.y);
}

class TemporaryProjectFile final {
public:
    TemporaryProjectFile()
        : path_(std::filesystem::temp_directory_path()
              / ("trackloom-task-9-" + juce::Uuid().toString().toStdString() + ".trackloom"))
    {
    }

    ~TemporaryProjectFile()
    {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void saveProjectFixture(
    const std::filesystem::path& path,
    bool withMidiClip,
    const char* name)
{
    trackloom::AppProjectSession source;
    source.createNewProject(name);
    if (withMidiClip) {
        const auto track = source.editProject().createTrack("Fixture MIDI", trackloom::TrackType::Instrument);
        const auto midi = trackloom::createDefaultMidiClipOnTrack(source, track.id);
        require(midi.success, "open fixture must contain a valid MIDI clip");
    }
    require(source.saveAs(path).success, "open fixture must save successfully");
}

void mainLayoutPlacesTheRealLoopEditorAndInspectorWithoutOverlap()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    trackloom::TrackLoomMainComponentDependencies dependencies;
    dependencies.audioHost = makeHeadlessHost();
    trackloom::TrackLoomMainComponent main(std::move(dependencies));
    main.setSize(1280, 820);

    auto* timeline = dynamic_cast<trackloom::TimelineLoopEditorComponent*>(
        findDescendantWithId(main, trackloom::timelineLoopEditorComponentId));
    auto* inspector = dynamic_cast<juce::Viewport*>(
        findDescendantWithId(main, trackloom::mainInspectorViewportComponentId));
    auto* midiSelector = dynamic_cast<juce::ComboBox*>(
        findDescendantWithId(main, trackloom::mainMidiClipSelectorComponentId));
    auto* addInstrument = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(main, trackloom::mainAddInstrumentTrackButtonComponentId));
    auto* createMidi = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(main, trackloom::mainCreateMidiClipButtonComponentId));
    auto* placeholder = dynamic_cast<juce::Label*>(
        findDescendantWithId(main, trackloom::mainMidiEditorPlaceholderComponentId));
    auto* collapse = dynamic_cast<juce::ToggleButton*>(
        findDescendantWithId(main, trackloom::mainMidiEditorToggleComponentId));
    auto* loopToggle = dynamic_cast<juce::ToggleButton*>(
        findDescendantWithId(main, trackloom::mainLoopToggleComponentId));
    auto* setLoop = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(main, trackloom::mainSetLoopButtonComponentId));
    auto* clearLoop = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(main, trackloom::mainClearLoopButtonComponentId));
    auto* loopStatus = dynamic_cast<juce::Label*>(
        findDescendantWithId(main, trackloom::mainLoopIntentStatusComponentId));
    require(timeline != nullptr && inspector != nullptr && midiSelector != nullptr
            && addInstrument != nullptr && createMidi != nullptr && placeholder != nullptr
            && collapse != nullptr && loopToggle != nullptr && setLoop != nullptr
            && clearLoop != nullptr && loopStatus != nullptr,
        "layout A must expose real timeline, inspector, selection, action, bottom, and loop controls");
    require(placeholder->getText() == juce::String::fromUTF8("D3 预留，尚未实现"),
        "the collapsible bottom panel must use the D3 placeholder text");
    require(inspector->getViewedComponent() != nullptr
            && inspector->getViewedComponent()->isParentOf(midiSelector)
            && inspector->getViewedComponent()->isParentOf(addInstrument)
            && inspector->getViewedComponent()->isParentOf(createMidi),
        "the existing inspector controls must have one non-owning viewed-content parent");

    const auto timelineBounds = boundsInMain(main, *timeline);
    const auto inspectorBounds = boundsInMain(main, *inspector);
    const auto placeholderBounds = boundsInMain(main, *placeholder);
    const auto loopBounds = boundsInMain(main, *loopToggle);
    require(!timelineBounds.isEmpty() && !inspectorBounds.isEmpty() && !placeholderBounds.isEmpty()
            && !loopBounds.isEmpty(),
        "layout A controls must have usable bounds at 1280 by 820");
    require(timelineBounds.getRight() <= inspectorBounds.getX()
            && timelineBounds.getY() > loopBounds.getBottom()
            && placeholderBounds.getY() >= timelineBounds.getBottom(),
        "timeline must sit below transport, left of inspector, and above the D3 placeholder");
    require(inspector->getViewedComponent()->getHeight() > inspector->getHeight(),
        "inspector content must create a real vertical scroll range");

    const auto expandedBottom = timelineBounds.getBottom();
    collapse->setToggleState(true, juce::sendNotification);
    const auto collapsedTimeline = boundsInMain(main, *timeline);
    require(!placeholder->isVisible() && collapsedTimeline.getBottom() > expandedBottom
            && collapsedTimeline.getRight() <= inspectorBounds.getX(),
        "collapsing the bottom placeholder must extend the timeline without overlapping inspector");

    main.setSize(trackloom::trackLoomMainMinimumWidth, trackloom::trackLoomMainMinimumHeight);
    collapse->setToggleState(false, juce::sendNotification);
    const auto minimumTimeline = boundsInMain(main, *timeline);
    const auto minimumInspector = boundsInMain(main, *inspector);
    const auto minimumPlaceholder = boundsInMain(main, *placeholder);
    const auto minimumToggle = boundsInMain(main, *collapse);
    const auto minimumNew = boundsInMain(main, *findDescendantWithId(main, trackloom::mainNewButtonComponentId));
    const auto minimumOpen = boundsInMain(main, *findDescendantWithId(main, trackloom::mainOpenButtonComponentId));
    const auto minimumSave = boundsInMain(main, *findDescendantWithId(main, trackloom::mainSaveButtonComponentId));
    const auto minimumPlay = boundsInMain(main, *findDescendantWithId(main, trackloom::mainPlayButtonComponentId));
    requireRectangle(minimumNew, 16, 174, 82, 34, "minimum New button");
    requireRectangle(minimumOpen, 104, 174, 82, 34, "minimum Open button");
    requireRectangle(minimumSave, 192, 174, 64, 34, "minimum Save button");
    requireRectangle(minimumPlay, 344, 174, 62, 34, "minimum Play button");
    requireRectangle(minimumTimeline, 16, 216, 609, 256, "minimum timeline");
    requireRectangle(minimumInspector, 635, 216, 309, 256, "minimum inspector");
    requireRectangle(minimumToggle, 16, 472, 928, 24, "minimum MIDI editor toggle");
    requireRectangle(minimumPlaceholder, 16, 496, 928, 128, "minimum D3 placeholder");
    const auto mainBounds = main.getLocalBounds();
    const auto containedByMain = [&mainBounds](juce::Rectangle<int> bounds) {
        return bounds.getX() >= mainBounds.getX() && bounds.getY() >= mainBounds.getY()
            && bounds.getRight() <= mainBounds.getRight() && bounds.getBottom() <= mainBounds.getBottom();
    };
    require(containedByMain(minimumNew) && containedByMain(minimumOpen)
            && containedByMain(minimumSave) && containedByMain(minimumPlay)
            && containedByMain(minimumTimeline) && containedByMain(minimumInspector)
            && containedByMain(minimumToggle) && containedByMain(minimumPlaceholder),
        "all minimum-window transport, timeline, inspector, and bottom regions must stay inside Main coordinates");
    require(minimumNew.getWidth() > 0 && minimumOpen.getWidth() > 0
            && minimumSave.getWidth() > 0 && minimumPlay.getWidth() > 0
            && minimumTimeline.getRight() <= minimumInspector.getX()
            && minimumTimeline.getBottom() <= minimumToggle.getY()
            && minimumInspector.getBottom() <= minimumToggle.getY()
            && minimumToggle.getBottom() <= minimumPlaceholder.getY(),
        "minimum-window transport and all four layout regions must remain non-overlapping and operable");
    require(inspector->getViewedComponent()->getHeight() > minimumInspector.getHeight(),
        "minimum inspector must retain a real vertical scroll range");
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

void mainSelectionUsesTheSameTruthForComboBoxAndTimeline()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    trackloom::TrackLoomMainComponentDependencies dependencies;
    dependencies.audioHost = makeHeadlessHost();
    trackloom::TrackLoomMainComponent main(std::move(dependencies));
    main.setSize(1280, 820);
    auto* addInstrument = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(main, trackloom::mainAddInstrumentTrackButtonComponentId));
    auto* createMidi = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(main, trackloom::mainCreateMidiClipButtonComponentId));
    auto* selector = dynamic_cast<juce::ComboBox*>(
        findDescendantWithId(main, trackloom::mainMidiClipSelectorComponentId));
    auto* timeline = dynamic_cast<trackloom::TimelineLoopEditorComponent*>(
        findDescendantWithId(main, trackloom::timelineLoopEditorComponentId));
    require(addInstrument != nullptr && createMidi != nullptr && selector != nullptr && timeline != nullptr,
        "selection test requires the real inspector actions, selector, and timeline");
    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::AddInstrumentTrack)).executed,
        "the standard Add Instrument Track command must establish a real target track");
    createMidi->onClick();
    createMidi->onClick();
    require(selector->getNumItems() == 2,
        "two real create actions must produce two selectable MIDI clips");

    const auto firstBounds = timeline->clipBounds("clip-1");
    const auto secondBounds = timeline->clipBounds("clip-2");
    require(firstBounds.has_value() && secondBounds.has_value(),
        "selection test requires paintable bounds for both real MIDI clips");
    const auto samplePoint = [](juce::Rectangle<float> bounds) {
        return juce::Point<int>(
            static_cast<int>(std::floor(bounds.getX())) + 2,
            static_cast<int>(std::floor(bounds.getY())) + 2);
    };
    const auto firstBeforeComboBox = paintedTimelinePixel(*timeline, samplePoint(*firstBounds));
    const auto secondBeforeComboBox = paintedTimelinePixel(*timeline, samplePoint(*secondBounds));
    selector->setSelectedId(1, juce::sendNotificationSync);
    require(selector->getSelectedId() == 1
            && paintedTimelinePixel(*timeline, samplePoint(*firstBounds)) == secondBeforeComboBox
            && paintedTimelinePixel(*timeline, samplePoint(*secondBounds)) == firstBeforeComboBox,
        "ComboBox selection must repaint first/second timeline clip selection before any timeline click");
    timeline->mouseDown(mouseEvent(*timeline, secondBounds->getCentre(), secondBounds->getCentre()));
    timeline->mouseUp(mouseEvent(*timeline, secondBounds->getCentre(), secondBounds->getCentre()));
    require(selector->getSelectedId() == 2,
        "clicking a real timeline MIDI clip must select the same second clip in ComboBox");

    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::DeleteSelectedMidiClip)).executed,
        "the standard selected-MIDI delete command must execute through the main component");
    require(selector->getSelectedId() == 0,
        "deleting the selected MIDI clip must leave selection empty instead of auto-selecting another clip");
}

void mainVisibleRangeWheelRefreshesOnlyTheCanvas()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    int titleChanges = 0;
    trackloom::TrackLoomMainComponentDependencies dependencies;
    dependencies.audioHost = makeHeadlessHost();
    dependencies.titleChanged = [&titleChanges](std::string) { ++titleChanges; };
    trackloom::TrackLoomMainComponent main(std::move(dependencies));
    main.setSize(1280, 820);
    auto* timeline = dynamic_cast<trackloom::TimelineLoopEditorComponent*>(
        findDescendantWithId(main, trackloom::timelineLoopEditorComponentId));
    auto* createMidi = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(main, trackloom::mainCreateMidiClipButtonComponentId));
    auto* extendMidiEnd = findDescendantTextButtonWithText(main, "延长片尾");
    require(timeline != nullptr, "visible-range integration requires Main's real embedded timeline");
    require(createMidi != nullptr
            && main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::AddInstrumentTrack)).executed,
        "visible-range integration requires a real Main-created instrument track");
    require(extendMidiEnd != nullptr, "visible-range integration requires Main's real MIDI length control");
    createMidi->onClick();
    for (int index = 0; index < 5; ++index) {
        extendMidiEnd->onClick();
    }
    requireBounds(timeline->clipBounds("clip-1"), 160.0f, 58.0f, 718.0f, 40.0f,
        "real Main MIDI clip spanning the hand-derived pre-wheel canvas range");

    const auto titlesBeforeWheel = titleChanges;
    const auto before = timeline->visibleTickRange();
    timeline->mouseWheelMove(
        mouseEvent(*timeline, { 380.0f, 100.0f }, { 380.0f, 100.0f }),
        { 10.0f, 0.0f, false, false, false });
    const auto after = timeline->visibleTickRange();
    require(before == trackloom::AppTimelineVisibleTickRange { 0, 7680 }
            && after == trackloom::AppTimelineVisibleTickRange { 7680, 15360 },
        "a real embedded horizontal wheel event must publish and retain its hand-derived shifted tick range");
    requireBounds(timeline->clipBounds("clip-1"), 160.0f, 58.0f, 89.75f, 40.0f,
        "canvas rebuilt for the shifted range must expose the hand-derived trailing MIDI clip geometry");
    require(titleChanges == titlesBeforeWheel,
        "visible-range callback must rebuild only the canvas rather than running Main full refresh/title presentation");
    requireColour(paintedTimelinePixel(*timeline, { (160 + timeline->getWidth()) / 2, 100 }),
        juce::Colour(0xff3a463c),
        "canvas rebuilt for the shifted range must paint the newly-visible 11520-tick measure gridline");
}

void mainProjectReplacementUsesTheChooserSeamAndClearsOnlyAfterSuccess()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    TemporaryProjectFile midiProject;
    TemporaryProjectFile replacementMidiProject;
    saveProjectFixture(midiProject.path(), true, "MIDI fixture");
    saveProjectFixture(replacementMidiProject.path(), true, "Replacement MIDI fixture");

    std::vector<trackloom::AppOpenProjectChooserCompletion> completions;
    trackloom::TrackLoomMainComponentDependencies dependencies;
    dependencies.audioHost = makeHeadlessHost();
    dependencies.chooseProjectToOpen = [&completions](auto completion) {
        completions.push_back(std::move(completion));
    };
    trackloom::TrackLoomMainComponent main(std::move(dependencies));
    main.setSize(1280, 820);
    auto* selector = dynamic_cast<juce::ComboBox*>(
        findDescendantWithId(main, trackloom::mainMidiClipSelectorComponentId));
    auto* addMidiNote = findDescendantTextButtonWithText(main, "添加默认音符");
    require(selector != nullptr, "chooser test requires the real MIDI selection ComboBox");
    require(addMidiNote != nullptr, "chooser test requires Main's real MIDI edit control");

    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::OpenProject)).executed
            && completions.size() == 1,
        "Open must expose exactly one captured chooser completion without a native dialog");
    completions.back()(midiProject.path());
    require(selector->getNumItems() == 1 && selector->getSelectedId() == 0,
        "a successfully opened MIDI project must begin with no implicit MIDI selection");

    selector->setSelectedId(1, juce::sendNotificationSync);
    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::NewProject)).executed
            && selector->getNumItems() == 0 && selector->getSelectedId() == 0 && !addMidiNote->isEnabled(),
        "a successful New Project must clear an existing MIDI selection");

    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::OpenProject)).executed
            && completions.size() == 2,
        "a New Project must leave Main able to enter the chooser seam again");
    completions.back()(midiProject.path());
    selector->setSelectedId(1, juce::sendNotificationSync);
    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::OpenProject)).executed
            && completions.size() == 3,
        "a selected clean project must still enter the chooser seam");
    completions.back()(replacementMidiProject.path());
    require(selector->getNumItems() == 1 && selector->getSelectedId() == 0,
        "a successful replacement retaining clip-1's stable ID must still clear the former MIDI selection");

    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::OpenProject)).executed
            && completions.size() == 4,
        "the chooser must remain usable after a successful replacement");
    completions.back()(midiProject.path());
    selector->setSelectedId(1, juce::sendNotificationSync);
    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::OpenProject)).executed
            && completions.size() == 5,
        "cancel verification must capture its own completion");
    completions.back()(std::nullopt);
    require(selector->getSelectedId() == 1,
        "cancelling the chooser must preserve the selected MIDI clip");

    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::OpenProject)).executed
            && completions.size() == 6,
        "open failure verification must capture its own completion");
    completions.back()(replacementMidiProject.path() / "does-not-exist.trackloom");
    require(selector->getSelectedId() == 1,
        "a failed project load must preserve the selected MIDI clip");
}

void geometryPaintsTheTimelineBandsAndExposesHandDerivedBounds()
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
    requireBounds(midi, 260.0f, 58.0f, 300.0f, 40.0f,
        "MIDI clip in 960 by 360 fixture");
    requireBounds(audio, 660.0f, 110.0f, 200.0f, 40.0f,
        "audio clip in 960 by 360 fixture");
    requireBounds(loopStart, 356.0f, 28.0f, 8.0f, 24.0f,
        "loop start handle in 960 by 360 fixture");
    requireBounds(loopEnd, 756.0f, 28.0f, 8.0f, 24.0f,
        "loop end handle in 960 by 360 fixture");

    juce::Image image(juce::Image::ARGB, 960, 360, true);
    {
        juce::Graphics graphics(image);
        component.paint(graphics);
    }
    requireColour(image.getPixelAt(8, 70), juce::Colour(0xff20231f),
        "fixed left header must use the panel colour");
    requireColour(image.getPixelAt(170, 70), juce::Colour(0xff151815),
        "right lane must retain the canvas colour outside clips");
    requireColour(image.getPixelAt(170, 10), juce::Colour(0xff20231f),
        "top ruler must use the panel colour");
    requireColour(image.getPixelAt(170, 35), juce::Colour(0xff20231f),
        "loop band before the loop ribbon must use the panel colour");
    requireColour(image.getPixelAt(560, 150), juce::Colour(0xff3a463c),
        "measure boundary must use the grid colour at its hand-derived x position");
    requireColour(image.getPixelAt(600, 35), juce::Colour(0xff35533d),
        "loop ribbon must visibly blend the loop accent over the panel");
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
    require(candidates == std::vector<std::int64_t> { 2400, 2880 },
        "known lane pixels must map to their hand-derived raw candidate ticks");
    require(component.previewLoopRange() == std::optional<trackloom::PlaybackLoopRange>({ candidates[1], 5760 }),
        "the component must retain the snapped preview returned by the application callback");

    component.mouseUp(mouseEvent(component, secondDrag, down, true));
    require(commitCalls == 1 && committed == trackloom::PlaybackLoopRange { candidates[1], 5760 },
        "mouse-up must commit exactly the returned preview once when it differs from the saved range");
}

void extremeTickRangesClampRawCandidatesAndBadSnapshotTicks()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    std::vector<std::int64_t> candidates;
    trackloom::TimelineLoopEditorComponent component({
        {},
        [&](trackloom::AppLoopBoundaryEdge, std::int64_t candidate) {
            candidates.push_back(candidate);
            return std::nullopt;
        },
        {},
        {}
    });
    component.setSize(960, 360);
    auto wide = fixtureStatus();
    wide.visibleRange = { 0, std::numeric_limits<std::int64_t>::max() };
    wide.playbackLoopRange = trackloom::PlaybackLoopRange { 0, 1 };
    component.setTimelineStatus(wide);
    const auto handle = *component.loopHandleBounds(trackloom::AppLoopBoundaryEdge::Start);
    component.mouseDown(mouseEvent(component, handle.getCentre(), handle.getCentre()));
    component.mouseDrag(mouseEvent(component, { 960.0f, handle.getCentreY() }, handle.getCentre(), true));
    require(candidates == std::vector<std::int64_t> { std::numeric_limits<std::int64_t>::max() },
        "the right lane endpoint of [0, INT64_MAX] must emit INT64_MAX exactly");

    auto narrow = fixtureStatus();
    narrow.visibleRange = { std::numeric_limits<std::int64_t>::max() - 1,
        std::numeric_limits<std::int64_t>::max() };
    narrow.clips = {
        { "bad-negative", "track-midi", trackloom::ClipType::Midi, "bad",
            std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::min() + 1 }
    };
    narrow.measureBoundaryTicks = { std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::int64_t>::max() };
    component.setTimelineStatus(narrow);
    require(!component.clipBounds("bad-negative").has_value(),
        "an extreme out-of-range clip must not wrap into the visible lane");
    juce::Image image(juce::Image::ARGB, 960, 360, true);
    {
        juce::Graphics graphics(image);
        component.paint(graphics);
    }

    narrow.playbackLoopRange = trackloom::PlaybackLoopRange {
        std::numeric_limits<std::int64_t>::max() - 1,
        std::numeric_limits<std::int64_t>::max() };
    component.setTimelineStatus(narrow);
    const auto narrowHandle = *component.loopHandleBounds(trackloom::AppLoopBoundaryEdge::Start);
    component.mouseDown(mouseEvent(component, narrowHandle.getCentre(), narrowHandle.getCentre()));
    component.mouseDrag(mouseEvent(component, { 960.0f, narrowHandle.getCentreY() }, narrowHandle.getCentre(), true));
    require(candidates == std::vector<std::int64_t> {
        std::numeric_limits<std::int64_t>::max(), std::numeric_limits<std::int64_t>::max() },
        "[INT64_MAX - 1, INT64_MAX] must map its right endpoint without overflow");
}

void loopHandlesClipToTheVisibleLoopBand()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    int previewCalls = 0;
    trackloom::TimelineLoopEditorComponent component({
        {},
        [&](trackloom::AppLoopBoundaryEdge, std::int64_t) {
            ++previewCalls;
            return std::nullopt;
        },
        {},
        {}
    });
    component.setSize(960, 360);
    auto status = fixtureStatus();
    status.visibleRange = { 1920, 5760 };
    status.playbackLoopRange = trackloom::PlaybackLoopRange { 0, 7680 };
    component.setTimelineStatus(status);
    require(!component.loopHandleBounds(trackloom::AppLoopBoundaryEdge::Start).has_value(),
        "a loop start entirely left of the visible range must not expose a handle");
    require(!component.loopHandleBounds(trackloom::AppLoopBoundaryEdge::End).has_value(),
        "a loop end entirely right of the visible range must not expose a handle");

    status.playbackLoopRange = trackloom::PlaybackLoopRange { 1920, 5760 };
    component.setTimelineStatus(status);
    requireBounds(component.loopHandleBounds(trackloom::AppLoopBoundaryEdge::Start),
        160.0f, 28.0f, 4.0f, 24.0f,
        "loop start exactly at visible start");
    requireBounds(component.loopHandleBounds(trackloom::AppLoopBoundaryEdge::End),
        956.0f, 28.0f, 4.0f, 24.0f,
        "loop end exactly at visible end");

    status.playbackLoopRange = trackloom::PlaybackLoopRange { 1910, 5770 };
    component.setTimelineStatus(status);
    require(!component.loopHandleBounds(trackloom::AppLoopBoundaryEdge::Start).has_value()
            && !component.loopHandleBounds(trackloom::AppLoopBoundaryEdge::End).has_value(),
        "loop edges just beyond the visible interval must not create clipped-in handles");
    juce::Image image(juce::Image::ARGB, 960, 360, true);
    {
        juce::Graphics graphics(image);
        component.paint(graphics);
    }
    requireColour(image.getPixelAt(161, 35), juce::Colour(0xff35533d),
        "an out-of-range start handle must not paint over the loop ribbon at the lane edge");
    requireColour(image.getPixelAt(959, 35), juce::Colour(0xff35533d),
        "an out-of-range end handle must not paint over the loop ribbon at the lane edge");
    const auto leftEdge = juce::Point<float> { 161.0f, 40.0f };
    component.mouseDown(mouseEvent(component, leftEdge, leftEdge));
    component.mouseDrag(mouseEvent(component, { 300.0f, 40.0f }, leftEdge, true));
    require(previewCalls == 0,
        "an out-of-range handle must not be a loop-drag hit target");

    status.clips = {
        { "left", "track-midi", trackloom::ClipType::Midi, "left", 0, 960 },
        { "crossing", "track-midi", trackloom::ClipType::Midi, "crossing", 0, 3000 },
        { "right", "track-midi", trackloom::ClipType::Midi, "right", 6720, 7680 }
    };
    component.setTimelineStatus(status);
    require(!component.clipBounds("left").has_value()
            && !component.clipBounds("right").has_value(),
        "clips wholly outside the visible lane must not expose hit-test geometry");
    requireBounds(component.clipBounds("crossing"), 160.0f, 58.0f, 225.0f, 40.0f,
        "a clip crossing visible start must clip exactly to the lane");
}

void wheelNormalizesNonFiniteAndExtremeInput()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    std::vector<trackloom::AppTimelineVisibleTickRange> published;
    trackloom::TimelineLoopEditorComponent component({
        {}, {}, {}, [&](trackloom::AppTimelineVisibleTickRange range) { published.push_back(range); }
    });
    component.setSize(960, 360);
    const auto point = juce::Point<float> { 500.0f, 100.0f };
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    for (const auto delta : std::array<float, 3> { nan,
             std::numeric_limits<float>::infinity(),
             -std::numeric_limits<float>::infinity() }) {
        auto status = fixtureStatus();
        status.visibleRange = { 100, 200 };
        component.setTimelineStatus(status);
        component.mouseWheelMove(mouseEvent(component, point, point), { delta, delta, false, false, false });
        require(component.visibleTickRange() == trackloom::AppTimelineVisibleTickRange { 100, 200 },
            "non-finite wheel input must be treated as zero without corrupting the range");
    }

    auto status = fixtureStatus();
    status.visibleRange = { 100, 200 };
    component.setTimelineStatus(status);
    component.mouseWheelMove(mouseEvent(component, point, point),
        { std::numeric_limits<float>::max(), 0.0f, false, false, false });
    require(component.visibleTickRange() == trackloom::AppTimelineVisibleTickRange {
        std::numeric_limits<std::int64_t>::max() - 100, std::numeric_limits<std::int64_t>::max() },
        "a finite huge positive horizontal wheel delta must saturate at the legal right edge");
    component.setTimelineStatus(status);
    component.mouseWheelMove(mouseEvent(component, point, point),
        { -std::numeric_limits<float>::max(), 0.0f, false, false, false });
    require(component.visibleTickRange() == trackloom::AppTimelineVisibleTickRange { 0, 100 },
        "a finite huge negative horizontal wheel delta must saturate at zero");
    component.setSize(0, 360);
    component.setTimelineStatus(status);
    for (int index = 0; index < 100; ++index) {
        component.mouseWheelMove(mouseEvent(component, point, point), { 0.0f, 1.0f, false, false, false });
    }
    const auto afterRepeatedZoom = component.visibleTickRange();
    require(afterRepeatedZoom.startTick >= 0 && afterRepeatedZoom.endTick > afterRepeatedZoom.startTick,
        "zero-width repeated zoom must keep the published range legal");
    require(published.size() == 105,
        "each real wheel event must publish exactly one normalized range");
}

void interactionAndGeometryMutationGuards()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    int selectionCalls = 0;
    int previewCalls = 0;
    int commitCalls = 0;
    std::vector<std::int64_t> previewCandidates;
    trackloom::TimelineLoopEditorComponent component({
        [&](std::string) { ++selectionCalls; },
        [&](trackloom::AppLoopBoundaryEdge edge, std::int64_t candidate) {
            ++previewCalls;
            previewCandidates.push_back(candidate);
            if (previewCalls == 1) {
                return std::optional<trackloom::PlaybackLoopRange> {};
            }
            return edge == trackloom::AppLoopBoundaryEdge::Start
                ? std::optional<trackloom::PlaybackLoopRange>({ candidate, 5760 })
                : std::optional<trackloom::PlaybackLoopRange>({ 1920, candidate });
        },
        [&](trackloom::PlaybackLoopRange) { ++commitCalls; },
        {}
    });
    component.setSize(960, 360);
    component.setTimelineStatus(fixtureStatus());
    const auto startHandle = *component.loopHandleBounds(trackloom::AppLoopBoundaryEdge::Start);
    const auto down = startHandle.getCentre();

    component.mouseDown(mouseEvent(component, down, down));
    component.mouseDrag(mouseEvent(component, { 410.0f, down.y }, down, true));
    component.mouseUp(mouseEvent(component, { 410.0f, down.y }, down, true));
    require(previewCalls == 1 && commitCalls == 0,
        "a null preview from the application must never commit on mouse-up");
    require(selectionCalls == 0,
        "pressing a loop handle must not select a MIDI clip");

    component.mouseDown(mouseEvent(component, down, down));
    component.mouseDrag(mouseEvent(component, { 410.0f, down.y }, down, true));
    component.mouseDrag(mouseEvent(component, { 410.0f, down.y }, down, true));
    require(previewCandidates == std::vector<std::int64_t> { 2400, 2400, 2400 },
        "repeated known drag positions must forward the same hand-derived raw tick each time");
    require(component.previewLoopRange().has_value(),
        "a valid preview must remain visible until a state reset or release");
    component.setTimelineStatus(fixtureStatus());
    require(!component.previewLoopRange().has_value(),
        "a snapshot refresh during drag must clear stale preview state");
    component.mouseUp(mouseEvent(component, { 410.0f, down.y }, down, true));
    require(commitCalls == 0,
        "mouse-up after a snapshot refresh must not commit the discarded drag");

    component.setSize(0, 360);
    component.setTimelineStatus(fixtureStatus());
    require(!component.clipBounds("midi-1").has_value()
            && !component.loopHandleBounds(trackloom::AppLoopBoundaryEdge::Start).has_value(),
        "zero width must not expose invalid clip or loop geometry");
    juce::Image zeroWidthImage(juce::Image::ARGB, 1, 360, true);
    {
        juce::Graphics graphics(zeroWidthImage);
        component.paint(graphics);
    }

    component.setSize(160, 360);
    component.setTimelineStatus(fixtureStatus());
    const auto headerOnlyClip = component.clipBounds("midi-1");
    require(!headerOnlyClip.has_value() || (headerOnlyClip->getX() >= 159.0f
            && headerOnlyClip->getRight() <= 160.0f),
        "header-only width must clip any remaining lane geometry to its one-pixel lane");
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

int main(int argumentCount, char* arguments[])
{
    trackloom::test::configureTestFailureOutput();
    try {
        const auto requested = argumentCount > 1 ? std::string(arguments[1]) : std::string {};
        const auto run = [&](std::string_view name, const auto& test) {
            if (requested.empty() || requested == name) {
                test();
            }
        };
        run("geometry", geometryPaintsTheTimelineBandsAndExposesHandDerivedBounds);
        run("selection", clicksSelectOnlyMidiClips);
        run("drag", loopDragPreviewsUntilMouseUpAndCommitsOnceOnlyWhenChanged);
        run("cancel", unchangedLoopReleaseAndCaptureLossDoNotCommit);
        run("extreme-ticks", extremeTickRangesClampRawCandidatesAndBadSnapshotTicks);
        run("loop-clipping", loopHandlesClipToTheVisibleLoopBand);
        run("wheel", wheelNormalizesNonFiniteAndExtremeInput);
        run("mutation-guards", interactionAndGeometryMutationGuards);
        run("range", wheelAndInvalidSnapshotKeepTheVisibleRangeLegal);
        run("main-layout", mainLayoutPlacesTheRealLoopEditorAndInspectorWithoutOverlap);
        run("main-selection", mainSelectionUsesTheSameTruthForComboBoxAndTimeline);
        run("main-visible-range", mainVisibleRangeWheelRefreshesOnlyTheCanvas);
        run("main-project-replacement", mainProjectReplacementUsesTheChooserSeamAndClearsOnlyAfterSuccess);
    } catch (const std::exception& error) {
        std::cerr << "D1 JUCE loop editor test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "D1 JUCE loop editor tests passed\n";
    return 0;
}
