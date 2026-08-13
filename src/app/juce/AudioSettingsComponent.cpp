#include "AudioSettingsComponent.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace trackloom {
namespace {

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
      initialSettings_(std::move(initialSettings)),
      callbacks_(std::move(callbacks))
{
    deviceSelector_.setComponentID(audioDeviceSelectorComponentId);
    sampleRateSelector_.setComponentID(audioSampleRateSelectorComponentId);
    bufferSelector_.setComponentID(audioBufferSelectorComponentId);
    channelsSelector_.setComponentID(audioChannelsSelectorComponentId);
    applyButton_.setComponentID(audioApplyButtonComponentId);
    testToneButton_.setComponentID(audioTestToneButtonComponentId);

    addAndMakeVisible(deviceSelector_);
    addAndMakeVisible(sampleRateSelector_);
    addAndMakeVisible(bufferSelector_);
    addAndMakeVisible(channelsSelector_);
    addAndMakeVisible(applyButton_);
    addAndMakeVisible(testToneButton_);

    deviceSelector_.onChange = [this] { rebuildFormatSelectors(); };
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
    const auto previousName = selectedSettings().outputDeviceName;
    devices_ = host_.refreshOutputDevices();
    deviceSelector_.clear(juce::dontSendNotification);
    int selectedIndex = -1;
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
        }
    }
    if (selectedIndex < 0 && deviceSelector_.getNumItems() > 0) {
        selectedIndex = 0;
    }
    deviceSelector_.setSelectedItemIndex(selectedIndex, juce::dontSendNotification);
    rebuildFormatSelectors();
}

AppAudioSettings AudioSettingsComponent::selectedSettings() const
{
    AppAudioSettings settings = initialSettings_;
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
            : result.message);
    }
    if (!result.success) {
        refreshEnabledState();
        return false;
    }
    initialSettings_ = settings;
    if (callbacks_.settingsApplied) {
        callbacks_.settingsApplied(settings);
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
}

void AudioSettingsComponent::timerCallback()
{
    host_.serviceNonRealtime();
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
    }
    selectMatchingValue(sampleRateSelector_, initialSettings_.requestedSampleRate);
    selectMatchingValue(bufferSelector_, initialSettings_.requestedBufferFrames);
    selectMatchingValue(channelsSelector_, initialSettings_.requestedOutputChannels);
    refreshEnabledState();
}

void AudioSettingsComponent::refreshEnabledState()
{
    const auto state = host_.snapshot().realtime.state;
    const auto stopped = state == RealtimePlaybackState::Stopped;
    const auto hasDevice = selectedDeviceInfo() != nullptr;
    applyButton_.setEnabled(stopped && hasDevice);
    testToneButton_.setEnabled(stopped && host_.deviceFormatSnapshot().available);
    deviceSelector_.setEnabled(stopped);
    sampleRateSelector_.setEnabled(stopped);
    bufferSelector_.setEnabled(stopped);
    channelsSelector_.setEnabled(stopped);
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
