#include "AppProjectSession.h"

#include <utility>

namespace trackloom {

AppProjectSessionResult AppProjectSessionResult::ok()
{
    return { true, AppProjectSessionFailureReason::None, "" };
}

AppProjectSessionResult AppProjectSessionResult::fail(
    AppProjectSessionFailureReason reason,
    std::string message)
{
    return { false, reason, std::move(message) };
}

AppProjectSession::AppProjectSession()
    : project_("Untitled")
{
}

const Project& AppProjectSession::project() const
{
    return project_;
}

Project& AppProjectSession::editProject()
{
    // 直接暴露可编辑工程是兼容旧应用动作的过渡入口。
    // 这些修改不会进入 CommandStack，因此必须清空旧历史，避免未来 undo 跳过未记录的编辑。
    commandStack_ = CommandStack {};
    dirty_ = true;
    return project_;
}

const std::optional<std::filesystem::path>& AppProjectSession::currentProjectPath() const
{
    return currentProjectPath_;
}

bool AppProjectSession::isDirty() const
{
    return dirty_;
}

CommandResult AppProjectSession::executeProjectCommand(std::unique_ptr<Command> command)
{
    if (!command) {
        return CommandResult::fail("Project command must not be null.");
    }

    const auto result = commandStack_.execute(project_, std::move(command));
    if (result.success) {
        dirty_ = true;
    }

    return result;
}

bool AppProjectSession::undoProjectEdit()
{
    if (!commandStack_.undo(project_)) {
        return false;
    }

    dirty_ = true;
    return true;
}

bool AppProjectSession::redoProjectEdit()
{
    if (!commandStack_.redo(project_)) {
        return false;
    }

    dirty_ = true;
    return true;
}

bool AppProjectSession::canUndoProjectEdit() const
{
    return commandStack_.canUndo();
}

bool AppProjectSession::canRedoProjectEdit() const
{
    return commandStack_.canRedo();
}

void AppProjectSession::createNewProject(std::string name)
{
    project_ = Project(std::move(name));
    commandStack_ = CommandStack {};
    currentProjectPath_.reset();
    dirty_ = false;
}

AppProjectSessionResult AppProjectSession::save()
{
    if (!currentProjectPath_.has_value()) {
        return AppProjectSessionResult::fail(
            AppProjectSessionFailureReason::MissingProjectPath,
            "Project file path is not set. Use save-as first.");
    }

    return saveAs(*currentProjectPath_);
}

AppProjectSessionResult AppProjectSession::saveAs(const std::filesystem::path& path)
{
    const auto saved = saveProjectToFileAtomically(project_, path);
    if (!saved.success) {
        return AppProjectSessionResult::fail(
            AppProjectSessionFailureReason::SaveFailed,
            saved.error);
    }

    currentProjectPath_ = path;
    dirty_ = false;
    return AppProjectSessionResult::ok();
}

AppProjectSessionResult AppProjectSession::openFrom(const std::filesystem::path& path)
{
    const auto loaded = loadProjectFromFile(path);
    if (!loaded.project.has_value()) {
        return AppProjectSessionResult::fail(
            AppProjectSessionFailureReason::OpenFailed,
            loaded.error);
    }

    project_ = std::move(*loaded.project);
    commandStack_ = CommandStack {};
    currentProjectPath_ = path;
    dirty_ = false;
    return AppProjectSessionResult::ok();
}

}
