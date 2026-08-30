#include "TimelineLoopEditorComponent.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace trackloom {
namespace {

constexpr float headerWidth = 160.0f;
constexpr float rulerHeight = 28.0f;
constexpr float loopBandHeight = 24.0f;
constexpr float trackRowHeight = 52.0f;
constexpr float clipVerticalInset = 6.0f;
constexpr float loopHandleWidth = 8.0f;
const auto canvasColour = juce::Colour(0xff151815);
const auto panelColour = juce::Colour(0xff20231f);
const auto gridColour = juce::Colour(0xff3a463c);
const auto primaryTextColour = juce::Colour(0xfff2f0e8);
const auto loopAccentColour = juce::Colour(0xff6ccf8d);

bool isValidRange(AppTimelineVisibleTickRange range) noexcept
{
    return range.startTick >= 0 && range.endTick > range.startTick;
}

AppTimelineVisibleTickRange makeVisibleRange(
    std::int64_t startTick,
    std::int64_t span) noexcept
{
    const auto maximumSpan = std::numeric_limits<std::int64_t>::max();
    const auto legalSpan = std::clamp(span, std::int64_t { 1 }, maximumSpan);
    const auto maximumStart = maximumSpan - legalSpan;
    const auto legalStart = std::clamp(startTick, std::int64_t { 0 }, maximumStart);
    return { legalStart, legalStart + legalSpan };
}

}

TimelineLoopEditorComponent::TimelineLoopEditorComponent(
    TimelineLoopEditorCallbacks callbacks)
    : callbacks_(std::move(callbacks))
{
    setComponentID(timelineLoopEditorComponentId);
    setOpaque(true);
}

void TimelineLoopEditorComponent::setTimelineStatus(AppTimelineCanvasStatus status)
{
    status_ = std::move(status);
    visibleRange_ = normalizeVisibleRange(status_.visibleRange);
    status_.visibleRange = visibleRange_;
    scrollStartTick_ = visibleRange_.startTick;
    zoom_ = 1.0;
    cancelLoopDrag();
    repaint();
}

void TimelineLoopEditorComponent::setSelectedMidiClipId(std::string clipId)
{
    selectedMidiClipId_ = std::move(clipId);
    repaint();
}

void TimelineLoopEditorComponent::cancelLoopDrag() noexcept
{
    draggingLoopEdge_.reset();
    previewLoopRange_.reset();
    repaint();
}

AppTimelineVisibleTickRange TimelineLoopEditorComponent::visibleTickRange() const noexcept
{
    return visibleRange_;
}

std::optional<PlaybackLoopRange> TimelineLoopEditorComponent::previewLoopRange() const noexcept
{
    return previewLoopRange_;
}

std::optional<juce::Rectangle<float>> TimelineLoopEditorComponent::clipBounds(
    std::string_view clipId) const
{
    const auto layoutValue = layout();
    if (layoutValue.lane.isEmpty()) {
        return std::nullopt;
    }

    for (const auto& clip : status_.clips) {
        if (clip.clipId != clipId) {
            continue;
        }
        const auto trackIndex = trackIndexForId(clip.trackId);
        if (!trackIndex.has_value() || clip.endTick <= clip.startTick) {
            return std::nullopt;
        }
        const auto row = trackLaneBounds(*trackIndex);
        if (!row.has_value()) {
            return std::nullopt;
        }
        const auto left = xForTick(clip.startTick);
        const auto right = xForTick(clip.endTick);
        const auto clipArea = juce::Rectangle<float>(
            std::min(left, right),
            row->getY() + clipVerticalInset,
            std::abs(right - left),
            std::max(0.0f, row->getHeight() - 2.0f * clipVerticalInset));
        if (clipArea.isEmpty()) {
            return std::nullopt;
        }
        const auto visibleClipArea = clipArea.getIntersection(layoutValue.lane);
        return visibleClipArea.isEmpty()
            ? std::nullopt
            : std::optional<juce::Rectangle<float>>(visibleClipArea);
    }
    return std::nullopt;
}

