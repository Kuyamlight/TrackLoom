#include "AudioSettingsComponent.h"
#include "AppPlaybackActions.h"
#include "AppProjectSession.h"
#include "TrackLoomMainComponent.h"
#include "support/FakeJuceAudioDeviceType.h"

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <cstdlib>
#endif

#include <iostream>
#include <filesystem>
#include <functional>
#include <latch>
#include <atomic>
#include <chrono>
#include <thread>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void configureTestFailureOutput()
{
#if defined(_MSC_VER)
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

void pumpGuiMessagesOnce(int milliseconds = 100)
{
    juce::Timer::callAfterDelay(milliseconds, [] {
        juce::MessageManager::getInstance()->stopDispatchLoop();
    });
    juce::MessageManager::getInstance()->runDispatchLoop();
}

void audioSettingsComponentExposesDedicatedSharedWasapiControls()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    trackloom::JuceAudioHost host([]() -> std::unique_ptr<juce::AudioIODeviceType> {
        return std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
    });
    trackloom::AudioSettingsComponent component(host, trackloom::AppAudioSettings {});
    component.refreshFromHost();
    require(dynamic_cast<juce::ComboBox*>(
                component.findChildWithID(trackloom::audioDeviceSelectorComponentId)) != nullptr,
            "dedicated audio device selector should be test-visible");
    require(dynamic_cast<juce::TextButton*>(
                component.findChildWithID(trackloom::audioTestToneButtonComponentId)) != nullptr,
            "dedicated test tone button should be test-visible");
}

std::unique_ptr<const trackloom::PreparedMidiPlaybackPlan> makeAudiblePlan(
    const trackloom::AudioDeviceFormatSnapshot& format)
{
    auto plan = std::make_unique<trackloom::PreparedMidiPlaybackPlan>();
    plan->sampleRate = format.sampleRate;
    plan->maximumBlockFrames = format.maximumBlockFrames;
    plan->outputChannelCount = format.outputChannelCount;
    plan->outputChannelMask = format.outputChannelMask;
    plan->instrumentSlots.push_back({
        trackloom::PreparedMidiInstrumentKind::BuiltInSine, 0.25f, 0.0f, 0
    });
    return plan;
}

void audioSettingsComponentShowsOnlySharedWasapiFormatsAndChannels()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    trackloom::JuceAudioHost host([]() -> std::unique_ptr<juce::AudioIODeviceType> {
        auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        type->setOutputDevices({"Shared A", "Shared B"}, 1);
        type->setAvailableFormats({44100.0, 48000.0}, {128, 256}, 256);
        return type;
    });
    trackloom::AudioSettingsComponent component(host, {});

    auto* device = dynamic_cast<juce::ComboBox*>(
        component.findChildWithID(trackloom::audioDeviceSelectorComponentId));
    auto* sampleRate = dynamic_cast<juce::ComboBox*>(
        component.findChildWithID(trackloom::audioSampleRateSelectorComponentId));
    auto* buffer = dynamic_cast<juce::ComboBox*>(
        component.findChildWithID(trackloom::audioBufferSelectorComponentId));
    auto* channels = dynamic_cast<juce::ComboBox*>(
        component.findChildWithID(trackloom::audioChannelsSelectorComponentId));
    require(device != nullptr && sampleRate != nullptr && buffer != nullptr && channels != nullptr,
        "all dedicated shared WASAPI selectors should have stable ComboBox ids");
    require(device->getNumItems() == 2
            && device->getItemText(0).contains("Windows Audio")
            && device->getItemText(1).contains("Windows Audio"),
        "device selector should expose only Windows Audio shared outputs");
    require(sampleRate->getNumItems() == 2
            && sampleRate->getItemText(0) == "44100"
            && sampleRate->getItemText(1) == "48000",
        "sample-rate selector should expose the shared device advertised formats");
    require(buffer->getNumItems() == 2
            && buffer->getItemText(0) == "128"
            && buffer->getItemText(1) == "256",
        "buffer selector should expose the shared device advertised sizes");
    require(channels->getNumItems() == 2
            && channels->getItemText(0) == "1"
            && channels->getItemText(1) == "2",
        "channel selector should expose only one or two output channels");
}

