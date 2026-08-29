#include "JuceAudioHost.h"
#include "support/FakeJuceAudioDeviceType.h"

#include <algorithm>
#include <iostream>
#include <memory>
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

std::unique_ptr<const trackloom::PreparedMidiPlaybackPlan> makePlan(
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

void throwFromRealtimeBlock(
    trackloom::PreparedMidiPlaybackRuntime&,
    float* const*,
    int,
    int)
{
    throw std::runtime_error("injected callback failure");
}

void snapshotTracksCallbackAndPlanAcrossTheStoppedServiceBoundary()
{
    trackloom::test::FakeJuceAudioDeviceType* observedType = nullptr;
    trackloom::JuceAudioHost host([&]() -> std::unique_ptr<juce::AudioIODeviceType> {
        auto type = std::make_unique<trackloom::test::FakeJuceAudioDeviceType>();
        observedType = type.get();
        return type;
    });
    require(host.openOutput({}).success, "quiescence fixture output must open");
    require(host.installAndStart(makePlan(host.deviceFormatSnapshot())).success,
        "quiescence fixture plan must start");

    const auto afterStart = host.snapshot();
    require(afterStart.callbackRunning && afterStart.planInstalled,
        "install/start must publish callback=true and plan=true");

    observedType->activeDevice()->runCallback(1);
    const auto runtimeStoppedBeforeService = host.snapshot();
    require(runtimeStoppedBeforeService.realtime.state
            == trackloom::RealtimePlaybackState::Stopped,
        "the empty plan must reach runtime Stopped in its callback");
    require(runtimeStoppedBeforeService.callbackRunning
            && runtimeStoppedBeforeService.planInstalled,
        "runtime Stopped before service must retain callback=true and plan=true");

    host.serviceNonRealtime();
    const auto afterService = host.snapshot();
    require(!afterService.callbackRunning && afterService.planInstalled,
        "service must stop the callback without silently releasing the owned plan");

    host.hardStopAndReset();
    const auto afterHardReset = host.snapshot();
    require(!afterHardReset.callbackRunning && !afterHardReset.planInstalled,
        "hard reset must publish callback=false and plan=false");
}

void faultedHardResetRetainsDiagnosticsAndClearsCallbackAndPlan()
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
    require(host.openOutput({}).success, "faulted quiescence fixture output must open");
    require(host.installAndStart(makePlan(host.deviceFormatSnapshot())).success,
        "faulted quiescence fixture plan must start");

    observedType->activeDevice()->runCallback(64);
    const auto faulted = host.snapshot();
    require(faulted.realtime.state == trackloom::RealtimePlaybackState::Faulted
            && faulted.realtime.lastError
                == trackloom::RealtimeAudioError::CallbackException,
        "callback exception must publish stable fault diagnostics");
    require(faulted.callbackRunning && faulted.planInstalled,
        "faulted callback path must expose the still-owned callback and plan");

    host.hardStopAndReset();
    const auto reset = host.snapshot();
    require(reset.realtime.state == trackloom::RealtimePlaybackState::Faulted
            && reset.realtime.lastError
                == trackloom::RealtimeAudioError::CallbackException,
        "fault hard reset must retain the original diagnostic classification");
    require(!reset.callbackRunning && !reset.planInstalled,
        "fault hard reset must clear callback and plan ownership evidence");
}

}

int main()
{
    try {
        snapshotTracksCallbackAndPlanAcrossTheStoppedServiceBoundary();
        faultedHardResetRetainsDiagnosticsAndClearsCallbackAndPlan();
    } catch (const std::exception& error) {
        std::cerr << "D1 JUCE host quiescence test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "D1 JUCE host quiescence tests passed\n";
    return 0;
}
