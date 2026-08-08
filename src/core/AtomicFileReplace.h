#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace trackloom {

enum class AtomicFileTargetAvailability {
    Available,
    Missing,
    Unknown
};

struct AtomicFileReplaceResult {
    bool success = false;
    std::string error;
    // 失败时明确 target 当前是否可用；Unknown 表示状态查询本身失败。
    AtomicFileTargetAvailability targetAvailability = AtomicFileTargetAvailability::Unknown;
    // 失败时指向应保留的恢复副本；成功但 backup 清理失败时也会返回遗留 backup。
    std::optional<std::filesystem::path> recoveryPath;

    static AtomicFileReplaceResult ok(std::optional<std::filesystem::path> preservedBackup = std::nullopt);
    static AtomicFileReplaceResult fail(
        std::string message,
        AtomicFileTargetAvailability targetAvailability,
        std::optional<std::filesystem::path> recoveryPath = std::nullopt);
};

// replacementPath 和 targetPath 必须位于同一卷。
// 成功后 replacementPath 不再存在；失败时不会主动删除已有 targetPath。
AtomicFileReplaceResult replaceFileAtomically(
    const std::filesystem::path& replacementPath,
    const std::filesystem::path& targetPath);

namespace detail {

inline constexpr std::uint32_t windowsErrorSuccess = 0;
inline constexpr std::uint32_t windowsErrorFileNotFound = 2;
inline constexpr std::uint32_t windowsErrorPathNotFound = 3;
inline constexpr std::uint32_t windowsErrorAccessDenied = 5;
inline constexpr std::uint32_t windowsErrorFileExists = 80;
inline constexpr std::uint32_t windowsErrorAlreadyExists = 183;
inline constexpr std::uint32_t windowsErrorUnableToRemoveReplaced = 1175;
inline constexpr std::uint32_t windowsErrorUnableToMoveReplacement = 1176;
inline constexpr std::uint32_t windowsErrorUnableToMoveReplacement2 = 1177;
inline constexpr std::uint32_t windowsMoveFileWriteThrough = 0x00000008;

struct WindowsPathQueryResult {
    bool success = false;
    bool exists = false;
    std::uint32_t error = windowsErrorSuccess;
};

struct WindowsFileOperationResult {
    bool success = false;
    std::uint32_t error = windowsErrorSuccess;
};

class WindowsAtomicFileOperations {
public:
    virtual ~WindowsAtomicFileOperations() = default;

    virtual WindowsPathQueryResult queryPath(const std::filesystem::path& path) = 0;
    virtual WindowsFileOperationResult replaceFile(
        const std::filesystem::path& targetPath,
        const std::filesystem::path& replacementPath,
        const std::filesystem::path& backupPath,
        std::uint32_t flags) = 0;
    virtual WindowsFileOperationResult moveFile(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& targetPath,
        std::uint32_t flags) = 0;
    virtual WindowsFileOperationResult removeFile(const std::filesystem::path& path) = 0;
};

AtomicFileReplaceResult replaceFileAtomicallyWithWindowsOperations(
    const std::filesystem::path& replacementPath,
    const std::filesystem::path& targetPath,
    WindowsAtomicFileOperations& operations);

}

}
