#include "TimelineLoopEditorComponent.h"
#include "TrackLoomMainComponent.h"
#include "AppMainMenu.h"
#include "AppMidiClipActions.h"
#include "AppProjectSession.h"
#include "support/FakeJuceAudioDeviceType.h"
#include "support/TestFailureOutput.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <latch>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
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

std::unique_ptr<trackloom::JuceAudioHost> makeFakeHost(
    std::function<void(trackloom::test::FakeJuceAudioDeviceType&)> configure = {})
{
    return std::make_unique<trackloom::JuceAudioHost>(
        [configure = std::move(configure)]() -> std::unique_ptr<juce::AudioIODeviceType> {
            auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
            if (configure) {
                configure(*type);
            }
            return type;
        });
}

class LatchRelease final {
public:
    explicit LatchRelease(std::latch& latch) noexcept : latch_(latch) {}
    ~LatchRelease() { release(); }

    void release() noexcept
    {
        if (!released_) {
            latch_.count_down();
            released_ = true;
        }
    }

private:
    std::latch& latch_;
    bool released_ = false;
};

bool waitUntil(
    const std::function<bool()>& predicate,
    std::chrono::milliseconds timeout = std::chrono::milliseconds { 5000 })
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (predicate()) {
            return true;
        }
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
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

juce::TextEditor* findDescendantTextEditorWithPlaceholder(
    juce::Component& component,
    const char* placeholder)
{
    if (auto* editor = dynamic_cast<juce::TextEditor*>(&component);
        editor != nullptr && editor->getTextToShowWhenEmpty() == juce::String::fromUTF8(placeholder)) {
        return editor;
    }
    for (int index = 0; index < component.getNumChildComponents(); ++index) {
        if (auto* child = findDescendantTextEditorWithPlaceholder(
                *component.getChildComponent(index), placeholder)) {
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
    const char* name,
    std::optional<trackloom::PlaybackLoopRange> loopRange = std::nullopt)
{
    trackloom::AppProjectSession source;
    source.createNewProject(name);
    if (withMidiClip) {
        const auto track = source.editProject().createTrack("Fixture MIDI", trackloom::TrackType::Instrument);
        const auto midi = trackloom::createDefaultMidiClipOnTrack(source, track.id);
        require(midi.success, "open fixture must contain a valid MIDI clip");
    }
    if (loopRange.has_value()) {
        require(source.editProject().setPlaybackLoopRange(loopRange),
            "open fixture must store its requested playback loop range");
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

trackloom::PlaybackLoopRange beginChangedEndPreview(
    trackloom::TimelineLoopEditorComponent& timeline)
{
    const auto handle = timeline.loopHandleBounds(trackloom::AppLoopBoundaryEdge::End);
    require(handle.has_value(), "changed-preview helper requires a stored loop end handle");
    const auto down = handle->getCentre();
    const auto dragged = juce::Point<float> {
        static_cast<float>(timeline.getWidth() - 2), down.y
    };
    timeline.mouseDown(mouseEvent(timeline, down, down));
    timeline.mouseDrag(mouseEvent(timeline, dragged, down, true));
    const auto preview = timeline.previewLoopRange();
    require(preview.has_value() && preview->endTick != 3840,
        "changed-preview helper must create a valid range distinct from the stored fixture range");
    return *preview;
}

trackloom::PreparedMidiPlaybackPlanBuildRequest captureMainPlaybackRequest(bool useSpace)
{
    std::mutex requestMutex;
    std::optional<trackloom::PreparedMidiPlaybackPlanBuildRequest> capturedRequest;
    std::latch builderStarted { 1 };
    std::latch allowBuilderToFinish { 1 };
    std::latch builderReturned { 1 };
    trackloom::TrackLoomMainComponentDependencies dependencies;
    dependencies.audioHost = makeFakeHost();
    dependencies.buildOperation = [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request,
                                      std::stop_token stopToken) {
        {
            std::scoped_lock lock(requestMutex);
            capturedRequest = request;
        }
        builderStarted.count_down();
        allowBuilderToFinish.wait();
        auto result = trackloom::buildPreparedMidiPlaybackPlan(std::move(request), stopToken);
        builderReturned.count_down();
        return result;
    };
    trackloom::TrackLoomMainComponent main(std::move(dependencies));
    LatchRelease unblockBuilder(allowBuilderToFinish);
    main.setSize(1280, 820);
    auto* createMidi = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(main, trackloom::mainCreateMidiClipButtonComponentId));
    auto* selector = dynamic_cast<juce::ComboBox*>(
        findDescendantWithId(main, trackloom::mainMidiClipSelectorComponentId));
    auto* setLoop = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(main, trackloom::mainSetLoopButtonComponentId));
    require(createMidi != nullptr && selector != nullptr && setLoop != nullptr,
        "playback request capture requires Main's real MIDI and loop controls");
    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::AddInstrumentTrack)).executed,
        "playback request capture requires a real instrument track");
    createMidi->onClick();
    createMidi->onClick();
    selector->setSelectedId(2, juce::sendNotificationSync);
    setLoop->onClick();

    const auto started = useSpace
        ? main.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey))
        : main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::PlayProject)).executed;
    require(started, "Play and Space request capture must both enter asynchronous preparation");
    require(waitUntil([&] { return builderStarted.try_wait(); }),
        "playback builder must start within the bounded test deadline");
    trackloom::PreparedMidiPlaybackPlanBuildRequest result;
    {
        std::scoped_lock lock(requestMutex);
        require(capturedRequest.has_value(), "started builder must expose its immutable request snapshot");
        result = *capturedRequest;
    }

    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::StopProject)).executed,
        "request-capture cleanup must cancel the blocked preparation through Main");
    unblockBuilder.release();
    require(waitUntil([&] { return builderReturned.try_wait(); }),
        "request-capture builder must return after its latch is released");
    main.serviceUiTimer();
    return result;
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
    saveProjectFixture(
        midiProject.path(), true, "MIDI fixture", trackloom::PlaybackLoopRange { 0, 3840 });
    saveProjectFixture(replacementMidiProject.path(), true, "Replacement MIDI fixture",
        trackloom::PlaybackLoopRange { 0, 3840 });

    std::vector<trackloom::AppOpenProjectChooserCompletion> completions;
    std::vector<std::string> titles;
    trackloom::TrackLoomMainComponentDependencies dependencies;
    dependencies.audioHost = makeHeadlessHost();
    dependencies.titleChanged = [&titles](std::string title) { titles.push_back(std::move(title)); };
    dependencies.chooseProjectToOpen = [&completions](auto completion) {
        completions.push_back(std::move(completion));
    };
    trackloom::TrackLoomMainComponent main(std::move(dependencies));
    main.setSize(1280, 820);
    auto* selector = dynamic_cast<juce::ComboBox*>(
        findDescendantWithId(main, trackloom::mainMidiClipSelectorComponentId));
    auto* timeline = dynamic_cast<trackloom::TimelineLoopEditorComponent*>(
        findDescendantWithId(main, trackloom::timelineLoopEditorComponentId));
    auto* loopToggle = dynamic_cast<juce::ToggleButton*>(
        findDescendantWithId(main, trackloom::mainLoopToggleComponentId));
    auto* setLoop = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(main, trackloom::mainSetLoopButtonComponentId));
    auto* addMidiNote = findDescendantTextButtonWithText(main, "添加默认音符");
    require(selector != nullptr && timeline != nullptr && loopToggle != nullptr && setLoop != nullptr,
        "chooser test requires the real MIDI selector, timeline, and loop controls");
    require(addMidiNote != nullptr, "chooser test requires Main's real MIDI edit control");

    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::OpenProject)).executed
            && completions.size() == 1,
        "Open must expose exactly one captured chooser completion without a native dialog");
    completions.back()(midiProject.path());
    require(selector->getNumItems() == 1 && selector->getSelectedId() == 0,
        "a successfully opened MIDI project must begin with no implicit MIDI selection");
    require(timeline->loopHandleBounds(trackloom::AppLoopBoundaryEdge::End).has_value()
            && !loopToggle->getToggleState(),
        "opening a stored v11 loop range must show it while leaving session playback disabled");

    const auto cleanTitle = titles.back();
    selector->setSelectedId(1, juce::sendNotificationSync);
    setLoop->onClick();
    require(loopToggle->getToggleState() && titles.back() == cleanTitle,
        "Set of the already-stored selected clip range must enable only the session and keep the project clean");
    loopToggle->setToggleState(false, juce::sendNotificationSync);
    loopToggle->setToggleState(true, juce::sendNotificationSync);
    require(titles.back() == cleanTitle,
        "a loop-toggle round trip must not dirty the saved project");
    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::UndoProject)).executed
            && timeline->loopHandleBounds(trackloom::AppLoopBoundaryEdge::End).has_value()
            && loopToggle->getToggleState(),
        "session-only Set and Toggle must not create history for a clean opened range");
    beginChangedEndPreview(*timeline);
    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::NewProject)).executed
            && selector->getNumItems() == 0 && selector->getSelectedId() == 0 && !addMidiNote->isEnabled()
            && !loopToggle->getToggleState() && !timeline->previewLoopRange().has_value(),
        "a successful New Project must clear selection, disable looping, and cancel preview");

    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::OpenProject)).executed
            && completions.size() == 2,
        "a New Project must leave Main able to enter the chooser seam again");
    completions.back()(midiProject.path());
    selector->setSelectedId(1, juce::sendNotificationSync);
    loopToggle->setToggleState(true, juce::sendNotificationSync);
    beginChangedEndPreview(*timeline);
    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::OpenProject)).executed
            && completions.size() == 3,
        "a selected clean project must still enter the chooser seam");
    completions.back()(replacementMidiProject.path());
    require(selector->getNumItems() == 1 && selector->getSelectedId() == 0
            && !loopToggle->getToggleState() && !timeline->previewLoopRange().has_value(),
        "a successful replacement retaining clip-1's stable ID must clear selection, loop session, and preview");

    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::OpenProject)).executed
            && completions.size() == 4,
        "the chooser must remain usable after a successful replacement");
    completions.back()(midiProject.path());
    selector->setSelectedId(1, juce::sendNotificationSync);
    loopToggle->setToggleState(true, juce::sendNotificationSync);
    const auto canceledPreview = beginChangedEndPreview(*timeline);
    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::OpenProject)).executed
            && completions.size() == 5,
        "cancel verification must capture its own completion");
    completions.back()(std::nullopt);
    require(selector->getSelectedId() == 1 && loopToggle->getToggleState()
            && timeline->previewLoopRange() == canceledPreview,
        "cancelling the chooser must preserve selection, loop session, and the changed preview");

    timeline->mouseCaptureLost();
    const auto failedPreview = beginChangedEndPreview(*timeline);
    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::OpenProject)).executed
            && completions.size() == 6,
        "open failure verification must capture its own completion");
    completions.back()(replacementMidiProject.path() / "does-not-exist.trackloom");
    require(selector->getSelectedId() == 1 && loopToggle->getToggleState()
            && timeline->previewLoopRange() == failedPreview,
        "a failed project load must preserve selection, loop session, and the changed preview");

    require(main.dispatchCommand(trackloom::appMainMenuRecentProjectCommandId(1)).executed,
        "the most recently successful Open must remain reachable through Task 7's dynamic recent command");
    require(selector->getSelectedId() == 0 && !loopToggle->getToggleState()
            && !timeline->previewLoopRange().has_value(),
        "a successful Recent replacement must use the same clear/off/cancel success boundary");

    selector->setSelectedId(1, juce::sendNotificationSync);
    addMidiNote->onClick();
    loopToggle->setToggleState(true, juce::sendNotificationSync);
    const auto dirtyPreview = beginChangedEndPreview(*timeline);
    const auto completionCountBeforeDirtyOpen = completions.size();
    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::OpenProject)).executed
            && completions.size() == completionCountBeforeDirtyOpen
            && selector->getSelectedId() == 1 && loopToggle->getToggleState()
            && timeline->previewLoopRange() == dirtyPreview,
        "dirty Open refusal must not enter the chooser or change selection, loop session, or preview");

    timeline->mouseCaptureLost();
    require(main.dispatchCommand(
                static_cast<int>(trackloom::AppMainMenuCommand::OpenCommandPalette)).executed,
        "dirty command-palette Open proof must expose the existing Task 8 command front door");
    auto* paletteQuery = findDescendantTextEditorWithPlaceholder(main, "搜索命令");
    require(paletteQuery != nullptr, "command-palette Open proof requires the real query editor");
    const auto palettePreview = beginChangedEndPreview(*timeline);
    paletteQuery->setText(juce::String::fromUTF8("打开工程"), false);
    require(static_cast<bool>(paletteQuery->onTextChange),
        "command-palette Open proof requires Main's real query callback");
    paletteQuery->onTextChange();
    require(main.keyPressed(juce::KeyPress(juce::KeyPress::returnKey))
            && completions.size() == completionCountBeforeDirtyOpen
            && timeline->previewLoopRange() == palettePreview,
        "dirty Open refusal through the command palette must not run a second full refresh that cancels preview");
}