void audioSettingsComponentDisablesApplyAndToneDuringPlayback()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    trackloom::JuceAudioHost host([]() -> std::unique_ptr<juce::AudioIODeviceType> {
        return std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
    });
    require(host.openOutput({}).success, "playing gate test output should open");
    require(host.installAndStart(makeAudiblePlan(host.deviceFormatSnapshot())).success,
        "playing gate test plan should start");
    trackloom::AudioSettingsComponent component(host, {});
    component.refreshFromHost();

    auto* apply = dynamic_cast<juce::TextButton*>(
        component.findChildWithID(trackloom::audioApplyButtonComponentId));
    auto* tone = dynamic_cast<juce::TextButton*>(
        component.findChildWithID(trackloom::audioTestToneButtonComponentId));
    require(apply != nullptr && tone != nullptr,
        "apply and test-tone controls should be real TextButtons");
    require(!apply->isEnabled() && !tone->isEnabled(),
        "Apply and test tone must be disabled while project playback is active");
    require(!component.applySelectedSettings() && !component.triggerTestTone(),
        "disabled audio settings actions must reject direct method calls too");
}

void audioSettingsComponentDisablesApplyAndToneDuringStoppingTail()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    trackloom::JuceAudioHost host([]() -> std::unique_ptr<juce::AudioIODeviceType> {
        return std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
    });
    require(host.openOutput({}).success, "stopping gate output should open");
    require(host.playTestTone().success, "stopping gate test tone should start");
    require(host.requestStop()
            && host.snapshot().realtime.state == trackloom::RealtimePlaybackState::Stopping,
        "stopping gate requires an active release tail");
    trackloom::AudioSettingsComponent component(host, {});

    auto* apply = dynamic_cast<juce::TextButton*>(
        component.findChildWithID(trackloom::audioApplyButtonComponentId));
    auto* tone = dynamic_cast<juce::TextButton*>(
        component.findChildWithID(trackloom::audioTestToneButtonComponentId));
    require(apply != nullptr && tone != nullptr
            && !apply->isEnabled() && !tone->isEnabled(),
        "Apply and test tone must remain disabled during the stopping tail");
}

void audioSettingsComponentTracksHostStateWithoutManualRefresh()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    trackloom::JuceAudioHost host([]() -> std::unique_ptr<juce::AudioIODeviceType> {
        return std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
    });
    require(host.openOutput({}).success, "automatic settings gate output should open");
    trackloom::AudioSettingsComponent component(host, {});
    auto* apply = dynamic_cast<juce::TextButton*>(
        component.findChildWithID(trackloom::audioApplyButtonComponentId));
    auto* tone = dynamic_cast<juce::TextButton*>(
        component.findChildWithID(trackloom::audioTestToneButtonComponentId));
    require(apply != nullptr && tone != nullptr && apply->isEnabled() && tone->isEnabled(),
        "Stopped settings controls should begin enabled");

    require(host.installAndStart(makeAudiblePlan(host.deviceFormatSnapshot())).success,
        "automatic settings gate plan should start");
    pumpGuiMessagesOnce();
    require(!apply->isEnabled() && !tone->isEnabled(),
        "an already-open settings window must disable actions when playback starts");
}

void audioSettingsComponentRecoversAfterTestToneStops()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::JuceAudioHost host([&]() -> std::unique_ptr<juce::AudioIODeviceType> {
        auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        observedType = type.get();
        return type;
    });
    require(host.openOutput({}).success, "test-tone recovery output should open");
    trackloom::AudioSettingsComponent component(host, {});
    auto* apply = dynamic_cast<juce::TextButton*>(
        component.findChildWithID(trackloom::audioApplyButtonComponentId));
    auto* tone = dynamic_cast<juce::TextButton*>(
        component.findChildWithID(trackloom::audioTestToneButtonComponentId));
    require(apply != nullptr && tone != nullptr,
        "test-tone recovery controls should be real TextButtons");

    require(component.triggerTestTone(), "test-tone recovery playback should start");
    require(!apply->isEnabled() && !tone->isEnabled(),
        "test-tone playback must disable both settings actions");
    for (int callback = 0;
         callback < 128
             && host.snapshot().realtime.state != trackloom::RealtimePlaybackState::Stopped;
         ++callback) {
        observedType->activeDevice()->runCallback(256);
        host.serviceNonRealtime();
    }
    require(host.snapshot().realtime.state == trackloom::RealtimePlaybackState::Stopped,
        "bounded test tone should reach Stopped in the fake backend");
    pumpGuiMessagesOnce();
    require(apply->isEnabled() && tone->isEnabled(),
        "settings actions must recover automatically after the test tone tail stops");
}

