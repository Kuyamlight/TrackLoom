#include "ProjectFile.h"

#include "AtomicFileReplace.h"

#include <exception>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace trackloom {
namespace {

std::filesystem::path temporaryPathFor(const std::filesystem::path& targetPath)
{
    auto temporaryPath = targetPath;
    temporaryPath += ".tmp";
    return temporaryPath;
}

void removeIfExists(const std::filesystem::path& path)
{
    std::error_code ignoredError;
    std::filesystem::remove(path, ignoredError);
}

FileOperationResult failAfterCleanup(const std::filesystem::path& temporaryPath, std::string message)
{
    // 保存失败时清理临时文件，避免下次保存误读旧的中间结果。
    removeIfExists(temporaryPath);
    return FileOperationResult::fail(std::move(message));
}

std::string utf8PathForMessage(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return { reinterpret_cast<const char*>(utf8.data()), utf8.size() };
}

std::string withRecoveryPath(std::string message, const std::filesystem::path& recoveryPath)
{
    return std::move(message) + " Recovery file preserved at: " + utf8PathForMessage(recoveryPath);
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

    const auto temporaryPath = temporaryPathFor(path);

    std::error_code temporaryStatusError;
    const auto temporaryAlreadyExists = std::filesystem::exists(temporaryPath, temporaryStatusError);
    if (temporaryStatusError) {
        return FileOperationResult::fail(withRecoveryPath(
            "Could not inspect the temporary project file path.",
            temporaryPath));
    }
    if (temporaryAlreadyExists) {
        return FileOperationResult::fail(withRecoveryPath(
            "Temporary project file already exists; refusing to overwrite a possible recovery copy.",
            temporaryPath));
    }

    try {
        const auto parentPath = path.parent_path();
        if (!parentPath.empty()) {
            std::error_code createError;
            std::filesystem::create_directories(parentPath, createError);
            if (createError) {
                return failAfterCleanup(temporaryPath, "Could not create project directory.");
            }
        }

        {
            std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
            if (!output) {
                return failAfterCleanup(temporaryPath, "Could not create temporary project file.");
            }

            output << saveProjectToText(project);
            if (!output) {
                return failAfterCleanup(temporaryPath, "Could not write temporary project file.");
            }
        }

        // 临时文件写完后立即用正式读取路径验证，防止写出当前读取器无法理解的内容。
        const auto validation = loadProjectFromFile(temporaryPath);
        if (!validation.project.has_value()) {
            return failAfterCleanup(temporaryPath, "Temporary project file did not validate: " + validation.error);
        }
    } catch (const std::exception& error) {
        // 这里捕获异常是为了让 UI 或 AI 调用层得到普通错误结果，而不是让保存流程崩出核心库。
        return failAfterCleanup(temporaryPath, error.what());
    }

    try {
        const auto replacement = replaceFileAtomically(temporaryPath, path);
        if (!replacement.success) {
            auto message = "Could not replace project file: " + replacement.error;
            if (replacement.recoveryPath.has_value()) {
                message = withRecoveryPath(std::move(message), *replacement.recoveryPath);
            }
            return FileOperationResult::fail(std::move(message));
        }

        return FileOperationResult::ok();
    } catch (const std::exception& error) {
        // 一旦进入原子替换阶段，不再猜测临时文件是否可删除；它可能是唯一可控恢复副本。
        return FileOperationResult::fail(withRecoveryPath(
            std::string("Could not replace project file: ") + error.what(),
            temporaryPath));
    }
}

}
