#include "AppProjectSession.h"

#include <type_traits>
#include <utility>

namespace trackloom {

AppProjectSessionResult AppProjectSessionResult::ok(
    std::string warning,
    std::vector<std::filesystem::path> recoveryPaths)
{
    return { true, AppProjectSessionFailureReason::None, "", std::move(warning), std::move(recoveryPaths) };
}

AppProjectSessionResult AppProjectSessionResult::fail(
    AppProjectSessionFailureReason reason,
    std::string message)
{
    return { false, reason, std::move(message), "", {} };
}

AppProjectSession::AppProjectSession()
    : AppProjectSession(saveProjectToFileAtomically, loadProjectFromFile)
{
}

AppProjectSession::AppProjectSession(detail::ProjectSaveOperation saveOperation)
    : AppProjectSession(saveOperation, loadProjectFromFile)
{
}

AppProjectSession::AppProjectSession(
    detail::ProjectSaveOperation saveOperation,
    detail::ProjectLoadOperation loadOperation)
    : project_("Untitled")
    , saveOperation_(saveOperation ? saveOperation : saveProjectToFileAtomically)
    , loadOperation_(loadOperation ? loadOperation : loadProjectFromFile)
{
}

AppProjectSessionReplacement::AppProjectSessionReplacement(
    Project project,
    std::optional<std::filesystem::path> path,
    std::uint64_t generation)
    : project_(std::move(project))
    , path_(std::move(path))
    , generation_(generation)
{
}

const Project& AppProjectSession::project() const
{
    return project_;
}

std::uint64_t AppProjectSession::projectEditGeneration() const noexcept
{
    return projectEditGeneration_;
}

AppProjectPlaybackSnapshot AppProjectSession::capturePlaybackSnapshot() const
{
    return { project_, projectEditGeneration_ };
}

Project& AppProjectSession::editProject()
{
    // 直接暴露可编辑工程是兼容旧应用动作的过渡入口。
    // 这些修改不会进入 CommandStack，因此必须清空旧历史，避免未来 undo 跳过未记录的编辑。
    commandStack_ = CommandStack {};
    dirty_ = true;
    advanceProjectEditGeneration();
    return project_;
}

void AppProjectSession::advanceProjectEditGeneration() noexcept
{
    ++projectEditGeneration_;
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
        advanceProjectEditGeneration();
    }

    return result;
}

bool AppProjectSession::undoProjectEdit()
{
    if (!commandStack_.undo(project_)) {
        return false;
    }

    dirty_ = true;
    advanceProjectEditGeneration();
    return true;
}

bool AppProjectSession::redoProjectEdit()
{
    if (!commandStack_.redo(project_)) {
        return false;
    }

    dirty_ = true;
    advanceProjectEditGeneration();
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

AppProjectSessionReplacement AppProjectSession::stageNewProject(std::string name) const
{
    return AppProjectSessionReplacement(
        Project(std::move(name)), std::nullopt, projectEditGeneration_ + 1);
}

AppProjectSessionReplacementStageResult AppProjectSession::stageOpenProject(
    const std::filesystem::path& path) const
{
    auto loaded = loadOperation_(path);
    if (!loaded.project.has_value()) {
        return { std::nullopt, std::move(loaded.error) };
    }

    return {
        AppProjectSessionReplacement(
            std::move(*loaded.project),
            std::optional<std::filesystem::path>(path),
            projectEditGeneration_ + 1),
        {}
    };
}

void AppProjectSession::commitProjectReplacement(
    AppProjectSessionReplacement&& replacement) noexcept
{
    static_assert(std::is_nothrow_swappable_v<Project>);
    static_assert(std::is_nothrow_swappable_v<CommandStack>);
    static_assert(std::is_nothrow_swappable_v<std::optional<std::filesystem::path>>);
    using std::swap;
    swap(project_, replacement.project_);
    swap(commandStack_, replacement.commandStack_);
    swap(currentProjectPath_, replacement.path_);
    swap(dirty_, replacement.dirty_);
    swap(projectEditGeneration_, replacement.generation_);
}

void AppProjectSession::createNewProject(std::string name)
{
    auto replacement = stageNewProject(std::move(name));
    commitProjectReplacement(std::move(replacement));
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
    const auto saved = saveOperation_(project_, path);
    if (!saved.success) {
        return AppProjectSessionResult::fail(
            AppProjectSessionFailureReason::SaveFailed,
            saved.error);
    }

    currentProjectPath_ = path;
    dirty_ = false;
    return AppProjectSessionResult::ok(saved.warning, saved.recoveryPaths);
}

AppProjectSessionResult AppProjectSession::openFrom(const std::filesystem::path& path)
{
    auto staged = stageOpenProject(path);
    if (!staged.replacement.has_value()) {
        return AppProjectSessionResult::fail(
            AppProjectSessionFailureReason::OpenFailed,
            std::move(staged.error));
    }

    auto success = AppProjectSessionResult::ok();
    commitProjectReplacement(std::move(*staged.replacement));
    return success;
}

}
