#include "ProjectFile.h"

#include "AtomicFileReplace.h"

#include <exception>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace trackloom {
namespace {

std::filesystem::path legacyTemporaryPathFor(const std::filesystem::path& targetPath)
{
    auto temporaryPath = targetPath;
    temporaryPath += ".tmp";
    return temporaryPath;
}

std::filesystem::path saveWorkspacePathFor(const std::filesystem::path& targetPath)
{
    auto workspacePath = targetPath;
    workspacePath += ".trackloom-save-workspace";
    return workspacePath;
}

bool cleanupOwnedSaveWorkspace(
    const std::filesystem::path& temporaryPath,
    const std::filesystem::path& workspacePath,
    bool temporaryFileOwned)
{
    bool cleanupSucceeded = true;
    if (temporaryFileOwned) {
        std::error_code removeTemporaryError;
        std::filesystem::remove(temporaryPath, removeTemporaryError);
        cleanupSucceeded = !removeTemporaryError;
    }

    // 只删除本调用原子取得的空目录；不递归删除，也不触碰竞争者或崩溃遗留内容。
    std::error_code removeWorkspaceError;
    std::filesystem::remove(workspacePath, removeWorkspaceError);
    return cleanupSucceeded && !removeWorkspaceError;
}

std::string utf8PathForMessage(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return { reinterpret_cast<const char*>(utf8.data()), utf8.size() };
}

std::string withRecoveryInspectionPath(std::string message, const std::filesystem::path& recoveryPath)
{
    return std::move(message) + " Recovery data may be available at: " + utf8PathForMessage(recoveryPath)
        + ". This path must be inspected before retrying.";
}

FileOperationResult failBeforeReplacement(
    const std::filesystem::path& temporaryPath,
    const std::filesystem::path& workspacePath,
    bool temporaryFileOwned,
    std::string message)
{
    if (!cleanupOwnedSaveWorkspace(temporaryPath, workspacePath, temporaryFileOwned)) {
        message = withRecoveryInspectionPath(std::move(message), workspacePath);
    }
    return FileOperationResult::fail(std::move(message));
}

}

FileOperationResult FileOperationResult::ok()
{
    return { true, "" };
}

FileOperationResult FileOperationResult::fail(std::string message)
{
    return { false, std::move(message) };
}

LoadProjectResult loadProjectFromFile(const std::filesystem::path& path)
{
    if (path.empty()) {
        return LoadProjectResult::fail("Project file path must not be empty.");
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return LoadProjectResult::fail("Could not open project file.");
    }

    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad()) {
        return LoadProjectResult::fail("Could not read project file.");
    }

    return loadProjectFromText(contents.str());
}

FileOperationResult saveProjectToFileAtomically(const Project& project, const std::filesystem::path& path)
{
    if (path.empty()) {
        return FileOperationResult::fail("Project file path must not be empty.");
    }

    const auto parentPath = path.parent_path();
    if (!parentPath.empty()) {
        std::error_code createError;
        std::filesystem::create_directories(parentPath, createError);
        if (createError) {
            return FileOperationResult::fail("Could not create project directory.");
        }
    }

    // 旧版本曾把恢复副本写在固定 .tmp 路径。只检查并报告，永不截断或删除它。
    const auto legacyTemporaryPath = legacyTemporaryPathFor(path);
    std::error_code legacyStatusError;
    const auto legacyTemporaryExists = std::filesystem::exists(legacyTemporaryPath, legacyStatusError);
    if (legacyStatusError) {
        return FileOperationResult::fail(withRecoveryInspectionPath(
            "Could not inspect the legacy temporary project file path.",
            legacyTemporaryPath));
    }
    if (legacyTemporaryExists) {
        return FileOperationResult::fail(withRecoveryInspectionPath(
            "Legacy temporary project file already exists; refusing to overwrite a possible recovery copy.",
            legacyTemporaryPath));
    }

    const auto workspacePath = saveWorkspacePathFor(path);
    std::error_code createWorkspaceError;
    const auto workspaceCreated = std::filesystem::create_directory(workspacePath, createWorkspaceError);
    if (!workspaceCreated) {
        auto message = createWorkspaceError
            ? "Could not create the private project save workspace."
            : "Project save workspace already exists; refusing to touch possible recovery data.";
        return FileOperationResult::fail(withRecoveryInspectionPath(std::move(message), workspacePath));
    }

    const auto temporaryPath = workspacePath / "replacement.tlproj";
    bool temporaryFileOwned = false;

    try {
        {
            std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
            temporaryFileOwned = output.is_open();
            if (!output) {
                return failBeforeReplacement(
                    temporaryPath,
                    workspacePath,
                    temporaryFileOwned,
                    "Could not create temporary project file.");
            }

            output << saveProjectToText(project);
            if (!output) {
                return failBeforeReplacement(
                    temporaryPath,
                    workspacePath,
                    temporaryFileOwned,
                    "Could not write temporary project file.");
            }
        }

        // 临时文件写完后立即用正式读取路径验证，防止写出当前读取器无法理解的内容。
        const auto validation = loadProjectFromFile(temporaryPath);
        if (!validation.project.has_value()) {
            return failBeforeReplacement(
                temporaryPath,
                workspacePath,
                temporaryFileOwned,
                "Temporary project file did not validate: " + validation.error);
        }
    } catch (const std::exception& error) {
        // 这里捕获异常是为了让 UI 或 AI 调用层得到普通错误结果，而不是让保存流程崩出核心库。
        return failBeforeReplacement(temporaryPath, workspacePath, temporaryFileOwned, error.what());
    }

    try {
        const auto replacement = replaceFileAtomically(temporaryPath, path);
        if (!replacement.success) {
            auto message = "Could not replace project file: " + replacement.error;
            if (replacement.recoveryPath.has_value()) {
                message = withRecoveryInspectionPath(std::move(message), *replacement.recoveryPath);
            }
            if (!replacement.recoveryPath.has_value() || *replacement.recoveryPath != workspacePath) {
                message = withRecoveryInspectionPath(std::move(message), workspacePath);
            }
            return FileOperationResult::fail(std::move(message));
        }

        std::error_code ignoredWorkspaceCleanupError;
        std::filesystem::remove(workspacePath, ignoredWorkspaceCleanupError);
        return FileOperationResult::ok();
    } catch (const std::exception& error) {
        // 一旦进入原子替换阶段，不再清理 workspace；它可能包含唯一可控恢复副本。
        return FileOperationResult::fail(withRecoveryInspectionPath(
            std::string("Could not replace project file: ") + error.what(),
            workspacePath));
    }
}

}
