#include "AppProjectFileActions.h"

#include <utility>

namespace trackloom {
namespace {

std::string actionSuccessMessage(AppProjectFileAction action)
{
    switch (action) {
    case AppProjectFileAction::Open:
        return "工程已打开。";
    case AppProjectFileAction::Save:
        return "工程已保存。";
    case AppProjectFileAction::SaveAs:
        return "工程已另存为。";
    }

    return "工程文件操作已完成。";
}

std::string actionCanceledMessage(AppProjectFileAction action)
{
    switch (action) {
    case AppProjectFileAction::Open:
        return "已取消打开工程。";
    case AppProjectFileAction::Save:
        return "已取消保存工程。";
    case AppProjectFileAction::SaveAs:
        return "已取消另存为。";
    }

    return "已取消工程文件操作。";
}

std::string actionFailurePrefix(AppProjectFileAction action)
{
    switch (action) {
    case AppProjectFileAction::Open:
        return "打开工程失败：";
    case AppProjectFileAction::Save:
        return "保存工程失败：";
    case AppProjectFileAction::SaveAs:
        return "另存为失败：";
    }

    return "工程文件操作失败：";
}

std::string utf8PathForMessage(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return { reinterpret_cast<const char*>(utf8.data()), utf8.size() };
}

std::vector<std::filesystem::path> uniqueRecoveryPaths(
    const std::vector<std::filesystem::path>& recoveryPaths)
{
    std::vector<std::filesystem::path> uniquePaths;

    for (const auto& recoveryPath : recoveryPaths) {
        auto alreadyPresent = false;
        for (const auto& existingPath : uniquePaths) {
            if (existingPath == recoveryPath) {
                alreadyPresent = true;
                break;
            }
        }

        if (!alreadyPresent) {
            uniquePaths.push_back(recoveryPath);
        }
    }

    return uniquePaths;
}

AppProjectFileActionFeedback feedback(
    bool success,
    AppProjectFileActionFeedbackKind kind,
    std::string message,
    std::vector<std::filesystem::path> recoveryPaths = {})
{
    return { success, kind, std::move(message), std::move(recoveryPaths) };
}

}

std::filesystem::path withTrackLoomProjectExtension(std::filesystem::path path)
{
    if (!path.has_extension()) {
        path.replace_extension(".trackloom");
    }

    return path;
}

AppProjectFileActionFeedback describeAppProjectFileActionResult(
    AppProjectFileAction action,
    const AppProjectSessionResult& result)
{
    if (result.success) {
        if (!result.warning.empty() || !result.recoveryPaths.empty()) {
            const auto warning = result.warning.empty()
                ? std::string("Recovery data requires inspection.")
                : result.warning;
            return feedback(
                true,
                AppProjectFileActionFeedbackKind::Warning,
                actionSuccessMessage(action) + " " + warning,
                uniqueRecoveryPaths(result.recoveryPaths));
        }

        return feedback(true, AppProjectFileActionFeedbackKind::Success, actionSuccessMessage(action));
    }

    if (result.failureReason == AppProjectSessionFailureReason::MissingProjectPath) {
        return feedback(
            false,
            AppProjectFileActionFeedbackKind::NeedsSaveAs,
            "当前工程还没有文件路径，请先使用“另存为”。");
    }

    return feedback(
        false,
        AppProjectFileActionFeedbackKind::Failure,
        actionFailurePrefix(action) + result.error);
}

AppProjectFileActionFeedback describeCanceledAppProjectFileAction(AppProjectFileAction action)
{
    return feedback(false, AppProjectFileActionFeedbackKind::Canceled, actionCanceledMessage(action));
}

AppProjectFileActionPresentation describeAppProjectFileActionPresentation(
    const AppProjectFileActionFeedback& feedback)
{
    if (feedback.kind != AppProjectFileActionFeedbackKind::Warning) {
        return { feedback.message, {}, false };
    }

    auto details = feedback.message;
    if (!feedback.recoveryPaths.empty()) {
        // warning 是必须原样保留的自由文本；恢复路径以这份结构化清单为权威。
        // 即使正文碰巧也提到某条路径，也不能再用子串匹配省略清单项，否则前缀路径会误判。
        details += "\n\n需要检查的恢复路径：";
        for (const auto& recoveryPath : feedback.recoveryPaths) {
            details += "\n- " + utf8PathForMessage(recoveryPath);
        }
    }

    return {
        "工程文件操作已完成，但有恢复数据需要检查。",
        std::move(details),
        true
    };
}

}
