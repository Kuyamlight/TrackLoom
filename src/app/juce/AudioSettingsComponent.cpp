#include "AudioSettingsComponent.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace trackloom {
namespace {

juce::String utf8(const char8_t* text)
{
    return juce::String::fromUTF8(reinterpret_cast<const char*>(text));
}

AppAudioSettings settingsFromActualFormat(const AudioDeviceFormatSnapshot& format)
{
    return {
        format.deviceName,
        format.sampleRate,
        format.maximumBlockFrames,
        format.outputChannelCount
    };
}

bool sameSampleRate(double left, double right)
{
    return std::abs(left - right) < 0.001;
}

juce::String describeFormat(const AppAudioSettings& settings)
{
    return juce::String(settings.requestedSampleRate, 0)
        + " Hz / " + juce::String(settings.requestedBufferFrames)
        + " / " + juce::String(settings.requestedOutputChannels);
}

template <typename Value>
void selectMatchingValue(juce::ComboBox& box, const Value& wanted)
{
    for (int item = 0; item < box.getNumItems(); ++item) {
        const auto text = box.getItemText(item);
        if constexpr (std::is_floating_point_v<Value>) {
            if (std::abs(text.getDoubleValue() - wanted) < 0.001) {
                box.setSelectedItemIndex(item, juce::dontSendNotification);
                return;
            }
        } else if (text.getIntValue() == wanted) {
            box.setSelectedItemIndex(item, juce::dontSendNotification);
            return;
        }
    }
    if (box.getNumItems() > 0) {
        box.setSelectedItemIndex(0, juce::dontSendNotification);
    }
}

}

AudioSettingsComponent::AudioSettingsComponent(
    JuceAudioHost& host,
    AppAudioSettings initialSettings,
    AudioSettingsComponentCallbacks callbacks)
    : host_(host),
      appliedSettings_(std::move(initialSettings)),
      callbacks_(std::move(callbacks))
{
    const auto actualFormat = host_.deviceFormatSnapshot();
    if (actualFormat.available
        && (appliedSettings_.outputDeviceName.empty()
            || appliedSettings_.outputDeviceName == actualFormat.deviceName)) {
        appliedSettings_ = settingsFromActualFormat(actualFormat);
    }

    deviceSelector_.setComponentID(audioDeviceSelectorComponentId);
    sampleRateSelector_.setComponentID(audioSampleRateSelectorComponentId);
    bufferSelector_.setComponentID(audioBufferSelectorComponentId);
    channelsSelector_.setComponentID(audioChannelsSelectorComponentId);
    applyButton_.setComponentID(audioApplyButtonComponentId);
    testToneButton_.setComponentID(audioTestToneButtonComponentId);
    applyStatus_.setComponentID(audioApplyStatusComponentId);
    applyButton_.setButtonText(utf8(u8"应用"));
    testToneButton_.setButtonText(utf8(u8"测试音"));
    applyStatus_.setJustificationType(juce::Justification::centredLeft);

    addAndMakeVisible(deviceSelector_);
    addAndMakeVisible(sampleRateSelector_);
    addAndMakeVisible(bufferSelector_);
    addAndMakeVisible(channelsSelector_);
    addAndMakeVisible(applyButton_);
    addAndMakeVisible(testToneButton_);
    addAndMakeVisible(applyStatus_);

    deviceSelector_.onChange = [this] {
        statusOverride_.clear();
        rebuildFormatSelectors();
    };
    sampleRateSelector_.onChange = [this] {
        statusOverride_.clear();
        refreshEnabledState();
    };
    bufferSelector_.onChange = [this] {
        statusOverride_.clear();
        refreshEnabledState();
    };
    channelsSelector_.onChange = [this] {
        statusOverride_.clear();
        refreshEnabledState();
    };
    applyButton_.onClick = [this] { applySelectedSettings(); };
    testToneButton_.onClick = [this] { triggerTestTone(); };
    refreshFromHost();
    startTimerHz(30);
}

AudioSettingsComponent::~AudioSettingsComponent()
{
    stopTimer();
}

void AudioSettingsComponent::refreshFromHost()
{
    host_.refreshOutputDevices();
    rebuildDevicesFromCache();
    lastObservedDeviceListRevision_ = host_.snapshot().deviceListRevision;
}

