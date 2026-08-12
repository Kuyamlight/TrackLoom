#include "JuceAudioHost.h"
#include "support/FakeJuceAudioDeviceType.h"

#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int observedBlockOperationCallCount = 0;
bool observedClearedBlock = false;
std::int64_t fakeTickValues[2] {0, 0};
int fakeTickIndex = 0;

std::int64_t nextFakeTick() noexcept
{
    const auto index = std::min(fakeTickIndex, 1);
    ++fakeTickIndex;
    return fakeTickValues[index];
}

void processObservedBlock(
    trackloom::PreparedMidiPlaybackRuntime& runtime,
    float* const* outputs,
    int channels,
    int frames)
{
    ++observedBlockOperationCallCount;
    runtime.processBlock(outputs, channels, frames);
}

void observeClearedBlock(
    trackloom::PreparedMidiPlaybackRuntime&,
    float* const* outputs,
    int channels,
    int frames)
{
    observedClearedBlock = outputs != nullptr && channels > 0 && frames > 0;
    for (int channel = 0; channel < channels && observedClearedBlock; ++channel) {
        observedClearedBlock = outputs[channel] != nullptr
            && std::all_of(outputs[channel], outputs[channel] + frames, [](float sample) {
                return sample == 0.0f;
            });
    }
}

void throwFromRealtimeBlock(
    trackloom::PreparedMidiPlaybackRuntime&,
    float* const* outputs,
    int channels,
    int frames)
{
    if (outputs != nullptr) {
        for (int channel = 0; channel < channels; ++channel) {
            if (outputs[channel] != nullptr) {
                std::fill_n(outputs[channel], frames, 0.5f);
            }
        }
    }
    throw std::runtime_error("injected realtime block failure");
}

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << message << '\n';
        throw std::runtime_error(message);
    }
}

std::unique_ptr<const trackloom::PreparedMidiPlaybackPlan> makePlan(
    const trackloom::AudioDeviceFormatSnapshot& format)
{
    auto plan = std::make_unique<trackloom::PreparedMidiPlaybackPlan>();
    plan->sampleRate = format.sampleRate;
    plan->maximumBlockFrames = format.maximumBlockFrames;
    plan->outputChannelCount = format.outputChannelCount;
    plan->outputChannelMask = format.outputChannelMask;
    plan->playbackStartSample = 0;
    plan->instrumentSlots.push_back({
        trackloom::PreparedMidiInstrumentKind::BuiltInSine,
        0.25f,
        0.0f,
        0
    });
    return plan;
}

void juceAudioHostDefaultFactoryCreatesTheWindowsAudioType()
{
    trackloom::JuceAudioHost host;

    const auto devices = host.refreshOutputDevices();
    for (const auto& device : devices) {
        require(device.id.rfind("Windows Audio/", 0) == 0,
            "the default factory must expose only Windows Audio device ids");
    }
    const auto missing = host.openOutput({
        "TrackLoom deliberately absent output 78f38d2e", 48000.0, 256, 2
    });
    require(!missing.success
            && missing.failureReason == trackloom::JuceAudioHostFailureReason::DeviceNotFound,
        "the default shared WASAPI factory must exist even when no hardware is required");
    require(missing.message.find("Windows Audio") != std::string::npos,
        "the no-hardware result must expose the default factory's Windows Audio type name");
}

void juceAudioHostOpensOutputOnlySharedDevice()
{
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::JuceAudioHost host([&]() -> std::unique_ptr<juce::AudioIODeviceType> {
        auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        observedType = type.get();
        return type;
    });
    const auto devices = host.refreshOutputDevices();
    require(devices.size() == 1, "fake output should be enumerated");
    const auto opened = host.openOutput({ "Fake Speakers", 48000.0, 256, 2 });
    require(opened.success, "supported fake format should open");
    require(observedType->lastOpenInputChannelCount() == 0,
        "audio milestone must request zero inputs");
    require(observedType->lastOpenOutputChannelCount() == 2,
        "audio milestone should request stereo");
    require(observedType->openCalls().back().inputChannelMask == 0
            && observedType->openCalls().back().outputChannelMask == 3,
        "stereo output must request no inputs and exactly the first two output bits");
    require(host.deviceFormatSnapshot().generation == 1,
        "first device instance should advance generation");
}

void juceAudioHostEnumeratesDefaultDeviceAndAdvertisedFormats()
{
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::JuceAudioHost host([&]() -> std::unique_ptr<juce::AudioIODeviceType> {
        auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        type->setOutputDevices({"Other Speakers", "Fake Speakers"}, 1);
        type->setAvailableFormats({44100.0, 48000.0}, {128, 256}, 256);
        observedType = type.get();
        return type;
    });

    const auto devices = host.refreshOutputDevices();

    require(devices.size() == 2, "all fake outputs should be enumerated");
    require(devices[1].id == "Windows Audio/Fake Speakers",
        "audio device id should use the shared Windows Audio prefix");
    require(devices[1].isDefault, "fake default output should be marked");
    require(devices[1].sampleRates == std::vector<double>({44100.0, 48000.0}),
        "enumeration should expose advertised sample rates");
    require(devices[1].bufferSizes == std::vector<int>({128, 256}),
        "enumeration should expose advertised buffer sizes");
    require(devices[1].maximumOutputChannels == 2,
        "enumeration should expose physical output channel count");
    require(observedType->calls().front() == "scan",
        "enumeration must scan before creating probe devices");
    const auto opened = host.openOutput({});
    require(opened.success && observedType->activeDevice()->getName() == "Fake Speakers",
        "an empty device request must open the enumerated default output");
}

void juceAudioHostFallsBackOnceAndReportsActualSharedFormat()
{
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::JuceAudioHost host([&]() -> std::unique_ptr<juce::AudioIODeviceType> {
        auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        type->setAvailableFormats({44100.0}, {512}, 512);
        type->failNextOpen();
        observedType = type.get();
        return type;
    });

    const auto opened = host.openOutput({"Fake Speakers", 48000.0, 256, 2});

    require(opened.success, "host should retry the one advertised shared format");
    require(observedType->openCalls().size() == 2,
        "failed request should have exactly one fallback attempt");
    require(observedType->openCalls()[1].sampleRate == 44100.0,
        "fallback should use the first advertised sample rate");
    require(observedType->openCalls()[1].bufferFrames == 512,
        "fallback should use the advertised default buffer");
    require(!opened.warning.empty(), "adjusted shared format should return a warning");
    require(opened.actualFormat.sampleRate == 44100.0,
        "result should report the opened sample rate");
    require(opened.actualFormat.maximumBlockFrames == 512,
        "result should report the opened maximum callback frames");
}