void mainSetLoopUsesTheSelectedMidiClipAndPublishesItsRange()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    trackloom::TrackLoomMainComponentDependencies dependencies;
    dependencies.audioHost = makeHeadlessHost();
    trackloom::TrackLoomMainComponent main(std::move(dependencies));
    main.setSize(1280, 820);

    auto* createMidi = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(main, trackloom::mainCreateMidiClipButtonComponentId));
    auto* setLoop = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(main, trackloom::mainSetLoopButtonComponentId));
    auto* clearLoop = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(main, trackloom::mainClearLoopButtonComponentId));
    auto* loopToggle = dynamic_cast<juce::ToggleButton*>(
        findDescendantWithId(main, trackloom::mainLoopToggleComponentId));
    auto* timeline = dynamic_cast<trackloom::TimelineLoopEditorComponent*>(
        findDescendantWithId(main, trackloom::timelineLoopEditorComponentId));
    require(createMidi != nullptr && setLoop != nullptr && clearLoop != nullptr
            && loopToggle != nullptr && timeline != nullptr,
        "loop wiring test requires the real inspector controls and timeline");
    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::AddInstrumentTrack)).executed,
        "loop wiring test requires a real instrument track");
    createMidi->onClick();
    require(setLoop->isEnabled() && !clearLoop->isEnabled() && !loopToggle->isEnabled(),
        "only a valid selected MIDI clip may enable Set before the project has a loop range");

    setLoop->onClick();
    require(loopToggle->getToggleState() && clearLoop->isEnabled() && loopToggle->isEnabled(),
        "setting the selected MIDI clip as loop must enable the session toggle and range controls");
    require(timeline->loopHandleBounds(trackloom::AppLoopBoundaryEdge::Start).has_value()
            && timeline->loopHandleBounds(trackloom::AppLoopBoundaryEdge::End).has_value(),
        "setting the selected MIDI clip as loop must publish the project range to the real timeline");
}

