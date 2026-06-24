#pragma once

#include "Project.h"

#include <optional>
#include <string>

namespace trackloom {

// LoadProjectResult 避免读取失败时返回半初始化 Project。
// 调用方必须先检查 project 是否有值，再使用读取结果。
struct LoadProjectResult {
    std::optional<Project> project;
    std::string error;

    static LoadProjectResult ok(Project project);
    static LoadProjectResult fail(std::string message);
};

std::string saveProjectToText(const Project& project);
LoadProjectResult loadProjectFromText(const std::string& text);

}
