#pragma once

#include "AppProjectReplacementTypes.h"

#include <filesystem>
#include <string>

namespace trackloom {

class AppLoopPlaybackState;
class AppPlaybackController;
class AppProjectSession;

struct AppProjectObjectSelection {
    std::string& selectedTrackId;
    std::string& selectedAudioTrackId;
    std::string& selectedAudioClipId;
    std::string& selectedMidiClipId;

    void resetForProjectReplacement() noexcept;
};

AppProjectReplacementSafety prepareAppProjectReplacement(
    AppPlaybackController& playback);
AppProjectReplacementFeedback createNewAppProjectIfSafe(
    AppProjectSession& session,
    AppPlaybackController& playback,
    AppLoopPlaybackState& loopState,
    std::string name);
AppProjectReplacementFeedback createNewAppProjectIfSafe(
    AppProjectSession& session,
    AppPlaybackController& playback,
    AppLoopPlaybackState& loopState,
    AppProjectObjectSelection selection,
    std::string name);
AppProjectReplacementFeedback openAppProjectIfSafe(
    AppProjectSession& session,
    AppPlaybackController& playback,
    AppLoopPlaybackState& loopState,
    const std::filesystem::path& path);
AppProjectReplacementFeedback openAppProjectIfSafe(
    AppProjectSession& session,
    AppPlaybackController& playback,
    AppLoopPlaybackState& loopState,
    AppProjectObjectSelection selection,
    const std::filesystem::path& path);

}