void mainLoopHistoryKeepsSessionEnablementOutOfUndoRedo()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    trackloom::TrackLoomMainComponentDependencies dependencies;
    dependencies.audioHost = makeHeadlessHost();
    trackloom::TrackLoomMainComponent main(std::move(dependencies));
    main.setSize(1280, 820);

    auto* createMidi = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(main, trackloom::mainCreateMidiClipButtonComponentId));
    auto* setLoop = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(main, trackloom::mainSetLoopButtonComponentId));
    auto* clearLoop = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(main, trackloom::mainClearLoopButtonComponentId));
    auto* loopToggle = dynamic_cast<juce::ToggleButton*>(
        findDescendantWithId(main, trackloom::mainLoopToggleComponentId));
    auto* timeline = dynamic_cast<trackloom::TimelineLoopEditorComponent*>(
        findDescendantWithId(main, trackloom::timelineLoopEditorComponentId));
    require(createMidi != nullptr && setLoop != nullptr && clearLoop != nullptr
            && loopToggle != nullptr && timeline != nullptr,
        "loop history test requires the real loop controls and embedded timeline");
    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::AddInstrumentTrack)).executed,
        "loop history test requires a real instrument track");
    createMidi->onClick();
    setLoop->onClick();
    require(loopToggle->getToggleState()
            && timeline->loopHandleBounds(trackloom::AppLoopBoundaryEdge::End).has_value(),
        "Set must create the range and enable this playback session");

    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::UndoProject)).executed,
        "the real Undo command must execute after Set");
    require(!timeline->loopHandleBounds(trackloom::AppLoopBoundaryEdge::End).has_value()
            && !loopToggle->getToggleState(),
        "Set -> Undo must remove the range and reconcile the impossible enabled-without-range state");

    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::RedoProject)).executed,
        "the real Redo command must execute after undoing Set");
    require(timeline->loopHandleBounds(trackloom::AppLoopBoundaryEdge::End).has_value()
            && !loopToggle->getToggleState(),
        "Set -> Undo -> Redo must restore only the project range, not session enablement");

    setLoop->onClick();
    require(loopToggle->getToggleState(),
        "setting the same stored range while disabled must re-enable only the session");
    loopToggle->setToggleState(false, juce::sendNotificationSync);
    loopToggle->setToggleState(true, juce::sendNotificationSync);
    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::UndoProject)).executed,
        "one Undo after session-only Set and toggle changes must still target the original range command");
    require(!timeline->loopHandleBounds(trackloom::AppLoopBoundaryEdge::End).has_value()
            && !loopToggle->getToggleState(),
        "session-only Set and toggle round trips must not enter project history");

    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::RedoProject)).executed,
        "Redo must restore the range before the Clear matrix");
    require(timeline->loopHandleBounds(trackloom::AppLoopBoundaryEdge::End).has_value()
            && !loopToggle->getToggleState(),
        "redo after session-only actions must restore the range disabled");
    clearLoop->onClick();
    require(!timeline->loopHandleBounds(trackloom::AppLoopBoundaryEdge::End).has_value()
            && !loopToggle->getToggleState(),
        "Clear must remove the range and leave the session disabled");
    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::UndoProject)).executed,
        "Undo must restore the cleared range");
    require(timeline->loopHandleBounds(trackloom::AppLoopBoundaryEdge::End).has_value()
            && !loopToggle->getToggleState(),
        "Clear -> Undo must restore only the project range");
    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::RedoProject)).executed,
        "Redo must clear the restored range again");
    require(!timeline->loopHandleBounds(trackloom::AppLoopBoundaryEdge::End).has_value()
            && !loopToggle->getToggleState(),
        "Clear -> Undo -> Redo must end without a range or latent enablement");
}

