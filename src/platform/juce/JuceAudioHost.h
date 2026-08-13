#pragma once

#include "RealtimePlaybackHost.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace trackloom {

struct JuceAudioOpenRequest {
    std::string outputDeviceName;
    double requestedSampleRate = 48000.0;
    int requestedBufferFrames = 256;
    int requestedOutputChannels = 2;
};

struct JuceAudioOutputDeviceInfo {
    std::string id;
    std::string name;
    bool isDefault = false;
    std::vector<double> sampleRates;
    std::vector<int> bufferSizes;
    int maximumOutputChannels = 0;
};

enum class JuceAudioHostFailureReason {
    None,
    UnsupportedPlatform,
    DeviceTypeUnavailable,
    DeviceNotFound,
    DeviceCreateFailed,
    DeviceOpenFailed,
    PlaybackActive
};

struct JuceAudioHostResult {
    bool success = false;
    JuceAudioHostFailureReason failureReason = JuceAudioHostFailureReason::None;
    std::string message;
    std::string warning;
    AudioDeviceFormatSnapshot actualFormat;
};

using JuceAudioDeviceTypeFactory =
    std::function<std::unique_ptr<juce::AudioIODeviceType>()>;
using JuceRealtimeTickOperation = std::int64_t (*)() noexcept;
using JuceRealtimeBlockOperation = void (*)(
    PreparedMidiPlaybackRuntime&,
    float* const*,
    int,
    int);

class JuceAudioHost final :
    public RealtimePlaybackHost,
    private juce::AudioIODeviceCallback,
    private juce::AudioIODeviceType::Listener {
public:
    explicit JuceAudioHost(
        JuceAudioDeviceTypeFactory factory = {},
        JuceRealtimeTickOperation tickOperation = nullptr,
        std::int64_t ticksPerSecond = 0,
        JuceRealtimeBlockOperation blockOperation = nullptr);
    ~JuceAudioHost() override;
    std::vector<JuceAudioOutputDeviceInfo> refreshOutputDevices();
    std::vector<JuceAudioOutputDeviceInfo> outputDevicesSnapshot() const;
    JuceAudioHostResult openOutput(const JuceAudioOpenRequest& request);
    void close() noexcept;
    JuceAudioHostResult playTestTone();
    AudioDeviceFormatSnapshot deviceFormatSnapshot() const override;
    RealtimePlaybackHostResult installAndStart(
        std::unique_ptr<const PreparedMidiPlaybackPlan> plan) override;
    bool requestStop() noexcept override;
    void serviceNonRealtime() override;
    void hardStopAndReset() noexcept override;
    RealtimePlaybackHostSnapshot snapshot() const override;

private:
    void audioDeviceIOCallbackWithContext(
        const float* const* inputs,
        int inputChannelCount,
        float* const* outputs,
        int outputChannelCount,
        int frames,
        const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError(const juce::String& error) override;
    void audioDeviceListChanged() override;
    void invalidateCurrentOutput(RealtimeAudioError error) noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
