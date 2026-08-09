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

AppProjectFileActionFeedback feedback(
    bool success,
    AppProjectFileActionFeedbackKind kind,
    std::string message)
{
    return { success, kind, std::move(message) };
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
        if (!result.warning.empty()) {
            return feedback(
                true,
                AppProjectFileActionFeedbackKind::Warning,
                actionSuccessMessage(action) + " " + result.warning);
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

}