void mainLoopDragCommitsOnceAndOneUndoRestoresTheStoredRange()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    trackloom::TrackLoomMainComponentDependencies dependencies;
    dependencies.audioHost = makeHeadlessHost();
    trackloom::TrackLoomMainComponent main(std::move(dependencies));
    main.setSize(1280, 820);

    auto* createMidi = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(main, trackloom::mainCreateMidiClipButtonComponentId));
    auto* setLoop = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(main, trackloom::mainSetLoopButtonComponentId));
    auto* timeline = dynamic_cast<trackloom::TimelineLoopEditorComponent*>(
        findDescendantWithId(main, trackloom::timelineLoopEditorComponentId));
    require(createMidi != nullptr && setLoop != nullptr && timeline != nullptr,
        "Main drag integration requires its real MIDI action, Set control, and timeline");
    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::AddInstrumentTrack)).executed,
        "Main drag integration requires a real instrument track");
    createMidi->onClick();
    setLoop->onClick();
    const auto originalEnd = timeline->loopHandleBounds(trackloom::AppLoopBoundaryEdge::End);
    require(originalEnd.has_value(), "Main drag integration requires the stored Set range");
    const auto down = originalEnd->getCentre();
    const auto firstDrag = juce::Point<float> {
        static_cast<float>(timeline->getWidth() - 180), down.y
    };
    const auto finalDrag = juce::Point<float> {
        static_cast<float>(timeline->getWidth() - 2), down.y
    };

    timeline->mouseDown(mouseEvent(*timeline, down, down));
    timeline->mouseDrag(mouseEvent(*timeline, firstDrag, down, true));
    timeline->mouseDrag(mouseEvent(*timeline, finalDrag, down, true));
    const auto finalPreview = timeline->previewLoopRange();
    require(finalPreview.has_value() && finalPreview->endTick > 3840,
        "multiple Main drags must publish the last valid application preview before mouse-up");
    timeline->mouseUp(mouseEvent(*timeline, finalDrag, down, true));
    const auto committedEnd = timeline->loopHandleBounds(trackloom::AppLoopBoundaryEdge::End);
    require(!timeline->previewLoopRange().has_value() && committedEnd.has_value()
            && committedEnd != originalEnd,
        "one mouse-up must commit the last preview and refresh the stored timeline range");

    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::UndoProject)).executed,
        "one real Undo must be available after the drag commit");
    require(timeline->loopHandleBounds(trackloom::AppLoopBoundaryEdge::End) == originalEnd,
        "one Undo after multiple drag events and one mouse-up must restore the old stored range");
}

