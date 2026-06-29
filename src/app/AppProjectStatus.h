#pragma once

#include "AppProjectSession.h"

#include <cstddef>
#include <string>

namespace trackloom {

// AppProjectStatus 是桌面 UI 可以直接显示的工程状态快照。
// 它只从 AppProjectSession 派生，不反向修改工程，避免界面层保存第二份状态。
struct AppProjectStatus {
    std::string projectName;
    bool hasProjectPath = false;
    std::string projectPath;
    bool dirty = false;
    std::size_t trackCount = 0;
    std::string windowTitle;
    std::string statusLine;
};

// describeAppProjectSession 把会话状态整理成稳定文案。
// JUCE UI、菜单状态和后续测试都应复用它，避免每个入口自己拼 dirty 标记和路径提示。
AppProjectStatus describeAppProjectSession(const AppProjectSession& session);

}
