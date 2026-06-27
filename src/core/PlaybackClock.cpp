#include "PlaybackClock.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace trackloom {
namespace {

constexpr double tickCeilEpsilon = 0.000000001;
constexpr double sampleFrameEpsilon = 0.000000001;

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

bool isValidLoopRange(const PlaybackLoopRange& loopRange)
{
    return loopRange.startTick >= 0 && loopRange.endTick > loopRange.startTick;
}

double positiveModulo(double value, double divisor)
{
    const auto result = std::fmod(value, divisor);
    if (result < 0.0) {
        return result + divisor;
    }

    return result;
}

std::optional<double> normalizedTransportSecondsInLoop(
    const Project& project,
    const Transport& transport,
    const PlaybackLoopRange& loopRange)
{
    const auto loopStartSeconds = project.tickToSeconds(loopRange.startTick);
    const auto loopEndSeconds = project.tickToSeconds(loopRange.endTick);
    const auto loopDurationSeconds = loopEndSeconds - loopStartSeconds;

    if (!std::isfinite(loopStartSeconds)
        || !std::isfinite(loopEndSeconds)
        || !std::isfinite(loopDurationSeconds)
        || loopDurationSeconds <= 0.0) {
        return std::nullopt;
    }

    const auto transportSeconds = static_cast<double>(transport.currentSample()) / transport.sampleRate();
    if (!std::isfinite(transportSeconds)) {
        return std::nullopt;
    }

    // Transport 保存的是绝对 sample 位置；循环播放要把它投影回循环自己的秒数范围。
    return loopStartSeconds + positiveModulo(transportSeconds - loopStartSeconds, loopDurationSeconds);
}

std::int64_t loopTickForSeconds(
    const Project& project,
    double seconds,
    const PlaybackLoopRange& loopRange)
{
    auto tick = secondsToTickCeil(project, seconds);

    if (tick < loopRange.startTick) {
        return loopRange.startTick;
    }
    if (tick >= loopRange.endTick) {
        return loopRange.startTick;
    }

    return tick;
}

std::optional<int> frameCountUntilLoopEnd(
    const Project& project,
    double sampleRate,
    std::int64_t startTick,
    std::int64_t loopEndTick)
{
    const auto startSeconds = project.tickToSeconds(startTick);
    const auto endSeconds = project.tickToSeconds(loopEndTick);
    const auto secondsUntilEnd = endSeconds - startSeconds;
    const auto framesUntilEnd = secondsUntilEnd * sampleRate;

    if (!std::isfinite(secondsUntilEnd) || !std::isfinite(framesUntilEnd) || framesUntilEnd <= 0.0) {
        return std::nullopt;
    }

    if (framesUntilEnd > static_cast<double>(std::numeric_limits<int>::max())) {
        return std::numeric_limits<int>::max();
    }

    // 精确边界可能算出 480.000000001；减去极小量后 ceil，避免多吃一帧。
    const auto roundedFrames = static_cast<int>(std::ceil(framesUntilEnd - sampleFrameEpsilon));
    return std::max(1, roundedFrames);
}

std::int64_t tickAtSegmentEnd(
    const Project& project,
    std::int64_t startTick,
    int frameCount,
    double sampleRate,
    std::int64_t loopEndTick)
{
    const auto startSeconds = project.tickToSeconds(startTick);
    const auto endSeconds = startSeconds + (static_cast<double>(frameCount) / sampleRate);
    auto endTick = secondsToTickCeil(project, endSeconds);

    endTick = std::min(endTick, loopEndTick);
    if (endTick < startTick) {
        return startTick;
    }

    return endTick;
}

int sampleOffsetForEventInLoopWindow(
    const Project& project,
    double sampleRate,
    const MidiPlaybackEvent& event,
    const LoopedPlaybackTickWindow& window)
{
    const auto windowStartSeconds = project.tickToSeconds(window.window.startTick);
    const auto eventSeconds = project.tickToSeconds(event.absoluteTick);
    const auto localOffsetSamples = std::llround((eventSeconds - windowStartSeconds) * sampleRate);
    const auto blockOffsetSamples = localOffsetSamples + window.sampleOffset;
    const auto minimumOffset = static_cast<std::int64_t>(window.sampleOffset);
    const auto maximumOffset = static_cast<std::int64_t>(window.sampleOffset + window.frameCount - 1);
    const auto clampedOffset = std::clamp<std::int64_t>(blockOffsetSamples, minimumOffset, maximumOffset);

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

std::vector<LoopedPlaybackTickWindow> playbackTickWindowsForLoopedBlock(
    const Project& project,
    const Transport& transport,
    int frameCount,
    const PlaybackLoopRange& loopRange)
{
    if (!transport.isPlaying()
        || frameCount <= 0
        || !isValidSampleRate(transport.sampleRate())
        || !isValidLoopRange(loopRange)
        || transport.currentSample() < 0) {
        return {};
    }

    const auto normalizedSeconds = normalizedTransportSecondsInLoop(project, transport, loopRange);
    if (!normalizedSeconds.has_value()) {
        return {};
    }

    std::vector<LoopedPlaybackTickWindow> windows;
    auto segmentStartTick = loopTickForSeconds(project, *normalizedSeconds, loopRange);
    auto remainingFrameCount = frameCount;
    auto blockSampleOffset = 0;

    while (remainingFrameCount > 0) {
        const auto framesUntilEnd = frameCountUntilLoopEnd(
            project,
            transport.sampleRate(),
            segmentStartTick,
            loopRange.endTick);

        if (!framesUntilEnd.has_value()) {
            return {};
        }

        if (*framesUntilEnd >= remainingFrameCount) {
            windows.push_back(LoopedPlaybackTickWindow {
                PlaybackTickWindow {
                    segmentStartTick,
                    tickAtSegmentEnd(
                        project,
                        segmentStartTick,
                        remainingFrameCount,
                        transport.sampleRate(),
                        loopRange.endTick)
                },
                blockSampleOffset,
                remainingFrameCount
            });
            break;
        }

        windows.push_back(LoopedPlaybackTickWindow {
            PlaybackTickWindow { segmentStartTick, loopRange.endTick },
            blockSampleOffset,
            *framesUntilEnd
        });

        remainingFrameCount -= *framesUntilEnd;
        blockSampleOffset += *framesUntilEnd;
        segmentStartTick = loopRange.startTick;
    }

    return windows;
}

std::vector<ScheduledMidiPlaybackEvent> collectScheduledMidiPlaybackEventsForLoopedBlock(
    const Project& project,
    const Transport& transport,
    int frameCount,
    const PlaybackLoopRange& loopRange)
{
    const auto windows = playbackTickWindowsForLoopedBlock(project, transport, frameCount, loopRange);
    std::vector<ScheduledMidiPlaybackEvent> scheduledEvents;

    for (const auto& window : windows) {
        const auto midiEvents = collectMidiPlaybackEvents(
            project,
            window.window.startTick,
            window.window.endTick);
        scheduledEvents.reserve(scheduledEvents.size() + midiEvents.size());

        for (const auto& event : midiEvents) {
            scheduledEvents.push_back(ScheduledMidiPlaybackEvent {
                event,
                sampleOffsetForEventInLoopWindow(project, transport.sampleRate(), event, window)
            });
        }
    }

    return scheduledEvents;
}

}
