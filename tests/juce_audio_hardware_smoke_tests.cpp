#include "JuceAudioHost.h"
#include "PreparedMidiPlaybackPlan.h"
#include "ProjectFile.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#if defined(_WIN32)
#include <objbase.h>
#endif

namespace {

constexpr int skipCode = 77;
constexpr double requestedSampleRate = 48000.0;
constexpr int requestedBufferFrames = 256;
constexpr int requestedOutputChannels = 2;

using ComResultCode = std::int32_t;
using ComInitializeOperation = std::function<ComResultCode()>;
using ComUninitializeOperation = std::function<void()>;
using HostConstructionOperation = std::function<int()>;
using AcceptedFormatOperation = std::function<int(
    const trackloom::AudioDeviceFormatSnapshot&)>;

int runWithComApartment(
    const ComInitializeOperation& initialize,
    const ComUninitializeOperation& uninitialize,
    const HostConstructionOperation& constructAndRunHost,
    std::ostream& output,
    std::ostream& error);

int runAfterNegotiatedFormatCheck(
    const trackloom::JuceAudioOpenRequest& request,
    const trackloom::AudioDeviceFormatSnapshot& actual,
    std::string_view deviationAcceptance,
    std::ostream& output,
    std::ostream& error,
    const AcceptedFormatOperation& continueWithActualFormat);

int runAfterAudioOpenCheck(
    const trackloom::JuceAudioOpenRequest& request,
    const trackloom::JuceAudioHostResult& open,
    std::string_view deviationAcceptance,
    std::ostream& output,
    std::ostream& error,
    const AcceptedFormatOperation& continueWithActualFormat);

class ScopedComApartment final {
public:
    ScopedComApartment(
        const ComInitializeOperation& initialize,
        const ComUninitializeOperation& uninitialize)
        : result_(initialize()), uninitialize_(uninitialize),
          ownsInitialization_(result_ == 0 || result_ == 1)
    {
    }

    ~ScopedComApartment()
    {
        if (ownsInitialization_ && uninitialize_) {
            uninitialize_();
        }
    }

    [[nodiscard]] ComResultCode result() const noexcept
    {
        return result_;
    }

