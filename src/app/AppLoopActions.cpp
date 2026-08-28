#include "AppLoopActions.h"

#include "AppTimelineCanvasStatus.h"
#include "Command.h"

#include <limits>
#include <memory>
#include <utility>

namespace trackloom {

namespace detail {

AppLoopClipTimingClassification classifyAppLoopClipTiming(
    std::int64_t startTick,
    std::int64_t lengthTick) noexcept
{
    if (startTick < 0 || lengthTick <= 0) {
        return AppLoopClipTimingClassification::InvalidTiming;
    }
    if (lengthTick > std::numeric_limits<std::int64_t>::max() - startTick) {
        return AppLoopClipTimingClassification::EndOverflow;
    }
    return AppLoopClipTimingClassification::Valid;
}

}

namespace {

AppLoopActionFeedback successFeedback(
    AppLoopActionFeedbackKind kind,
    std::string message)
{
    return { true, kind, std::move(message) };
}

AppLoopActionFeedback failureFeedback(
    AppLoopActionFeedbackKind kind,
    std::string message)
{
    return { false, kind, std::move(message) };
}

AppLoopPreviewResult previewFailure(
    AppLoopActionFeedbackKind kind,
    std::string message)
{
    return { false, kind, std::nullopt, std::move(message) };
}

std::optional<std::int64_t> snappedBoundaryForEdge(
    const AppTimelineMeasureBoundaryNeighbors& neighbors,
    AppLoopBoundaryEdge edge,
    std::int64_t fixedTick,
    std::int64_t candidateTick) noexcept
{
    std::optional<std::int64_t> snapped;
    const auto consider = [&](const std::optional<std::int64_t>& tick) {
        if (!tick.has_value()
            || (edge == AppLoopBoundaryEdge::Start && *tick >= fixedTick)
            || (edge == AppLoopBoundaryEdge::End && *tick <= fixedTick)) {
            return;
        }

        if (!snapped.has_value()) {
            snapped = tick;
            return;
        }

        const auto currentDistance = *snapped <= candidateTick
            ? candidateTick - *snapped
            : *snapped - candidateTick;
        const auto nextDistance = *tick <= candidateTick
            ? candidateTick - *tick
            : *tick - candidateTick;
        if (nextDistance < currentDistance
            || (nextDistance == currentDistance && *tick < *snapped)) {
            snapped = tick;
        }
    };

    consider(neighbors.atOrBeforeTick);
    consider(neighbors.atOrAfterTick);
    return snapped;
}

}

bool AppLoopPlaybackState::enabled() const noexcept
{
    return enabled_;
}

bool AppLoopPlaybackState::setEnabled(const Project& project, bool enabled) noexcept
{
    if (enabled && !project.playbackLoopRange().has_value()) {
        enabled_ = false;
        return false;
    }

    enabled_ = enabled;
    return true;
}

void AppLoopPlaybackState::reconcile(const Project& project) noexcept
{
    if (!project.playbackLoopRange().has_value()) {
        enabled_ = false;
    }
}

void AppLoopPlaybackState::resetForProjectReplacement() noexcept
{
    enabled_ = false;
}

std::optional<PlaybackLoopRange> effectiveAppPlaybackLoopRange(
    const Project& project,
    const AppLoopPlaybackState& state) noexcept
{
    if (!state.enabled()) {
        return std::nullopt;
    }

    return project.playbackLoopRange();
}

AppLoopActionFeedback setAppPlaybackLoopFromSelectedMidiClip(
    AppProjectSession& session,
    std::string_view selectedMidiClipId,
    AppLoopPlaybackState& state)
{
    if (selectedMidiClipId.empty()) {
        return failureFeedback(
            AppLoopActionFeedbackKind::MissingSelection,
            "无法设为循环：未选择 MIDI 片段。");
    }

    const auto clip = session.project().findClipById(std::string(selectedMidiClipId));
    if (!clip.has_value()) {
        return failureFeedback(
            AppLoopActionFeedbackKind::MissingClip,
            "无法设为循环：选中的片段不存在。");
    }
    if (clip->type != ClipType::Midi) {
        return failureFeedback(
            AppLoopActionFeedbackKind::IncompatibleClipType,
            "无法设为循环：只能使用 MIDI 片段。");
    }

    const auto timing = detail::classifyAppLoopClipTiming(clip->startTick, clip->lengthTick);
    if (timing == detail::AppLoopClipTimingClassification::InvalidTiming) {
        return failureFeedback(
            AppLoopActionFeedbackKind::InvalidLoopRange,
            "无法设为循环：片段时间范围无效。");
    }
    if (timing == detail::AppLoopClipTimingClassification::EndOverflow) {
        return failureFeedback(
            AppLoopActionFeedbackKind::ClipEndOverflow,
            "无法设为循环：片段终点超出可表示 tick 范围。");
    }
    const auto endTick = clip->startTick + clip->lengthTick;

    return commitAppPlaybackLoopRange(session, { clip->startTick, endTick }, state);
}

AppLoopPreviewResult previewAppPlaybackLoopBoundaryDrag(
    const Project& project,
    AppLoopBoundaryEdge edge,
    std::int64_t candidateTick)
{
    const auto& existingRange = project.playbackLoopRange();
    if (!existingRange.has_value()) {
        return previewFailure(
            AppLoopActionFeedbackKind::MissingLoopRange,
            "无法预览循环边界：工程没有循环范围。");
    }
    if (!isValidPlaybackLoopRange(*existingRange)) {
        return previewFailure(
            AppLoopActionFeedbackKind::InvalidLoopRange,
            "无法预览循环边界：工程循环范围无效。");
    }

    const auto search = findAppTimelineMeasureBoundaryNeighbors(project, candidateTick);
    if (!search.success) {
        return previewFailure(
            AppLoopActionFeedbackKind::InvalidLoopRange,
            "无法预览循环边界：候选 tick 无效。");
    }

    const auto fixedTick = edge == AppLoopBoundaryEdge::Start
        ? existingRange->endTick
        : existingRange->startTick;
    const auto snapped = snappedBoundaryForEdge(
        search.neighbors, edge, fixedTick, candidateTick);
    if (!snapped.has_value()) {
        return previewFailure(
            AppLoopActionFeedbackKind::InvalidLoopRange,
            "无法预览循环边界：没有不交叉的小节边界。");
    }

    const PlaybackLoopRange range = edge == AppLoopBoundaryEdge::Start
        ? PlaybackLoopRange { *snapped, fixedTick }
        : PlaybackLoopRange { fixedTick, *snapped };
    if (!isValidPlaybackLoopRange(range)) {
        return previewFailure(
            AppLoopActionFeedbackKind::InvalidLoopRange,
            "无法预览循环边界：吸附后的范围无效。");
    }

    return { true, AppLoopActionFeedbackKind::Success, range, "循环边界预览已吸附到小节。" };
}

AppLoopActionFeedback commitAppPlaybackLoopRange(
    AppProjectSession& session,
    PlaybackLoopRange range,
    AppLoopPlaybackState& state)
{
    if (!isValidPlaybackLoopRange(range)) {
        return failureFeedback(
            AppLoopActionFeedbackKind::InvalidLoopRange,
            "无法设置循环范围：范围无效。");
    }

    if (session.project().playbackLoopRange() == range) {
        if (!state.enabled()) {
            state.setEnabled(session.project(), true);
            return successFeedback(
                AppLoopActionFeedbackKind::SessionOnlyEnabled,
                "循环范围未变，已仅在当前会话启用循环播放。");
        }
        return successFeedback(
            AppLoopActionFeedbackKind::NoOp,
            "循环范围和会话状态均未变化。");
    }

    const auto result = session.executeProjectCommand(
        std::make_unique<SetProjectPlaybackLoopCommand>(range));
    if (!result.success) {
        return failureFeedback(
            AppLoopActionFeedbackKind::CommandFailed,
            "无法设置循环范围：" + result.message);
    }

    state.setEnabled(session.project(), true);
    return successFeedback(AppLoopActionFeedbackKind::Success, "循环范围已设置并在当前会话启用。");
}

AppLoopActionFeedback clearAppPlaybackLoopRange(
    AppProjectSession& session,
    AppLoopPlaybackState& state)
{
    if (!session.project().playbackLoopRange().has_value()) {
        state.reconcile(session.project());
        return successFeedback(AppLoopActionFeedbackKind::NoOp, "工程没有循环范围。");
    }

    const auto result = session.executeProjectCommand(
        std::make_unique<SetProjectPlaybackLoopCommand>(std::nullopt));
    if (!result.success) {
        return failureFeedback(
            AppLoopActionFeedbackKind::CommandFailed,
            "无法清除循环范围：" + result.message);
    }

    state.reconcile(session.project());
    return successFeedback(AppLoopActionFeedbackKind::Success, "循环范围已清除。");
}

AppLoopActionFeedback toggleAppLoopPlaybackEnabled(
    const Project& project,
    AppLoopPlaybackState& state)
{
    if (!project.playbackLoopRange().has_value()) {
        state.reconcile(project);
        return failureFeedback(
            AppLoopActionFeedbackKind::MissingLoopRange,
            "无法切换循环播放：工程没有循环范围。");
    }

    state.setEnabled(project, !state.enabled());
    return successFeedback(AppLoopActionFeedbackKind::Success,
        state.enabled() ? "当前会话已启用循环播放。" : "当前会话已关闭循环播放。");
}

}