void juceAudioHostDistinguishesDeviceCreationFailureFromMissingDevice()
{
    trackloom::JuceAudioHost host([]() -> std::unique_ptr<juce::AudioIODeviceType> {
        auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        type->setCreateShouldFail(true);
        return type;
    });

    const auto opened = host.openOutput({"Fake Speakers", 48000.0, 256, 2});

    require(!opened.success, "a device instance creation failure must reject open");
    require(opened.failureReason == trackloom::JuceAudioHostFailureReason::DeviceCreateFailed,
        "an enumerated name whose instance cannot be created is not a missing device");
}

void juceAudioHostReportsUnavailableDeviceType()
{
    trackloom::JuceAudioHost host([]() -> std::unique_ptr<juce::AudioIODeviceType> {
        return {};
    });

    const auto opened = host.openOutput({"Fake Speakers", 48000.0, 256, 2});

    require(!opened.success, "a missing device type must reject open");
    require(opened.failureReason
            == trackloom::JuceAudioHostFailureReason::DeviceTypeUnavailable,
        "a missing shared WASAPI type must expose DeviceTypeUnavailable");
}

void juceAudioHostReportsMissingNamedDevice()
{
    trackloom::JuceAudioHost host([]() -> std::unique_ptr<juce::AudioIODeviceType> {
        return std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
    });

    const auto opened = host.openOutput({"Absent Speakers", 48000.0, 256, 2});

    require(!opened.success, "an absent named device must reject open");
    require(opened.failureReason == trackloom::JuceAudioHostFailureReason::DeviceNotFound,
        "an absent named device must expose DeviceNotFound");
}

void juceAudioHostReportsPermanentDeviceOpenFailureAfterOneFallback()
{
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::JuceAudioHost host([&]() -> std::unique_ptr<juce::AudioIODeviceType> {
        auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        type->setOpenShouldFail(true);
        observedType = type.get();
        return type;
    });

    const auto opened = host.openOutput({"Fake Speakers", 48000.0, 256, 2});

    require(!opened.success, "a device that rejects both formats must reject open");
    require(opened.failureReason == trackloom::JuceAudioHostFailureReason::DeviceOpenFailed,
        "a permanent backend open failure must expose DeviceOpenFailed");
    require(observedType->openCalls().size() == 2,
        "a permanent failure must attempt only the request and one advertised fallback");
}

void juceAudioHostRejectsInvalidOpenRequestBeforeTouchingTheBackend()
{
    int factoryCallCount = 0;
    trackloom::JuceAudioHost host([&]() -> std::unique_ptr<juce::AudioIODeviceType> {
        ++factoryCallCount;
        return std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
    });

    const auto opened = host.openOutput({"Fake Speakers", 0.0, 0, 3});

    require(!opened.success, "an invalid numeric format request must reject open");
    require(opened.failureReason == trackloom::JuceAudioHostFailureReason::DeviceOpenFailed,
        "an invalid numeric request must expose DeviceOpenFailed");
    require(factoryCallCount == 0,
        "request validation must happen before device discovery or backend access");
}

void juceAudioHostRequestsTheFirstOutputBitForMono()
{
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::JuceAudioHost host([&]() -> std::unique_ptr<juce::AudioIODeviceType> {
        auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        observedType = type.get();
        return type;
    });

    const auto opened = host.openOutput({"Fake Speakers", 48000.0, 256, 1});

    require(opened.success, "a supported mono fake output should open");
    require(observedType->openCalls().back().inputChannelMask == 0,
        "mono output must still request an empty input mask");
    require(observedType->openCalls().back().outputChannelMask == 1,
        "mono output must request only the first output bit");
    require(opened.actualFormat.outputChannelCount == 1
            && opened.actualFormat.outputChannelMask == 1,
        "the actual mono format must preserve count and mask");
}

void juceAudioHostAdvancesGenerationOncePerReplacementDeviceInstance()
{
    trackloom::JuceAudioHost host([]() -> std::unique_ptr<juce::AudioIODeviceType> {
        return std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
    });
    const auto first = host.openOutput({"Fake Speakers", 48000.0, 256, 2});
    require(first.success, "first fake output should open");

    const auto second = host.openOutput({"Fake Speakers", 48000.0, 256, 2});

    require(second.success, "replacement fake output should open");
    require(second.actualFormat.generation == first.actualFormat.generation + 1,
        "one replacement operation should advance format generation once");
}

void juceAudioHostPreservesTheCurrentInstanceWhenReplacementOpenFails()
{
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::JuceAudioHost host([&]() -> std::unique_ptr<juce::AudioIODeviceType> {
        auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        observedType = type.get();
        return type;
    });
    const auto first = host.openOutput({});
    require(first.success, "replacement-failure test output should first open");
    auto* const firstDevice = observedType->activeDevice();
    observedType->setOpenShouldFail(true);
    observedType->clearCalls();

    const auto replacement = host.openOutput({});

    require(!replacement.success
            && replacement.failureReason
                == trackloom::JuceAudioHostFailureReason::DeviceOpenFailed,
        "a failed replacement instance must report DeviceOpenFailed");
    require(observedType->activeDevice() == firstDevice
            && firstDevice != nullptr && firstDevice->isOpen(),
        "a failed replacement must leave the current open instance intact");
    require(host.deviceFormatSnapshot().generation == first.actualFormat.generation,
        "a failed replacement must not advance format generation");
    require(std::find(observedType->calls().begin(), observedType->calls().end(), "stop")
            == observedType->calls().end(),
        "a failed replacement must not stop the current device");
}