void mainCaptureLossCancelsAChangedPreviewWithoutCommitOrHistory()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    trackloom::TrackLoomMainComponentDependencies dependencies;
    dependencies.audioHost = makeHeadlessHost();
    trackloom::TrackLoomMainComponent main(std::move(dependencies));
    main.setSize(1280, 820);

    auto* createMidi = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(main, trackloom::mainCreateMidiClipButtonComponentId));
    auto* setLoop = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(main, trackloom::mainSetLoopButtonComponentId));
    auto* loopToggle = dynamic_cast<juce::ToggleButton*>(
        findDescendantWithId(main, trackloom::mainLoopToggleComponentId));
    auto* timeline = dynamic_cast<trackloom::TimelineLoopEditorComponent*>(
        findDescendantWithId(main, trackloom::timelineLoopEditorComponentId));
    require(createMidi != nullptr && setLoop != nullptr && loopToggle != nullptr && timeline != nullptr,
        "Main capture cancellation requires its real loop controls and embedded timeline");
    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::AddInstrumentTrack)).executed,
        "Main capture cancellation requires a real instrument track");
    createMidi->onClick();
    setLoop->onClick();
    const auto storedEnd = timeline->loopHandleBounds(trackloom::AppLoopBoundaryEdge::End);
    require(storedEnd.has_value(), "capture cancellation requires a stored Set range");
    const auto changedPreview = beginChangedEndPreview(*timeline);
    require(changedPreview != trackloom::PlaybackLoopRange { 0, 3840 },
        "capture cancellation proof must not use an unchanged preview");

    const auto laterMouseUp = juce::Point<float> {
        static_cast<float>(timeline->getWidth() - 2), storedEnd->getCentreY()
    };
    timeline->mouseCaptureLost();
    timeline->mouseUp(mouseEvent(*timeline, laterMouseUp, storedEnd->getCentre(), true));
    require(!timeline->previewLoopRange().has_value()
            && timeline->loopHandleBounds(trackloom::AppLoopBoundaryEdge::End) == storedEnd,
        "public capture loss followed by mouse-up must clear preview without changing the stored range");

    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::UndoProject)).executed,
        "Undo after capture loss must still target the original Set command");
    require(!timeline->loopHandleBounds(trackloom::AppLoopBoundaryEdge::End).has_value()
            && !loopToggle->getToggleState(),
        "capture loss must add no commit: one Undo must remove the original Set range entirely");
}