    [[nodiscard]] bool canContinue() const noexcept
    {
        constexpr ComResultCode changedMode =
            static_cast<ComResultCode>(0x80010106u);
        return ownsInitialization_ || result_ == changedMode;
    }

private:
    ComResultCode result_ = 0;
    ComUninitializeOperation uninitialize_;
    bool ownsInitialization_ = false;
};

int runWithComApartment(
    const ComInitializeOperation& initialize,
    const ComUninitializeOperation& uninitialize,
    const HostConstructionOperation& constructAndRunHost,
    std::ostream& output,
    std::ostream& error)
{
    constexpr ComResultCode initialized = 0;
    constexpr ComResultCode alreadyInitialized = 1;
    constexpr ComResultCode changedMode = static_cast<ComResultCode>(0x80010106u);

    ScopedComApartment apartment(initialize, uninitialize);
    if (!apartment.canContinue()) {
        const auto oldFlags = error.flags();
        const auto oldFill = error.fill();
        error << "COM initialization failed hresult=0x"
              << std::hex << std::setw(8) << std::setfill('0')
              << static_cast<std::uint32_t>(apartment.result()) << '\n';
        error.flags(oldFlags);
        error.fill(oldFill);
        return 1;
    }
    if (apartment.result() == initialized) {
        output << "COM status=initialized result=S_OK\n";
    } else if (apartment.result() == alreadyInitialized) {
        output << "COM status=initialized result=S_FALSE\n";
    } else if (apartment.result() == changedMode) {
        output << "COM status=existing apartment result=RPC_E_CHANGED_MODE\n";
    }
    return constructAndRunHost();
}

int runAfterNegotiatedFormatCheck(
    const trackloom::JuceAudioOpenRequest& request,
    const trackloom::AudioDeviceFormatSnapshot& actual,
    std::string_view deviationAcceptance,
    std::ostream& output,
    std::ostream& error,
    const AcceptedFormatOperation& continueWithActualFormat)
{
    const auto requestedMask = request.requestedOutputChannels <= 0
        ? std::uint64_t {0}
        : request.requestedOutputChannels >= 64
            ? ~std::uint64_t {0}
            : (std::uint64_t {1} << request.requestedOutputChannels) - 1;
    const auto sampleRateDiffers = actual.sampleRate != request.requestedSampleRate;
    const auto blockFramesDiffers =
        actual.maximumBlockFrames != request.requestedBufferFrames;
    const auto outputChannelsDiffer =
        actual.outputChannelCount != request.requestedOutputChannels;
    const auto outputMaskDiffers = actual.outputChannelMask != requestedMask;

    output << "request sample_rate=" << request.requestedSampleRate
           << " block_frames=" << request.requestedBufferFrames
           << " output_channels=" << request.requestedOutputChannels
           << " output_channel_mask=" << requestedMask << '\n';
    output << "actual sample_rate=" << actual.sampleRate
           << " block_frames=" << actual.maximumBlockFrames
           << " output_channels=" << actual.outputChannelCount
           << " output_channel_mask=" << actual.outputChannelMask << '\n';
    output << "format_difference field=sample_rate requested="
           << request.requestedSampleRate << " actual=" << actual.sampleRate
           << " differs=" << (sampleRateDiffers ? "true" : "false") << '\n';
    output << "format_difference field=block_frames requested="
           << request.requestedBufferFrames << " actual=" << actual.maximumBlockFrames
           << " differs=" << (blockFramesDiffers ? "true" : "false") << '\n';
    output << "format_difference field=output_channels requested="
           << request.requestedOutputChannels << " actual=" << actual.outputChannelCount
           << " differs=" << (outputChannelsDiffer ? "true" : "false") << '\n';
    output << "format_difference field=output_channel_mask requested="
           << requestedMask << " actual=" << actual.outputChannelMask
           << " differs=" << (outputMaskDiffers ? "true" : "false") << '\n';

    const auto actualIsValid = actual.available
        && !actual.deviceId.empty()
        && !actual.deviceName.empty()
        && std::isfinite(actual.sampleRate)
        && actual.sampleRate > 0.0
        && actual.maximumBlockFrames > 0
        && actual.outputChannelCount > 0
        && actual.outputChannelCount <= 64
        && actual.outputChannelMask != 0
        && std::popcount(actual.outputChannelMask)
            == actual.outputChannelCount;
    if (!actualIsValid) {
        error << "Hardware audio smoke test failed: invalid actual audio format\n";
        return 1;
    }

    const auto deviates = sampleRateDiffers || blockFramesDiffers
        || outputChannelsDiffer || outputMaskDiffers;
    if (deviates && deviationAcceptance != "1") {
        error << "Hardware audio smoke test failed: negotiated format differs; "
                 "review the printed fields and set "
                 "TRACKLOOM_ACCEPT_AUDIO_FORMAT_DEVIATION=1 to accept it\n";
        return 1;
    }
    if (deviates) {
        output << "format_deviation accepted=true "
                  "source=TRACKLOOM_ACCEPT_AUDIO_FORMAT_DEVIATION\n";
    } else {
        output << "format_deviation accepted=not_required\n";
    }
    return continueWithActualFormat(actual);
}

int runAfterAudioOpenCheck(
    const trackloom::JuceAudioOpenRequest& request,
    const trackloom::JuceAudioHostResult& open,
    std::string_view deviationAcceptance,
    std::ostream& output,
    std::ostream& error,
    const AcceptedFormatOperation& continueWithActualFormat)
{
    if (!open.success) {
        error << "Hardware audio smoke test failed: output device open failed: "
              << open.message << '\n';
        return 1;
    }
    return runAfterNegotiatedFormatCheck(
        request,
        open.actualFormat,
        deviationAcceptance,
        output,
        error,
        continueWithActualFormat);
}

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

const char* envValue(const char* name)
{
    return std::getenv(name);
}

bool hardwareSmokeEnabled()
{
    const auto* enabled = envValue("TRACKLOOM_AUDIO_HARDWARE_SMOKE");
    return enabled != nullptr && std::string(enabled) == "1";
}

std::string requestedOutputName()
{
    const auto* name = envValue("TRACKLOOM_AUDIO_OUTPUT_NAME");
    return name == nullptr ? std::string{} : std::string{name};
}

std::string_view formatDeviationAcceptance()
{
    const auto* acceptance = envValue("TRACKLOOM_ACCEPT_AUDIO_FORMAT_DEVIATION");
    return acceptance == nullptr ? std::string_view{} : std::string_view {acceptance};
}

int smokeSeconds()
{
    const auto* seconds = envValue("TRACKLOOM_AUDIO_SMOKE_SECONDS");
    if (seconds == nullptr) {
        return 600;
    }
    const auto parsed = std::atoi(seconds);
    return parsed > 0 ? parsed : 600;
}

void printAvailableDevices(const std::vector<trackloom::JuceAudioOutputDeviceInfo>& devices)
{
    if (devices.empty()) {
        std::cout << "No JUCE audio output devices are currently visible.\n";
        return;
    }
    std::cout << "Available JUCE audio output devices:\n";
    for (const auto& device : devices) {
        std::cout << "  id=\"" << device.id << "\" name=\"" << device.name
                  << "\" default=" << (device.isDefault ? "true" : "false") << '\n';
    }
}

trackloom::PreparedMidiPlaybackPlanBuildRequest makeRequest(
    const trackloom::Project& project,
    const trackloom::AudioDeviceFormatSnapshot& format)
{
    trackloom::PreparedMidiPlaybackPlanBuildRequest request;
    request.projectSnapshot = project;
    request.sampleRate = format.sampleRate;
    request.maximumBlockFrames = format.maximumBlockFrames;
    request.outputChannelCount = format.outputChannelCount;
    request.outputChannelMask = format.outputChannelMask;
    request.playbackStartSample = 0;
    request.loopRange = trackloom::PlaybackLoopRange{0, 3840};
    return request;
}

void printSnapshot(const trackloom::RealtimePlaybackHostSnapshot& snapshot)
{
    const auto& format = snapshot.format;
    const auto& diagnostics = snapshot.realtime;
    std::cout << "actual_device id=\"" << format.deviceId << "\" name=\""
              << format.deviceName << "\" sample_rate=" << format.sampleRate
              << " block_frames=" << format.maximumBlockFrames
              << " output_channels=" << format.outputChannelCount
              << " output_channel_mask=" << format.outputChannelMask << '\n';
    std::cout << "diagnostics xrun=" << snapshot.xRunCount
              << " callback_timeout=" << diagnostics.callbackTimeoutCount
              << " callback_exception=" << diagnostics.callbackExceptionCount
              << " oversized_block=" << diagnostics.oversizedBlockCount
              << " voice_steal=" << diagnostics.voiceStealCount
              << " stale_note_off=" << diagnostics.staleNoteOffCount
              << " callbacks=" << diagnostics.callbackCount
              << " rendered_samples=" << diagnostics.renderedSampleCount << '\n';
}

void verifyDiagnostics(const trackloom::RealtimePlaybackHostSnapshot& snapshot)
{
    const auto& diagnostics = snapshot.realtime;
    if (snapshot.xRunCount != -1) {
        require(snapshot.xRunCount == 0, "hardware smoke xrun count should be zero");
    } else {
        require(diagnostics.callbackTimeoutCount == 0,
            "hardware smoke callback timeout count should be zero when xrun is unavailable");
    }
    require(diagnostics.callbackExceptionCount == 0, "hardware smoke callback exceptions should be zero");
    require(diagnostics.oversizedBlockCount == 0, "hardware smoke oversized blocks should be zero");
    require(diagnostics.voiceStealCount == 0, "hardware smoke voice stealing should be zero");
    require(diagnostics.staleNoteOffCount == 0, "hardware smoke stale Note Off count should be zero");
}

void verifyCompletedRun(
    const trackloom::RealtimePlaybackHostSnapshot& snapshot,
    bool stopRequestSucceeded,
    double requestedSeconds,
    double observedSeconds)
{
    require(stopRequestSucceeded, "hardware smoke stop request should succeed");
    require(snapshot.realtime.state == trackloom::RealtimePlaybackState::Stopped,
        "hardware smoke should stop before completion is accepted");
    require(snapshot.realtime.lastError == trackloom::RealtimeAudioError::None,
        "hardware smoke should finish without a realtime error");
    require(observedSeconds >= requestedSeconds - 0.1,
        "hardware smoke observed duration should cover the requested duration");
    require(snapshot.realtime.callbackCount > 0,
        "hardware smoke should receive at least one audio callback");
    require(snapshot.realtime.renderedSampleCount > 0,
        "hardware smoke should render at least one sample");
    require(snapshot.format.sampleRate > 0.0,
        "hardware smoke final format should contain a sample rate");
    const auto minimumExpectedSamples = snapshot.format.sampleRate * observedSeconds * 0.8;
    require(static_cast<double>(snapshot.realtime.renderedSampleCount) >= minimumExpectedSamples,
        "hardware smoke rendered samples should cover at least 80 percent of wall duration");
    verifyDiagnostics(snapshot);
}

bool completionIsRejected(
    const trackloom::RealtimePlaybackHostSnapshot& snapshot,
    bool stopRequestSucceeded,
    double requestedSeconds,
    double observedSeconds)
{
    try {
        verifyCompletedRun(snapshot, stopRequestSucceeded, requestedSeconds, observedSeconds);
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

void runComApartmentSelfTest()
{
    constexpr ComResultCode initialized = 0;
    constexpr ComResultCode alreadyInitialized = 1;
    constexpr ComResultCode changedMode = static_cast<ComResultCode>(0x80010106u);
    constexpr ComResultCode failed = static_cast<ComResultCode>(0x80004005u);

    const auto exercise = [](ComResultCode result) {
        std::vector<std::string> events;
        std::ostringstream output;
        std::ostringstream error;
        const auto exitCode = runWithComApartment(
            [&]() {
                events.push_back("initialize");
                return result;
            },
            [&]() { events.push_back("uninitialize"); },
            [&]() {
                events.push_back("host");
                return 23;
            },
            output,
            error);
        return std::tuple {exitCode, events, output.str(), error.str()};
    };

    const auto [initializedExit, initializedEvents, initializedOutput, initializedError] =
        exercise(initialized);
    require(initializedExit == 23,
        "S_OK COM initialization should run and return the host operation result");
    require(initializedEvents == std::vector<std::string>({"initialize", "host", "uninitialize"}),
        "S_OK must initialize before host construction and uninitialize exactly once afterward");
    require(initializedOutput.find("initialized") != std::string::npos
            && initializedError.empty(),
        "S_OK should report an initialized COM apartment without an error");

    const auto [existingExit, existingEvents, existingOutput, existingError] =
        exercise(alreadyInitialized);
    require(existingExit == 23,
        "S_FALSE COM initialization should still run the host operation");
    require(existingEvents == std::vector<std::string>({"initialize", "host", "uninitialize"}),
        "S_FALSE must retain and release the successful COM initialization exactly once");
    require(existingOutput.find("initialized") != std::string::npos
            && existingError.empty(),
        "S_FALSE should report a usable initialized COM apartment without an error");

    const auto [changedExit, changedEvents, changedOutput, changedError] =
        exercise(changedMode);
    require(changedExit == 23,
        "RPC_E_CHANGED_MODE should continue in the thread's existing COM apartment");
    require(changedEvents == std::vector<std::string>({"initialize", "host"}),
        "RPC_E_CHANGED_MODE must never uninitialize an apartment owned by another caller");
    require(changedOutput.find("existing apartment") != std::string::npos
            && changedError.empty(),
        "RPC_E_CHANGED_MODE should explicitly report the existing apartment");

    const auto [failedExit, failedEvents, failedOutput, failedError] = exercise(failed);
    require(failedExit == 1,
        "an unexpected COM initialization failure should stop the runner");
    require(failedEvents == std::vector<std::string>({"initialize"}),
        "a failed COM initialization must stop before host construction and never uninitialize");
    require(failedOutput.empty()
            && failedError.find("0x80004005") != std::string::npos,
        "a COM initialization failure should expose a stable HRESULT diagnostic");
}

void runNegotiatedFormatSelfTest()
{
    const trackloom::JuceAudioOpenRequest request {"Fake Speakers", 48000.0, 256, 2};
    trackloom::AudioDeviceFormatSnapshot matching;
    matching.deviceId = "Windows Audio/Fake Speakers";
    matching.deviceName = "Fake Speakers";
    matching.sampleRate = 48000.0;
    matching.maximumBlockFrames = 256;
    matching.outputChannelCount = 2;
    matching.outputChannelMask = 3;
    matching.available = true;

    const auto exercise = [&](const trackloom::AudioDeviceFormatSnapshot& actual,
                              std::string_view acceptance) {
        int continuationCalls = 0;
        trackloom::AudioDeviceFormatSnapshot received;
        std::ostringstream output;
        std::ostringstream error;
        trackloom::JuceAudioHostResult open;
        open.success = true;
        open.actualFormat = actual;
        const auto exitCode = runAfterAudioOpenCheck(
            request,
            open,
            acceptance,
            output,
            error,
            [&](const trackloom::AudioDeviceFormatSnapshot& format) {
                ++continuationCalls;
                received = format;
                return 29;
            });
        return std::tuple {
            exitCode, continuationCalls, received, output.str(), error.str()};
    };

    const auto [matchingExit, matchingCalls, matchingReceived, matchingOutput,
                matchingError] = exercise(matching, "");
    require(matchingExit == 29 && matchingCalls == 1,
        "a matching negotiated format should continue without confirmation");
    require(matchingReceived.maximumBlockFrames == 256
            && matchingReceived.outputChannelMask == 3,
        "a matching format continuation should receive the actual device format");
    require(matchingError.empty()
            && matchingOutput.find("field=sample_rate requested=48000 actual=48000 differs=false")
                != std::string::npos
            && matchingOutput.find("field=block_frames requested=256 actual=256 differs=false")
                != std::string::npos
            && matchingOutput.find("field=output_channels requested=2 actual=2 differs=false")
                != std::string::npos
            && matchingOutput.find("field=output_channel_mask requested=3 actual=3 differs=false")
                != std::string::npos,
        "the format report should print all four matching fields before continuing");

    auto deviated = matching;
    deviated.sampleRate = 44100.0;
    deviated.maximumBlockFrames = 1920;
    deviated.outputChannelCount = 1;
    deviated.outputChannelMask = 1;
    const auto [rejectedExit, rejectedCalls, rejectedReceived, rejectedOutput,
                rejectedError] = exercise(deviated, "");
    require(rejectedExit == 1 && rejectedCalls == 0,
        "a negotiated format deviation must stop before plan construction without confirmation");
    require(rejectedOutput.find("field=sample_rate requested=48000 actual=44100 differs=true")
                != std::string::npos
            && rejectedOutput.find("field=block_frames requested=256 actual=1920 differs=true")
                != std::string::npos
            && rejectedOutput.find("field=output_channels requested=2 actual=1 differs=true")
                != std::string::npos
            && rejectedOutput.find("field=output_channel_mask requested=3 actual=1 differs=true")
                != std::string::npos,
        "a rejected deviation should still print every request/actual field difference");
    require(rejectedError.find("TRACKLOOM_ACCEPT_AUDIO_FORMAT_DEVIATION=1")
            != std::string::npos,
        "a rejected deviation should name the exact opt-in required to continue");

    const auto [wrongOptInExit, wrongOptInCalls, wrongOptInReceived,
                wrongOptInOutput, wrongOptInError] = exercise(deviated, "true");
    require(wrongOptInExit == 1 && wrongOptInCalls == 0,
        "only the exact deviation confirmation value 1 should continue");

    const auto [acceptedExit, acceptedCalls, acceptedReceived, acceptedOutput,
                acceptedError] = exercise(deviated, "1");
    require(acceptedExit == 29 && acceptedCalls == 1,
        "an explicitly accepted negotiated format deviation should continue");
    require(acceptedReceived.sampleRate == 44100.0
            && acceptedReceived.maximumBlockFrames == 1920
            && acceptedReceived.outputChannelCount == 1
            && acceptedReceived.outputChannelMask == 1,
        "an accepted deviation must build and run from the actual format, not the request");
    require(acceptedOutput.find("format_deviation accepted=true") != std::string::npos
            && acceptedError.empty(),
        "accepted format deviation evidence should be explicit");

    auto invalid = matching;
    invalid.available = false;
    const auto [invalidExit, invalidCalls, invalidReceived, invalidOutput,
                invalidError] = exercise(invalid, "1");
    require(invalidExit == 1 && invalidCalls == 0,
        "an invalid actual format must stop before plan construction even with confirmation");
    require(invalidError.find("invalid actual audio format") != std::string::npos,
        "an invalid actual format should have a stable failure diagnostic");

    trackloom::JuceAudioHostResult failedOpen;
    failedOpen.success = false;
    failedOpen.message = "device refused the requested format";
    int failedOpenContinuationCalls = 0;
    std::ostringstream failedOpenOutput;
    std::ostringstream failedOpenError;
    const auto failedOpenExit = runAfterAudioOpenCheck(
        request,
        failedOpen,
        "1",
        failedOpenOutput,
        failedOpenError,
        [&](const trackloom::AudioDeviceFormatSnapshot&) {
            ++failedOpenContinuationCalls;
            return 29;
        });
    require(failedOpenExit == 1 && failedOpenContinuationCalls == 0,
        "an audio device open failure must stop before format acceptance and plan construction");
    require(failedOpenError.str().find("device refused the requested format") != std::string::npos,
        "an audio device open failure should retain the host diagnostic");
}

void runCompletionSelfTest()
{
    runComApartmentSelfTest();
    runNegotiatedFormatSelfTest();
    trackloom::RealtimePlaybackHostSnapshot healthy;
    healthy.format.sampleRate = 48000.0;
    healthy.realtime.state = trackloom::RealtimePlaybackState::Stopped;
    healthy.realtime.lastError = trackloom::RealtimeAudioError::None;
    healthy.realtime.callbackCount = 100;
    healthy.realtime.renderedSampleCount = 48000;
    require(!completionIsRejected(healthy, true, 1.0, 1.0),
        "healthy hardware completion should be accepted");

    auto zeroCallback = healthy;
    zeroCallback.realtime.callbackCount = 0;
    zeroCallback.realtime.renderedSampleCount = 0;
    require(completionIsRejected(zeroCallback, true, 1.0, 1.0),
        "zero callback hardware completion should be rejected");

    auto deviceError = healthy;
    deviceError.realtime.lastError = trackloom::RealtimeAudioError::DeviceError;
    require(completionIsRejected(deviceError, true, 1.0, 1.0),
        "realtime device error hardware completion should be rejected");

    auto stopping = healthy;
    stopping.realtime.state = trackloom::RealtimePlaybackState::Stopping;
    require(completionIsRejected(stopping, true, 1.0, 1.0),
        "stop timeout hardware completion should be rejected");

    require(completionIsRejected(healthy, false, 1.0, 1.0),
        "failed stop request hardware completion should be rejected");
}

}

int main()
{
    const auto constructAndRunHost = []() {
        if (const auto* selfTest = envValue("TRACKLOOM_AUDIO_HARDWARE_SMOKE_SELF_TEST");
            selfTest != nullptr && std::string(selfTest) == "1") {
            try {
                runCompletionSelfTest();
            } catch (const std::exception& error) {
                std::cerr << "Hardware audio smoke completion self-test failed: "
                          << error.what() << '\n';
                return 1;
            }
            std::cout << "Hardware audio smoke completion self-test passed\n";
            return 0;
        }

        trackloom::JuceAudioHost host;
        try {
            const auto devices = host.refreshOutputDevices();
            if (!hardwareSmokeEnabled()) {
                std::cout << "Skipping hardware audio smoke test. Set TRACKLOOM_AUDIO_HARDWARE_SMOKE=1 to enable it.\n";
                printAvailableDevices(devices);
                return skipCode;
            }

            const auto outputName = requestedOutputName();
            if (!outputName.empty()) {
                const auto selected = std::find_if(devices.begin(), devices.end(), [&](const auto& device) {
                    return device.name == outputName;
                });
                require(selected != devices.end(), "requested audio output display name was not found");
                std::cout << "selected_device id=\"" << selected->id << "\" name=\""
                          << selected->name << "\"\n";
            } else {
                std::cout << "selected_device default=true\n";
            }

            const trackloom::JuceAudioOpenRequest openRequest {
                outputName,
                requestedSampleRate,
                requestedBufferFrames,
                requestedOutputChannels
            };
            const auto open = host.openOutput(openRequest);
            const auto formatGate = runAfterAudioOpenCheck(
                openRequest,
                open,
                formatDeviationAcceptance(),
                std::cout,
                std::cerr,
                [&](const trackloom::AudioDeviceFormatSnapshot& format) {
                    std::cout << "opened_device id=\"" << format.deviceId
                              << "\" name=\"" << format.deviceName << "\"\n";

                    const auto loaded = trackloom::loadProjectFromFile(
                        std::filesystem::path(
                            "tests/fixtures/audio/minimum-audible-midi/reference.trackloom"));
                    require(loaded.project.has_value(),
                        "hardware reference fixture should load: " + loaded.error);
                    std::vector<double> planBuildMilliseconds;
                    std::unique_ptr<const trackloom::PreparedMidiPlaybackPlan> installedPlan;
                    for (int iteration = 0; iteration < 10; ++iteration) {
                        const auto started = std::chrono::steady_clock::now();
                        auto built = trackloom::buildPreparedMidiPlaybackPlan(
                            makeRequest(*loaded.project, format));
                        const auto elapsed = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - started).count();
                        require(built.plan != nullptr,
                            "hardware reference plan should build");
                        require(elapsed <= 250.0,
                            "hardware reference plan build should complete within 250 ms");
                        planBuildMilliseconds.push_back(elapsed);
                        installedPlan = std::move(built.plan);
                    }
                    for (std::size_t index = 0;
                         index < planBuildMilliseconds.size(); ++index) {
                        std::cout << "plan_build_ms[" << index + 1 << "]="
                                  << planBuildMilliseconds[index] << '\n';
                    }
                    const auto installed = host.installAndStart(std::move(installedPlan));
                    require(installed.success,
                        "hardware smoke should install and start reference plan: "
                            + installed.message);

                    const auto requestedSeconds = static_cast<double>(smokeSeconds());
                    const auto playbackStarted = std::chrono::steady_clock::now();
                    const auto deadline = playbackStarted
                        + std::chrono::duration<double>(requestedSeconds);
                    while (std::chrono::steady_clock::now() < deadline) {
                        host.serviceNonRealtime();
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                    const auto stopRequested = host.requestStop();
                    for (int attempt = 0; attempt < 100; ++attempt) {
                        host.serviceNonRealtime();
                        if (host.snapshot().realtime.state
                            == trackloom::RealtimePlaybackState::Stopped) {
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    const auto snapshot = host.snapshot();
                    const auto observedSeconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - playbackStarted).count();
                    std::cout << "duration requested_seconds=" << requestedSeconds
                              << " observed_seconds=" << observedSeconds
                              << " final_state="
                              << static_cast<unsigned int>(snapshot.realtime.state)
                              << " final_error="
                              << static_cast<unsigned int>(snapshot.realtime.lastError)
                              << '\n';
                    printSnapshot(snapshot);
                    verifyCompletedRun(
                        snapshot, stopRequested, requestedSeconds, observedSeconds);
                    return 0;
                });
            if (formatGate != 0) {
                host.hardStopAndReset();
                host.close();
                return formatGate;
            }
            host.hardStopAndReset();
            host.close();
        } catch (const std::exception& error) {
            host.hardStopAndReset();
            host.close();
            std::cerr << "Hardware audio smoke test failed: " << error.what() << '\n';
            return 1;
        }
        std::cout << "Hardware audio smoke test passed\n";
        return 0;
    };

#if defined(_WIN32)
    return runWithComApartment(
        []() {
            return static_cast<ComResultCode>(
                CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        },
        []() { CoUninitialize(); },
        constructAndRunHost,
        std::cout,
        std::cerr);
#else
    return constructAndRunHost();
#endif
}
