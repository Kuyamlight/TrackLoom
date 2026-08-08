#pragma once

#include <filesystem>
#include <string>

namespace trackloom {

struct AtomicFileReplaceResult {
    bool success = false;
    std::string error;

    static AtomicFileReplaceResult ok();
    static AtomicFileReplaceResult fail(std::string message);
};

// replacementPath 和 targetPath 必须位于同一卷。
// 成功后 replacementPath 不再存在；失败时不会主动删除已有 targetPath。
AtomicFileReplaceResult replaceFileAtomically(
    const std::filesystem::path& replacementPath,
    const std::filesystem::path& targetPath);

}
