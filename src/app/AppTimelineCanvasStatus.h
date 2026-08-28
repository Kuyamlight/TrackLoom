#pragma once

#include "Project.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trackloom {

// 画布快照是应用层有界数据。调用方应缩小可见 tick 范围后再重试超出预算的请求。
inline constexpr std::size_t appTimelineCanvasMaxMeasureBoundaryCount = 65'536;

struct AppTimelineVisibleTickRange {
    std::int64_t startTick = 0;
    std::int64_t endTick = 0;
    bool operator==(const AppTimelineVisibleTickRange&) const = default;
};

struct AppTimelineCanvasTrackRow {
    std::string trackId;
    TrackType type = TrackType::Instrument;
    std::string name;
};

struct AppTimelineCanvasClipRow {
    std::string clipId;
    std::string trackId;
    ClipType type = ClipType::Midi;
    std::string name;
    std::int64_t startTick = 0;
    std::int64_t endTick = 0;
};

enum class AppTimelineCanvasFailureReason {
    None,
    InvalidVisibleRange,
    InvalidCandidateTick,
    ClipEndOverflow,
    MeasureBoundaryCapacityExceeded
};

struct AppTimelineMeasureBoundaryNeighbors {
    std::optional<std::int64_t> atOrBeforeTick;
    std::optional<std::int64_t> atOrAfterTick;
};

struct AppTimelineMeasureBoundarySearchResult {
    bool success = false;
    AppTimelineCanvasFailureReason failureReason =
        AppTimelineCanvasFailureReason::InvalidCandidateTick;
    AppTimelineMeasureBoundaryNeighbors neighbors;
};

struct AppTimelineCanvasStatus {
    AppTimelineVisibleTickRange visibleRange;
    std::vector<AppTimelineCanvasTrackRow> tracks;
    std::vector<AppTimelineCanvasClipRow> clips;
    std::optional<PlaybackLoopRange> playbackLoopRange;
    std::vector<std::int64_t> measureBoundaryTicks;
};

struct AppTimelineCanvasBuildResult {
    bool success = false;
    AppTimelineCanvasFailureReason failureReason =
        AppTimelineCanvasFailureReason::InvalidVisibleRange;
    AppTimelineCanvasStatus status;
    std::string message;
};

AppTimelineCanvasBuildResult buildAppTimelineCanvasStatus(
    const Project& project,
    AppTimelineVisibleTickRange visibleRange);

AppTimelineMeasureBoundarySearchResult
findAppTimelineMeasureBoundaryNeighbors(
    const Project& project,
    std::int64_t candidateTick);

}