void audioSettingsComponentTestToneTouchesOnlyTheHost()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::JuceAudioHost host([&]() -> std::unique_ptr<juce::AudioIODeviceType> {
        auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        observedType = type.get();
        return type;
    });
    require(host.openOutput({}).success, "test-tone isolation output should open");
    observedType->clearCalls();
    trackloom::AppProjectSession session;
    trackloom::AppPlaybackController playback(host);
    const auto generationBefore = session.projectEditGeneration();
    const auto dirtyBefore = session.isDirty();
    const auto canUndoBefore = session.canUndoProjectEdit();
    const auto playbackStartBefore = playback.currentSample();
    trackloom::AudioSettingsComponent component(host, {});
    observedType->clearCalls();

    require(component.triggerTestTone(),
        "Stopped audio settings should ask the host to play the bounded test tone");
    require(observedType->calls() == std::vector<std::string>({"stop", "start"}),
        "test tone must call the host start protocol exactly once");
    require(session.projectEditGeneration() == generationBefore
            && session.isDirty() == dirtyBefore
            && session.canUndoProjectEdit() == canUndoBefore
            && playback.currentSample() == playbackStartBefore,
        "test tone must not change project generation, dirty, history, or playback start");
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

bool componentTreeContainsText(const juce::Component& component, const juce::String& text)
{
    if (const auto* label = dynamic_cast<const juce::Label*>(&component);
        label != nullptr && label->getText().contains(text)) {
        return true;
    }
    for (int index = 0; index < component.getNumChildComponents(); ++index) {
        if (componentTreeContainsText(*component.getChildComponent(index), text)) {
            return true;
        }
    }
    return false;
}

juce::Component* findDescendantWithId(juce::Component& component, const char* id)
{
    if (component.getComponentID() == id) {
        return &component;
    }
    for (int index = 0; index < component.getNumChildComponents(); ++index) {
        if (auto* found = findDescendantWithId(*component.getChildComponent(index), id)) {
            return found;
        }
    }
    return nullptr;
}

void trackLoomMainComponentRejectsANullAudioHost()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    bool rejected = false;
    try {
        trackloom::TrackLoomMainComponent component({});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "TrackLoom main component must reject a null audioHost dependency");
}

void trackLoomMainComponentRepairsEmptyOperationsAndShowsStableControlIds()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    trackloom::TrackLoomMainComponentDependencies dependencies;
    dependencies.audioHost = makeFakeHost([](auto& type) { type.setOutputDevices({}); });
    dependencies.buildOperation = {};
    dependencies.loadAudioSettings = {};
    dependencies.saveAudioSettings = {};
    trackloom::TrackLoomMainComponent component(std::move(dependencies));

    auto* newButton = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(component, trackloom::mainNewButtonComponentId));
    auto* openButton = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(component, trackloom::mainOpenButtonComponentId));
    auto* saveButton = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(component, trackloom::mainSaveButtonComponentId));
    auto* playButton = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(component, trackloom::mainPlayButtonComponentId));
    auto* status = dynamic_cast<juce::Label*>(
        findDescendantWithId(component, trackloom::mainPlaybackStatusComponentId));
    require(newButton != nullptr && openButton != nullptr && saveButton != nullptr
            && playButton != nullptr && status != nullptr,
        "main project/playback controls must expose the stable real JUCE component types");
    require(newButton->isEnabled() && openButton->isEnabled() && saveButton->isEnabled(),
        "New, Open, and Save must remain enabled when no output device exists");
    require(!playButton->isEnabled(),
        "Play must be disabled when the shared output device is unavailable");
}

void trackLoomMainComponentEnablesPlayWhenStartupOutputOpens()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    trackloom::TrackLoomMainComponentDependencies dependencies;
    dependencies.audioHost = makeFakeHost();
    trackloom::TrackLoomMainComponent component(std::move(dependencies));

    auto* playButton = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(component, trackloom::mainPlayButtonComponentId));
    require(playButton != nullptr && playButton->isEnabled(),
        "Play must be enabled immediately when startup opens a shared output");
}

void trackLoomMainComponentPollsDeviceRemovalWhileIdle()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::TrackLoomMainComponentDependencies dependencies;
    dependencies.audioHost = makeFakeHost(
        [&](trackloom::test::FakeJuceAudioDeviceType& type) { observedType = &type; });
    trackloom::TrackLoomMainComponent component(std::move(dependencies));
    auto* playButton = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(component, trackloom::mainPlayButtonComponentId));
    require(playButton != nullptr && playButton->isEnabled(),
        "idle device-removal test requires an initially available output");

    observedType->setOutputDevices({});
    observedType->notifyDeviceListChanged();
    pumpGuiMessagesOnce(100);

    require(!playButton->isEnabled(),
        "the always-running UI timer must disable Play after idle device removal");
}

