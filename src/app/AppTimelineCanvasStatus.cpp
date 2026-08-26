#include "AppTimelineCanvasStatus.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace trackloom {
namespace {

bool checkedMultiplyNonNegative(
    std::int64_t left,
    std::int64_t right,
    std::int64_t& result) noexcept
{
    if (left < 0 || right < 0) {
        return false;
    }
    if (left != 0 && right > std::numeric_limits<std::int64_t>::max() / left) {
        return false;
    }

    result = left * right;
    return true;
}

bool checkedAddNonNegative(
    std::int64_t left,
    std::int64_t right,
    std::int64_t& result) noexcept
{
    if (left < 0 || right < 0
        || right > std::numeric_limits<std::int64_t>::max() - left) {
        return false;
    }

    result = left + right;
    return true;
}

bool measureLengthTicks(
    const TimeSignatureEvent& event,
    std::int64_t& result) noexcept
{
    std::int64_t numeratorTicks = 0;
    std::int64_t wholeNoteTicks = 0;
    if (!checkedMultiplyNonNegative(
            static_cast<std::int64_t>(event.numerator),
            Project::ticksPerQuarterNote,
            numeratorTicks)
        || !checkedMultiplyNonNegative(numeratorTicks, 4, wholeNoteTicks)
        || event.denominator <= 0) {
        return false;
    }

    result = wholeNoteTicks / event.denominator;
    return result > 0;
}

struct MeasureBoundaryPrimitiveResult {
    bool success = false;
    AppTimelineMeasureBoundaryNeighbors neighbors;
};

MeasureBoundaryPrimitiveResult findMeasureBoundaryNeighbors(
    const std::vector<TimeSignatureEvent>& sortedEvents,
    std::int64_t candidateTick)
{
    if (candidateTick < 0 || sortedEvents.empty()) {
        return {};
    }

    const auto nextEvent = std::upper_bound(
        sortedEvents.begin(),
        sortedEvents.end(),
        candidateTick,
        [](std::int64_t tick, const TimeSignatureEvent& event) {
            return tick < event.tick;
        });
    if (nextEvent == sortedEvents.begin()) {
        return {};
    }

    const auto currentEvent = nextEvent - 1;
    std::int64_t measureTicks = 0;
    if (!measureLengthTicks(*currentEvent, measureTicks)) {
        return {};
    }

    const auto measureIndex = (candidateTick - currentEvent->tick) / measureTicks;
    std::int64_t measureOffset = 0;
    std::int64_t lowerBoundary = 0;
    if (!checkedMultiplyNonNegative(measureIndex, measureTicks, measureOffset)
        || !checkedAddNonNegative(currentEvent->tick, measureOffset, lowerBoundary)) {
        return {};
    }

    MeasureBoundaryPrimitiveResult result;
    result.success = true;
    result.neighbors.atOrBeforeTick = lowerBoundary;
    if (candidateTick == lowerBoundary) {
        result.neighbors.atOrAfterTick = lowerBoundary;
        return result;
    }

    std::int64_t naturalUpperBoundary = 0;
    const bool naturalUpperIsRepresentable = checkedAddNonNegative(
        lowerBoundary, measureTicks, naturalUpperBoundary);
    if (nextEvent != sortedEvents.end()
        && (!naturalUpperIsRepresentable || nextEvent->tick < naturalUpperBoundary)) {
        result.neighbors.atOrAfterTick = nextEvent->tick;
    } else if (naturalUpperIsRepresentable) {
        result.neighbors.atOrAfterTick = naturalUpperBoundary;
    }

    return result;
}

AppTimelineCanvasBuildResult failedCanvasBuild(
    AppTimelineCanvasFailureReason reason,
    std::string message)
{
    AppTimelineCanvasBuildResult result;
    result.failureReason = reason;
    result.message = std::move(message);
    return result;
}

bool collectVisibleMeasureBoundaries(
    const Project& project,
    AppTimelineVisibleTickRange visibleRange,
    std::vector<std::int64_t>& boundaries)
{
    const auto first = findMeasureBoundaryNeighbors(
        project.timeSignatureEvents(), visibleRange.startTick);
    if (!first.success) {
        return false;
    }

    auto boundary = first.neighbors.atOrAfterTick;
    while (boundary.has_value() && *boundary <= visibleRange.endTick) {
        boundaries.push_back(*boundary);
        if (*boundary == std::numeric_limits<std::int64_t>::max()) {
            break;
        }

        const auto next = findMeasureBoundaryNeighbors(
            project.timeSignatureEvents(), *boundary + 1);
        if (!next.success
            || (next.neighbors.atOrAfterTick.has_value()
                && *next.neighbors.atOrAfterTick <= *boundary)) {
            return false;
        }
        boundary = next.neighbors.atOrAfterTick;
    }

    return true;
}

}

AppTimelineCanvasBuildResult buildAppTimelineCanvasStatus(
    const Project& project,
    AppTimelineVisibleTickRange visibleRange)
{
    if (visibleRange.startTick < 0 || visibleRange.endTick < visibleRange.startTick) {
        return failedCanvasBuild(
            AppTimelineCanvasFailureReason::InvalidVisibleRange,
            "可见时间范围无效。");
    }

    AppTimelineCanvasStatus status;
    status.visibleRange = visibleRange;
    status.tracks.reserve(project.tracks().size());
    for (const auto& track : project.tracks()) {
        status.tracks.push_back({ track.id, track.type, track.name });
    }

    status.clips.reserve(project.clips().size());
    for (const auto& clip : project.clips()) {
        std::int64_t endTick = 0;
        if (!checkedAddNonNegative(clip.startTick, clip.lengthTick, endTick)) {
            return failedCanvasBuild(
                AppTimelineCanvasFailureReason::ClipEndOverflow,
                "片段终点超出可表示的 tick 范围。");
        }

        status.clips.push_back({
            clip.id,
            clip.trackId,
            clip.type,
            clip.name,
            clip.startTick,
            endTick
        });
    }

    status.playbackLoopRange = project.playbackLoopRange();
    if (!collectVisibleMeasureBoundaries(
            project, visibleRange, status.measureBoundaryTicks)) {
        return failedCanvasBuild(
            AppTimelineCanvasFailureReason::InvalidVisibleRange,
            "无法生成可见范围内的小节边界。");
    }

    AppTimelineCanvasBuildResult result;
    result.success = true;
    result.failureReason = AppTimelineCanvasFailureReason::None;
    result.status = std::move(status);
    result.message = "时间线画布快照已生成。";
    return result;
}

AppTimelineMeasureBoundarySearchResult
findAppTimelineMeasureBoundaryNeighbors(
    const Project& project,
    std::int64_t candidateTick)
{
    const auto primitive = findMeasureBoundaryNeighbors(
        project.timeSignatureEvents(), candidateTick);
    if (!primitive.success) {
        return {};
    }

    AppTimelineMeasureBoundarySearchResult result;
    result.success = true;
    result.failureReason = AppTimelineCanvasFailureReason::None;
    result.neighbors = primitive.neighbors;
    return result;
}

}