std::optional<juce::Rectangle<float>> TimelineLoopEditorComponent::loopHandleBounds(
    AppLoopBoundaryEdge edge) const
{
    const auto loopRange = displayedLoopRange();
    const auto layoutValue = layout();
    if (!loopRange.has_value() || !isValidPlaybackLoopRange(*loopRange)
        || layoutValue.loopBand.isEmpty() || layoutValue.lane.isEmpty()) {
        return std::nullopt;
    }
    const auto tick = edge == AppLoopBoundaryEdge::Start
        ? loopRange->startTick
        : loopRange->endTick;
    if (tick < visibleRange_.startTick || tick > visibleRange_.endTick) {
        return std::nullopt;
    }
    const auto handle = juce::Rectangle<float>(
        xForTick(tick) - loopHandleWidth * 0.5f,
        layoutValue.loopBand.getY(),
        loopHandleWidth,
        layoutValue.loopBand.getHeight());
    const auto visibleHandle = handle.getIntersection(layoutValue.loopBand);
    return visibleHandle.isEmpty()
        ? std::nullopt
        : std::optional<juce::Rectangle<float>>(visibleHandle);
}

void TimelineLoopEditorComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(canvasColour);
    const auto layoutValue = layout();
    if (getWidth() <= 0 || getHeight() <= 0) {
        return;
    }

    graphics.setColour(panelColour);
    graphics.fillRect(layoutValue.header);
    graphics.fillRect(layoutValue.ruler);
    graphics.fillRect(layoutValue.loopBand);

    graphics.setColour(gridColour);
    for (const auto boundaryTick : status_.measureBoundaryTicks) {
        const auto x = xForTick(boundaryTick);
        if (x >= layoutValue.lane.getX() && x <= layoutValue.lane.getRight()) {
            graphics.drawVerticalLine(static_cast<int>(std::round(x)), layoutValue.ruler.getBottom(), getHeight());
        }
    }

    if (const auto loopRange = displayedLoopRange();
        loopRange.has_value() && isValidPlaybackLoopRange(*loopRange)) {
        const auto left = std::clamp(
            xForTick(loopRange->startTick), layoutValue.lane.getX(), layoutValue.lane.getRight());
        const auto right = std::clamp(
            xForTick(loopRange->endTick), layoutValue.lane.getX(), layoutValue.lane.getRight());
        if (right > left) {
            graphics.setColour(loopAccentColour.withAlpha(0.28f));
            graphics.fillRect(juce::Rectangle<float>(
                left, layoutValue.loopBand.getY(), right - left, layoutValue.loopBand.getHeight()));
        }
    }

    for (std::size_t index = 0; index < status_.tracks.size(); ++index) {
        const auto row = trackLaneBounds(index);
        if (!row.has_value()) {
            continue;
        }
        const auto headerRow = juce::Rectangle<float>(
            layoutValue.header.getX(), row->getY(), layoutValue.header.getWidth(), row->getHeight());
        graphics.setColour(gridColour);
        graphics.drawHorizontalLine(static_cast<int>(std::round(row->getY())),
            layoutValue.header.getX(), layoutValue.lane.getRight());
        graphics.setColour(primaryTextColour.withAlpha(0.88f));
        graphics.setFont(juce::FontOptions(13.0f));
        graphics.drawFittedText(
            juce::String::fromUTF8(status_.tracks[index].name.c_str()),
            headerRow.toNearestInt().reduced(8, 0),
            juce::Justification::centredLeft,
            1);
    }

    for (const auto& clip : status_.clips) {
        const auto bounds = clipBounds(clip.clipId);
        if (!bounds.has_value() || bounds->isEmpty()) {
            continue;
        }
        const auto midi = clip.type == ClipType::Midi;
        const auto selected = midi && clip.clipId == selectedMidiClipId_;
        graphics.setColour(selected
            ? loopAccentColour.withAlpha(0.82f)
            : (midi ? juce::Colour(0xff526b58) : juce::Colour(0xff4b5351)));
        graphics.fillRect(*bounds);
        graphics.setColour(selected ? primaryTextColour : primaryTextColour.withAlpha(0.75f));
        graphics.setFont(juce::FontOptions(12.0f));
        graphics.drawFittedText(
            juce::String::fromUTF8(clip.name.c_str()),
            bounds->toNearestInt().reduced(5, 0),
            juce::Justification::centredLeft,
            1);
    }

    for (const auto edge : { AppLoopBoundaryEdge::Start, AppLoopBoundaryEdge::End }) {
        if (const auto handle = loopHandleBounds(edge); handle.has_value()) {
            graphics.setColour(loopAccentColour);
            graphics.fillRect(*handle);
        }
    }
}

