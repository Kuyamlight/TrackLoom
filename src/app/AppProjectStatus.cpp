#include "AppProjectStatus.h"

#include <filesystem>
#include <string>

namespace trackloom {
namespace {

std::string savedStateText(bool dirty)
{
    return dirty ? "有未保存修改" : "已保存";
}

std::string pathToUtf8String(const std::filesystem::path& path)
{
    const auto utf8Path = path.u8string();
    return { reinterpret_cast<const char*>(utf8Path.data()), utf8Path.size() };
}

std::string projectPathText(const AppProjectSession& session)
{
    if (!session.currentProjectPath().has_value()) {
        return "未保存工程";
    }

    // 这里保留完整路径，方便用户确认当前文件位置；后续可在 UI 层再做中间省略。
    return pathToUtf8String(*session.currentProjectPath());
}

}

AppProjectStatus describeAppProjectSession(const AppProjectSession& session)
{
    AppProjectStatus status;
    status.projectName = session.project().name();
    status.hasProjectPath = session.currentProjectPath().has_value();
    status.projectPath = status.hasProjectPath ? pathToUtf8String(*session.currentProjectPath()) : "";
    status.dirty = session.isDirty();
    status.trackCount = session.project().tracks().size();

    // 星号是常见桌面软件 dirty 标记；真正阻止关闭或保存提示由后续命令层实现。
    status.windowTitle = status.projectName + (status.dirty ? "* - TrackLoom" : " - TrackLoom");
    status.statusLine = projectPathText(session)
        + " - " + std::to_string(status.trackCount) + " 条轨道 - " + savedStateText(status.dirty);
    return status;
}

}
