#include "AppProjectReplacementActions.h"

#include "AppLoopActions.h"
#include "AppPlaybackActions.h"
#include "AppProjectSession.h"

#include <type_traits>
#include <utility>

namespace trackloom {
namespace {

static_assert(noexcept(
    std::declval<AppLoopPlaybackState&>().resetForProjectReplacement()));
static_assert(noexcept(
    std::declval<AppProjectObjectSelection&>().resetForProjectReplacement()));

AppProjectReplacementFeedback replacementFailure(
    AppProjectReplacementFailureReason reason,
    std::string message)
{
    return { false, reason, std::move(message) };
}

AppProjectReplacementFeedback dirtyProjectFailure()
{
    return replacementFailure(
        AppProjectReplacementFailureReason::DirtyProject,
        "当前工程有未保存修改，请先保存或另存为。");
}

AppProjectReplacementFeedback createNewAppProjectIfSafeImpl(
    AppProjectSession& session,
    AppPlaybackController& playback,
    AppLoopPlaybackState& loopState,
    AppProjectObjectSelection* selection,
    std::string name)
{
    auto exceptionFailure = replacementFailure(
        AppProjectReplacementFailureReason::OpenFailed,
        "新建工程失败：替换状态准备未完成。");
    try {
        if (session.isDirty()) {
            return dirtyProjectFailure();
        }

        const auto safety = prepareAppProjectReplacement(playback);
        if (!safety.safe) {
            return replacementFailure(safety.failureReason, safety.message);
        }

        auto success = AppProjectReplacementFeedback {
            true, AppProjectReplacementFailureReason::None, "已新建空白工程。"
        };
        auto stagedSession = session.stageNewProject(std::move(name));
        auto stagedPlayback = playback.stageProjectReplacementReset();

        session.commitProjectReplacement(std::move(stagedSession));
        playback.commitProjectReplacementReset(std::move(stagedPlayback));
        loopState.resetForProjectReplacement();
        if (selection != nullptr) {
            selection->resetForProjectReplacement();
        }
        return success;
    } catch (...) {
        return exceptionFailure;
    }
}

AppProjectReplacementFeedback openAppProjectIfSafeImpl(
    AppProjectSession& session,
    AppPlaybackController& playback,
    AppLoopPlaybackState& loopState,
    AppProjectObjectSelection* selection,
    const std::filesystem::path& path)
{
    auto exceptionFailure = replacementFailure(
        AppProjectReplacementFailureReason::OpenFailed,
        "打开工程失败：替换状态准备未完成。");
    try {
        if (session.isDirty()) {
            return dirtyProjectFailure();
        }

        const auto safety = prepareAppProjectReplacement(playback);
        if (!safety.safe) {
            return replacementFailure(safety.failureReason, safety.message);
        }

        auto success = AppProjectReplacementFeedback {
            true,
            AppProjectReplacementFailureReason::None,
            "已打开工程：" + path.string()
        };
        auto stagedSession = session.stageOpenProject(path);
        if (!stagedSession.replacement.has_value()) {
            return replacementFailure(
                AppProjectReplacementFailureReason::OpenFailed,
                "打开工程失败：" + stagedSession.error);
        }
        auto stagedPlayback = playback.stageProjectReplacementReset();

        session.commitProjectReplacement(std::move(*stagedSession.replacement));
        playback.commitProjectReplacementReset(std::move(stagedPlayback));
        loopState.resetForProjectReplacement();
        if (selection != nullptr) {
            selection->resetForProjectReplacement();
        }
        return success;
    } catch (...) {
        return exceptionFailure;
    }
}

}

void AppProjectObjectSelection::resetForProjectReplacement() noexcept
{
    selectedTrackId.clear();
    selectedAudioTrackId.clear();
    selectedAudioClipId.clear();
    selectedMidiClipId.clear();
}

AppProjectReplacementSafety prepareAppProjectReplacement(
    AppPlaybackController& playback)
{
    return playback.prepareForProjectReplacement();
}

AppProjectReplacementFeedback createNewAppProjectIfSafe(
    AppProjectSession& session,
    AppPlaybackController& playback,
    AppLoopPlaybackState& loopState,
    std::string name)
{
    return createNewAppProjectIfSafeImpl(
        session, playback, loopState, nullptr, std::move(name));
}

AppProjectReplacementFeedback createNewAppProjectIfSafe(
    AppProjectSession& session,
    AppPlaybackController& playback,
    AppLoopPlaybackState& loopState,
    AppProjectObjectSelection selection,
    std::string name)
{
    return createNewAppProjectIfSafeImpl(
        session, playback, loopState, &selection, std::move(name));
}

AppProjectReplacementFeedback openAppProjectIfSafe(
    AppProjectSession& session,
    AppPlaybackController& playback,
    AppLoopPlaybackState& loopState,
    const std::filesystem::path& path)
{
    return openAppProjectIfSafeImpl(
        session, playback, loopState, nullptr, path);
}

AppProjectReplacementFeedback openAppProjectIfSafe(
    AppProjectSession& session,
    AppPlaybackController& playback,
    AppLoopPlaybackState& loopState,
    AppProjectObjectSelection selection,
    const std::filesystem::path& path)
{
    return openAppProjectIfSafeImpl(
        session, playback, loopState, &selection, path);
}

}