void trackLoomMainComponentFallsBackAfterInvalidSettingsWithAChineseWarning()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    const std::filesystem::path expectedPath = "C:/fake/TrackLoom/audio-settings.txt";
    int loadCalls = 0;
    std::filesystem::path observedPath;
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::TrackLoomMainComponentDependencies dependencies;
    dependencies.audioHost = makeFakeHost(
        [&](trackloom::test::FakeJuceAudioDeviceType& type) { observedType = &type; });
    dependencies.audioSettingsPath = expectedPath;
    dependencies.loadAudioSettings = [&](const std::filesystem::path& path) {
        ++loadCalls;
        observedPath = path;
        return trackloom::AppAudioSettingsLoadResult {
            trackloom::AppAudioSettingsLoadKind::Invalid,
            {"bad", 1.0, 1, 1},
            "音频设置文件无效，已恢复默认设置。"
        };
    };
    trackloom::TrackLoomMainComponent component(std::move(dependencies));

    require(loadCalls == 1 && observedPath == expectedPath,
        "main component startup must load the injected audio settings path exactly once");
    require(componentTreeContainsText(component, juce::String::fromUTF8("音频设置文件无效")),
        "invalid audio settings must fall back and surface a Chinese warning");
    require(observedType != nullptr && observedType->openCalls().size() == 1
            && observedType->openCalls().front().sampleRate == 48000.0
            && observedType->openCalls().front().bufferFrames == 256
            && observedType->openCalls().front().outputChannelCount == 2,
        "invalid audio settings must open the default shared format rather than invalid values");
}

void trackLoomMainComponentDispatches1303ToExactlyOneSettingsPresenter()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    int presentationCalls = 0;
    std::unique_ptr<trackloom::AudioSettingsComponent> presented;
    trackloom::TrackLoomMainComponentDependencies dependencies;
    dependencies.audioHost = makeFakeHost();
    dependencies.presentAudioSettings = [&](auto component) {
        ++presentationCalls;
        presented = std::move(component);
    };
    trackloom::TrackLoomMainComponent component(std::move(dependencies));

    const auto result = component.dispatchCommand(1303);

    require(result.executed
            && result.kind == trackloom::AppCommandDispatchResultKind::Executed
            && result.command == trackloom::AppCommandKind::OpenAudioSettings,
        "dispatchCommand(1303) must execute the real OpenAudioSettings handler");
    require(presentationCalls == 1 && presented != nullptr,
        "dispatchCommand(1303) must deliver exactly one dedicated settings component");
}

void trackLoomMainComponentShowsPreparingWithoutDrivingTheAudioCallback()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    std::latch builderStarted {1};
    std::latch allowBuilderToFinish {1};
    trackloom::TrackLoomMainComponentDependencies dependencies;
    dependencies.audioHost = std::make_unique<trackloom::JuceAudioHost>(
        [&]() -> std::unique_ptr<juce::AudioIODeviceType> {
            auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
            observedType = type.get();
            return type;
        });
    auto* const observedHost = dependencies.audioHost.get();
    dependencies.buildOperation = [&](
        trackloom::PreparedMidiPlaybackPlanBuildRequest request,
        std::stop_token stopToken) {
        builderStarted.count_down();
        allowBuilderToFinish.wait();
        return trackloom::buildPreparedMidiPlaybackPlan(std::move(request), stopToken);
    };
    trackloom::TrackLoomMainComponent component(std::move(dependencies));
    component.serviceUiTimer();
    const auto transportBefore = observedHost->snapshot().realtime.projectSamplePosition;
    const auto callbackCountBefore = observedHost->snapshot().realtime.callbackCount;

    const auto dispatched = component.dispatchCommand(1101);
    builderStarted.wait();
    auto* status = dynamic_cast<juce::Label*>(
        findDescendantWithId(component, trackloom::mainPlaybackStatusComponentId));
    const auto preparingWasVisible = dispatched.executed && status != nullptr
        && status->getText().contains(juce::String::fromUTF8("正在准备音频…"));

    for (int poll = 0; poll < 5; ++poll) {
        component.serviceUiTimer();
    }
    const auto callbackWasNotDriven =
        observedHost->snapshot().realtime.callbackCount == callbackCountBefore;
    const auto transportDidNotAdvance = transportBefore == 0
        && observedHost->snapshot().realtime.projectSamplePosition == transportBefore;
    allowBuilderToFinish.count_down();

    require(preparingWasVisible,
        "blocked playback preparation must show 正在准备音频…");
    require(callbackWasNotDriven,
        "serviceUiTimer must poll only and never simulate an audio device callback");
    require(transportDidNotAdvance,
        "serviceUiTimer must not advance transport while the builder is blocked");
}

