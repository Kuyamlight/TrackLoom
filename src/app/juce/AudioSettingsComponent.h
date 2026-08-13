#pragma once

#include "AppAudioSettings.h"
#include "JuceAudioHost.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <string>
#include <vector>

namespace trackloom {

inline constexpr auto audioDeviceSelectorComponentId = "trackloom-audio-device";
inline constexpr auto audioSampleRateSelectorComponentId = "trackloom-audio-sample-rate";
inline constexpr auto audioBufferSelectorComponentId = "trackloom-audio-buffer";
inline constexpr auto audioChannelsSelectorComponentId = "trackloom-audio-channels";
inline constexpr auto audioApplyButtonComponentId = "trackloom-audio-apply";
inline constexpr auto audioTestToneButtonComponentId = "trackloom-audio-test-tone";
inline constexpr auto audioApplyStatusComponentId = "trackloom-audio-apply-status";

struct AudioSettingsComponentCallbacks {
    std::function<void(const AppAudioSettings&)> settingsApplied;
    std::function<void(std::string)> feedback;
};

class AudioSettingsComponent final
    : public juce::Component
    , private juce::Timer {
public:
    AudioSettingsComponent(
        JuceAudioHost& host,
        AppAudioSettings initialSettings,
        AudioSettingsComponentCallbacks callbacks = {});
    ~AudioSettingsComponent() override;

    void refreshFromHost();
    AppAudioSettings selectedSettings() const;
    bool applySelectedSettings();
    bool triggerTestTone();
    void resized() override;

private:
    void timerCallback() override;
    void rebuildDevicesFromCache();
    void rebuildFormatSelectors();
    void refreshEnabledState();
    bool candidateMatchesAppliedFormat() const;
    const JuceAudioOutputDeviceInfo* selectedDeviceInfo() const;

    JuceAudioHost& host_;
    AppAudioSettings appliedSettings_;
    AudioSettingsComponentCallbacks callbacks_;
    std::vector<JuceAudioOutputDeviceInfo> devices_;
    juce::ComboBox deviceSelector_;
    juce::ComboBox sampleRateSelector_;
    juce::ComboBox bufferSelector_;
    juce::ComboBox channelsSelector_;
    juce::TextButton applyButton_;
    juce::TextButton testToneButton_;
    juce::Label applyStatus_;
    juce::String statusOverride_;
    std::uint64_t lastObservedDeviceListRevision_ = 0;
};

}
