#include "ProjectFile.h"

#include "AtomicFileReplace.h"

#include <exception>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

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

bool removeSaveWorkspace(const std::filesystem::path& workspacePath, std::error_code& error)
{
    return std::filesystem::remove(workspacePath, error);
}

detail::TemporaryProjectWriteResult writeTemporaryProjectFile(
    const std::filesystem::path& temporaryPath,
    const std::string& canonicalText)
{
    std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!output.is_open() || !output) {
        return { false, "Could not create temporary project file." };
    }

    if (canonicalText.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        output.close();
        return { false, "Temporary project file is too large to write." };
    }

    output.write(canonicalText.data(), static_cast<std::streamsize>(canonicalText.size()));
    if (!output) {
        output.close();
        return { false, "Could not write temporary project file." };
    }

    output.flush();
    if (!output) {
        output.close();
        return { false, "Could not flush temporary project file." };
    }

    output.close();
    if (output.fail()) {
        return { false, "Could not close temporary project file." };
    }

    return { true, "" };
}

bool readTemporaryProjectBytes(
    const std::filesystem::path& temporaryPath,
    std::string& bytes,
    std::string& error)
{
    std::ifstream input(temporaryPath, std::ios::binary);
    if (!input.is_open() || !input) {
        error = "Could not reopen temporary project file for verification.";
        return false;
    }

    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad()) {
        input.close();
        error = "Could not read temporary project file for verification.";
        return false;
    }

    input.close();
    if (input.fail()) {
        error = "Could not close temporary project file after verification.";
        return false;
    }

    bytes = contents.str();
    return true;
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

void addRecoveryPath(
    std::vector<std::filesystem::path>& recoveryPaths,
    const std::filesystem::path& recoveryPath)
{
    for (const auto& existingPath : recoveryPaths) {
        if (existingPath == recoveryPath) {
            return;
        }
    }

    recoveryPaths.push_back(recoveryPath);
}

std::string cleanupWarningFor(const std::vector<std::filesystem::path>& recoveryPaths)
{
    auto warning = std::string("Project was saved, but cleanup left recovery data that must be inspected before the next save.");
    for (const auto& recoveryPath : recoveryPaths) {
        warning = withRecoveryInspectionPath(std::move(warning), recoveryPath);
    }
    return warning;
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

FileOperationResult FileOperationResult::ok(
    std::string warning,
    std::vector<std::filesystem::path> recoveryPaths)
{
    return { true, "", std::move(warning), std::move(recoveryPaths) };
}

FileOperationResult FileOperationResult::fail(std::string message)
{
    return { false, std::move(message), "", {} };
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

FileOperationResult detail::saveProjectToFileAtomicallyWithReplaceOperation(
    const Project& project,
    const std::filesystem::path& path,
    AtomicFileReplaceOperation replaceOperation)
{
    return detail::saveProjectToFileAtomicallyWithOperations(
        project,
        path,
        replaceOperation,
        removeSaveWorkspace);
}

FileOperationResult detail::saveProjectToFileAtomicallyWithOperations(
    const Project& project,
    const std::filesystem::path& path,
    AtomicFileReplaceOperation replaceOperation,
    SaveWorkspaceRemoveOperation removeWorkspace)
{
    return detail::saveProjectToFileAtomicallyWithOperations(
        project,
        path,
        writeTemporaryProjectFile,
        replaceOperation,
        removeWorkspace);
}

FileOperationResult detail::saveProjectToFileAtomicallyWithOperations(
    const Project& project,
    const std::filesystem::path& path,
    TemporaryProjectWriteOperation writeOperation,
    AtomicFileReplaceOperation replaceOperation,
    SaveWorkspaceRemoveOperation removeWorkspace)
{
    if (writeOperation == nullptr || replaceOperation == nullptr || removeWorkspace == nullptr) {
        return FileOperationResult::fail("Project file operation callbacks must not be null.");
    }

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
    const bool temporaryFileOwned = true;

    try {
        const auto canonicalText = saveProjectToText(project);
        const auto writeResult = writeOperation(temporaryPath, canonicalText);
        if (!writeResult.success) {
            const auto message = writeResult.error.empty()
                ? "Could not write temporary project file."
                : writeResult.error;
            return failBeforeReplacement(
                temporaryPath,
                workspacePath,
                temporaryFileOwned,
                message);
        }

        // 临时文件写完后立即用正式读取路径验证，防止写出当前读取器无法理解的内容。
        std::string temporaryBytes;
        std::string readError;
        if (!readTemporaryProjectBytes(temporaryPath, temporaryBytes, readError)) {
            return failBeforeReplacement(
                temporaryPath,
                workspacePath,
                temporaryFileOwned,
                std::move(readError));
        }

        if (temporaryBytes != canonicalText) {
            return failBeforeReplacement(
                temporaryPath,
                workspacePath,
                temporaryFileOwned,
                "Temporary project file bytes did not match the serialized project.");
        }

        const auto validation = loadProjectFromText(temporaryBytes);
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

    AtomicFileReplaceResult replacement;
    try {
        replacement = replaceOperation(temporaryPath, path);
    } catch (const std::exception& error) {
        return FileOperationResult::fail(withRecoveryInspectionPath(
            std::string("Could not replace project file: ") + error.what(),
            workspacePath));
    }

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

    std::vector<std::filesystem::path> recoveryPaths;
    if (replacement.recoveryPath.has_value()) {
        addRecoveryPath(recoveryPaths, *replacement.recoveryPath);
    }

    try {
        std::error_code workspaceCleanupError;
        const auto workspaceRemoved = removeWorkspace(workspacePath, workspaceCleanupError);
        auto workspaceNeedsRecovery = static_cast<bool>(workspaceCleanupError);
        if (!workspaceRemoved && !workspaceCleanupError) {
            std::error_code workspaceStatusError;
            const auto workspaceStillExists = std::filesystem::exists(workspacePath, workspaceStatusError);
            workspaceNeedsRecovery = workspaceStatusError || workspaceStillExists;
        }
        if (workspaceNeedsRecovery) {
            addRecoveryPath(recoveryPaths, workspacePath);
        }

        if (!recoveryPaths.empty()) {
            return FileOperationResult::ok(cleanupWarningFor(recoveryPaths), std::move(recoveryPaths));
        }

        return FileOperationResult::ok();
    } catch (const std::exception& error) {
        addRecoveryPath(recoveryPaths, workspacePath);
        auto warning = cleanupWarningFor(recoveryPaths);
        warning += " Cleanup error: ";
        warning += error.what();
        return FileOperationResult::ok(std::move(warning), std::move(recoveryPaths));
    }
}

FileOperationResult saveProjectToFileAtomically(const Project& project, const std::filesystem::path& path)
{
    return detail::saveProjectToFileAtomicallyWithReplaceOperation(project, path, replaceFileAtomically);
}

}
