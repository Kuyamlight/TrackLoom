#pragma once

#include "AppProjectSession.h"

#include <filesystem>
#include <string>
#include <vector>

namespace trackloom {

// AppProjectFileAction 描述用户触发的工程文件动作。
// 它只表达 UI 意图；真正的保存和打开仍由 AppProjectSession 执行。
enum class AppProjectFileAction {
    Open,
    Save,
    SaveAs
};

// AppProjectFileActionFeedbackKind 给 UI 提供稳定状态分类。
// message 可以显示给用户，但按钮启用、分支处理不应解析 message 文本。
enum class AppProjectFileActionFeedbackKind {
    Success,
    // 文件动作已经完成，但 UI 必须显示 warning。
    Warning,
    Failure,
    Canceled,
    NeedsSaveAs
};

struct AppProjectFileActionFeedback {
    bool success = false;
    AppProjectFileActionFeedbackKind kind = AppProjectFileActionFeedbackKind::Failure;
    std::string message;
    // 恢复位置保持结构化，避免 UI 从 message 文本反向解析路径。
    std::vector<std::filesystem::path> recoveryPaths;
};

// AppProjectFileActionPresentation 是 JUCE 无关的文件动作展示指令。
// summary 适合单行状态栏；details 保留完整 warning 正文，并把结构化恢复路径列为权威清单。
struct AppProjectFileActionPresentation {
    std::string summary;
    std::string details;
    bool showWarningDetails = false;
};

// withTrackLoomProjectExtension 在用户保存时补默认扩展名。
// 已显式输入扩展名时保持原样，避免替用户改写他们选择的测试文件或未来包格式。
std::filesystem::path withTrackLoomProjectExtension(std::filesystem::path path);

// describeAppProjectFileActionResult 把会话操作结果变成可显示文案。
// 这个函数不修改会话，只统一成功、失败和需要另存为的用户提示。
AppProjectFileActionFeedback describeAppProjectFileActionResult(
    AppProjectFileAction action,
    const AppProjectSessionResult& result);

// describeCanceledAppProjectFileAction 专门描述用户取消文件选择。
// 取消不是保存或打开失败，UI 不应把它显示成错误。
AppProjectFileActionFeedback describeCanceledAppProjectFileAction(AppProjectFileAction action);

// describeAppProjectFileActionPresentation 决定单行摘要和详情展示方式。
// 只有 Warning 会要求 UI 主动弹出详情，其他分类不会误触发警告窗口。
AppProjectFileActionPresentation describeAppProjectFileActionPresentation(
    const AppProjectFileActionFeedback& feedback);

}
