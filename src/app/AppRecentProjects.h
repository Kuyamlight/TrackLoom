#pragma once

#include <cstddef>
#include <filesystem>
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

// 最近工程使用 UTF-8 文本逐行保存，便于人工检查，也方便后续迁移到正式设置格式。
bool saveAppRecentProjects(
    const AppRecentProjects& recentProjects,
    const std::filesystem::path& settingsPath);

// 缺失或无法读取设置文件时返回空列表，桌面壳不应因为本地偏好损坏而打不开。
AppRecentProjects loadAppRecentProjects(
    const std::filesystem::path& settingsPath,
    std::size_t maxEntries = defaultMaxAppRecentProjects);

}
