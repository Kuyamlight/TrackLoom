#include "AppRecentProjects.h"

#include <algorithm>
#include <fstream>
#include <string>

namespace trackloom {
namespace {

std::string pathToUtf8Line(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(
        reinterpret_cast<const char*>(utf8.data()),
        utf8.size());
}

std::filesystem::path pathFromUtf8Line(const std::string& line)
{
    std::u8string utf8;
    utf8.reserve(line.size());
    for (const unsigned char byte : line) {
        utf8.push_back(static_cast<char8_t>(byte));
    }

    return std::filesystem::path(utf8);
}

}

AppRecentProjects::AppRecentProjects(std::size_t maxEntries)
    : maxEntries_(maxEntries)
{
}

std::size_t AppRecentProjects::maxEntries() const
{
    return maxEntries_;
}

const std::vector<std::filesystem::path>& AppRecentProjects::paths() const
{
    return paths_;
}

void AppRecentProjects::record(const std::filesystem::path& path)
{
    if (maxEntries_ == 0 || path.empty()) {
        return;
    }

    const auto normalizedPath = path.lexically_normal();
    paths_.erase(
        std::remove(paths_.begin(), paths_.end(), normalizedPath),
        paths_.end());
    paths_.insert(paths_.begin(), normalizedPath);

    if (paths_.size() > maxEntries_) {
        paths_.resize(maxEntries_);
    }
}

void AppRecentProjects::clear()
{
    paths_.clear();
}

AppRecentProjectsStatus describeAppRecentProjects(const AppRecentProjects& recentProjects)
{
    AppRecentProjectsStatus status;
    status.emptyMessage = "暂无最近工程。打开或保存工程后，这里会显示最近使用的工程文件。";

    std::size_t number = 1;
    for (const auto& path : recentProjects.paths()) {
        AppRecentProjectRow row;
        row.number = number;
        row.path = path;
        row.displayName = path.filename().string();
        row.fullPath = path.string();
        row.summary = std::to_string(number) + ". " + row.displayName + " - " + row.fullPath;
        status.rows.push_back(std::move(row));
        ++number;
    }

    return status;
}

AppRecentProjectRecordResult recordAndSaveAppRecentProject(
    AppRecentProjects& recentProjects,
    const std::filesystem::path& projectPath,
    const std::filesystem::path& settingsPath)
{
    if (projectPath.empty()) {
        return {};
    }

    recentProjects.record(projectPath);
    return {
        true,
        saveAppRecentProjects(recentProjects, settingsPath)
    };
}

bool saveAppRecentProjects(
    const AppRecentProjects& recentProjects,
    const std::filesystem::path& settingsPath)
{
    if (settingsPath.empty()) {
        return false;
    }

    const auto parent = settingsPath.parent_path();
    if (!parent.empty()) {
        std::error_code createDirectoryError;
        std::filesystem::create_directories(parent, createDirectoryError);
        if (createDirectoryError) {
            return false;
        }
    }

    std::ofstream output(settingsPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }

    for (const auto& path : recentProjects.paths()) {
        output << pathToUtf8Line(path) << '\n';
    }

    return output.good();
}

AppRecentProjects loadAppRecentProjects(
    const std::filesystem::path& settingsPath,
    std::size_t maxEntries)
{
    AppRecentProjects recentProjects(maxEntries);
    if (settingsPath.empty()) {
        return recentProjects;
    }

    std::error_code existsError;
    const auto settingsFileExists = std::filesystem::exists(settingsPath, existsError);
    if (existsError || !settingsFileExists) {
        return recentProjects;
    }

    std::ifstream input(settingsPath, std::ios::binary);
    if (!input) {
        return recentProjects;
    }

    std::vector<std::filesystem::path> storedPaths;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) {
            storedPaths.push_back(pathFromUtf8Line(line));
        }
    }

    // 文件按“最新在前”保存；反向 record 可以复用去重和限长规则，同时保留首个出现项。
    for (auto it = storedPaths.rbegin(); it != storedPaths.rend(); ++it) {
        recentProjects.record(*it);
    }

    return recentProjects;
}

}