void juceAudioHostValidatesPlanBeforeTouchingStoppedDevice()
{
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::JuceAudioHost host([&]() -> std::unique_ptr<juce::AudioIODeviceType> {
        auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        observedType = type.get();
        return type;
    });
    require(host.openOutput({}).success, "validator test output should open");
    observedType->clearCalls();
    auto plan = makePlan(host.deviceFormatSnapshot());
    const_cast<trackloom::PreparedMidiPlaybackPlan&>(*plan).sampleRate = 0.0;

    const auto started = host.installAndStart(std::move(plan));

    require(!started.success, "invalid plan must not start");
    require(started.failureReason == trackloom::RealtimePlaybackHostFailureReason::InvalidPlan,
        "invalid plan should expose the stable invalid-plan reason");
    require(observedType->calls().empty(),
        "plan validation failure must not stop or start the device");
}

void juceAudioHostReportsAValidPlanWhenNoOutputIsOpen()
{
    trackloom::JuceAudioHost host(
        []() -> std::unique_ptr<juce::AudioIODeviceType> {
            return std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        });
    trackloom::AudioDeviceFormatSnapshot format;
    format.sampleRate = 48000.0;
    format.maximumBlockFrames = 256;
    format.outputChannelCount = 2;
    format.outputChannelMask = 3;

    const auto started = host.installAndStart(makePlan(format));

    require(!started.success
            && started.failureReason
                == trackloom::RealtimePlaybackHostFailureReason::DeviceNotOpen,
        "a valid prepared plan without an open device must report DeviceNotOpen");
}

void juceAudioHostRejectsEveryNumericFormatMismatchBeforeDeviceStop()
{
    for (int mismatch = 0; mismatch < 4; ++mismatch) {
        trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
        trackloom::JuceAudioHost host([&]() -> std::unique_ptr<juce::AudioIODeviceType> {
            auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
            if (mismatch == 3) {
                type->setNegotiatedOutputMask(5);
            }
            observedType = type.get();
            return type;
        });
        require(host.openOutput({}).success, "format mismatch test output should open");
        observedType->clearCalls();
        auto plan = makePlan(host.deviceFormatSnapshot());
        auto& mutablePlan = const_cast<trackloom::PreparedMidiPlaybackPlan&>(*plan);
        if (mismatch == 0) {
            mutablePlan.sampleRate = 44100.0;
        } else if (mismatch == 1) {
            mutablePlan.maximumBlockFrames = 128;
        } else if (mismatch == 2) {
            mutablePlan.outputChannelCount = 1;
            mutablePlan.outputChannelMask = 1;
        } else {
            mutablePlan.outputChannelMask = 3;
        }

        const auto started = host.installAndStart(std::move(plan));

        require(!started.success, "mismatched plan must not start");
        require(started.failureReason
                == trackloom::RealtimePlaybackHostFailureReason::DeviceFormatMismatch,
            "all four numeric format mismatches should share one stable reason");
        require(observedType->calls().empty(),
            "format mismatch must be rejected before device.stop");
    }
}

void juceAudioHostStopsDeviceThenStartsRuntimeBeforeDeviceCallback()
{
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::JuceAudioHost host([&]() -> std::unique_ptr<juce::AudioIODeviceType> {
        auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        observedType = type.get();
        return type;
    });
    require(host.openOutput({}).success, "start ordering test output should open");
    observedType->clearCalls();
    bool runtimeWasPlayingAtDeviceStart = false;
    observedType->setStartObserver([&]() {
        runtimeWasPlayingAtDeviceStart = host.snapshot().realtime.state
            == trackloom::RealtimePlaybackState::Playing;
    });

    const auto started = host.installAndStart(makePlan(host.deviceFormatSnapshot()));

    require(started.success, "matching prepared plan should start");
    require(observedType->calls() == std::vector<std::string>({"stop", "start"}),
        "install should stop pending callbacks before device start");
    require(runtimeWasPlayingAtDeviceStart,
        "runtime.start must happen before device.start invokes the callback");
}

void juceAudioHostDrainsAPendingCallbackBeforeReplacingTheOwnedPlan()
{
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    observedBlockOperationCallCount = 0;
    trackloom::JuceAudioHost host(
        [&]() -> std::unique_ptr<juce::AudioIODeviceType> {
            auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
            observedType = type.get();
            return type;
        },
        nullptr,
        0,
        processObservedBlock);
    require(host.openOutput({}).success, "pending-callback test output should open");
    require(host.installAndStart(makePlan(host.deviceFormatSnapshot())).success,
        "first pending-callback plan should start");
    observedType->activeDevice()->runCallback(1);
    require(host.snapshot().realtime.state == trackloom::RealtimePlaybackState::Stopped,
        "the empty first plan should stop before replacement");
    const auto callbacksBeforeReplacement = observedBlockOperationCallCount;
    observedType->setPendingCallbackFramesOnStop(1);
    bool pendingCallbackCompletedBeforeStart = false;
    observedType->setStartObserver([&]() {
        pendingCallbackCompletedBeforeStart = observedBlockOperationCallCount
            == callbacksBeforeReplacement + 1;
    });
    observedType->clearCalls();

    const auto replacement = host.installAndStart(makePlan(host.deviceFormatSnapshot()));

    require(replacement.success, "a stopped runtime should accept a replacement plan");
    require(observedType->calls() == std::vector<std::string>({"stop", "start"}),
        "replacement must drain the backend before starting its next callback");
    require(pendingCallbackCompletedBeforeStart,
        "device.stop must finish the old pending callback before plan replacement/start");
}

void juceAudioHostDoesNotTruncateStoppingTailToInstallAnotherPlan()
{
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::JuceAudioHost host([&]() -> std::unique_ptr<juce::AudioIODeviceType> {
        auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        observedType = type.get();
        return type;
    });
    require(host.openOutput({}).success, "stopping test output should open");
    require(host.installAndStart(makePlan(host.deviceFormatSnapshot())).success,
        "stopping test plan should start");
    require(host.requestStop(), "playing runtime should enter Stopping");
    observedType->clearCalls();

    const auto replacement = host.installAndStart(makePlan(host.deviceFormatSnapshot()));

    require(!replacement.success, "Stopping runtime must reject plan replacement");
    require(replacement.failureReason
            == trackloom::RealtimePlaybackHostFailureReason::PlaybackNotStopped,
        "Stopping replacement should expose PlaybackNotStopped");
    require(observedType->calls().empty(),
        "rejected replacement must not call device.stop and truncate the tail");
}

