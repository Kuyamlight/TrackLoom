#pragma once

#include "AppCommandShortcuts.h"

#include <filesystem>
#include <vector>

namespace trackloom {

// saveAppShortcutCustomBindings 只保存用户覆盖项，不保存默认快捷键。
// 默认表继续由代码版本管理，避免升级后旧设置文件遮住新增默认快捷键。
bool saveAppShortcutCustomBindings(
    const std::vector<AppShortcutBinding>& customBindings,
    const std::filesystem::path& settingsPath);

// loadAppShortcutCustomBindings 从本地 UTF-8 文本设置中读取用户覆盖项。
// 缺失文件、坏行和当前不支持的组合键都会被忽略，让本地偏好损坏不影响应用启动。
std::vector<AppShortcutBinding> loadAppShortcutCustomBindings(
    const std::filesystem::path& settingsPath);

// loadAppShortcutCustomization 读取本地覆盖项后，复用统一合并规则生成活动快捷键表。
// UI 可同时读取 bindings 和 conflicts，决定是启用快捷键还是提示用户修正冲突。
AppShortcutCustomizationResult loadAppShortcutCustomization(
    const std::filesystem::path& settingsPath);

}
