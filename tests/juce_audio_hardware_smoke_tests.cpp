#include "JuceAudioHost.h"
#include "PreparedMidiPlaybackPlan.h"
#include "ProjectFile.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int skipCode = 77;

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

}

int main()
{
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

        const auto open = host.openOutput({outputName, 48000.0, 256, 2});
        require(open.success, "hardware smoke should open selected audio output: " + open.message);
        const auto& format = open.actualFormat;
        std::cout << "opened_device id=\"" << format.deviceId << "\" name=\""
                  << format.deviceName << "\"\n";
        require(format.sampleRate == 48000.0 && format.maximumBlockFrames == 256
                && format.outputChannelCount == 2 && format.outputChannelMask == 3,
            "actual audio format differs from the confirmed 48 kHz, 256-frame stereo request");

        const auto loaded = trackloom::loadProjectFromFile(
            std::filesystem::path("tests/fixtures/audio/minimum-audible-midi/reference.trackloom"));
        require(loaded.project.has_value(), "hardware reference fixture should load: " + loaded.error);
        std::vector<double> planBuildMilliseconds;
        std::unique_ptr<const trackloom::PreparedMidiPlaybackPlan> installedPlan;
        for (int iteration = 0; iteration < 10; ++iteration) {
            const auto started = std::chrono::steady_clock::now();
            auto built = trackloom::buildPreparedMidiPlaybackPlan(makeRequest(*loaded.project, format));
            const auto elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            require(built.plan != nullptr, "hardware reference plan should build");
            require(elapsed <= 250.0, "hardware reference plan build should complete within 250 ms");
            planBuildMilliseconds.push_back(elapsed);
            installedPlan = std::move(built.plan);
        }
        for (std::size_t index = 0; index < planBuildMilliseconds.size(); ++index) {
            std::cout << "plan_build_ms[" << index + 1 << "]=" << planBuildMilliseconds[index] << '\n';
        }
        const auto installed = host.installAndStart(std::move(installedPlan));
        require(installed.success, "hardware smoke should install and start reference plan: " + installed.message);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(smokeSeconds());
        while (std::chrono::steady_clock::now() < deadline) {
            host.serviceNonRealtime();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        host.requestStop();
        for (int attempt = 0; attempt < 100; ++attempt) {
            host.serviceNonRealtime();
            if (host.snapshot().realtime.state == trackloom::RealtimePlaybackState::Stopped) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto snapshot = host.snapshot();
        printSnapshot(snapshot);
        verifyDiagnostics(snapshot);
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
}
