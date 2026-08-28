#pragma once

#include "AppProjectSession.h"
#include "LoopRange.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace trackloom {

namespace detail {

enum class AppLoopClipTimingClassification {
    Valid,
    InvalidTiming,
    EndOverflow
};

AppLoopClipTimingClassification classifyAppLoopClipTiming(
    std::int64_t startTick,
    std::int64_t lengthTick) noexcept;

}

class AppLoopPlaybackState final {
public:
    bool enabled() const noexcept;
    bool setEnabled(const Project& project, bool enabled) noexcept;
    void reconcile(const Project& project) noexcept;
    void resetForProjectReplacement() noexcept;

private:
    bool enabled_ = false;
};

enum class AppLoopBoundaryEdge { Start, End };

enum class AppLoopActionFeedbackKind {
    Success,
    SessionOnlyEnabled,
    NoOp,
    MissingSelection,
    MissingClip,
    IncompatibleClipType,
    ClipEndOverflow,
    InvalidLoopRange,
    MissingLoopRange,
    CommandFailed
};

struct AppLoopActionFeedback {
    bool success = false;
    AppLoopActionFeedbackKind kind =
        AppLoopActionFeedbackKind::CommandFailed;
    std::string message;
};

struct AppLoopPreviewResult {
    bool success = false;
    AppLoopActionFeedbackKind kind =
        AppLoopActionFeedbackKind::InvalidLoopRange;
    std::optional<PlaybackLoopRange> range;
    std::string message;
};

std::optional<PlaybackLoopRange> effectiveAppPlaybackLoopRange(
    const Project& project,
    const AppLoopPlaybackState& state) noexcept;
AppLoopActionFeedback setAppPlaybackLoopFromSelectedMidiClip(
    AppProjectSession& session,
    std::string_view selectedMidiClipId,
    AppLoopPlaybackState& state);
AppLoopPreviewResult previewAppPlaybackLoopBoundaryDrag(
    const Project& project,
    AppLoopBoundaryEdge edge,
    std::int64_t candidateTick);
AppLoopActionFeedback commitAppPlaybackLoopRange(
    AppProjectSession& session,
    PlaybackLoopRange range,
    AppLoopPlaybackState& state);
AppLoopActionFeedback clearAppPlaybackLoopRange(
    AppProjectSession& session,
    AppLoopPlaybackState& state);
AppLoopActionFeedback toggleAppLoopPlaybackEnabled(
    const Project& project,
    AppLoopPlaybackState& state);

}
