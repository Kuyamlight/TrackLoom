#include "AppProjectReplacementActions.h"

#include "AppLoopActions.h"
#include "AppPlaybackActions.h"
#include "AppProjectSession.h"

#include <utility>

namespace trackloom {
namespace {

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
    if (session.isDirty()) {
        return dirtyProjectFailure();
    }

    const auto safety = prepareAppProjectReplacement(playback);
    if (!safety.safe) {
        return replacementFailure(safety.failureReason, safety.message);
    }

    session.createNewProject(std::move(name));
    playback.resetAfterProjectReplacement();
    loopState.resetForProjectReplacement();
    return { true, AppProjectReplacementFailureReason::None, "已新建空白工程。" };
}

AppProjectReplacementFeedback openAppProjectIfSafe(
    AppProjectSession& session,
    AppPlaybackController& playback,
    AppLoopPlaybackState& loopState,
    const std::filesystem::path& path)
{
    if (session.isDirty()) {
        return dirtyProjectFailure();
    }

    const auto safety = prepareAppProjectReplacement(playback);
    if (!safety.safe) {
        return replacementFailure(safety.failureReason, safety.message);
    }

    const auto opened = session.openFrom(path);
    if (!opened.success) {
        return replacementFailure(
            AppProjectReplacementFailureReason::OpenFailed,
            "打开工程失败：" + opened.error);
    }

    playback.resetAfterProjectReplacement();
    loopState.resetForProjectReplacement();
    return { true, AppProjectReplacementFailureReason::None, "已打开工程：" + path.string() };
}

}
