#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace trackloom::test {

class FakeJuceAudioDeviceType;

struct FakeJuceAudioOpenCall {
    int inputChannelCount = 0;
    int outputChannelCount = 0;
    std::uint64_t inputChannelMask = 0;
    std::uint64_t outputChannelMask = 0;
    double sampleRate = 0.0;
    int bufferFrames = 0;
};

inline std::uint64_t fakeJuceChannelMask(const juce::BigInteger& channels) noexcept
{
    std::uint64_t mask = 0;
    for (int bit = 0; bit < 64; ++bit) {
        if (channels[bit]) {
            mask |= std::uint64_t {1} << bit;
        }
    }
    return mask;
}

class FakeJuceAudioDevice final : public juce::AudioIODevice {
public:
    FakeJuceAudioDevice(FakeJuceAudioDeviceType& owner, const juce::String& name);
    ~FakeJuceAudioDevice() override;

    juce::StringArray getOutputChannelNames() override;
    juce::StringArray getInputChannelNames() override;
    juce::Array<double> getAvailableSampleRates() override;
    juce::Array<int> getAvailableBufferSizes() override;
    int getDefaultBufferSize() override;
    juce::String open(
        const juce::BigInteger& inputChannels,
        const juce::BigInteger& outputChannels,
        double sampleRate,
        int bufferSizeSamples) override;
    void close() override;
    bool isOpen() override;
    void start(juce::AudioIODeviceCallback* callback) override;
    void stop() override;
    bool isPlaying() override;
    juce::String getLastError() override;
    int getCurrentBufferSizeSamples() override;
    double getCurrentSampleRate() override;
    int getCurrentBitDepth() override;
    juce::BigInteger getActiveOutputChannels() const override;
    juce::BigInteger getActiveInputChannels() const override;
    int getOutputLatencyInSamples() override;
    int getInputLatencyInSamples() override;
    int getXRunCount() const noexcept override;
    std::vector<std::vector<float>> runCallback(int frames, float initialValue = 1.0f);
    void triggerAudioDeviceError();
    void triggerDeviceListChangedFromCallback();

private:
    FakeJuceAudioDeviceType& owner_;
    juce::AudioIODeviceCallback* callback_ = nullptr;
    juce::BigInteger activeInputs_;
    juce::BigInteger activeOutputs_;
    double sampleRate_ = 0.0;
    int bufferFrames_ = 0;
    bool open_ = false;
    bool playing_ = false;
};

class FakeJuceAudioDeviceType final : public juce::AudioIODeviceType {
public:
    FakeJuceAudioDeviceType()
        : juce::AudioIODeviceType("Windows Audio")
    {
    }

    void scanForDevices() override
    {
        calls_.push_back("scan");
        if (scanObserver_) {
            scanObserver_();
        }
    }

    juce::StringArray getDeviceNames(bool wantInputNames) const override
    {
        if (wantInputNames) {
            return {};
        }
        juce::StringArray result;
        for (const auto& name : outputDeviceNames_) {
            result.add(name);
        }
        return result;
    }

    int getDefaultDeviceIndex(bool forInput) const override
    {
        return forInput ? -1 : defaultOutputIndex_;
    }

    int getIndexOfDevice(juce::AudioIODevice* device, bool asInput) const override
    {
        if (asInput || device == nullptr) {
            return -1;
        }
        return getDeviceNames(false).indexOf(device->getName());
    }

    bool hasSeparateInputsAndOutputs() const override
    {
        return true;
    }

    juce::AudioIODevice* createDevice(
        const juce::String& outputDeviceName,
        const juce::String& inputDeviceName) override
    {
        calls_.push_back("create:" + outputDeviceName.toStdString());
        if (createShouldFail_ || inputDeviceName.isNotEmpty()
            || !getDeviceNames(false).contains(outputDeviceName)) {
            return nullptr;
        }
        auto device = std::make_unique<FakeJuceAudioDevice>(*this, outputDeviceName);
        lastCreatedDevice_ = device.get();
        return device.release();
    }

    int lastOpenInputChannelCount() const noexcept
    {
        return lastOpenInputChannelCount_;
    }

    int lastOpenOutputChannelCount() const noexcept
    {
        return lastOpenOutputChannelCount_;
    }