void TimelineLoopEditorComponent::mouseDown(const juce::MouseEvent& event)
{
    for (const auto edge : { AppLoopBoundaryEdge::Start, AppLoopBoundaryEdge::End }) {
        if (const auto handle = loopHandleBounds(edge);
            handle.has_value() && handle->contains(event.position)) {
            draggingLoopEdge_ = edge;
            previewLoopRange_.reset();
            repaint();
            return;
        }
    }

    for (const auto& clip : status_.clips) {
        if (const auto bounds = clipBounds(clip.clipId);
            bounds.has_value() && bounds->contains(event.position)) {
            hoveredClipId_ = clip.clipId;
            if (clip.type == ClipType::Midi) {
                selectedMidiClipId_ = clip.clipId;
                if (callbacks_.midiClipSelected) {
                    callbacks_.midiClipSelected(clip.clipId);
                }
            }
            repaint();
            return;
        }
    }
    hoveredClipId_.reset();
}

void TimelineLoopEditorComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (!draggingLoopEdge_.has_value() || !callbacks_.previewLoopBoundary) {
        return;
    }
    previewLoopRange_ = callbacks_.previewLoopBoundary(
        *draggingLoopEdge_, rawCandidateTick(event.position.x));
    repaint();
}

void TimelineLoopEditorComponent::mouseUp(const juce::MouseEvent&)
{
    if (!draggingLoopEdge_.has_value()) {
        return;
    }
    if (juce::ComponentPeer::getCurrentModifiersRealtime().isAnyMouseButtonDown()) {
        cancelLoopDrag();
        return;
    }
    const auto preview = previewLoopRange_;
    draggingLoopEdge_.reset();
    previewLoopRange_.reset();
    if (preview.has_value() && isValidPlaybackLoopRange(*preview)
        && preview != status_.playbackLoopRange && callbacks_.commitLoopRange) {
        callbacks_.commitLoopRange(*preview);
    }
    repaint();
}

void TimelineLoopEditorComponent::mouseCaptureLost() noexcept
{
    cancelLoopDrag();
}

void TimelineLoopEditorComponent::mouseWheelMove(
    const juce::MouseEvent&,
    const juce::MouseWheelDetails& wheel)
{
    const auto current = visibleRange_;
    const auto span = current.endTick - current.startTick;
    const auto deltaX = std::isfinite(wheel.deltaX) ? static_cast<double>(wheel.deltaX) : 0.0;
    const auto deltaY = std::isfinite(wheel.deltaY) ? static_cast<double>(wheel.deltaY) : 0.0;
    const auto zoomFactor = deltaY > 0.0 ? 0.8 : (deltaY < 0.0 ? 1.25 : 1.0);
    const auto maximumSpan = std::numeric_limits<std::int64_t>::max();
    const auto scaledSpan = static_cast<double>(span) * zoomFactor;
    const auto zoomedSpan = scaledSpan >= static_cast<double>(maximumSpan)
        ? maximumSpan
        : std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(scaledSpan)));
    zoom_ = std::clamp(zoom_ / zoomFactor, 0.0001, 1'000'000.0);
    const auto maximumStart = std::numeric_limits<std::int64_t>::max() - zoomedSpan;
    const auto scaledShift = static_cast<double>(zoomedSpan) * deltaX * 0.1;
    const auto minimumShift = -current.startTick;
    const auto maximumShift = maximumStart - current.startTick;
    const auto clampedShift = std::clamp(
        scaledShift,
        static_cast<double>(minimumShift),
        static_cast<double>(maximumShift));
    const auto shift = clampedShift <= static_cast<double>(minimumShift)
        ? minimumShift
        : (clampedShift >= static_cast<double>(maximumShift)
            ? maximumShift
            : static_cast<std::int64_t>(std::llround(clampedShift)));
    visibleRange_ = makeVisibleRange(current.startTick + shift, zoomedSpan);
    scrollStartTick_ = visibleRange_.startTick;
    status_.visibleRange = visibleRange_;
    if (callbacks_.visibleRangeChanged) {
        callbacks_.visibleRangeChanged(visibleRange_);
    }
    repaint();
}