void mainPlayAndSpaceCaptureTheSameLoopRangeAndResolvedStart()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    const auto playRequest = captureMainPlaybackRequest(false);
    const auto spaceRequest = captureMainPlaybackRequest(true);
    const auto expectedRange = trackloom::PlaybackLoopRange { 3840, 7680 };
    require(playRequest.loopRange == expectedRange && spaceRequest.loopRange == expectedRange,
        "Play and Space must capture the same selected second-clip loop range");
    require(playRequest.playbackStartSample == 96000
            && spaceRequest.playbackStartSample == 96000
            && playRequest.playbackStartSample == spaceRequest.playbackStartSample,
        "Play and Space must both resolve the 3840-tick loop start to 96000 samples at 48 kHz / 120 BPM");
}

void mainPreparingLoopChangeInvalidatesTheWorkerAndNewPreservesUiState()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    TemporaryProjectFile fixture;
    saveProjectFixture(
        fixture.path(), true, "Preparing fixture", trackloom::PlaybackLoopRange { 0, 3840 });
    std::vector<trackloom::AppOpenProjectChooserCompletion> completions;
    std::vector<std::string> titles;
    std::latch builderStarted { 1 };
    std::latch allowBuilderToFinish { 1 };
    std::latch builderReturned { 1 };
    trackloom::JuceAudioHost* observedHost = nullptr;
    trackloom::TrackLoomMainComponentDependencies dependencies;
    dependencies.audioHost = makeFakeHost();
    observedHost = dependencies.audioHost.get();
    dependencies.chooseProjectToOpen = [&completions](auto completion) {
        completions.push_back(std::move(completion));
    };
    dependencies.titleChanged = [&titles](std::string title) { titles.push_back(std::move(title)); };
    dependencies.buildOperation = [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request,
                                      std::stop_token stopToken) {
        builderStarted.count_down();
        allowBuilderToFinish.wait();
        auto result = trackloom::buildPreparedMidiPlaybackPlan(std::move(request), stopToken);
        builderReturned.count_down();
        return result;
    };
    trackloom::TrackLoomMainComponent main(std::move(dependencies));
    LatchRelease unblockBuilder(allowBuilderToFinish);
    main.setSize(1280, 820);
    auto* selector = dynamic_cast<juce::ComboBox*>(
        findDescendantWithId(main, trackloom::mainMidiClipSelectorComponentId));
    auto* loopToggle = dynamic_cast<juce::ToggleButton*>(
        findDescendantWithId(main, trackloom::mainLoopToggleComponentId));
    auto* loopIntent = dynamic_cast<juce::Label*>(
        findDescendantWithId(main, trackloom::mainLoopIntentStatusComponentId));
    auto* timeline = dynamic_cast<trackloom::TimelineLoopEditorComponent*>(
        findDescendantWithId(main, trackloom::timelineLoopEditorComponentId));
    require(selector != nullptr && loopToggle != nullptr && loopIntent != nullptr && timeline != nullptr,
        "Preparing integration requires Main's real selector, toggle, intent label, and timeline");
    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::OpenProject)).executed
            && completions.size() == 1,
        "Preparing fixture must enter the injected chooser exactly once");
    completions.back()(fixture.path());
    selector->setSelectedId(1, juce::sendNotificationSync);
    loopToggle->setToggleState(true, juce::sendNotificationSync);
    const auto projectTitle = titles.back();
    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::PlayProject)).executed,
        "Preparing integration must start playback through Main's Play command");
    require(waitUntil([&] { return builderStarted.try_wait(); }),
        "Preparing integration builder must start within the bounded deadline");
    require(main.dispatchCommand(
                static_cast<int>(trackloom::AppMainMenuCommand::OpenCommandPalette)).executed,
        "Preparing disabled-command proof must open the real command palette");
    auto* paletteQuery = findDescendantTextEditorWithPlaceholder(main, "搜索命令");
    require(paletteQuery != nullptr,
        "Preparing disabled-command proof requires the real command-palette query editor");
    const auto changedPreview = beginChangedEndPreview(*timeline);
    paletteQuery->setText(juce::String::fromUTF8("新建工程"), false);
    require(static_cast<bool>(paletteQuery->onTextChange),
        "Preparing disabled-command proof requires Main's real query callback");
    paletteQuery->onTextChange();
    require(main.keyPressed(juce::KeyPress(juce::KeyPress::returnKey))
            && timeline->previewLoopRange() == changedPreview,
        "a disabled Preparing replacement command must report refusal without a full refresh that cancels preview");

    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::NewProject)).executed,
        "New command must dispatch even when its replacement action refuses Preparing");
    require(titles.back() == projectTitle && selector->getNumItems() == 1
            && selector->getSelectedId() == 1 && loopToggle->getToggleState()
            && timeline->previewLoopRange() == changedPreview
            && timeline->loopHandleBounds(trackloom::AppLoopBoundaryEdge::End).has_value(),
        "Preparing New refusal must preserve project, MIDI selection, loop session, and changed preview");
    require(main.dispatchCommand(trackloom::appMainMenuRecentProjectCommandId(1)).executed
            && titles.back() == projectTitle && selector->getSelectedId() == 1
            && loopToggle->getToggleState() && timeline->previewLoopRange() == changedPreview,
        "Preparing Recent refusal must preserve the same project, selection, loop session, and changed preview");

    loopToggle->setToggleState(false, juce::sendNotificationSync);
    require(loopIntent->getText() == juce::String::fromUTF8("循环设置已变化，请重新播放")
            && !observedHost->snapshot().planInstalled,
        "changing loop intent during Preparing must immediately reject the old worker and show the controller message");
    unblockBuilder.release();
    require(waitUntil([&] { return builderReturned.try_wait(); }),
        "invalidated Preparing worker must return after its latch is released");
    require(waitUntil([&] {
        main.serviceUiTimer();
        return !observedHost->snapshot().planInstalled;
    }), "invalidated old worker must never install a playback plan");
    require(loopIntent->getText() == juce::String::fromUTF8("循环设置已变化，请重新播放"),
        "PreparationInvalidated message must be derived from controller status after worker cleanup");
}