void AudioSettingsComponent::rebuildDevicesFromCache()
{
    const auto previousName = selectedSettings().outputDeviceName;
    devices_ = host_.outputDevicesSnapshot();
    deviceSelector_.clear(juce::dontSendNotification);
    int selectedIndex = -1;
    bool retainedPrevious = false;
    for (std::size_t index = 0; index < devices_.size(); ++index) {
        const auto& device = devices_[index];
        if (device.id.rfind("Windows Audio/", 0) != 0) {
            continue;
        }
        deviceSelector_.addItem(
            "Windows Audio / " + juce::String::fromUTF8(device.name.c_str()),
            static_cast<int>(index) + 1);
        if ((!previousName.empty() && device.name == previousName)
            || (selectedIndex < 0 && device.isDefault)) {
            selectedIndex = deviceSelector_.getNumItems() - 1;
            retainedPrevious = !previousName.empty() && device.name == previousName;
        }
    }
    if (selectedIndex < 0 && deviceSelector_.getNumItems() > 0) {
        selectedIndex = 0;
    }
    deviceSelector_.setSelectedItemIndex(selectedIndex, juce::dontSendNotification);
    if (!retainedPrevious && !previousName.empty()) {
        statusOverride_.clear();
    }
    rebuildFormatSelectors();
}

AppAudioSettings AudioSettingsComponent::selectedSettings() const
{
    AppAudioSettings settings = appliedSettings_;
    if (const auto* device = selectedDeviceInfo()) {
        settings.outputDeviceName = device->name;
    }
    if (sampleRateSelector_.getSelectedItemIndex() >= 0) {
        settings.requestedSampleRate = sampleRateSelector_.getText().getDoubleValue();
    }
    if (bufferSelector_.getSelectedItemIndex() >= 0) {
        settings.requestedBufferFrames = bufferSelector_.getText().getIntValue();
    }
    if (channelsSelector_.getSelectedItemIndex() >= 0) {
        settings.requestedOutputChannels = channelsSelector_.getText().getIntValue();
    }
    return settings;
}

bool AudioSettingsComponent::applySelectedSettings()
{
    refreshEnabledState();
    if (!applyButton_.isEnabled()) {
        return false;
    }
    const auto settings = selectedSettings();
    const auto result = host_.openOutput({
        settings.outputDeviceName,
        settings.requestedSampleRate,
        settings.requestedBufferFrames,
        settings.requestedOutputChannels
    });
    if (callbacks_.feedback) {
        callbacks_.feedback(result.success
            ? (result.warning.empty() ? result.message : result.warning)
            : "应用音频设置失败：" + result.message);
    }
    if (!result.success) {
        statusOverride_ = utf8(u8"应用失败：")
            + juce::String::fromUTF8(result.message.c_str());
        refreshEnabledState();
        return false;
    }
    appliedSettings_ = settingsFromActualFormat(result.actualFormat);
    statusOverride_ = describeFormat(settings) == describeFormat(appliedSettings_)
            && settings.outputDeviceName == appliedSettings_.outputDeviceName
        ? utf8(u8"已应用")
        : utf8(u8"已应用（请求 ") + describeFormat(settings)
            + utf8(u8" -> 实际 ") + describeFormat(appliedSettings_) + ")";
    rebuildFormatSelectors();
    if (callbacks_.settingsApplied) {
        callbacks_.settingsApplied(appliedSettings_);
    }
    refreshEnabledState();
    return true;
}

bool AudioSettingsComponent::triggerTestTone()
{
    refreshEnabledState();
    if (!testToneButton_.isEnabled()) {
        return false;
    }
    const auto result = host_.playTestTone();
    if (callbacks_.feedback) {
        callbacks_.feedback(result.success ? "测试音已开始" : result.message);
    }
    refreshEnabledState();
    return result.success;
}

void AudioSettingsComponent::resized()
{
    auto area = getLocalBounds().reduced(12);
    constexpr int rowHeight = 28;
    constexpr int gap = 6;
    deviceSelector_.setBounds(area.removeFromTop(rowHeight));
    area.removeFromTop(gap);
    sampleRateSelector_.setBounds(area.removeFromTop(rowHeight));
    area.removeFromTop(gap);
    bufferSelector_.setBounds(area.removeFromTop(rowHeight));
    area.removeFromTop(gap);
    channelsSelector_.setBounds(area.removeFromTop(rowHeight));
    area.removeFromTop(gap);
    auto buttons = area.removeFromTop(rowHeight);
    applyButton_.setBounds(buttons.removeFromLeft(buttons.getWidth() / 2).reduced(2));
    testToneButton_.setBounds(buttons.reduced(2));
    area.removeFromTop(gap);
    applyStatus_.setBounds(area.removeFromTop(rowHeight));
}

void AudioSettingsComponent::timerCallback()
{
    host_.serviceNonRealtime();
    const auto deviceListRevision = host_.snapshot().deviceListRevision;
    if (deviceListRevision != lastObservedDeviceListRevision_) {
        rebuildDevicesFromCache();
        lastObservedDeviceListRevision_ = deviceListRevision;
    }
    refreshEnabledState();
}