    FakeJuceAudioDevice* lastCreatedDevice() const noexcept
    {
        return lastCreatedDevice_;
    }

    FakeJuceAudioDevice* activeDevice() const noexcept
    {
        return activeDevice_;
    }

    const std::vector<std::string>& calls() const noexcept
    {
        return calls_;
    }

    const std::vector<FakeJuceAudioOpenCall>& openCalls() const noexcept
    {
        return openCalls_;
    }

    void clearCalls()
    {
        calls_.clear();
        openCalls_.clear();
    }

    void setOutputDevices(std::vector<std::string> names, int defaultIndex = 0)
    {
        outputDeviceNames_.clear();
        for (const auto& name : names) {
            outputDeviceNames_.push_back(name);
        }
        defaultOutputIndex_ = defaultIndex;
    }

    void setOutputChannelCount(int count)
    {
        outputChannelNames_.clear();
        for (int channel = 0; channel < count; ++channel) {
            outputChannelNames_.add("Output " + juce::String(channel + 1));
        }
    }

    void setAvailableFormats(
        std::vector<double> sampleRates,
        std::vector<int> bufferSizes,
        int defaultBufferSize)
    {
        sampleRates_.clear();
        for (const auto sampleRate : sampleRates) {
            sampleRates_.add(sampleRate);
        }
        bufferSizes_.clear();
        for (const auto bufferSize : bufferSizes) {
            bufferSizes_.add(bufferSize);
        }
        defaultBufferSize_ = defaultBufferSize;
    }

    void failNextOpen(int count = 1)
    {
        openFailuresRemaining_ = std::max(0, count);
    }

    void setOpenShouldFail(bool shouldFail)
    {
        openShouldFail_ = shouldFail;
    }

    void setCreateShouldFail(bool shouldFail)
    {
        createShouldFail_ = shouldFail;
    }

    void setNegotiatedFormat(double sampleRate, int bufferFrames, int outputChannels)
    {
        negotiatedSampleRate_ = sampleRate;
        negotiatedBufferFrames_ = bufferFrames;
        negotiatedOutputChannels_ = outputChannels;
    }

    void setNegotiatedOutputMask(std::uint64_t mask)
    {
        negotiatedOutputMask_ = mask;
    }

    void clearNegotiatedFormat()
    {
        negotiatedSampleRate_.reset();
        negotiatedBufferFrames_.reset();
        negotiatedOutputChannels_.reset();
        negotiatedOutputMask_.reset();
    }

    void setStartShouldPlay(bool shouldPlay)
    {
        startShouldPlay_ = shouldPlay;
    }

    void setStartFormat(double sampleRate, int bufferFrames, int outputChannels)
    {
        startSampleRate_ = sampleRate;
        startBufferFrames_ = bufferFrames;
        startOutputChannels_ = outputChannels;
    }

    void setStartOutputMask(std::uint64_t mask)
    {
        startOutputMask_ = mask;
    }

    void clearStartFormat()
    {
        startSampleRate_.reset();
        startBufferFrames_.reset();
        startOutputChannels_.reset();
        startOutputMask_.reset();
    }

    void setStartObserver(std::function<void()> observer)
    {
        startObserver_ = std::move(observer);
    }

    void setScanObserver(std::function<void()> observer)
    {
        scanObserver_ = std::move(observer);
    }

    void setCallbackFramesDuringStart(int frames)
    {
        callbackFramesDuringStart_ = std::max(0, frames);
    }

    void setPendingCallbackFramesOnStop(int frames)
    {
        pendingCallbackFramesOnStop_ = std::max(0, frames);
    }

    void setXRunCount(int count)
    {
        xRunCount_ = count;
    }

    void notifyDeviceListChanged()
    {
        callDeviceChangeListeners();
    }

private:
    friend class FakeJuceAudioDevice;

