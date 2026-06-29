#pragma once

#include "AppProjectSession.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace trackloom {

constexpr std::size_t defaultMaxAppRecentProjects = 8;

// AppRecentProjects 只管理桌面应用的“最近工程”偏好。
// 它不进入 Project 工程文件，避免把某台电脑的本地使用记录写进可分享工程。
class AppRecentProjects final {
public:
    explicit AppRecentProjects(std::size_t maxEntries = defaultMaxAppRecentProjects);

    std::size_t maxEntries() const;
    const std::vector<std::filesystem::path>& paths() const;

    // record 把路径放到列表最前面；已有路径会先移除再前置，保证列表不重复。
    void record(const std::filesystem::path& path);
    void clear();

private:
    std::size_t maxEntries_;
    std::vector<std::filesystem::path> paths_;
};

struct AppRecentProjectRow {
    std::size_t number = 0;
    std::filesystem::path path;
    std::string displayName;
    std::string fullPath;
    std::string summary;
};

struct AppRecentProjectsStatus {
    std::vector<AppRecentProjectRow> rows;
    std::string emptyMessage;
};

struct AppRecentProjectRecordResult {
    bool recorded = false;
    bool saved = false;
};

enum class AppRecentProjectOpenFeedbackKind {
    Success,
    MissingRecentProject,
    DirtyProject,
    OpenFailed
};

struct AppRecentProjectOpenFeedback {
    bool success = false;
    AppRecentProjectOpenFeedbackKind kind = AppRecentProjectOpenFeedbackKind::OpenFailed;
    std::filesystem::path path;
    bool savedRecentProjects = false;
    std::string message;
};

// describeAppRecentProjects 把本地最近工程列表转换成 UI 可直接展示的只读快照。
// UI、菜单和诊断面板不需要重复解释路径排序、编号和空列表提示。
AppRecentProjectsStatus describeAppRecentProjects(const AppRecentProjects& recentProjects);

// recordAndSaveAppRecentProject 先更新内存列表，再尝试写入本地设置文件。
// 设置保存失败不回滚内存状态，避免本地偏好问题影响当前工程操作。
AppRecentProjectRecordResult recordAndSaveAppRecentProject(
    AppRecentProjects& recentProjects,
    const std::filesystem::path& projectPath,
    const std::filesystem::path& settingsPath);

// openAppRecentProjectByNumber 是最近工程菜单、按钮和快捷键共用的打开入口。
// 编号使用用户可见的 1-based 行号；打开成功后才会提升最近工程顺序。
AppRecentProjectOpenFeedback openAppRecentProjectByNumber(
    AppProjectSession& session,
    AppRecentProjects& recentProjects,
    std::size_t number,
    const std::filesystem::path& settingsPath);

// 最近工程使用 UTF-8 文本逐行保存，便于人工检查，也方便后续迁移到正式设置格式。
bool saveAppRecentProjects(
    const AppRecentProjects& recentProjects,
    const std::filesystem::path& settingsPath);

// 缺失或无法读取设置文件时返回空列表，桌面壳不应因为本地偏好损坏而打不开。
AppRecentProjects loadAppRecentProjects(
    const std::filesystem::path& settingsPath,
    std::size_t maxEntries = defaultMaxAppRecentProjects);

}