void juceAudioHostRejectsReopenWhilePlayingOrStoppingWithoutTouchingDevice()
{
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::JuceAudioHost host([&]() -> std::unique_ptr<juce::AudioIODeviceType> {
        auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        observedType = type.get();
        return type;
    });
    require(host.openOutput({}).success, "active reopen test output should open");
    require(host.installAndStart(makePlan(host.deviceFormatSnapshot())).success,
        "active reopen test plan should start");
    observedType->clearCalls();

    const auto whilePlaying = host.openOutput({});
    require(!whilePlaying.success
            && whilePlaying.failureReason
                == trackloom::JuceAudioHostFailureReason::PlaybackActive,
        "Playing must reject device reopen with PlaybackActive");
    require(host.requestStop(), "the active reopen test should enter Stopping");
    const auto whileStopping = host.openOutput({});
    require(!whileStopping.success
            && whileStopping.failureReason
                == trackloom::JuceAudioHostFailureReason::PlaybackActive,
        "Stopping must reject device reopen with PlaybackActive");
    require(observedType->calls().empty(),
        "active reopen rejection must not scan, stop, close, or replace the device");
}

void juceAudioHostCanReenumerateAndReopenAfterClose()
{
    int factoryCallCount = 0;
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::JuceAudioHost host([&]() -> std::unique_ptr<juce::AudioIODeviceType> {
        ++factoryCallCount;
        auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        observedType = type.get();
        return type;
    });
    const auto first = host.openOutput({});
    require(first.success, "close/reopen test output should first open");

    require(host.installAndStart(makePlan(first.actualFormat)).success,
        "close ordering test should close while the backend is playing");
    host.close();
    const auto closedFormat = host.deviceFormatSnapshot();
    const auto generationAfterFirstClose = closedFormat.generation;
    host.close();
    const auto devicesAfterClose = host.refreshOutputDevices();
    const auto reopened = host.openOutput({});

    require(!closedFormat.available
            && closedFormat.generation == first.actualFormat.generation + 1,
        "close must invalidate the format and advance generation once");
    require(devicesAfterClose.size() == 1,
        "close must retain the device type and listener for later enumeration");
    require(reopened.success
            && reopened.actualFormat.generation == closedFormat.generation + 1,
        "the retained device type must support reopening a fresh instance");
    require(factoryCallCount == 1,
        "close/reopen must reuse the host-owned shared device type");
    require(observedType->activeDevice() != nullptr,
        "reopen must publish a fresh active device instance");
    require(generationAfterFirstClose + 1 == reopened.actualFormat.generation,
        "an idempotent second close must not advance generation");
}

void juceAudioHostRollsBackWhenDeviceStartDoesNotBeginPlayback()
{
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::JuceAudioHost host([&]() -> std::unique_ptr<juce::AudioIODeviceType> {
        auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        type->setStartShouldPlay(false);
        observedType = type.get();
        return type;
    });
    require(host.openOutput({}).success, "start failure test output should open");
    observedType->clearCalls();

    const auto started = host.installAndStart(makePlan(host.deviceFormatSnapshot()));

    require(!started.success, "a device that never begins playing must reject start");
    require(started.failureReason
            == trackloom::RealtimePlaybackHostFailureReason::DeviceStartFailed,
        "isPlaying false must expose DeviceStartFailed");
    require(observedType->calls() == std::vector<std::string>({"stop", "start", "stop"}),
        "start failure must stop pending callbacks, attempt start, then stop again");
    require(host.snapshot().realtime.state == trackloom::RealtimePlaybackState::Stopped,
        "start failure must hard reset the playback runtime");

    observedType->setStartShouldPlay(true);
    require(host.installAndStart(makePlan(host.deviceFormatSnapshot())).success,
        "a failed start must not prevent a later clean restart");
}

void juceAudioHostPublishesAndGuardsUnexpectedStartFormatChanges()
{
    for (int mismatch = 0; mismatch < 4; ++mismatch) {
        trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
        observedBlockOperationCallCount = 0;
        trackloom::JuceAudioHost host(
            [&]() -> std::unique_ptr<juce::AudioIODeviceType> {
                auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
                observedType = type.get();
                return type;
            },
            nullptr,
            0,
            processObservedBlock);
        require(host.openOutput({}).success, "format change test output should open");
        const auto formatBeforeStart = host.deviceFormatSnapshot();
        if (mismatch == 0) {
            observedType->setStartFormat(44100.0, 256, 2);
        } else if (mismatch == 1) {
            observedType->setStartFormat(48000.0, 128, 2);
        } else if (mismatch == 2) {
            observedType->setStartFormat(48000.0, 256, 1);
        } else {
            observedType->setStartOutputMask(5);
        }
        observedType->setCallbackFramesDuringStart(64);

        const auto started = host.installAndStart(makePlan(formatBeforeStart));
        const auto changedFormat = host.deviceFormatSnapshot();

        require(!started.success, "each start-time format change must reject the old plan");
        require(started.failureReason
                == trackloom::RealtimePlaybackHostFailureReason::DeviceFormatMismatch,
            "each start-time format field must expose DeviceFormatMismatch independently");
        require(observedBlockOperationCallCount == 0,
            "callbacks after a start-time mismatch must remain silent until rollback");
        require(changedFormat.generation == formatBeforeStart.generation + 1,
            "an observed start-time format change must advance format generation once");
        require(changedFormat.available,
            "a rejected stale plan must not hide an otherwise open output device");
        require(host.snapshot().realtime.state == trackloom::RealtimePlaybackState::Stopped,
            "format mismatch rollback must hard reset the playback runtime");
        if (mismatch == 0) {
            require(changedFormat.sampleRate == 44100.0,
                "sample-rate-only start changes must update the snapshot");
        } else if (mismatch == 1) {
            require(changedFormat.maximumBlockFrames == 128,
                "smaller start blocks must still update the exact format snapshot");
        } else if (mismatch == 2) {
            require(changedFormat.outputChannelCount == 1
                    && changedFormat.outputChannelMask == 1,
                "channel-count-only start changes must update count and mask");
        } else {
            require(changedFormat.outputChannelCount == 2
                    && changedFormat.outputChannelMask == 5,
                "mask-only start changes must update the exact active mask");
        }
    }
}

