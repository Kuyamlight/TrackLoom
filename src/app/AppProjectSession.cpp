#include "AppProjectSession.h"

#include <utility>

namespace trackloom {

AppProjectSessionResult AppProjectSessionResult::ok()
{
    return { true, "" };
}

AppProjectSessionResult AppProjectSessionResult::fail(std::string message)
{
    return { false, std::move(message) };
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

void AppProjectSession::createNewProject(std::string name)
{
    project_ = Project(std::move(name));
    currentProjectPath_.reset();
    dirty_ = false;
}

AppProjectSessionResult AppProjectSession::save()
{
    if (!currentProjectPath_.has_value()) {
        return AppProjectSessionResult::fail("Project file path is not set. Use save-as first.");
    }

    return saveAs(*currentProjectPath_);
}

AppProjectSessionResult AppProjectSession::saveAs(const std::filesystem::path& path)
{
    const auto saved = saveProjectToFileAtomically(project_, path);
    if (!saved.success) {
        return AppProjectSessionResult::fail(saved.error);
    }

    currentProjectPath_ = path;
    dirty_ = false;
    return AppProjectSessionResult::ok();
}

AppProjectSessionResult AppProjectSession::openFrom(const std::filesystem::path& path)
{
    const auto loaded = loadProjectFromFile(path);
    if (!loaded.project.has_value()) {
        return AppProjectSessionResult::fail(loaded.error);
    }

    project_ = std::move(*loaded.project);
    currentProjectPath_ = path;
    dirty_ = false;
    return AppProjectSessionResult::ok();
}

}
