#pragma once

#include "AppLoopActions.h"
#include "AppTimelineCanvasStatus.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace trackloom {

inline constexpr auto timelineLoopEditorComponentId =
    "trackloom-timeline-loop-editor";

struct TimelineLoopEditorCallbacks {
    std::function<void(std::string)> midiClipSelected;
    std::function<std::optional<PlaybackLoopRange>(
        AppLoopBoundaryEdge, std::int64_t)> previewLoopBoundary;
    std::function<void(PlaybackLoopRange)> commitLoopRange;
    std::function<void(AppTimelineVisibleTickRange)> visibleRangeChanged;
};

class TimelineLoopEditorComponent final : public juce::Component {
public:
    explicit TimelineLoopEditorComponent(
        TimelineLoopEditorCallbacks callbacks = {});
    void setTimelineStatus(AppTimelineCanvasStatus status);
    void setSelectedMidiClipId(std::string clipId);
    void cancelLoopDrag() noexcept;
    AppTimelineVisibleTickRange visibleTickRange() const noexcept;
    std::optional<PlaybackLoopRange> previewLoopRange() const noexcept;
    std::optional<juce::Rectangle<float>> clipBounds(
        std::string_view clipId) const;
    std::optional<juce::Rectangle<float>> loopHandleBounds(
        AppLoopBoundaryEdge edge) const;
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    // JUCE 8 的 Component 没有 mouseCaptureLost virtual hook；宿主在失去捕获时显式调用此入口。
    void mouseCaptureLost() noexcept;
    void mouseWheelMove(
        const juce::MouseEvent&,
        const juce::MouseWheelDetails&) override;

private:
    struct Layout {
        juce::Rectangle<float> header;
        juce::Rectangle<float> lane;
        juce::Rectangle<float> ruler;
        juce::Rectangle<float> loopBand;
    };

    static AppTimelineVisibleTickRange normalizeVisibleRange(
        AppTimelineVisibleTickRange range) noexcept;
    Layout layout() const noexcept;
    std::optional<std::size_t> trackIndexForId(std::string_view trackId) const;
    std::optional<juce::Rectangle<float>> trackLaneBounds(std::size_t index) const;
    std::optional<PlaybackLoopRange> displayedLoopRange() const noexcept;
    float xForTick(std::int64_t tick) const noexcept;
    std::int64_t rawCandidateTick(float x) const noexcept;

    TimelineLoopEditorCallbacks callbacks_;
    AppTimelineCanvasStatus status_;
    std::string selectedMidiClipId_;
    AppTimelineVisibleTickRange visibleRange_ { 0, 1 };
    double zoom_ = 1.0;
    std::int64_t scrollStartTick_ = 0;
    std::optional<std::string> hoveredClipId_;
    std::optional<AppLoopBoundaryEdge> draggingLoopEdge_;
    std::optional<PlaybackLoopRange> previewLoopRange_;
};

}
