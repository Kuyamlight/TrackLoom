#include "AtomicFileReplace.h"

#include <system_error>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace trackloom {
namespace {

bool isMissingPathError(std::uint32_t error)
{
    return error == detail::windowsErrorFileNotFound || error == detail::windowsErrorPathNotFound;
}

std::string windowsErrorMessage(const char* operation, std::uint32_t error)
{
#ifdef _WIN32
    const std::error_code errorCode(static_cast<int>(error), std::system_category());
    return std::string(operation) + " failed (Windows error " + std::to_string(error) + "): " + errorCode.message();
#else
    return std::string(operation) + " failed (Windows error " + std::to_string(error) + ").";
#endif
}

AtomicFileTargetAvailability availabilityFromQuery(const detail::WindowsPathQueryResult& query)
{
    if (!query.success) {
        return AtomicFileTargetAvailability::Unknown;
    }
    return query.exists ? AtomicFileTargetAvailability::Available : AtomicFileTargetAvailability::Missing;
}

std::optional<std::filesystem::path> recoveryPathFromQueries(
    const std::filesystem::path& replacementPath,
    const detail::WindowsPathQueryResult& replacementQuery,
    const std::filesystem::path& backupPath,
    const detail::WindowsPathQueryResult& backupQuery,
    const std::filesystem::path& targetPath,
    const detail::WindowsPathQueryResult& targetQuery)
{
    if (!replacementQuery.success || replacementQuery.exists) {
        return replacementPath;
    }
    if (!backupQuery.success || backupQuery.exists) {
        return backupPath;
    }
    if (targetQuery.success && targetQuery.exists) {
        return targetPath;
    }
    return std::nullopt;
}

std::filesystem::path recoveryDirectoryFor(const std::filesystem::path& targetPath)
{
    auto directory = targetPath;
    directory += ".trackloom-recovery";
    return directory;
}

std::string appendCleanupDiagnostic(
    std::string message,
    const char* operation,
    const detail::WindowsFileOperationResult& cleanup)
{
    if (!cleanup.success) {
        message += " ";
        message += windowsErrorMessage(operation, cleanup.error);
    }
    return message;
}

AtomicFileReplaceResult successfulReplacementAfterBackupCleanup(
    const std::filesystem::path& backupPath,
    const std::filesystem::path& recoveryDirectory,
    detail::WindowsAtomicFileOperations& operations)
{
    const auto backupCleanup = operations.removeFile(backupPath);
    if (!backupCleanup.success) {
        return AtomicFileReplaceResult::ok(backupPath);
    }

    const auto directoryCleanup = operations.removeDirectory(recoveryDirectory);
    if (!directoryCleanup.success) {
        return AtomicFileReplaceResult::ok(recoveryDirectory);
    }
    return AtomicFileReplaceResult::ok();
}

#ifdef _WIN32

class NativeWindowsAtomicFileOperations final : public detail::WindowsAtomicFileOperations {
public:
    detail::WindowsPathQueryResult queryPath(const std::filesystem::path& path) override
    {
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return { true, true, detail::windowsErrorSuccess };
        }

        const auto error = static_cast<std::uint32_t>(GetLastError());
        if (isMissingPathError(error)) {
            return { true, false, error };
        }
        return { false, false, error };
    }

    detail::WindowsFileOperationResult replaceFile(
        const std::filesystem::path& targetPath,
        const std::filesystem::path& replacementPath,
        const std::filesystem::path& backupPath,
        std::uint32_t flags) override
    {
        if (ReplaceFileW(
                targetPath.c_str(),
                replacementPath.c_str(),
                backupPath.c_str(),
                static_cast<DWORD>(flags),
                nullptr,
                nullptr)) {
            return { true, detail::windowsErrorSuccess };
        }
        return { false, static_cast<std::uint32_t>(GetLastError()) };
    }

    detail::WindowsFileOperationResult moveFile(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& targetPath,
        std::uint32_t flags) override
    {
        if (MoveFileExW(sourcePath.c_str(), targetPath.c_str(), static_cast<DWORD>(flags))) {
            return { true, detail::windowsErrorSuccess };
        }
        return { false, static_cast<std::uint32_t>(GetLastError()) };
    }

    detail::WindowsFileOperationResult removeFile(const std::filesystem::path& path) override
    {
        if (DeleteFileW(path.c_str())) {
            return { true, detail::windowsErrorSuccess };
        }
        return { false, static_cast<std::uint32_t>(GetLastError()) };
    }

    detail::WindowsFileOperationResult createDirectory(const std::filesystem::path& path) override
    {
        if (CreateDirectoryW(path.c_str(), nullptr)) {
            return { true, detail::windowsErrorSuccess };
        }
        return { false, static_cast<std::uint32_t>(GetLastError()) };
    }

