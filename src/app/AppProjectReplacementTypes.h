#pragma once

#include <string>

namespace trackloom {

enum class AppProjectReplacementFailureReason {
    None,
    DirtyProject,
    PlaybackActive,
    PreparationWorkerActive,
    HostNotQuiescent,
    HostResetFailed,
    OpenFailed
};

struct AppProjectReplacementSafety {
    bool safe = false;
    AppProjectReplacementFailureReason failureReason =
        AppProjectReplacementFailureReason::HostNotQuiescent;
    std::string message;
};

struct AppProjectReplacementFeedback {
    bool success = false;
    AppProjectReplacementFailureReason failureReason =
        AppProjectReplacementFailureReason::OpenFailed;
    std::string message;
};

}