void trackLoomMainComponentTimerRefreshesOnlyPlaybackPresentation()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    int titleChangeCount = 0;
    trackloom::TrackLoomMainComponentDependencies dependencies;
    dependencies.audioHost = makeFakeHost();
    dependencies.titleChanged = [&](std::string) { ++titleChangeCount; };
    trackloom::TrackLoomMainComponent component(std::move(dependencies));
    const auto titleChangesAfterConstruction = titleChangeCount;

    component.serviceUiTimer();

    require(titleChangeCount == titleChangesAfterConstruction,
        "UI timer must not rerun project/title refresh while polling playback");
}

void trackLoomMainComponentShowsEveryRealtimeDiagnosticField()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::TrackLoomMainComponentDependencies dependencies;
    dependencies.audioHost = std::make_unique<trackloom::JuceAudioHost>(
        [&]() -> std::unique_ptr<juce::AudioIODeviceType> {
            auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
            observedType = type.get();
            return type;
        });
    trackloom::TrackLoomMainComponent component(std::move(dependencies));
    observedType->setXRunCount(7);
    component.serviceUiTimer();

    auto* status = dynamic_cast<juce::Label*>(
        findDescendantWithId(component, trackloom::mainPlaybackStatusComponentId));
    require(status != nullptr, "diagnostic playback status should be a test-visible Label");
    const auto text = status->getText();
    require(text.contains("Fake Speakers")
            && text.contains("48000")
            && text.contains("256")
            && text.contains(juce::String::fromUTF8("声道"))
            && text.contains("callback")
            && text.contains("timeout")
            && text.contains("oversized")
            && text.contains("xrun 7")
            && text.contains("voice stealing")
            && text.contains("stale Note Off"),
        "diagnostics must show device, format, callbacks, timeout, oversized, xrun, voice stealing, and stale Note Off");

    observedType->setXRunCount(-1);
    component.serviceUiTimer();
    require(status->getText().contains(juce::String::fromUTF8("xrun 后端未提供")),
        "diagnostics must describe an unavailable backend xrun counter");
}

void trackLoomMainComponentSavesAppliedSettingsOnlyAfterHostSuccess()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    const std::filesystem::path expectedPath = "C:/fake/TrackLoom/audio-settings.txt";
    int saveCalls = 0;
    std::filesystem::path savedPath;
    trackloom::AppAudioSettings savedSettings;
    std::unique_ptr<trackloom::AudioSettingsComponent> presented;
    trackloom::TrackLoomMainComponentDependencies dependencies;
    dependencies.audioHost = std::make_unique<trackloom::JuceAudioHost>(
        [&]() -> std::unique_ptr<juce::AudioIODeviceType> {
            auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
            observedType = type.get();
            return type;
        });
    dependencies.audioSettingsPath = expectedPath;
    dependencies.saveAudioSettings = [&](
        const trackloom::AppAudioSettings& settings,
        const std::filesystem::path& path) {
        ++saveCalls;
        savedPath = path;
        savedSettings = settings;
        return true;
    };
    dependencies.presentAudioSettings = [&](auto component) {
        presented = std::move(component);
    };
    trackloom::TrackLoomMainComponent component(std::move(dependencies));
    require(component.dispatchCommand(1303).executed && presented != nullptr,
        "audio settings save test must receive the dedicated presenter component");

    observedType->setOpenShouldFail(true);
    require(!presented->applySelectedSettings() && saveCalls == 0,
        "failed host Apply must not save audio settings");
    observedType->setOpenShouldFail(false);
    require(presented->applySelectedSettings(),
        "successful host Apply should accept the selected shared format");
    require(saveCalls == 1 && savedPath == expectedPath
            && savedSettings.outputDeviceName == "Fake Speakers"
            && savedSettings.requestedSampleRate == 48000.0
            && savedSettings.requestedBufferFrames == 256
            && savedSettings.requestedOutputChannels == 2,
        "successful Apply must save the selected settings once to the injected path");
}