void mainOpenCompletionRechecksPreparingAndPreservesEveryUiState()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    TemporaryProjectFile fixture;
    TemporaryProjectFile replacement;
    saveProjectFixture(
        fixture.path(), true, "Completion fixture", trackloom::PlaybackLoopRange { 0, 3840 });
    saveProjectFixture(replacement.path(), true, "Should not open",
        trackloom::PlaybackLoopRange { 0, 3840 });
    std::vector<trackloom::AppOpenProjectChooserCompletion> completions;
    std::vector<std::string> titles;
    std::latch builderStarted { 1 };
    std::latch allowBuilderToFinish { 1 };
    std::latch builderReturned { 1 };
    trackloom::JuceAudioHost* observedHost = nullptr;
    trackloom::TrackLoomMainComponentDependencies dependencies;
    dependencies.audioHost = makeFakeHost();
    observedHost = dependencies.audioHost.get();
    dependencies.chooseProjectToOpen = [&completions](auto completion) {
        completions.push_back(std::move(completion));
    };
    dependencies.titleChanged = [&titles](std::string title) { titles.push_back(std::move(title)); };
    dependencies.buildOperation = [&](trackloom::PreparedMidiPlaybackPlanBuildRequest request,
                                      std::stop_token stopToken) {
        builderStarted.count_down();
        allowBuilderToFinish.wait();
        auto result = trackloom::buildPreparedMidiPlaybackPlan(std::move(request), stopToken);
        builderReturned.count_down();
        return result;
    };
    trackloom::TrackLoomMainComponent main(std::move(dependencies));
    LatchRelease unblockBuilder(allowBuilderToFinish);
    main.setSize(1280, 820);
    auto* selector = dynamic_cast<juce::ComboBox*>(
        findDescendantWithId(main, trackloom::mainMidiClipSelectorComponentId));
    auto* loopToggle = dynamic_cast<juce::ToggleButton*>(
        findDescendantWithId(main, trackloom::mainLoopToggleComponentId));
    auto* timeline = dynamic_cast<trackloom::TimelineLoopEditorComponent*>(
        findDescendantWithId(main, trackloom::timelineLoopEditorComponentId));
    require(selector != nullptr && loopToggle != nullptr && timeline != nullptr,
        "completion-gate integration requires Main's real selector, toggle, and timeline");
    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::OpenProject)).executed
            && completions.size() == 1,
        "completion-gate fixture must open through the one chooser seam");
    completions.back()(fixture.path());
    selector->setSelectedId(1, juce::sendNotificationSync);
    loopToggle->setToggleState(true, juce::sendNotificationSync);
    const auto projectTitle = titles.back();
    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::OpenProject)).executed
            && completions.size() == 2,
        "Open's first safety gate must capture a completion while playback is stopped");
    auto pendingCompletion = completions.back();
    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::PlayProject)).executed,
        "completion-gate integration must enter Preparing after chooser capture");
    require(waitUntil([&] { return builderStarted.try_wait(); }),
        "completion-gate builder must start within the bounded deadline");
    const auto changedPreview = beginChangedEndPreview(*timeline);

    pendingCompletion(replacement.path());
    require(titles.back() == projectTitle && selector->getNumItems() == 1
            && selector->getSelectedId() == 1 && loopToggle->getToggleState()
            && timeline->previewLoopRange() == changedPreview
            && !observedHost->snapshot().planInstalled,
        "Open completion's second safety gate must refuse Preparing without changing project, selection, loop, or preview");

    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::StopProject)).executed,
        "completion-gate cleanup must cancel Preparing through Main");
    unblockBuilder.release();
    require(waitUntil([&] { return builderReturned.try_wait(); }),
        "completion-gate builder must return after cleanup releases its latch");
    main.serviceUiTimer();
}

