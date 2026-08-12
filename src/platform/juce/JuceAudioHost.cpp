#include "JuceAudioHost.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <utility>

namespace trackloom {
namespace {

static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

std::unique_ptr<juce::AudioIODeviceType> createSharedWasapiType()
{
    return std::unique_ptr<juce::AudioIODeviceType>(
        juce::AudioIODeviceType::createAudioIODeviceType_WASAPI(
            juce::WASAPIDeviceMode::shared));
}

std::uint64_t channelMaskFromBigInteger(const juce::BigInteger& channels) noexcept
{
    std::uint64_t mask = 0;
    for (int bit = 0; bit < 64; ++bit) {
        if (channels[bit]) {
            mask |= std::uint64_t {1} << bit;
        }
    }
    return mask;
}

void clearOutputs(float* const* outputs, int channels, int frames) noexcept
{
    if (outputs == nullptr || channels <= 0 || frames <= 0) {
        return;
    }
    for (int channel = 0; channel < channels; ++channel) {
        if (outputs[channel] != nullptr) {
            std::fill_n(outputs[channel], frames, 0.0f);
        }
    }
}

void processRealtimeBlock(
    PreparedMidiPlaybackRuntime& runtime,
    float* const* outputs,
    int channels,
    int frames)
{
    runtime.processBlock(outputs, channels, frames);
}

std::int64_t readHighResolutionTicks() noexcept
{
    return juce::Time::getHighResolutionTicks();
}

}

struct JuceAudioHost::Impl {
    explicit Impl(
        JuceAudioDeviceTypeFactory requestedFactory,
        JuceRealtimeTickOperation requestedTickOperation,
        std::int64_t requestedTicksPerSecond,
        JuceRealtimeBlockOperation requestedBlockOperation)
        : factory(requestedFactory ? std::move(requestedFactory)
                                   : JuceAudioDeviceTypeFactory(createSharedWasapiType)),
          tickOperation(requestedTickOperation != nullptr
                  ? requestedTickOperation
                  : readHighResolutionTicks),
          ticksPerSecond(requestedTicksPerSecond > 0
                  ? requestedTicksPerSecond
                  : juce::Time::getHighResolutionTicksPerSecond()),
          blockOperation(requestedBlockOperation != nullptr
                  ? requestedBlockOperation
                  : processRealtimeBlock)
    {
    }