    std::vector<juce::String> outputDeviceNames_ {"Fake Speakers"};
    int defaultOutputIndex_ = 0;
    juce::StringArray outputChannelNames_ {"Left", "Right"};
    juce::Array<double> sampleRates_ {48000.0};
    juce::Array<int> bufferSizes_ {256};
    int defaultBufferSize_ = 256;
    bool createShouldFail_ = false;
    bool openShouldFail_ = false;
    bool startShouldPlay_ = true;
    int xRunCount_ = 0;
    int openFailuresRemaining_ = 0;
    std::optional<double> negotiatedSampleRate_;
    std::optional<int> negotiatedBufferFrames_;
    std::optional<int> negotiatedOutputChannels_;
    std::optional<std::uint64_t> negotiatedOutputMask_;
    std::optional<double> startSampleRate_;
    std::optional<int> startBufferFrames_;
    std::optional<int> startOutputChannels_;
    std::optional<std::uint64_t> startOutputMask_;
    std::function<void()> startObserver_;
    std::function<void()> scanObserver_;
    int callbackFramesDuringStart_ = 0;
    int pendingCallbackFramesOnStop_ = 0;
    int lastOpenInputChannelCount_ = -1;
    int lastOpenOutputChannelCount_ = -1;
    FakeJuceAudioDevice* lastCreatedDevice_ = nullptr;
    FakeJuceAudioDevice* activeDevice_ = nullptr;
    std::vector<std::string> calls_;
    std::vector<FakeJuceAudioOpenCall> openCalls_;
};

inline FakeJuceAudioDevice::FakeJuceAudioDevice(
    FakeJuceAudioDeviceType& owner,
    const juce::String& name)
    : juce::AudioIODevice(name, owner.getTypeName()), owner_(owner)
{
}

inline FakeJuceAudioDevice::~FakeJuceAudioDevice()
{
    if (owner_.lastCreatedDevice_ == this) {
        owner_.lastCreatedDevice_ = nullptr;
    }
    if (owner_.activeDevice_ == this) {
        owner_.activeDevice_ = nullptr;
    }
}

inline juce::StringArray FakeJuceAudioDevice::getOutputChannelNames()
{
    return owner_.outputChannelNames_;
}

inline juce::StringArray FakeJuceAudioDevice::getInputChannelNames()
{
    return {};
}

inline juce::Array<double> FakeJuceAudioDevice::getAvailableSampleRates()
{
    return owner_.sampleRates_;
}

inline juce::Array<int> FakeJuceAudioDevice::getAvailableBufferSizes()
{
    return owner_.bufferSizes_;
}

inline int FakeJuceAudioDevice::getDefaultBufferSize()
{
    return owner_.defaultBufferSize_;
}

inline juce::String FakeJuceAudioDevice::open(
    const juce::BigInteger& inputChannels,
    const juce::BigInteger& outputChannels,
    double sampleRate,
    int bufferSizeSamples)
{
    owner_.calls_.push_back("open");
    owner_.lastOpenInputChannelCount_ = inputChannels.countNumberOfSetBits();
    owner_.lastOpenOutputChannelCount_ = outputChannels.countNumberOfSetBits();
    owner_.openCalls_.push_back({
        owner_.lastOpenInputChannelCount_,
        owner_.lastOpenOutputChannelCount_,
        fakeJuceChannelMask(inputChannels),
        fakeJuceChannelMask(outputChannels),
        sampleRate,
        bufferSizeSamples
    });
    if (owner_.openShouldFail_ || owner_.openFailuresRemaining_ > 0) {
        owner_.openFailuresRemaining_ = std::max(0, owner_.openFailuresRemaining_ - 1);
        return "fake open failure";
    }
    activeInputs_ = inputChannels;
    activeOutputs_.clear();
    if (owner_.negotiatedOutputMask_) {
        for (int bit = 0; bit < 64; ++bit) {
            if ((*owner_.negotiatedOutputMask_ & (std::uint64_t {1} << bit)) != 0) {
                activeOutputs_.setBit(bit);
            }
        }
    } else {
        const auto outputCount = owner_.negotiatedOutputChannels_.value_or(
            outputChannels.countNumberOfSetBits());
        for (int channel = 0; channel < outputCount; ++channel) {
            activeOutputs_.setBit(channel);
        }
    }
    sampleRate_ = owner_.negotiatedSampleRate_.value_or(sampleRate);
    bufferFrames_ = owner_.negotiatedBufferFrames_.value_or(bufferSizeSamples);
    open_ = true;
    owner_.activeDevice_ = this;
    return {};
}