    detail::WindowsFileOperationResult removeDirectory(const std::filesystem::path& path) override
    {
        if (RemoveDirectoryW(path.c_str())) {
            return { true, detail::windowsErrorSuccess };
        }
        return { false, static_cast<std::uint32_t>(GetLastError()) };
    }
};

static_assert(detail::windowsErrorFileNotFound == ERROR_FILE_NOT_FOUND);
static_assert(detail::windowsErrorPathNotFound == ERROR_PATH_NOT_FOUND);
static_assert(detail::windowsErrorAccessDenied == ERROR_ACCESS_DENIED);
static_assert(detail::windowsErrorFileExists == ERROR_FILE_EXISTS);
static_assert(detail::windowsErrorAlreadyExists == ERROR_ALREADY_EXISTS);
static_assert(detail::windowsErrorUnableToRemoveReplaced == ERROR_UNABLE_TO_REMOVE_REPLACED);
static_assert(detail::windowsErrorUnableToMoveReplacement == ERROR_UNABLE_TO_MOVE_REPLACEMENT);
static_assert(detail::windowsErrorUnableToMoveReplacement2 == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2);
static_assert(detail::windowsMoveFileWriteThrough == MOVEFILE_WRITE_THROUGH);

#endif

AtomicFileTargetAvailability queryFilesystemTargetAvailability(const std::filesystem::path& targetPath)
{
    std::error_code statusError;
    const auto exists = std::filesystem::exists(targetPath, statusError);
    if (statusError) {
        return AtomicFileTargetAvailability::Unknown;
    }
    return exists ? AtomicFileTargetAvailability::Available : AtomicFileTargetAvailability::Missing;
}

}

AtomicFileReplaceResult AtomicFileReplaceResult::ok(std::optional<std::filesystem::path> recoveryPath)
{
    return {
        true,
        "",
        AtomicFileTargetAvailability::Available,
        std::move(recoveryPath)
    };
}

AtomicFileReplaceResult AtomicFileReplaceResult::fail(
    std::string message,
    AtomicFileTargetAvailability targetAvailability,
    std::optional<std::filesystem::path> recoveryPath)
{
    return {
        false,
        std::move(message),
        targetAvailability,
        std::move(recoveryPath)
    };
}