void trackLoomMainComponentCancelsPreparationBeforeDestroyingTheHost()
{
    using namespace std::chrono_literals;
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    std::latch builderStarted {1};
    std::atomic<bool> cancellationObserved {false};
    {
        trackloom::TrackLoomMainComponentDependencies dependencies;
        dependencies.audioHost = makeFakeHost();
        dependencies.buildOperation = [&](
            trackloom::PreparedMidiPlaybackPlanBuildRequest,
            std::stop_token stopToken) {
            builderStarted.count_down();
            while (!stopToken.stop_requested()) {
                std::this_thread::yield();
            }
            cancellationObserved.store(true, std::memory_order_release);
            return trackloom::PreparedMidiPlaybackPlanBuildResult {
                trackloom::PreparedMidiPlaybackPlanBuildFailureReason::Cancelled,
                nullptr
            };
        };
        auto component = std::make_unique<trackloom::TrackLoomMainComponent>(
            std::move(dependencies));
        component->serviceUiTimer();
        require(component->dispatchCommand(1101).executed,
            "lifecycle test must start asynchronous preparation");
        builderStarted.wait();
        component.reset();
    }
    require(cancellationObserved.load(std::memory_order_acquire),
        "main component destruction must cancel and join its builder before host destruction");
}

void trackLoomMainComponentOwnsItsDefaultSettingsWindow()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    const auto windowsBefore = juce::TopLevelWindow::getNumTopLevelWindows();
    {
        trackloom::TrackLoomMainComponentDependencies dependencies;
        dependencies.audioHost = makeFakeHost();
        trackloom::TrackLoomMainComponent component(std::move(dependencies));
        require(component.dispatchCommand(1303).executed,
            "default OpenAudioSettings presenter must execute");
        require(juce::TopLevelWindow::getNumTopLevelWindows() == windowsBefore + 1,
            "default presenter must display and own one real settings window");
    }
    require(juce::TopLevelWindow::getNumTopLevelWindows() == windowsBefore,
        "destroying the main component must close its owned settings window before the host");
}

void productionAudioSettingsPathUsesTheFixedApplicationDataLocation()
{
    const std::filesystem::path applicationData =
        "C:/Users/TrackLoomTest/AppData/Roaming";
    require(
        trackloom::trackLoomAudioSettingsPath(applicationData)
            == applicationData / "TrackLoom" / "audio-settings.txt",
        "production audio settings must use user application data/TrackLoom/audio-settings.txt");
}

}

int main()
{
    configureTestFailureOutput();
    try {
        audioSettingsComponentExposesDedicatedSharedWasapiControls();
        audioSettingsComponentShowsOnlySharedWasapiFormatsAndChannels();
        audioSettingsComponentDisablesApplyAndToneDuringPlayback();
        audioSettingsComponentDisablesApplyAndToneDuringStoppingTail();
        audioSettingsComponentTestToneTouchesOnlyTheHost();
        trackLoomMainComponentRejectsANullAudioHost();
        trackLoomMainComponentRepairsEmptyOperationsAndShowsStableControlIds();
        trackLoomMainComponentTimerRefreshesOnlyPlaybackPresentation();
        trackLoomMainComponentEnablesPlayWhenStartupOutputOpens();
        trackLoomMainComponentPollsDeviceRemovalWhileIdle();
        audioSettingsComponentTracksHostStateWithoutManualRefresh();
        audioSettingsComponentRecoversAfterTestToneStops();
        trackLoomMainComponentFallsBackAfterInvalidSettingsWithAChineseWarning();
        trackLoomMainComponentDispatches1303ToExactlyOneSettingsPresenter();
        trackLoomMainComponentShowsPreparingWithoutDrivingTheAudioCallback();
        trackLoomMainComponentShowsEveryRealtimeDiagnosticField();
        trackLoomMainComponentSavesAppliedSettingsOnlyAfterHostSuccess();
        trackLoomMainComponentCancelsPreparationBeforeDestroyingTheHost();
        trackLoomMainComponentOwnsItsDefaultSettingsWindow();
        productionAudioSettingsPathUsesTheFixedApplicationDataLocation();
    } catch (const std::exception& error) {
        std::cerr << "TrackLoom app JUCE audio test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "TrackLoom app JUCE audio tests passed\n";
    return 0;
}