AppTimelineVisibleTickRange TimelineLoopEditorComponent::normalizeVisibleRange(
    AppTimelineVisibleTickRange range) noexcept
{
    return isValidRange(range) ? range : AppTimelineVisibleTickRange { 0, 1 };
}

TimelineLoopEditorComponent::Layout TimelineLoopEditorComponent::layout() const noexcept
{
    const auto width = std::max(0.0f, static_cast<float>(getWidth()));
    const auto height = std::max(0.0f, static_cast<float>(getHeight()));
    const auto actualHeaderWidth = std::min(headerWidth, std::max(0.0f, width - 1.0f));
    const auto actualRulerHeight = std::min(rulerHeight, height);
    const auto actualLoopHeight = std::min(loopBandHeight, std::max(0.0f, height - actualRulerHeight));
    return {
        { 0.0f, 0.0f, actualHeaderWidth, height },
        { actualHeaderWidth, 0.0f, width - actualHeaderWidth, height },
        { actualHeaderWidth, 0.0f, width - actualHeaderWidth, actualRulerHeight },
        { actualHeaderWidth, actualRulerHeight, width - actualHeaderWidth, actualLoopHeight }
    };
}

std::optional<std::size_t> TimelineLoopEditorComponent::trackIndexForId(
    std::string_view trackId) const
{
    for (std::size_t index = 0; index < status_.tracks.size(); ++index) {
        if (status_.tracks[index].trackId == trackId) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<juce::Rectangle<float>> TimelineLoopEditorComponent::trackLaneBounds(
    std::size_t index) const
{
    const auto layoutValue = layout();
    if (index >= status_.tracks.size() || layoutValue.lane.isEmpty()) {
        return std::nullopt;
    }
    const auto y = layoutValue.loopBand.getBottom()
        + static_cast<float>(index) * trackRowHeight;
    const auto height = std::min(trackRowHeight, std::max(0.0f, layoutValue.lane.getBottom() - y));
    if (height <= 0.0f) {
        return std::nullopt;
    }
    return juce::Rectangle<float> {
        layoutValue.lane.getX(), y, layoutValue.lane.getWidth(), height
    };
}

std::optional<PlaybackLoopRange> TimelineLoopEditorComponent::displayedLoopRange() const noexcept
{
    return previewLoopRange_.has_value() ? previewLoopRange_ : status_.playbackLoopRange;
}

float TimelineLoopEditorComponent::xForTick(std::int64_t tick) const noexcept
{
    const auto lane = layout().lane;
    const auto span = visibleRange_.endTick - visibleRange_.startTick;
    if (lane.isEmpty() || span <= 0) {
        return lane.getX();
    }
    if (tick <= visibleRange_.startTick) {
        return lane.getX();
    }
    if (tick >= visibleRange_.endTick) {
        return lane.getRight();
    }
    const auto offset = tick - visibleRange_.startTick;
    const auto ratio = static_cast<double>(offset)
        / static_cast<double>(span);
    return lane.getX() + static_cast<float>(ratio * lane.getWidth());
}

std::int64_t TimelineLoopEditorComponent::rawCandidateTick(float x) const noexcept
{
    const auto lane = layout().lane;
    const auto span = visibleRange_.endTick - visibleRange_.startTick;
    if (lane.isEmpty() || span <= 0) {
        return visibleRange_.startTick;
    }
    if (x <= lane.getX()) {
        return visibleRange_.startTick;
    }
    if (x >= lane.getRight()) {
        return visibleRange_.endTick;
    }
    const auto ratio = std::clamp(
        static_cast<double>(x - lane.getX()) / static_cast<double>(lane.getWidth()),
        0.0,
        1.0);
    const auto scaledOffset = ratio * static_cast<double>(span);
    if (!std::isfinite(scaledOffset) || scaledOffset <= 0.0) {
        return visibleRange_.startTick;
    }
    if (scaledOffset >= static_cast<double>(span)) {
        return visibleRange_.endTick;
    }
    const auto offset = static_cast<std::int64_t>(std::llround(scaledOffset));
    return visibleRange_.startTick + std::clamp<std::int64_t>(offset, 0, span);
}

}
