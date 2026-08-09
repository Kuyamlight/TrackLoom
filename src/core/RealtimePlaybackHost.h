#pragma once

#include "PreparedMidiPlaybackRuntime.h"

#include <cstdint>
#include <memory>
#include <string>

namespace trackloom {

struct AudioDeviceFormatSnapshot {
    std::uint64_t generation = 0;
    std::string deviceId;
    std::string deviceName;
    double sampleRate = 0.0;
    int maximumBlockFrames = 0;
    int outputChannelCount = 0;
    std::uint64_t outputChannelMask = 0;
    bool available = false;
};

struct RealtimePlaybackHostSnapshot {
    AudioDeviceFormatSnapshot format;
    RealtimeAudioDiagnosticsSnapshot realtime;
    int xRunCount = -1;
    bool deviceListRefreshPending = false;
};

enum class RealtimePlaybackHostFailureReason {
    None,
    DeviceUnavailable,
    DeviceNotOpen,
    DeviceFormatMismatch,
    PlaybackNotStopped,
    InvalidPlan,
    DeviceStartFailed
};

struct RealtimePlaybackHostResult {
    bool success = false;
    RealtimePlaybackHostFailureReason failureReason =
        RealtimePlaybackHostFailureReason::None;
    std::string message;
};

class RealtimePlaybackHost {
public:
    virtual ~RealtimePlaybackHost() = default;
    virtual AudioDeviceFormatSnapshot deviceFormatSnapshot() const = 0;
    virtual RealtimePlaybackHostResult installAndStart(
        std::unique_ptr<const PreparedMidiPlaybackPlan> plan) = 0;
    virtual bool requestStop() noexcept = 0;
    virtual void serviceNonRealtime() = 0;
    virtual void hardStopAndReset() noexcept = 0;
    virtual RealtimePlaybackHostSnapshot snapshot() const = 0;
};

}
