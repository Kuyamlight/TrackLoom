#pragma once

#include "Command.h"
#include "Project.h"
#include "ProjectFile.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace trackloom {

// AppProjectSessionFailureReason 给 UI 和快捷键提供稳定失败原因。
// message 只用于显示或日志，不应作为逻辑分支依据。
enum class AppProjectSessionFailureReason {
    None,
    MissingProjectPath,
    SaveFailed,
    OpenFailed
};

// AppProjectSessionResult 是桌面壳工程会话操作的统一结果。
// 它只描述应用层动作是否成功；底层工程文件格式仍由 ProjectFile 负责。
struct AppProjectSessionResult {
    bool success = false;
    AppProjectSessionFailureReason failureReason = AppProjectSessionFailureReason::None;
    std::string error;

    static AppProjectSessionResult ok();
    static AppProjectSessionResult fail(AppProjectSessionFailureReason reason, std::string message);
};

// AppProjectSession 保存桌面应用“当前打开的工程”状态。
// 它不定义工程文件格式，只组合 Project、当前文件路径和 dirty 标志，供 UI、快捷键和 AI 工具复用。
class AppProjectSession final {
public:
    AppProjectSession();

    const Project& project() const;

    // 请求可编辑工程时先标记 dirty，避免调用方改了工程却忘记告诉桌面壳。
    Project& editProject();

    const std::optional<std::filesystem::path>& currentProjectPath() const;
    bool isDirty() const;

    // 通过核心 Command 执行工程编辑，才能进入撤销/重做历史。
    // 失败命令不会标脏，也不会进入历史栈。
    CommandResult executeProjectCommand(std::unique_ptr<Command> command);
    bool undoProjectEdit();
    bool redoProjectEdit();
    bool canUndoProjectEdit() const;
    bool canRedoProjectEdit() const;

    void createNewProject(std::string name = "Untitled");

    AppProjectSessionResult save();
    AppProjectSessionResult saveAs(const std::filesystem::path& path);
    AppProjectSessionResult openFrom(const std::filesystem::path& path);

private:
    Project project_;
    CommandStack commandStack_;
    std::optional<std::filesystem::path> currentProjectPath_;
    bool dirty_ = false;
};

}