void juceAudioHostCallbackInvokesInjectedRuntimeBlockOperation()
{
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    observedBlockOperationCallCount = 0;
    trackloom::JuceAudioHost host(
        [&]() -> std::unique_ptr<juce::AudioIODeviceType> {
            auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
            observedType = type.get();
            return type;
        },
        nullptr,
        0,
        processObservedBlock);
    require(host.openOutput({}).success, "callback test output should open");
    auto plan = makePlan(host.deviceFormatSnapshot());
    const_cast<trackloom::PreparedMidiPlaybackPlan&>(*plan).events.push_back({
        0,
        1,
        0,
        0,
        1,
        69,
        127,
        trackloom::PreparedMidiEventType::NoteOn
    });
    require(host.installAndStart(std::move(plan)).success,
        "callback test plan should start");

    const auto output = observedType->activeDevice()->runCallback(64);
    const auto diagnostics = host.snapshot().realtime;

    require(observedBlockOperationCallCount == 1,
        "each JUCE callback must invoke the configured runtime block operation once");
    require(diagnostics.callbackCount == 1,
        "the configured operation should reach the real playback runtime");
    require(diagnostics.renderedSampleCount == 64,
        "the runtime must receive the callback's actual frame count");
    const auto nonZero = std::any_of(output.begin(), output.end(), [](const auto& channel) {
        return std::any_of(channel.begin(), channel.end(), [](float sample) {
            return sample != 0.0f;
        });
    });
    require(nonZero, "a valid audible plan should produce non-zero callback output");
}

void juceAudioHostClearsEveryOutputBeforeTheInjectedOperation()
{
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    observedClearedBlock = false;
    trackloom::JuceAudioHost host(
        [&]() -> std::unique_ptr<juce::AudioIODeviceType> {
            auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
            observedType = type.get();
            return type;
        },
        nullptr,
        0,
        observeClearedBlock);
    require(host.openOutput({}).success, "entry-clear test output should open");
    require(host.installAndStart(makePlan(host.deviceFormatSnapshot())).success,
        "entry-clear test plan should start");

    const auto output = observedType->activeDevice()->runCallback(32, 1.0f);

    require(observedClearedBlock,
        "the host must clear every non-null output before invoking the block operation");
    for (const auto& channel : output) {
        require(std::all_of(channel.begin(), channel.end(), [](float sample) {
            return sample == 0.0f;
        }), "an operation that writes nothing must leave the callback entirely silent");
    }
}

void juceAudioHostContainsRealtimeBlockExceptionsAndSilencesTheWholeBlock()
{
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::JuceAudioHost host(
        [&]() -> std::unique_ptr<juce::AudioIODeviceType> {
            auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
            observedType = type.get();
            return type;
        },
        nullptr,
        0,
        throwFromRealtimeBlock);
    require(host.openOutput({}).success, "exception test output should open");
    const auto generationBeforeFault = host.deviceFormatSnapshot().generation;
    require(host.installAndStart(makePlan(host.deviceFormatSnapshot())).success,
        "exception test plan should start");

    std::vector<std::vector<float>> output;
    bool exceptionEscaped = false;
    try {
        output = observedType->activeDevice()->runCallback(32, 1.0f);
    } catch (...) {
        exceptionEscaped = true;
    }
    const auto diagnostics = host.snapshot().realtime;

    require(!exceptionEscaped, "no exception may escape the JUCE audio callback");
    require(diagnostics.callbackExceptionCount == 1,
        "a throwing block operation must increment the callback exception count");
    require(diagnostics.lastError == trackloom::RealtimeAudioError::CallbackException,
        "callback exceptions must publish the stable CallbackException error");
    require(diagnostics.state == trackloom::RealtimePlaybackState::Faulted,
        "callback exceptions must hard reset playback into Faulted state");
    for (const auto& channel : output) {
        require(std::all_of(channel.begin(), channel.end(), [](float sample) {
            return sample == 0.0f;
        }), "callback exceptions must re-clear the entire output block");
    }

    observedType->clearCalls();
    host.serviceNonRealtime();
    const auto afterService = host.snapshot();
    require(observedType->calls() == std::vector<std::string>({"stop", "close"}),
        "non-realtime service must drain and close a runtime-faulted device");
    require(!afterService.format.available
            && afterService.format.generation == generationBeforeFault + 1,
        "runtime-fault cleanup must invalidate the device format generation");
    require(afterService.realtime.state == trackloom::RealtimePlaybackState::Faulted
            && afterService.realtime.lastError
                == trackloom::RealtimeAudioError::CallbackException,
        "runtime-fault cleanup must retain the stable callback error");
}

void juceAudioHostCountsCallbackTimeEqualToTheRealtimeDeadline()
{
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    fakeTickValues[0] = 100;
    fakeTickValues[1] = 101;
    fakeTickIndex = 0;
    trackloom::JuceAudioHost host(
        [&]() -> std::unique_ptr<juce::AudioIODeviceType> {
            auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
            observedType = type.get();
            return type;
        },
        nextFakeTick,
        1000,
        nullptr);
    require(host.openOutput({}).success, "deadline test output should open");
    require(host.installAndStart(makePlan(host.deviceFormatSnapshot())).success,
        "deadline test plan should start");

    observedType->activeDevice()->runCallback(48);

    require(fakeTickIndex == 2, "each callback should sample the injected clock twice");
    require(host.snapshot().realtime.callbackTimeoutCount == 1,
        "48 frames at 48 kHz taking exactly one 1 kHz tick must count as a timeout");
}

void juceAudioHostDefersDeviceListScanningToNonRealtimeService()
{
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::JuceAudioHost host([&]() -> std::unique_ptr<juce::AudioIODeviceType> {
        auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        observedType = type.get();
        return type;
    });
    require(host.openOutput({}).success, "device-list test output should open");
    observedType->clearCalls();

    observedType->notifyDeviceListChanged();

    require(host.snapshot().deviceListRefreshPending,
        "device-list notification must publish a refresh request");
    require(observedType->calls().empty(),
        "the notification callback must not scan, stop, close, or create devices");

    host.serviceNonRealtime();

    require(!host.snapshot().deviceListRefreshPending,
        "non-realtime service must consume the refresh request");
    require(!observedType->calls().empty() && observedType->calls().front() == "scan",
        "non-realtime service must rescan after a list-change request");
    require(std::find(observedType->calls().begin(), observedType->calls().end(), "stop")
            == observedType->calls().end(),
        "a refresh that still finds the current output must not stop it");
    require(host.deviceFormatSnapshot().available,
        "a refresh that still finds the current output must preserve availability");
}