void mainPlayingLoopChangeKeepsPlayingAndShowsTheControllerMessage()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::JuceAudioHost* observedHost = nullptr;
    trackloom::TrackLoomMainComponentDependencies dependencies;
    dependencies.audioHost = makeFakeHost(
        [&](trackloom::test::FakeJuceAudioDeviceType& type) { observedType = &type; });
    observedHost = dependencies.audioHost.get();
    trackloom::TrackLoomMainComponent main(std::move(dependencies));
    main.setSize(1280, 820);
    auto* createMidi = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(main, trackloom::mainCreateMidiClipButtonComponentId));
    auto* setLoop = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(main, trackloom::mainSetLoopButtonComponentId));
    auto* loopToggle = dynamic_cast<juce::ToggleButton*>(
        findDescendantWithId(main, trackloom::mainLoopToggleComponentId));
    auto* loopIntent = dynamic_cast<juce::Label*>(
        findDescendantWithId(main, trackloom::mainLoopIntentStatusComponentId));
    require(createMidi != nullptr && setLoop != nullptr && loopToggle != nullptr && loopIntent != nullptr
            && observedType != nullptr && observedType->activeDevice() != nullptr,
        "Playing integration requires real Main loop controls and a live fake audio device");
    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::AddInstrumentTrack)).executed,
        "Playing integration requires a real instrument track");
    createMidi->onClick();
    setLoop->onClick();
    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::PlayProject)).executed,
        "Playing integration must start through Main's Play command");
    require(waitUntil([&] {
        main.serviceUiTimer();
        return observedHost->snapshot().planInstalled
            && observedHost->snapshot().realtime.state == trackloom::RealtimePlaybackState::Playing;
    }), "fake-device preparation must install and enter Playing within the bounded deadline");

    loopToggle->setToggleState(false, juce::sendNotificationSync);
    const auto playingAfterChange = observedHost->snapshot();
    require(playingAfterChange.planInstalled
            && playingAfterChange.realtime.state == trackloom::RealtimePlaybackState::Playing
            && loopIntent->getText().contains(juce::String::fromUTF8("停止并重新播放后生效")),
        "changing loop intent during Playing must keep playback active and show the controller-derived restart message");

    require(main.dispatchCommand(static_cast<int>(trackloom::AppMainMenuCommand::StopProject)).executed,
        "Playing integration cleanup must request Stop through Main");
    require(waitUntil([&] {
        if (observedType->activeDevice() != nullptr) {
            observedType->activeDevice()->runCallback(256);
        }
        main.serviceUiTimer();
        const auto snapshot = observedHost->snapshot();
        return snapshot.realtime.state == trackloom::RealtimePlaybackState::Stopped
            && !snapshot.callbackRunning;
    }), "Playing integration cleanup must drain the fake callback and reach Stopped without hanging");
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
        run("main-loop-set", mainSetLoopUsesTheSelectedMidiClipAndPublishesItsRange);
        run("main-loop-history", mainLoopHistoryKeepsSessionEnablementOutOfUndoRedo);
        run("main-loop-drag-history", mainLoopDragCommitsOnceAndOneUndoRestoresTheStoredRange);
        run("main-capture-cancel", mainCaptureLossCancelsAChangedPreviewWithoutCommitOrHistory);
        run("main-play-space", mainPlayAndSpaceCaptureTheSameLoopRangeAndResolvedStart);
        run("main-preparing-loop", mainPreparingLoopChangeInvalidatesTheWorkerAndNewPreservesUiState);
        run("main-open-second-gate", mainOpenCompletionRechecksPreparingAndPreservesEveryUiState);
        run("main-playing-loop", mainPlayingLoopChangeKeepsPlayingAndShowsTheControllerMessage);
    } catch (const std::exception& error) {
        std::cerr << "D1 JUCE loop editor test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "D1 JUCE loop editor tests passed\n";
    return 0;
}