namespace detail {

AtomicFileReplaceResult replaceFileAtomicallyWithWindowsOperations(
    const std::filesystem::path& replacementPath,
    const std::filesystem::path& targetPath,
    WindowsAtomicFileOperations& operations)
{
    const auto replacementQuery = operations.queryPath(replacementPath);
    if (!replacementQuery.success) {
        return AtomicFileReplaceResult::fail(
            windowsErrorMessage("Replacement path query", replacementQuery.error),
            AtomicFileTargetAvailability::Unknown,
            replacementPath);
    }

    if (!replacementQuery.exists) {
        const auto targetQuery = operations.queryPath(targetPath);
        return AtomicFileReplaceResult::fail(
            "Replacement file does not exist.",
            availabilityFromQuery(targetQuery));
    }

    const auto recoveryDirectory = recoveryDirectoryFor(targetPath);
    const auto createRecoveryDirectory = operations.createDirectory(recoveryDirectory);
    if (!createRecoveryDirectory.success) {
        return AtomicFileReplaceResult::fail(
            windowsErrorMessage("CreateDirectoryW recovery directory", createRecoveryDirectory.error),
            AtomicFileTargetAvailability::Unknown,
            recoveryDirectory);
    }

    const auto backupPath = recoveryDirectory / "original-target";
    const auto targetQuery = operations.queryPath(targetPath);
    if (!targetQuery.success) {
        const auto cleanup = operations.removeDirectory(recoveryDirectory);
        return AtomicFileReplaceResult::fail(
            appendCleanupDiagnostic(
                windowsErrorMessage("Target path query", targetQuery.error),
                "RemoveDirectoryW recovery directory",
                cleanup),
            AtomicFileTargetAvailability::Unknown,
            replacementPath);
    }

    if (!targetQuery.exists) {
        const auto move = operations.moveFile(replacementPath, targetPath, windowsMoveFileWriteThrough);
        if (move.success) {
            const auto cleanup = operations.removeDirectory(recoveryDirectory);
            if (!cleanup.success) {
                return AtomicFileReplaceResult::ok(recoveryDirectory);
            }
            return AtomicFileReplaceResult::ok();
        }

        const auto targetAfterMove = operations.queryPath(targetPath);
        const auto replacementAfterMove = operations.queryPath(replacementPath);
        const auto cleanup = operations.removeDirectory(recoveryDirectory);
        std::optional<std::filesystem::path> recoveryPath;
        if (!replacementAfterMove.success || replacementAfterMove.exists) {
            recoveryPath = replacementPath;
        } else if (targetAfterMove.success && targetAfterMove.exists) {
            recoveryPath = targetPath;
        }
        return AtomicFileReplaceResult::fail(
            appendCleanupDiagnostic(
                windowsErrorMessage("MoveFileExW installing replacement", move.error),
                "RemoveDirectoryW recovery directory",
                cleanup),
            availabilityFromQuery(targetAfterMove),
            std::move(recoveryPath));
    }

    const auto replace = operations.replaceFile(targetPath, replacementPath, backupPath, 0);
    if (replace.success) {
        return successfulReplacementAfterBackupCleanup(backupPath, recoveryDirectory, operations);
    }

    if (replace.error == windowsErrorUnableToRemoveReplaced
        || replace.error == windowsErrorUnableToMoveReplacement) {
        const auto cleanup = operations.removeDirectory(recoveryDirectory);
        return AtomicFileReplaceResult::fail(
            appendCleanupDiagnostic(
                windowsErrorMessage("ReplaceFileW", replace.error),
                "RemoveDirectoryW recovery directory",
                cleanup),
            AtomicFileTargetAvailability::Available,
            replacementPath);
    }

    if (replace.error != windowsErrorUnableToMoveReplacement2) {
        const auto targetAfterReplace = operations.queryPath(targetPath);
        const auto replacementAfterReplace = operations.queryPath(replacementPath);
        const auto backupAfterReplace = operations.queryPath(backupPath);
        auto message = windowsErrorMessage("ReplaceFileW", replace.error);
        if (backupAfterReplace.success && !backupAfterReplace.exists) {
            message = appendCleanupDiagnostic(
                std::move(message),
                "RemoveDirectoryW recovery directory",
                operations.removeDirectory(recoveryDirectory));
        }
        return AtomicFileReplaceResult::fail(
            std::move(message),
            availabilityFromQuery(targetAfterReplace),
            recoveryPathFromQueries(
                replacementPath,
                replacementAfterReplace,
                backupPath,
                backupAfterReplace,
                targetPath,
                targetAfterReplace));
    }

    const auto install = operations.moveFile(replacementPath, targetPath, windowsMoveFileWriteThrough);
    if (install.success) {
        return successfulReplacementAfterBackupCleanup(backupPath, recoveryDirectory, operations);
    }

    const auto restore = operations.moveFile(backupPath, targetPath, windowsMoveFileWriteThrough);
    if (restore.success) {
        const auto replacementAfterRestore = operations.queryPath(replacementPath);
        const auto cleanup = operations.removeDirectory(recoveryDirectory);
        return AtomicFileReplaceResult::fail(
            appendCleanupDiagnostic(
                windowsErrorMessage("MoveFileExW installing replacement after ReplaceFileW error 1177", install.error)
                    + " The original target was restored.",
                "RemoveDirectoryW recovery directory",
                cleanup),
            AtomicFileTargetAvailability::Available,
            (!replacementAfterRestore.success || replacementAfterRestore.exists)
                ? std::optional<std::filesystem::path>(replacementPath)
                : std::optional<std::filesystem::path>(targetPath));
    }

    const auto targetAfterRecovery = operations.queryPath(targetPath);
    const auto replacementAfterRecovery = operations.queryPath(replacementPath);
    const auto backupAfterRecovery = operations.queryPath(backupPath);
    return AtomicFileReplaceResult::fail(
        windowsErrorMessage("MoveFileExW installing replacement after ReplaceFileW error 1177", install.error)
            + " "
            + windowsErrorMessage("MoveFileExW restoring backup", restore.error),
        availabilityFromQuery(targetAfterRecovery),
        recoveryPathFromQueries(
            backupPath,
            backupAfterRecovery,
            replacementPath,
            replacementAfterRecovery,
            targetPath,
            targetAfterRecovery));
}

}

AtomicFileReplaceResult replaceFileAtomically(
    const std::filesystem::path& replacementPath,
    const std::filesystem::path& targetPath)
{
#ifdef _WIN32
    NativeWindowsAtomicFileOperations operations;
    return detail::replaceFileAtomicallyWithWindowsOperations(replacementPath, targetPath, operations);
#else
    std::error_code replacementStatusError;
    if (!std::filesystem::exists(replacementPath, replacementStatusError)) {
        if (replacementStatusError) {
            return AtomicFileReplaceResult::fail(
                "Could not query replacement path: " + replacementStatusError.message(),
                queryFilesystemTargetAvailability(targetPath),
                replacementPath);
        }
        return AtomicFileReplaceResult::fail(
            "Replacement file does not exist.",
            queryFilesystemTargetAvailability(targetPath));
    }

    std::error_code renameError;
    std::filesystem::rename(replacementPath, targetPath, renameError);
    if (renameError) {
        return AtomicFileReplaceResult::fail(
            "Could not rename replacement file: " + renameError.message(),
            queryFilesystemTargetAvailability(targetPath),
            replacementPath);
    }

    return AtomicFileReplaceResult::ok();
#endif
}

}