void juceAudioHostInvalidatesPlaybackWhenTheCurrentOutputDisappears()
{
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::JuceAudioHost host([&]() -> std::unique_ptr<juce::AudioIODeviceType> {
        auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        observedType = type.get();
        return type;
    });
    require(host.openOutput({}).success, "removal test output should open");
    auto plan = makePlan(host.deviceFormatSnapshot());
    const_cast<trackloom::PreparedMidiPlaybackPlan&>(*plan).events.push_back({
        0, 1, 0, 0, 1, 60, 127, trackloom::PreparedMidiEventType::NoteOn
    });
    require(host.installAndStart(std::move(plan)).success,
        "removal test plan should start");
    observedType->activeDevice()->runCallback(64);
    const auto generationBeforeRemoval = host.deviceFormatSnapshot().generation;
    observedType->clearCalls();
    observedType->setOutputDevices({});

    observedType->notifyDeviceListChanged();
    require(observedType->calls().empty(),
        "device removal notification itself must remain a lock-free request");

    host.serviceNonRealtime();
    const auto afterRemoval = host.snapshot();

    require(observedType->calls() == std::vector<std::string>({"scan", "stop", "close"}),
        "service must scan, drain callbacks, then close a removed current output");
    require(!afterRemoval.format.available
            && afterRemoval.format.generation == generationBeforeRemoval + 1,
        "removing the current output must invalidate format and advance generation");
    require(afterRemoval.realtime.state == trackloom::RealtimePlaybackState::Faulted
            && afterRemoval.realtime.lastError == trackloom::RealtimeAudioError::DeviceError,
        "removing the current output must hard reset playback with DeviceError");
    require(observedType->activeDevice() == nullptr,
        "the removed output instance must be closed and released");

    host.hardStopAndReset();
    observedType->setOutputDevices({"Fake Speakers"});
    require(host.openOutput({}).success,
        "the retained device type should permit recovery when output returns");
    require(host.installAndStart(makePlan(host.deviceFormatSnapshot())).success,
        "removed-device cleanup must not retain an unusable old plan");
}

void juceAudioHostDefersDeviceErrorsAndSilencesUntilServiceCleanup()
{
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    observedBlockOperationCallCount = 0;
    trackloom::JuceAudioHost host(
        [&]() -> std::unique_ptr<juce::AudioIODeviceType> {
            auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
            observedType = type.get();
            return type;
        },
        nullptr,
        0,
        processObservedBlock);
    require(host.openOutput({}).success, "device-error test output should open");
    auto plan = makePlan(host.deviceFormatSnapshot());
    const_cast<trackloom::PreparedMidiPlaybackPlan&>(*plan).events.push_back({
        0, 1, 0, 0, 1, 60, 127, trackloom::PreparedMidiEventType::NoteOn
    });
    require(host.installAndStart(std::move(plan)).success,
        "device-error test plan should start");
    observedType->activeDevice()->runCallback(64);
    const auto generationBeforeError = host.deviceFormatSnapshot().generation;
    observedType->clearCalls();

    observedType->activeDevice()->triggerAudioDeviceError();
    require(observedType->calls().empty(),
        "audioDeviceError must not stop, close, log, or otherwise touch the backend");
    const auto outputAfterError = observedType->activeDevice()->runCallback(64, 1.0f);
    require(observedBlockOperationCallCount == 1,
        "callbacks after a device error flag must not re-enter the playback runtime");
    for (const auto& channel : outputAfterError) {
        require(std::all_of(channel.begin(), channel.end(), [](float sample) {
            return sample == 0.0f;
        }), "callbacks after a device error must remain entirely silent");
    }

    host.serviceNonRealtime();
    const auto afterService = host.snapshot();

    require(observedType->calls() == std::vector<std::string>({"stop", "close"}),
        "non-realtime device-error cleanup must drain callbacks before close");
    require(!afterService.format.available
            && afterService.format.generation == generationBeforeError + 1,
        "device-error cleanup must invalidate format and advance generation");
    require(afterService.realtime.state == trackloom::RealtimePlaybackState::Faulted
            && afterService.realtime.lastError == trackloom::RealtimeAudioError::DeviceError,
        "device-error cleanup must publish a stable DeviceError fault");

    const auto reopened = host.openOutput({});
    require(reopened.success, "an available output should reopen after device-error cleanup");
    require(host.snapshot().realtime.state == trackloom::RealtimePlaybackState::Stopped,
        "a successfully replaced device instance must clear the serviced device fault");
    require(host.installAndStart(makePlan(reopened.actualFormat)).success,
        "a fresh plan should start after the output device is restarted");
}

void juceAudioHostCoalescesSimultaneousRemovalAndDeviceErrorCleanup()
{
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::JuceAudioHost host([&]() -> std::unique_ptr<juce::AudioIODeviceType> {
        auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        observedType = type.get();
        return type;
    });
    const auto opened = host.openOutput({});
    require(opened.success, "coalesced-fault test output should open");
    auto plan = makePlan(opened.actualFormat);
    const_cast<trackloom::PreparedMidiPlaybackPlan&>(*plan).events.push_back({
        0, 1, 0, 0, 1, 60, 127, trackloom::PreparedMidiEventType::NoteOn
    });
    require(host.installAndStart(std::move(plan)).success,
        "coalesced-fault test plan should start");
    observedType->setOutputDevices({});
    observedType->clearCalls();

    observedType->activeDevice()->triggerAudioDeviceError();
    observedType->activeDevice()->triggerDeviceListChangedFromCallback();
    host.serviceNonRealtime();
    const auto afterService = host.snapshot();

    require(observedType->calls() == std::vector<std::string>({"scan", "stop", "close"}),
        "simultaneous fault flags must teardown the output exactly once");
    require(afterService.format.generation == opened.actualFormat.generation + 1
            && !afterService.format.available,
        "coalesced fault cleanup must advance generation exactly once");
    require(afterService.realtime.state == trackloom::RealtimePlaybackState::Faulted
            && afterService.realtime.lastError == trackloom::RealtimeAudioError::DeviceError,
        "coalesced device faults must retain the stable DeviceError state");
}

