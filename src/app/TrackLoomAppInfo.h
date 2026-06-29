#pragma once

#include <string_view>

namespace trackloom {

// DesktopAppInfo 保存桌面程序启动时需要公开给系统和设置目录的稳定身份。
// 这些值不属于工程文件内容；它们只用于窗口标题、应用版本和本地配置命名。
struct DesktopAppInfo {
    std::string_view applicationName;
    std::string_view applicationVersion;
    std::string_view organizationName;
};

// 返回桌面壳当前使用的应用身份。
// 单独放在小型库里，是为了让测试和 JUCE 应用入口读取同一份信息，避免后续改名时漂移。
DesktopAppInfo desktopAppInfo();

}