void AudioSettingsComponent::rebuildFormatSelectors()
{
    sampleRateSelector_.clear(juce::dontSendNotification);
    bufferSelector_.clear(juce::dontSendNotification);
    channelsSelector_.clear(juce::dontSendNotification);
    if (const auto* device = selectedDeviceInfo()) {
        for (std::size_t index = 0; index < device->sampleRates.size(); ++index) {
            sampleRateSelector_.addItem(
                juce::String(device->sampleRates[index], 0),
                static_cast<int>(index) + 1);
        }
        for (std::size_t index = 0; index < device->bufferSizes.size(); ++index) {
            bufferSelector_.addItem(
                juce::String(device->bufferSizes[index]),
                static_cast<int>(index) + 1);
        }
        const auto channels = std::clamp(device->maximumOutputChannels, 0, 2);
        for (int channelCount = 1; channelCount <= channels; ++channelCount) {
            channelsSelector_.addItem(juce::String(channelCount), channelCount);
        }
        const auto actual = host_.deviceFormatSnapshot();
        if (actual.available && actual.deviceName == device->name) {
            bool foundSampleRate = false;
            for (int index = 0; index < sampleRateSelector_.getNumItems(); ++index) {
                foundSampleRate = foundSampleRate || sameSampleRate(
                    sampleRateSelector_.getItemText(index).getDoubleValue(),
                    actual.sampleRate);
            }
            if (!foundSampleRate && actual.sampleRate > 0.0) {
                sampleRateSelector_.addItem(
                    juce::String(actual.sampleRate, 0),
                    sampleRateSelector_.getNumItems() + 1);
            }
            bool foundBuffer = false;
            for (int index = 0; index < bufferSelector_.getNumItems(); ++index) {
                foundBuffer = foundBuffer
                    || bufferSelector_.getItemText(index).getIntValue()
                        == actual.maximumBlockFrames;
            }
            if (!foundBuffer && actual.maximumBlockFrames > 0) {
                bufferSelector_.addItem(
                    juce::String(actual.maximumBlockFrames),
                    bufferSelector_.getNumItems() + 1);
            }
            bool foundChannels = false;
            for (int index = 0; index < channelsSelector_.getNumItems(); ++index) {
                foundChannels = foundChannels
                    || channelsSelector_.getItemText(index).getIntValue()
                        == actual.outputChannelCount;
            }
            if (!foundChannels && actual.outputChannelCount > 0) {
                channelsSelector_.addItem(
                    juce::String(actual.outputChannelCount),
                    channelsSelector_.getNumItems() + 1);
            }
        }
    }
    selectMatchingValue(sampleRateSelector_, appliedSettings_.requestedSampleRate);
    selectMatchingValue(bufferSelector_, appliedSettings_.requestedBufferFrames);
    selectMatchingValue(channelsSelector_, appliedSettings_.requestedOutputChannels);
    refreshEnabledState();
}

void AudioSettingsComponent::refreshEnabledState()
{
    const auto state = host_.snapshot().realtime.state;
    const auto stopped = state == RealtimePlaybackState::Stopped;
    const auto canConfigure = state != RealtimePlaybackState::Playing
        && state != RealtimePlaybackState::Stopping;
    const auto hasDevice = selectedDeviceInfo() != nullptr;
    applyButton_.setEnabled(canConfigure && hasDevice);
    const auto candidateApplied = candidateMatchesAppliedFormat();
    testToneButton_.setEnabled(
        stopped && host_.deviceFormatSnapshot().available && candidateApplied);
    deviceSelector_.setEnabled(canConfigure);
    sampleRateSelector_.setEnabled(canConfigure);
    bufferSelector_.setEnabled(canConfigure);
    channelsSelector_.setEnabled(canConfigure);
    if (statusOverride_.isNotEmpty()) {
        applyStatus_.setText(statusOverride_, juce::dontSendNotification);
    } else if (!candidateApplied) {
        applyStatus_.setText(utf8(u8"待应用"), juce::dontSendNotification);
    } else if (host_.deviceFormatSnapshot().available) {
        applyStatus_.setText(utf8(u8"已应用"), juce::dontSendNotification);
    } else {
        applyStatus_.setText(utf8(u8"未应用"), juce::dontSendNotification);
    }
}

bool AudioSettingsComponent::candidateMatchesAppliedFormat() const
{
    const auto actual = host_.deviceFormatSnapshot();
    if (!actual.available) {
        return false;
    }
    const auto candidate = selectedSettings();
    return candidate.outputDeviceName == actual.deviceName
        && sameSampleRate(candidate.requestedSampleRate, actual.sampleRate)
        && candidate.requestedBufferFrames == actual.maximumBlockFrames
        && candidate.requestedOutputChannels == actual.outputChannelCount;
}

const JuceAudioOutputDeviceInfo* AudioSettingsComponent::selectedDeviceInfo() const
{
    const auto selectedId = deviceSelector_.getSelectedId();
    if (selectedId <= 0) {
        return nullptr;
    }
    const auto index = static_cast<std::size_t>(selectedId - 1);
    return index < devices_.size() ? &devices_[index] : nullptr;
}

}
