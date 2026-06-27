#include "PlaybackClock.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace trackloom {
namespace {

constexpr double tickCeilEpsilon = 0.000000001;

bool isValidSampleRate(double sampleRate)
{
    return std::isfinite(sampleRate) && sampleRate > 0.0;
}

double ticksToSeconds(std::int64_t ticks, double beatsPerMinute)
{
    return (static_cast<double>(ticks) / static_cast<double>(Project::ticksPerQuarterNote))
        * (60.0 / beatsPerMinute);
}

std::int64_t secondsToTicksCeil(double seconds, double beatsPerMinute)
{
    if (seconds <= 0.0) {
        return 0;
    }

    const auto ticks = seconds
        * beatsPerMinute
        * static_cast<double>(Project::ticksPerQuarterNote)
        / 60.0;

    // 边界值可能出现 959.999999999 这类浮点误差；减去极小量后再 ceil，能保持精确边界稳定。
    return static_cast<std::int64_t>(std::ceil(ticks - tickCeilEpsilon));
}

std::int64_t secondsToTickCeil(const Project& project, double seconds)
{
    if (!std::isfinite(seconds) || seconds <= 0.0) {
        return 0;
    }

    const auto& tempoEvents = project.tempoEvents();
    if (tempoEvents.empty()) {
        return secondsToTicksCeil(seconds, 120.0);
    }

    double segmentStartSeconds = 0.0;
    std::int64_t segmentStartTick = 0;
    double segmentBpm = tempoEvents.front().beatsPerMinute;

    for (std::size_t index = 1; index < tempoEvents.size(); ++index) {
        const auto& nextEvent = tempoEvents[index];
        const auto segmentTickLength = nextEvent.tick - segmentStartTick;
        const auto segmentLengthSeconds = ticksToSeconds(segmentTickLength, segmentBpm);
        const auto segmentEndSeconds = segmentStartSeconds + segmentLengthSeconds;

        if (seconds <= segmentEndSeconds + tickCeilEpsilon) {
            const auto localSeconds = seconds - segmentStartSeconds;
            const auto localTicks = secondsToTicksCeil(localSeconds, segmentBpm);
            return segmentStartTick + localTicks;
        }

        segmentStartSeconds = segmentEndSeconds;
        segmentStartTick = nextEvent.tick;
        segmentBpm = nextEvent.beatsPerMinute;
    }

    const auto localSeconds = seconds - segmentStartSeconds;
    return segmentStartTick + secondsToTicksCeil(localSeconds, segmentBpm);
}

int sampleOffsetForEvent(
    const Project& project,
    const Transport& transport,
    const MidiPlaybackEvent& event,
    int frameCount)
{
    const auto blockStartSeconds = static_cast<double>(transport.currentSample()) / transport.sampleRate();
    const auto eventSeconds = project.tickToSeconds(event.absoluteTick);
    const auto eventOffsetSamples = std::llround((eventSeconds - blockStartSeconds) * transport.sampleRate());
    const auto clampedOffset = std::clamp<std::int64_t>(eventOffsetSamples, 0, frameCount - 1);

    return static_cast<int>(clampedOffset);
}

}

std::optional<PlaybackTickWindow> playbackTickWindowForBlock(
    const Project& project,
    const Transport& transport,
    int frameCount)
{
    if (!transport.isPlaying() || frameCount <= 0 || !isValidSampleRate(transport.sampleRate())) {
        return std::nullopt;
    }

    const auto currentSample = transport.currentSample();
    if (currentSample < 0 || currentSample > std::numeric_limits<std::int64_t>::max() - frameCount) {
        return std::nullopt;
    }

    const auto startSeconds = static_cast<double>(currentSample) / transport.sampleRate();
    const auto endSeconds = static_cast<double>(currentSample + frameCount) / transport.sampleRate();

    return PlaybackTickWindow {
        secondsToTickCeil(project, startSeconds),
        secondsToTickCeil(project, endSeconds)
    };
}

std::vector<MidiPlaybackEvent> collectMidiPlaybackEventsForBlock(
    const Project& project,
    const Transport& transport,
    int frameCount)
{
    const auto window = playbackTickWindowForBlock(project, transport, frameCount);
    if (!window.has_value()) {
        return {};
    }

    return collectMidiPlaybackEvents(project, window->startTick, window->endTick);
}

std::vector<ScheduledMidiPlaybackEvent> collectScheduledMidiPlaybackEventsForBlock(
    const Project& project,
    const Transport& transport,
    int frameCount)
{
    const auto window = playbackTickWindowForBlock(project, transport, frameCount);
    if (!window.has_value()) {
        return {};
    }

    const auto midiEvents = collectMidiPlaybackEvents(project, window->startTick, window->endTick);
    std::vector<ScheduledMidiPlaybackEvent> scheduledEvents;
    scheduledEvents.reserve(midiEvents.size());

    for (const auto& event : midiEvents) {
        scheduledEvents.push_back(ScheduledMidiPlaybackEvent {
            event,
            sampleOffsetForEvent(project, transport, event, frameCount)
        });
    }

    return scheduledEvents;
}

}
