#pragma once

#include "AppProjectReplacementTypes.h"

#include <filesystem>
#include <string>

namespace trackloom {

class AppLoopPlaybackState;
class AppPlaybackController;
class AppProjectSession;

AppProjectReplacementSafety prepareAppProjectReplacement(
    AppPlaybackController& playback);
AppProjectReplacementFeedback createNewAppProjectIfSafe(
    AppProjectSession& session,
    AppPlaybackController& playback,
    AppLoopPlaybackState& loopState,
    std::string name);
AppProjectReplacementFeedback openAppProjectIfSafe(
    AppProjectSession& session,
    AppPlaybackController& playback,
    AppLoopPlaybackState& loopState,
    const std::filesystem::path& path);

}