void juceAudioHostServicesStoppingOnlyAfterTheRuntimeFinishesItsTail()
{
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::JuceAudioHost host([&]() -> std::unique_ptr<juce::AudioIODeviceType> {
        auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        observedType = type.get();
        return type;
    });
    require(host.openOutput({}).success, "Stopping service test output should open");
    auto plan = makePlan(host.deviceFormatSnapshot());
    const_cast<trackloom::PreparedMidiPlaybackPlan&>(*plan).events.push_back({
        0, 1, 0, 0, 1, 60, 127, trackloom::PreparedMidiEventType::NoteOn
    });
    require(host.installAndStart(std::move(plan)).success,
        "Stopping service test plan should start");
    observedType->activeDevice()->runCallback(64);
    require(host.requestStop(), "playing audio should enter Stopping");
    observedType->clearCalls();

    host.serviceNonRealtime();

    require(observedType->calls().empty(),
        "service must not call device.stop while the callback is rendering release tail");
    require(host.snapshot().realtime.state == trackloom::RealtimePlaybackState::Stopping,
        "service must preserve Stopping until callbacks finish release");
    require(observedType->activeDevice()->isPlaying(),
        "the device must keep running while release tail remains");

    for (int callback = 0;
         callback < 10
            && host.snapshot().realtime.state == trackloom::RealtimePlaybackState::Stopping;
         ++callback) {
        observedType->activeDevice()->runCallback(256);
    }
    require(host.snapshot().realtime.state == trackloom::RealtimePlaybackState::Stopped,
        "release callbacks should eventually publish Stopped");
    require(observedType->calls().empty(),
        "the audio callback must not stop the device from the realtime thread");

    host.serviceNonRealtime();

    require(observedType->calls() == std::vector<std::string>({"stop"}),
        "service must stop the device only after runtime reaches Stopped");
    require(!observedType->activeDevice()->isPlaying(),
        "the non-realtime service stop must end backend playback");
}

void juceAudioHostSamplesXrunsOnlyFromNonRealtimeService()
{
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::JuceAudioHost host([&]() -> std::unique_ptr<juce::AudioIODeviceType> {
        auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        observedType = type.get();
        return type;
    });
    require(host.openOutput({}).success, "xrun test output should open");
    observedType->setXRunCount(7);

    require(host.snapshot().xRunCount == -1,
        "xrun state must not query JUCE from the snapshot accessor");
    observedType->clearCalls();

    host.serviceNonRealtime();

    require(host.snapshot().xRunCount == 7,
        "non-realtime service must publish the current device xrun count");
    require(observedType->calls().empty(),
        "sampling xruns must not scan, stop, start, or close the device");
}

void juceAudioHostRequiresARebuiltPlanAfterAnOversizedCallback()
{
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::JuceAudioHost host([&]() -> std::unique_ptr<juce::AudioIODeviceType> {
        auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        observedType = type.get();
        return type;
    });
    require(host.openOutput({}).success, "oversized-block test output should open");
    const auto originalFormat = host.deviceFormatSnapshot();
    auto plan = makePlan(originalFormat);
    const_cast<trackloom::PreparedMidiPlaybackPlan&>(*plan).events.push_back({
        0, 1, 0, 0, 1, 60, 127, trackloom::PreparedMidiEventType::NoteOn
    });
    require(host.installAndStart(std::move(plan)).success,
        "oversized-block test plan should start");

    const auto oversizedOutput = observedType->activeDevice()->runCallback(512, 1.0f);
    const auto afterCallback = host.snapshot();
    require(afterCallback.realtime.state == trackloom::RealtimePlaybackState::Faulted
            && afterCallback.realtime.lastError
                == trackloom::RealtimeAudioError::OversizedBlock,
        "an oversized callback must fault the stale prepared plan");
    require(afterCallback.realtime.oversizedBlockCount == 1
            && afterCallback.realtime.largestObservedBlockFrames == 512,
        "the runtime must publish the oversized callback size");
    for (const auto& channel : oversizedOutput) {
        require(std::all_of(channel.begin(), channel.end(), [](float sample) {
            return sample == 0.0f;
        }), "an oversized callback must leave the entire output silent");
    }
    observedType->clearCalls();

    host.serviceNonRealtime();
    const auto rebuiltFormat = host.deviceFormatSnapshot();

    require(observedType->calls() == std::vector<std::string>({"stop"}),
        "oversized recovery must drain callbacks without discarding the open device");
    require(rebuiltFormat.available
            && rebuiltFormat.maximumBlockFrames == 512
            && rebuiltFormat.generation == originalFormat.generation + 1,
        "oversized recovery must publish a larger prepared maximum and generation");
    require(host.snapshot().realtime.state == trackloom::RealtimePlaybackState::Stopped,
        "oversized recovery must require a fresh plan from a stopped runtime");

    observedType->setStartFormat(48000.0, 512, 2);
    auto rebuiltPlan = makePlan(rebuiltFormat);
    const_cast<trackloom::PreparedMidiPlaybackPlan&>(*rebuiltPlan).events.push_back({
        0, 1, 0, 0, 1, 60, 127, trackloom::PreparedMidiEventType::NoteOn
    });
    require(host.installAndStart(std::move(rebuiltPlan)).success,
        "a plan rebuilt for the larger callback ceiling should accept that start block size");
    observedType->activeDevice()->runCallback(512);
    require(host.snapshot().realtime.state == trackloom::RealtimePlaybackState::Playing,
        "the rebuilt plan must accept the observed callback size");
}

