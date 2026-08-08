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

#ifdef _WIN32

constexpr int maximumRaceRetries = 3;

bool isMissingPathError(DWORD error)
{
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

bool isExistingPathError(DWORD error)
{
    return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS;
}

std::string windowsErrorMessage(const char* operation, DWORD error)
{
    const std::error_code errorCode(static_cast<int>(error), std::system_category());
    return std::string(operation) + " failed (Windows error " + std::to_string(error) + "): " + errorCode.message();
}

struct PathExistenceResult {
    bool querySucceeded = false;
    bool exists = false;
    DWORD error = ERROR_SUCCESS;
};

PathExistenceResult queryPathExistence(const std::filesystem::path& path)
{
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return { true, true, ERROR_SUCCESS };
    }

    const auto error = GetLastError();
    if (isMissingPathError(error)) {
        return { true, false, error };
    }

    return { false, false, error };
}

#endif

}

AtomicFileReplaceResult AtomicFileReplaceResult::ok()
{
    return { true, "" };
}

AtomicFileReplaceResult AtomicFileReplaceResult::fail(std::string message)
{
    return { false, std::move(message) };
}

AtomicFileReplaceResult replaceFileAtomically(
    const std::filesystem::path& replacementPath,
    const std::filesystem::path& targetPath)
{
#ifdef _WIN32
    const auto replacementExistence = queryPathExistence(replacementPath);
    if (!replacementExistence.querySucceeded) {
        return AtomicFileReplaceResult::fail(
            windowsErrorMessage("GetFileAttributesW for replacement", replacementExistence.error));
    }
    if (!replacementExistence.exists) {
        return AtomicFileReplaceResult::fail("Replacement file does not exist.");
    }

    for (int attempt = 0; attempt < maximumRaceRetries; ++attempt) {
        const auto targetExistence = queryPathExistence(targetPath);
        if (!targetExistence.querySucceeded) {
            return AtomicFileReplaceResult::fail(
                windowsErrorMessage("GetFileAttributesW for target", targetExistence.error));
        }

        if (targetExistence.exists) {
            if (ReplaceFileW(
                    targetPath.c_str(),
                    replacementPath.c_str(),
                    nullptr,
                    REPLACEFILE_WRITE_THROUGH,
                    nullptr,
                    nullptr)) {
                return AtomicFileReplaceResult::ok();
            }

            const auto error = GetLastError();
            if (isMissingPathError(error) && attempt + 1 < maximumRaceRetries) {
                const auto replacementAfterFailure = queryPathExistence(replacementPath);
                if (!replacementAfterFailure.querySucceeded) {
                    return AtomicFileReplaceResult::fail(
                        windowsErrorMessage("GetFileAttributesW for replacement", replacementAfterFailure.error));
                }
                if (!replacementAfterFailure.exists) {
                    return AtomicFileReplaceResult::fail(
                        windowsErrorMessage("ReplaceFileW", error));
                }
                continue;
            }

            return AtomicFileReplaceResult::fail(windowsErrorMessage("ReplaceFileW", error));
        }

        if (MoveFileExW(
                replacementPath.c_str(),
                targetPath.c_str(),
                MOVEFILE_WRITE_THROUGH)) {
            return AtomicFileReplaceResult::ok();
        }

        const auto error = GetLastError();
        if (isExistingPathError(error) && attempt + 1 < maximumRaceRetries) {
            continue;
        }

        return AtomicFileReplaceResult::fail(windowsErrorMessage("MoveFileExW", error));
    }

    return AtomicFileReplaceResult::fail("Target path changed repeatedly during atomic replacement.");
#else
    std::error_code replacementStatusError;
    if (!std::filesystem::exists(replacementPath, replacementStatusError)) {
        if (replacementStatusError) {
            return AtomicFileReplaceResult::fail(
                "Could not query replacement path: " + replacementStatusError.message());
        }
        return AtomicFileReplaceResult::fail("Replacement file does not exist.");
    }

    std::error_code renameError;
    std::filesystem::rename(replacementPath, targetPath, renameError);
    if (renameError) {
        return AtomicFileReplaceResult::fail("Could not rename replacement file: " + renameError.message());
    }

    return AtomicFileReplaceResult::ok();
#endif
}

}