    JuceAudioDeviceTypeFactory factory;
    std::unique_ptr<juce::AudioIODeviceType> deviceType;
    std::unique_ptr<juce::AudioIODevice> device;
    std::vector<JuceAudioOutputDeviceInfo> devices;
    AudioDeviceFormatSnapshot format;
    PreparedMidiPlaybackRuntime runtime;
    std::unique_ptr<const PreparedMidiPlaybackPlan> plan;
    JuceRealtimeTickOperation tickOperation = nullptr;
    std::int64_t ticksPerSecond = 0;
    JuceRealtimeBlockOperation blockOperation = nullptr;
    int xRunCount = -1;
    std::atomic<std::uint32_t> formatMismatch {0};
    std::atomic<std::uint64_t> callbackSampleRateBits {0};
    std::atomic<std::uint64_t> startSampleRateBits {0};
    std::atomic<std::uint64_t> startBlockFrames {0};
    std::atomic<std::uint64_t> startOutputChannelCount {0};
    std::atomic<std::uint64_t> startOutputChannelMask {0};
    std::atomic<std::uint32_t> deviceListRefreshPending {0};
    std::atomic<std::uint32_t> deviceErrorPending {0};
};

JuceAudioHost::JuceAudioHost(
    JuceAudioDeviceTypeFactory factory,
    JuceRealtimeTickOperation tickOperation,
    std::int64_t ticksPerSecond,
    JuceRealtimeBlockOperation blockOperation)
    : impl_(std::make_unique<Impl>(
          std::move(factory), tickOperation, ticksPerSecond, blockOperation))
{
}

JuceAudioHost::~JuceAudioHost()
{
    close();
    if (impl_->deviceType != nullptr) {
        impl_->deviceType->removeListener(this);
    }
}

std::vector<JuceAudioOutputDeviceInfo> JuceAudioHost::refreshOutputDevices()
{
    if (impl_->deviceType == nullptr && impl_->factory) {
        impl_->deviceType = impl_->factory();
        if (impl_->deviceType != nullptr) {
            impl_->deviceType->addListener(this);
        }
    }
    impl_->devices.clear();
    if (impl_->deviceType == nullptr) {
        return impl_->devices;
    }

    impl_->deviceType->scanForDevices();
    const auto names = impl_->deviceType->getDeviceNames(false);
    const auto defaultIndex = impl_->deviceType->getDefaultDeviceIndex(false);
    for (int index = 0; index < names.size(); ++index) {
        JuceAudioOutputDeviceInfo info;
        info.name = names[index].toStdString();
        info.id = impl_->deviceType->getTypeName().toStdString() + "/" + info.name;
        info.isDefault = index == defaultIndex;
        std::unique_ptr<juce::AudioIODevice> probe(
            impl_->deviceType->createDevice(names[index], {}));
        if (probe != nullptr) {
            for (const auto rate : probe->getAvailableSampleRates()) {
                info.sampleRates.push_back(rate);
            }
            for (const auto frames : probe->getAvailableBufferSizes()) {
                info.bufferSizes.push_back(frames);
            }
            info.maximumOutputChannels = probe->getOutputChannelNames().size();
        }
        impl_->devices.push_back(std::move(info));
    }
    return impl_->devices;
}

JuceAudioHostResult JuceAudioHost::openOutput(const JuceAudioOpenRequest& request)
{
    const auto runtimeState = impl_->runtime.snapshot().state;
    if (runtimeState == RealtimePlaybackState::Playing
        || runtimeState == RealtimePlaybackState::Stopping) {
        return {false, JuceAudioHostFailureReason::PlaybackActive,
            "playback must stop before opening an output device"};
    }
    if (!std::isfinite(request.requestedSampleRate)
        || request.requestedSampleRate <= 0.0
        || request.requestedBufferFrames <= 0
        || request.requestedOutputChannels < 1
        || request.requestedOutputChannels > 2) {
        return {false, JuceAudioHostFailureReason::DeviceOpenFailed,
            "requested output format is invalid"};
    }

    const auto devices = refreshOutputDevices();
    if (impl_->deviceType == nullptr) {
        return {false, JuceAudioHostFailureReason::DeviceTypeUnavailable,
            "shared WASAPI device type is unavailable"};
    }

    const JuceAudioOutputDeviceInfo* selected = nullptr;
    if (request.outputDeviceName.empty()) {
        const auto found = std::find_if(devices.begin(), devices.end(), [](const auto& info) {
            return info.isDefault;
        });
        if (found != devices.end()) {
            selected = &*found;
        }
    } else {
        const auto found = std::find_if(devices.begin(), devices.end(), [&](const auto& info) {
            return info.name == request.outputDeviceName;
        });
        if (found != devices.end()) {
            selected = &*found;
        }
    }
    if (selected == nullptr) {
        return {false, JuceAudioHostFailureReason::DeviceNotFound,
            "requested output device was not found in "
                + impl_->deviceType->getTypeName().toStdString()};
    }

    std::unique_ptr<juce::AudioIODevice> candidate(
        impl_->deviceType->createDevice(juce::String(selected->name), {}));
    if (candidate == nullptr) {
        return {false, JuceAudioHostFailureReason::DeviceCreateFailed,
            "output device could not be created"};
    }

    juce::BigInteger inputChannels;
    juce::BigInteger outputChannels;
    for (int channel = 0; channel < request.requestedOutputChannels; ++channel) {
        outputChannels.setBit(channel);
    }
    auto error = candidate->open(inputChannels, outputChannels,
        request.requestedSampleRate, request.requestedBufferFrames);
    bool usedFallback = false;
    if (error.isNotEmpty()) {
        candidate->close();
        if (selected->sampleRates.empty()) {
            return {false, JuceAudioHostFailureReason::DeviceOpenFailed,
                "output device could not be opened"};
        }
        auto fallbackBuffer = candidate->getDefaultBufferSize();
        if (fallbackBuffer <= 0 && !selected->bufferSizes.empty()) {
            fallbackBuffer = selected->bufferSizes.front();
        }
        if (fallbackBuffer <= 0) {
            return {false, JuceAudioHostFailureReason::DeviceOpenFailed,
                "output device has no usable shared buffer size"};
        }
        error = candidate->open(inputChannels, outputChannels,
            selected->sampleRates.front(), fallbackBuffer);
        usedFallback = true;
        if (error.isNotEmpty()) {
            candidate->close();
            return {false, JuceAudioHostFailureReason::DeviceOpenFailed,
                "output device could not be opened"};
        }
    }

    if (impl_->device != nullptr) {
        impl_->device->stop();
        impl_->device->close();
        impl_->device.reset();
    }
    impl_->runtime.hardReset();
    impl_->plan.reset();
    impl_->device = std::move(candidate);
    impl_->xRunCount = -1;
    impl_->formatMismatch.store(0, std::memory_order_release);
    impl_->deviceErrorPending.store(0, std::memory_order_release);
    ++impl_->format.generation;
    impl_->format.deviceId = selected->id;
    impl_->format.deviceName = selected->name;
    impl_->format.sampleRate = impl_->device->getCurrentSampleRate();
    impl_->callbackSampleRateBits.store(
        std::bit_cast<std::uint64_t>(impl_->format.sampleRate),
        std::memory_order_release);
    impl_->format.maximumBlockFrames = impl_->device->getCurrentBufferSizeSamples();
    impl_->format.outputChannelMask = channelMaskFromBigInteger(
        impl_->device->getActiveOutputChannels());
    impl_->format.outputChannelCount =
        impl_->device->getActiveOutputChannels().countNumberOfSetBits();
    impl_->format.available = true;

    JuceAudioHostResult result;
    result.success = true;
    result.message = "output device opened";
    if (usedFallback
        || impl_->format.sampleRate != request.requestedSampleRate
        || impl_->format.maximumBlockFrames != request.requestedBufferFrames
        || impl_->format.outputChannelCount != request.requestedOutputChannels
        || impl_->format.outputChannelMask
            != ((std::uint64_t {1} << request.requestedOutputChannels) - 1)) {
        result.warning = "shared WASAPI adjusted the requested output format";
    }
    result.actualFormat = impl_->format;
    return result;
}

void JuceAudioHost::close() noexcept
{
    if (impl_->device == nullptr) {
        return;
    }
    impl_->device->stop();
    impl_->runtime.hardReset();
    impl_->device->close();
    impl_->plan.reset();
    impl_->device.reset();
    ++impl_->format.generation;
    impl_->format.available = false;
}

JuceAudioHostResult JuceAudioHost::playTestTone()
{
    JuceAudioHostResult result;
    result.actualFormat = impl_->format;
    if (impl_->runtime.snapshot().state != RealtimePlaybackState::Stopped) {
        result.failureReason = JuceAudioHostFailureReason::PlaybackActive;
        result.message = "test tone requires stopped project playback";
        return result;
    }
    if (impl_->device == nullptr || !impl_->format.available
        || !impl_->device->isOpen()) {
        result.failureReason = JuceAudioHostFailureReason::DeviceOpenFailed;
        result.message = "test tone requires an open output device";
        return result;
    }

    auto plan = std::make_unique<PreparedMidiPlaybackPlan>();
    plan->sampleRate = impl_->format.sampleRate;
    plan->maximumBlockFrames = impl_->format.maximumBlockFrames;
    plan->outputChannelCount = impl_->format.outputChannelCount;
    plan->outputChannelMask = impl_->format.outputChannelMask;
    plan->playbackStartSample = 0;
    plan->instrumentSlots.push_back({
        PreparedMidiInstrumentKind::BuiltInSine,
        0.02f / 0.045f,
        0.0f,
        0
    });
    plan->events.push_back({
        0,
        1,
        0,
        0,
        1,
        69,
        127,
        PreparedMidiEventType::NoteOn
    });
    plan->events.push_back({
        static_cast<std::int64_t>(std::llround(impl_->format.sampleRate * 0.2)),
        1,
        1,
        0,
        1,
        69,
        0,
        PreparedMidiEventType::NoteOff
    });
    const auto started = installAndStart(std::move(plan));
    result.actualFormat = impl_->format;
    if (!started.success) {
        result.failureReason = started.failureReason
                == RealtimePlaybackHostFailureReason::PlaybackNotStopped
            ? JuceAudioHostFailureReason::PlaybackActive
            : JuceAudioHostFailureReason::DeviceOpenFailed;
        result.message = started.message;
        return result;
    }
    result.success = true;
    result.failureReason = JuceAudioHostFailureReason::None;
    result.message = "test tone started";
    return result;
}

AudioDeviceFormatSnapshot JuceAudioHost::deviceFormatSnapshot() const
{
    return impl_->format;
}

RealtimePlaybackHostResult JuceAudioHost::installAndStart(
    std::unique_ptr<const PreparedMidiPlaybackPlan> plan)
{
    if (plan == nullptr || !validatePreparedMidiPlaybackPlan(*plan).valid) {
        return {false, RealtimePlaybackHostFailureReason::InvalidPlan,
            "prepared playback plan is invalid"};
    }
    if (impl_->device == nullptr || !impl_->format.available
        || !impl_->device->isOpen()) {
        return {false, RealtimePlaybackHostFailureReason::DeviceNotOpen,
            "output device is not open"};
    }
    if (plan->sampleRate != impl_->format.sampleRate
        || plan->maximumBlockFrames != impl_->format.maximumBlockFrames
        || plan->outputChannelCount != impl_->format.outputChannelCount
        || plan->outputChannelMask != impl_->format.outputChannelMask) {
        return {false, RealtimePlaybackHostFailureReason::DeviceFormatMismatch,
            "prepared playback plan does not match the output format"};
    }
    if (impl_->runtime.snapshot().state != RealtimePlaybackState::Stopped) {
        return {false, RealtimePlaybackHostFailureReason::PlaybackNotStopped,
            "playback runtime is not stopped"};
    }

    impl_->device->stop();
    if (impl_->runtime.snapshot().state != RealtimePlaybackState::Stopped) {
        return {false, RealtimePlaybackHostFailureReason::PlaybackNotStopped,
            "playback runtime changed state while stopping the device"};
    }
    if (!impl_->runtime.installPlan(plan.get())) {
        return {false, RealtimePlaybackHostFailureReason::InvalidPlan,
            "prepared playback plan could not be installed"};
    }
    impl_->plan = std::move(plan);
    if (!impl_->runtime.start()) {
        impl_->runtime.hardReset();
        impl_->plan.reset();
        return {false, RealtimePlaybackHostFailureReason::PlaybackNotStopped,
            "playback runtime could not start"};
    }

    impl_->formatMismatch.store(0, std::memory_order_release);
    impl_->device->start(this);
    if (impl_->formatMismatch.load(std::memory_order_acquire) != 0) {
        impl_->device->stop();
        impl_->runtime.hardReset();
        impl_->plan.reset();
        const auto observedSampleRate = std::bit_cast<double>(
            impl_->startSampleRateBits.load(std::memory_order_relaxed));
        const auto observedBlockFrames = static_cast<int>(
            impl_->startBlockFrames.load(std::memory_order_relaxed));
        const auto observedOutputChannelCount = static_cast<int>(
            impl_->startOutputChannelCount.load(std::memory_order_relaxed));
        const auto observedOutputChannelMask =
            impl_->startOutputChannelMask.load(std::memory_order_relaxed);
        if (observedSampleRate != impl_->format.sampleRate
            || observedBlockFrames != impl_->format.maximumBlockFrames
            || observedOutputChannelCount != impl_->format.outputChannelCount
            || observedOutputChannelMask != impl_->format.outputChannelMask) {
            ++impl_->format.generation;
            impl_->format.sampleRate = observedSampleRate;
            impl_->format.maximumBlockFrames = observedBlockFrames;
            impl_->format.outputChannelCount = observedOutputChannelCount;
            impl_->format.outputChannelMask = observedOutputChannelMask;
        }
        return {false, RealtimePlaybackHostFailureReason::DeviceFormatMismatch,
            "output format changed while starting playback"};
    }
    if (!impl_->device->isPlaying()) {
        impl_->device->stop();
        impl_->runtime.hardReset();
        impl_->plan.reset();
        return {false, RealtimePlaybackHostFailureReason::DeviceStartFailed,
            "output device did not start"};
    }
    return {true, RealtimePlaybackHostFailureReason::None,
        "playback started"};
}

bool JuceAudioHost::requestStop() noexcept
{
    return impl_->runtime.requestStop();
}

void JuceAudioHost::serviceNonRealtime()
{
    if (impl_->device != nullptr) {
        impl_->xRunCount = impl_->device->getXRunCount();
    }
    if (impl_->deviceListRefreshPending.exchange(0, std::memory_order_acq_rel) != 0) {
        refreshOutputDevices();
        if (impl_->device != nullptr) {
            const auto currentStillExists = std::any_of(
                impl_->devices.begin(), impl_->devices.end(), [&](const auto& info) {
                    return info.name == impl_->format.deviceName;
                });
            if (!currentStillExists) {
                impl_->device->stop();
                impl_->runtime.hardReset(RealtimeAudioError::DeviceError);
                impl_->device->close();
                impl_->plan.reset();
                impl_->device.reset();
                ++impl_->format.generation;
                impl_->format.available = false;
                impl_->formatMismatch.store(0, std::memory_order_release);
            }
        }
    }
    if (impl_->deviceErrorPending.exchange(0, std::memory_order_acq_rel) != 0
        && impl_->device != nullptr) {
        impl_->device->stop();
        impl_->runtime.hardReset(RealtimeAudioError::DeviceError);
        impl_->device->close();
        impl_->plan.reset();
        impl_->device.reset();
        ++impl_->format.generation;
        impl_->format.available = false;
        impl_->formatMismatch.store(0, std::memory_order_release);
    }
    if (impl_->device != nullptr) {
        const auto realtime = impl_->runtime.snapshot();
        if (realtime.state == RealtimePlaybackState::Faulted
            && realtime.lastError == RealtimeAudioError::OversizedBlock
            && realtime.largestObservedBlockFrames
                > static_cast<std::uint64_t>(impl_->format.maximumBlockFrames)) {
            impl_->device->stop();
            impl_->runtime.hardReset();
            impl_->plan.reset();
            impl_->format.maximumBlockFrames = static_cast<int>(
                realtime.largestObservedBlockFrames);
            ++impl_->format.generation;
            impl_->formatMismatch.store(0, std::memory_order_release);
        }
    }
    if (impl_->device != nullptr) {
        const auto realtime = impl_->runtime.snapshot();
        if (realtime.state == RealtimePlaybackState::Faulted) {
            impl_->device->stop();
            impl_->runtime.hardReset(realtime.lastError);
            impl_->device->close();
            impl_->plan.reset();
            impl_->device.reset();
            ++impl_->format.generation;
            impl_->format.available = false;
            impl_->formatMismatch.store(0, std::memory_order_release);
        }
    }
    if (impl_->device != nullptr
        && impl_->runtime.snapshot().state == RealtimePlaybackState::Stopped
        && impl_->device->isPlaying()) {
        impl_->device->stop();
    }
}

void JuceAudioHost::hardStopAndReset() noexcept
{
    if (impl_->device != nullptr) {
        impl_->device->stop();
    }
    impl_->runtime.hardReset();
    impl_->plan.reset();
}

RealtimePlaybackHostSnapshot JuceAudioHost::snapshot() const
{
    RealtimePlaybackHostSnapshot result;
    result.format = impl_->format;
    result.realtime = impl_->runtime.snapshot();
    result.xRunCount = impl_->xRunCount;
    result.deviceListRefreshPending =
        impl_->deviceListRefreshPending.load(std::memory_order_acquire) != 0;
    return result;
}

void JuceAudioHost::audioDeviceIOCallbackWithContext(
    const float* const*,
    int,
    float* const* outputs,
    int outputChannelCount,
    int frames,
    const juce::AudioIODeviceCallbackContext&)
{
    clearOutputs(outputs, outputChannelCount, frames);
    if (impl_->formatMismatch.load(std::memory_order_acquire) != 0
        || impl_->deviceErrorPending.load(std::memory_order_acquire) != 0) {
        return;
    }
    const auto startTicks = impl_->tickOperation();
    try {
        impl_->blockOperation(impl_->runtime, outputs, outputChannelCount, frames);
    } catch (...) {
        clearOutputs(outputs, outputChannelCount, frames);
        impl_->runtime.recordCallbackException();
        impl_->runtime.hardReset(RealtimeAudioError::CallbackException);
    }
    const auto endTicks = impl_->tickOperation();
    const auto callbackSampleRate = std::bit_cast<double>(
        impl_->callbackSampleRateBits.load(std::memory_order_acquire));
    if (frames > 0 && impl_->ticksPerSecond > 0 && endTicks >= startTicks
        && std::isfinite(callbackSampleRate) && callbackSampleRate > 0.0) {
        const auto elapsedTicks = static_cast<double>(endTicks - startTicks);
        const auto elapsedScaled = elapsedTicks * callbackSampleRate;
        const auto deadlineScaled = static_cast<double>(frames)
            * static_cast<double>(impl_->ticksPerSecond);
        if (elapsedScaled >= deadlineScaled) {
            impl_->runtime.recordCallbackTimeout();
        }
    }
}

void JuceAudioHost::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    if (device == nullptr) {
        impl_->startSampleRateBits.store(0, std::memory_order_relaxed);
        impl_->startBlockFrames.store(0, std::memory_order_relaxed);
        impl_->startOutputChannelCount.store(0, std::memory_order_relaxed);
        impl_->startOutputChannelMask.store(0, std::memory_order_relaxed);
        impl_->formatMismatch.store(1, std::memory_order_release);
        return;
    }
    const auto activeOutputs = device->getActiveOutputChannels();
    const auto sampleRate = device->getCurrentSampleRate();
    const auto blockFrames = device->getCurrentBufferSizeSamples();
    const auto outputChannelCount = activeOutputs.countNumberOfSetBits();
    const auto outputChannelMask = channelMaskFromBigInteger(activeOutputs);
    impl_->startSampleRateBits.store(
        std::bit_cast<std::uint64_t>(sampleRate), std::memory_order_relaxed);
    impl_->callbackSampleRateBits.store(
        std::bit_cast<std::uint64_t>(sampleRate), std::memory_order_release);
    impl_->startBlockFrames.store(
        static_cast<std::uint64_t>(std::max(0, blockFrames)), std::memory_order_relaxed);
    impl_->startOutputChannelCount.store(
        static_cast<std::uint64_t>(std::max(0, outputChannelCount)),
        std::memory_order_relaxed);
    impl_->startOutputChannelMask.store(outputChannelMask, std::memory_order_relaxed);
    const auto mismatch = sampleRate != impl_->format.sampleRate
        || blockFrames != impl_->format.maximumBlockFrames
        || outputChannelCount != impl_->format.outputChannelCount
        || outputChannelMask != impl_->format.outputChannelMask;
    impl_->formatMismatch.store(mismatch ? 1u : 0u, std::memory_order_release);
}

void JuceAudioHost::audioDeviceStopped()
{
}

void JuceAudioHost::audioDeviceError(const juce::String&)
{
    impl_->deviceErrorPending.store(1, std::memory_order_release);
}

void JuceAudioHost::audioDeviceListChanged()
{
    impl_->deviceListRefreshPending.store(1, std::memory_order_release);
}

}