inline void FakeJuceAudioDevice::close()
{
    owner_.calls_.push_back("close");
    callback_ = nullptr;
    playing_ = false;
    open_ = false;
    if (owner_.activeDevice_ == this) {
        owner_.activeDevice_ = nullptr;
    }
}

inline bool FakeJuceAudioDevice::isOpen()
{
    return open_;
}

inline void FakeJuceAudioDevice::start(juce::AudioIODeviceCallback* callback)
{
    owner_.calls_.push_back("start");
    callback_ = callback;
    if (owner_.startObserver_) {
        owner_.startObserver_();
    }
    sampleRate_ = owner_.startSampleRate_.value_or(sampleRate_);
    bufferFrames_ = owner_.startBufferFrames_.value_or(bufferFrames_);
    if (owner_.startOutputMask_) {
        activeOutputs_.clear();
        for (int bit = 0; bit < 64; ++bit) {
            if ((*owner_.startOutputMask_ & (std::uint64_t {1} << bit)) != 0) {
                activeOutputs_.setBit(bit);
            }
        }
    } else if (owner_.startOutputChannels_) {
        activeOutputs_.clear();
        for (int channel = 0; channel < *owner_.startOutputChannels_; ++channel) {
            activeOutputs_.setBit(channel);
        }
    }
    if (callback_ != nullptr) {
        callback_->audioDeviceAboutToStart(this);
    }
    if (callback_ != nullptr && owner_.callbackFramesDuringStart_ > 0) {
        runCallback(owner_.callbackFramesDuringStart_);
    }
    playing_ = open_ && owner_.startShouldPlay_;
}

inline void FakeJuceAudioDevice::stop()
{
    owner_.calls_.push_back("stop");
    auto* oldCallback = callback_;
    if (oldCallback != nullptr && owner_.pendingCallbackFramesOnStop_ > 0) {
        runCallback(owner_.pendingCallbackFramesOnStop_);
    }
    callback_ = nullptr;
    playing_ = false;
    if (oldCallback != nullptr) {
        oldCallback->audioDeviceStopped();
    }
}

inline bool FakeJuceAudioDevice::isPlaying()
{
    return playing_;
}

inline juce::String FakeJuceAudioDevice::getLastError()
{
    return {};
}

inline int FakeJuceAudioDevice::getCurrentBufferSizeSamples()
{
    return bufferFrames_;
}

inline double FakeJuceAudioDevice::getCurrentSampleRate()
{
    return sampleRate_;
}

inline int FakeJuceAudioDevice::getCurrentBitDepth()
{
    return 32;
}

inline juce::BigInteger FakeJuceAudioDevice::getActiveOutputChannels() const
{
    return activeOutputs_;
}

inline juce::BigInteger FakeJuceAudioDevice::getActiveInputChannels() const
{
    return activeInputs_;
}

inline int FakeJuceAudioDevice::getOutputLatencyInSamples()
{
    return 0;
}

inline int FakeJuceAudioDevice::getInputLatencyInSamples()
{
    return 0;
}

inline int FakeJuceAudioDevice::getXRunCount() const noexcept
{
    return owner_.xRunCount_;
}

inline std::vector<std::vector<float>> FakeJuceAudioDevice::runCallback(
    int frames,
    float initialValue)
{
    const auto channels = activeOutputs_.countNumberOfSetBits();
    std::vector<std::vector<float>> result(
        static_cast<std::size_t>(channels),
        std::vector<float>(static_cast<std::size_t>(std::max(0, frames)), initialValue));
    std::vector<float*> outputs;
    outputs.reserve(result.size());
    for (auto& channel : result) {
        outputs.push_back(channel.data());
    }
    if (callback_ != nullptr) {
        const juce::AudioIODeviceCallbackContext context;
        callback_->audioDeviceIOCallbackWithContext(
            nullptr,
            0,
            outputs.empty() ? nullptr : outputs.data(),
            channels,
            frames,
            context);
    }
    return result;
}

inline void FakeJuceAudioDevice::triggerAudioDeviceError()
{
    if (callback_ != nullptr) {
        callback_->audioDeviceError("fake device error");
    }
}

inline void FakeJuceAudioDevice::triggerDeviceListChangedFromCallback()
{
    if (callback_ != nullptr) {
        owner_.callDeviceChangeListeners();
    }
}

}
