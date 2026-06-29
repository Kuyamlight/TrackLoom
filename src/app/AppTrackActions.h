#pragma once

#include "AppProjectSession.h"

#include <string>

namespace trackloom {

// AppTrackActionFeedbackKind 给 UI、快捷键和测试提供稳定分支。
// 中文 message 只用于展示，不应被后续逻辑解析。
enum class AppTrackActionFeedbackKind {
    Success,
    MissingTrack,
    IncompatibleTrackType,
    EmptyName,
    AlreadyAtBoundary,
    RenameFailed,
    MoveFailed,
    DeleteFailed,
    CreateFailed
};

// AppTrackActionFeedback 是桌面轨道动作的统一展示结果。
// trackId 只在 success 为 true 时有效，便于 UI 保持或清理当前选择。
struct AppTrackActionFeedback {
    bool success = false;
    AppTrackActionFeedbackKind kind = AppTrackActionFeedbackKind::CreateFailed;
    std::string message;
    std::string trackId;
};

// createDefaultInstrumentTrack 创建首屏使用的默认乐器轨。
// 这里固定创建 Instrument N；轨道重命名和轨道类型选择属于后续编辑入口。
AppTrackActionFeedback createDefaultInstrumentTrack(AppProjectSession& session);

// deleteInstrumentTrackById 删除一个已存在的乐器轨。
// 当前首屏只允许删除目标乐器轨；音频轨和文件夹轨由后续正式轨道编辑器处理。
AppTrackActionFeedback deleteInstrumentTrackById(
    AppProjectSession& session,
    const std::string& trackId);

// renameTrackById 修改目标轨道名称。
// 名称会先去掉首尾空白；空名称会被拒绝，避免 UI 保存不可见轨道名。
AppTrackActionFeedback renameTrackById(
    AppProjectSession& session,
    const std::string& trackId,
    std::string name);

// moveInstrumentTrackUp / Down 调整当前乐器轨在工程轨道列表中的顺序。
// 当前首屏只暴露乐器轨选择，因此这里保持与删除入口相同的类型边界。
AppTrackActionFeedback moveInstrumentTrackUp(
    AppProjectSession& session,
    const std::string& trackId);
AppTrackActionFeedback moveInstrumentTrackDown(
    AppProjectSession& session,
    const std::string& trackId);

}
