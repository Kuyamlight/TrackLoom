#pragma once

#include "AtomicFileReplace.h"
#include "Project.h"
#include "ProjectSerializer.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace trackloom {

// FileOperationResult 用于报告文件写入是否成功。
// 文件系统错误经常来自权限、路径或磁盘状态，调用方需要拿到可显示的原因。
struct FileOperationResult {
    bool success = false;
    std::string error;
    // 保存已成功但清理留下恢复数据时的非致命提示。
    std::string warning;
    // 调用方在下次保存前必须检查的恢复位置。
    std::vector<std::filesystem::path> recoveryPaths;

    static FileOperationResult ok(
        std::string warning = {},
        std::vector<std::filesystem::path> recoveryPaths = {});
    static FileOperationResult fail(std::string message);
};

// 保存工程时先写临时文件，验证后再替换目标文件。
// 这样可以降低“目标文件被写到一半”的风险，但它不是完整备份系统。
FileOperationResult saveProjectToFileAtomically(const Project& project, const std::filesystem::path& path);

namespace detail {

// 测试保存流程的替换后处理边界；公开生产入口始终使用 replaceFileAtomically。
using AtomicFileReplaceOperation = AtomicFileReplaceResult (*) (
    const std::filesystem::path& replacementPath,
    const std::filesystem::path& targetPath);

using SaveWorkspaceRemoveOperation = bool (*) (
    const std::filesystem::path& workspacePath,
    std::error_code& error);

FileOperationResult saveProjectToFileAtomicallyWithReplaceOperation(
    const Project& project,
    const std::filesystem::path& path,
    AtomicFileReplaceOperation replaceOperation);

FileOperationResult saveProjectToFileAtomicallyWithOperations(
    const Project& project,
    const std::filesystem::path& path,
    AtomicFileReplaceOperation replaceOperation,
    SaveWorkspaceRemoveOperation removeWorkspace);

}

// 从文件读取工程，失败时沿用 LoadProjectResult 返回错误，不产生半初始化 Project。
LoadProjectResult loadProjectFromFile(const std::filesystem::path& path);

}
