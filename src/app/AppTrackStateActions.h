#pragma once

#include "AppProjectSession.h"

#include <string>

namespace trackloom {

// AppTrackStateActionTarget 标识本次切换的是哪一种轨道状态。
// UI 可以用它做按钮状态或提示；普通逻辑不要解析中文 message。
enum class AppTrackStateActionTarget {
    Mute,
    Solo,
    Disable,
    Hide
};

// AppTrackStateActionFeedbackKind 给 UI、快捷键和 AI 工具提供稳定分支。
// 失败原因用枚举表达，避免后续代码依赖展示文案。
enum class AppTrackStateActionFeedbackKind {
    Success,
    MissingTrack,
    UpdateFailed
};

// AppTrackStateActionFeedback 是轨道状态切换后的统一结果。
// enabled 表示切换后该状态是否开启，便于 UI 同步按钮或状态栏。
struct AppTrackStateActionFeedback {
    bool success = false;
    AppTrackStateActionFeedbackKind kind = AppTrackStateActionFeedbackKind::UpdateFailed;
    AppTrackStateActionTarget target = AppTrackStateActionTarget::Mute;
    std::string message;
    std::string trackId;
    bool enabled = false;
};

// 以下函数只负责“切换”单个状态，不负责选择轨道。
// 轨道选择属于 UI、快捷键或 AI 工具的上层职责。
AppTrackStateActionFeedback toggleTrackMuted(AppProjectSession& session, const std::string& trackId);
AppTrackStateActionFeedback toggleTrackSoloed(AppProjectSession& session, const std::string& trackId);
AppTrackStateActionFeedback toggleTrackDisabled(AppProjectSession& session, const std::string& trackId);
AppTrackStateActionFeedback toggleTrackHidden(AppProjectSession& session, const std::string& trackId);

}