void juceAudioHostPlaysABoundedTemporaryTestToneOnlyWhileStopped()
{
    {
        trackloom::JuceAudioHost unopened(
            []() -> std::unique_ptr<juce::AudioIODeviceType> {
                return std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
            });
        const auto result = unopened.playTestTone();
        require(!result.success
                && result.failureReason
                    == trackloom::JuceAudioHostFailureReason::DeviceOpenFailed,
            "test tone must report a closed output without touching playback");
    }

    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::JuceAudioHost host([&]() -> std::unique_ptr<juce::AudioIODeviceType> {
        auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        observedType = type.get();
        return type;
    });
    require(host.openOutput({}).success, "test-tone output should open");
    auto projectPlan = makePlan(host.deviceFormatSnapshot());
    const_cast<trackloom::PreparedMidiPlaybackPlan&>(*projectPlan).events.push_back({
        0, 1, 0, 0, 1, 60, 127, trackloom::PreparedMidiEventType::NoteOn
    });
    require(host.installAndStart(std::move(projectPlan)).success,
        "project playback should start before the test-tone gate is checked");
    observedType->clearCalls();

    const auto activeResult = host.playTestTone();

    require(!activeResult.success
            && activeResult.failureReason
                == trackloom::JuceAudioHostFailureReason::PlaybackActive,
        "test tone must be rejected while project playback is active");
    require(observedType->calls().empty(),
        "rejecting a test tone must not interrupt active playback");
    host.hardStopAndReset();
    observedType->clearCalls();

    const auto toneResult = host.playTestTone();
    require(toneResult.success, "a stopped host with an open output should start a test tone");
    require(toneResult.actualFormat.generation == host.deviceFormatSnapshot().generation,
        "test-tone success must return the active device format snapshot");
    require(observedType->calls() == std::vector<std::string>({"stop", "start"}),
        "test tone must use the same stop/install/runtime-start/device-start protocol");

    float peak = 0.0f;
    bool heardTone = false;
    const auto renderAndMeasure = [&](int totalFrames) {
        while (totalFrames > 0) {
            const auto frames = std::min(totalFrames,
                host.deviceFormatSnapshot().maximumBlockFrames);
            const auto output = observedType->activeDevice()->runCallback(frames);
            for (const auto& channel : output) {
                for (const auto sample : channel) {
                    peak = std::max(peak, std::abs(sample));
                    heardTone = heardTone || sample != 0.0f;
                }
            }
            totalFrames -= frames;
        }
    };
    renderAndMeasure(9600);
    require(host.snapshot().realtime.state == trackloom::RealtimePlaybackState::Playing,
        "the 440 Hz note must remain on for exactly 200 ms at 48 kHz");
    renderAndMeasure(1439);
    require(host.snapshot().realtime.state == trackloom::RealtimePlaybackState::Playing,
        "the test tone must retain its voice until the 30 ms release completes");
    renderAndMeasure(1);
    require(host.snapshot().realtime.state == trackloom::RealtimePlaybackState::Stopped,
        "the test tone must stop after its 30 ms release");
    require(heardTone && peak > 0.019f && peak <= 0.020001f,
        "test-tone voice amplitude must be audible but capped at 0.02");

    host.serviceNonRealtime();
    auto laterProjectPlan = makePlan(host.deviceFormatSnapshot());
    const_cast<trackloom::PreparedMidiPlaybackPlan&>(*laterProjectPlan).events.push_back({
        0, 2, 0, 0, 1, 64, 127, trackloom::PreparedMidiEventType::NoteOn
    });
    require(host.installAndStart(std::move(laterProjectPlan)).success,
        "completed temporary tone ownership must not prevent later project playback");
}

}

int main()
{
    try {
        juceAudioHostDefaultFactoryCreatesTheWindowsAudioType();
        juceAudioHostOpensOutputOnlySharedDevice();
        juceAudioHostEnumeratesDefaultDeviceAndAdvertisedFormats();
        juceAudioHostFallsBackOnceAndReportsActualSharedFormat();
        juceAudioHostDistinguishesDeviceCreationFailureFromMissingDevice();
        juceAudioHostReportsUnavailableDeviceType();
        juceAudioHostReportsMissingNamedDevice();
        juceAudioHostReportsPermanentDeviceOpenFailureAfterOneFallback();
        juceAudioHostRejectsInvalidOpenRequestBeforeTouchingTheBackend();
        juceAudioHostRequestsTheFirstOutputBitForMono();
        juceAudioHostAdvancesGenerationOncePerReplacementDeviceInstance();
        juceAudioHostPreservesTheCurrentInstanceWhenReplacementOpenFails();
        juceAudioHostValidatesPlanBeforeTouchingStoppedDevice();
        juceAudioHostReportsAValidPlanWhenNoOutputIsOpen();
        juceAudioHostRejectsEveryNumericFormatMismatchBeforeDeviceStop();
        juceAudioHostStopsDeviceThenStartsRuntimeBeforeDeviceCallback();
        juceAudioHostDrainsAPendingCallbackBeforeReplacingTheOwnedPlan();
        juceAudioHostDoesNotTruncateStoppingTailToInstallAnotherPlan();
        juceAudioHostRejectsReopenWhilePlayingOrStoppingWithoutTouchingDevice();
        juceAudioHostCanReenumerateAndReopenAfterClose();
        juceAudioHostRollsBackWhenDeviceStartDoesNotBeginPlayback();
        juceAudioHostPublishesAndGuardsUnexpectedStartFormatChanges();
        juceAudioHostCallbackInvokesInjectedRuntimeBlockOperation();
        juceAudioHostClearsEveryOutputBeforeTheInjectedOperation();
        juceAudioHostContainsRealtimeBlockExceptionsAndSilencesTheWholeBlock();
        juceAudioHostCountsCallbackTimeEqualToTheRealtimeDeadline();
        juceAudioHostDefersDeviceListScanningToNonRealtimeService();
        juceAudioHostInvalidatesPlaybackWhenTheCurrentOutputDisappears();
        juceAudioHostDefersDeviceErrorsAndSilencesUntilServiceCleanup();
        juceAudioHostCoalescesSimultaneousRemovalAndDeviceErrorCleanup();
        juceAudioHostServicesStoppingOnlyAfterTheRuntimeFinishesItsTail();
        juceAudioHostSamplesXrunsOnlyFromNonRealtimeService();
        juceAudioHostRequiresARebuiltPlanAfterAnOversizedCallback();
        juceAudioHostPlaysABoundedTemporaryTestToneOnlyWhileStopped();
    } catch (const std::exception& error) {
        std::cerr << "JUCE audio host test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "JUCE audio host tests passed\n";
    return 0;
}
