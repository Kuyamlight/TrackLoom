#pragma once

#include "Project.h"
#include "ProjectSerializer.h"

#include <filesystem>
#include <string>

namespace trackloom {

// FileOperationResult 用于报告文件写入是否成功。
// 文件系统错误经常来自权限、路径或磁盘状态，调用方需要拿到可显示的原因。
struct FileOperationResult {
    bool success = false;
    std::string error;

    static FileOperationResult ok();
    static FileOperationResult fail(std::string message);
};

// 保存工程时先写临时文件，验证后再替换目标文件。
// 这样可以降低“目标文件被写到一半”的风险，但它不是完整备份系统。
FileOperationResult saveProjectToFileAtomically(const Project& project, const std::filesystem::path& path);

// 从文件读取工程，失败时沿用 LoadProjectResult 返回错误，不产生半初始化 Project。
LoadProjectResult loadProjectFromFile(const std::filesystem::path& path);

}
