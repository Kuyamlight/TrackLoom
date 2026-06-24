#include "Command.h"
#include "ProjectFile.h"
#include "Project.h"
#include "ProjectSerializer.h"

#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path makeTestDirectory(const std::string& name)
{
    const auto path = std::filesystem::temp_directory_path() / "trackloom_tests" / name;
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

void projectStartsEmpty()
{
    trackloom::Project project("Demo");

    require(project.formatVersion() == 1, "format version should start at 1");
    require(project.name() == "Demo", "project name should be stored");
    require(project.tracks().empty(), "new project should not contain tracks");
}

void addTrackCommandSupportsUndoAndRedo()
{
    trackloom::Project project;
    trackloom::CommandStack commands;

    auto result = commands.execute(
        project,
        std::make_unique<trackloom::AddTrackCommand>("Piano", trackloom::TrackType::Instrument));

    require(result.success, "add track command should succeed");
    require(project.tracks().size() == 1, "project should contain one track");
    require(project.tracks().front().id == "track-1", "first track id should be stable");

    require(commands.undo(project), "undo should be available");
    require(project.tracks().empty(), "undo should remove the track");

    require(commands.redo(project), "redo should be available");
    require(project.tracks().size() == 1, "redo should restore the track");
    require(project.tracks().front().id == "track-1", "redo should preserve track id");
}

void invalidCommandDoesNotModifyProject()
{
    trackloom::Project project;
    trackloom::CommandStack commands;

    auto result = commands.execute(
        project,
        std::make_unique<trackloom::AddTrackCommand>("", trackloom::TrackType::Audio));

    require(!result.success, "empty track name should fail validation");
    require(project.tracks().empty(), "failed command should not modify project");
    require(!commands.canUndo(), "failed command should not enter undo stack");
}

void projectCanRoundTripThroughText()
{
    trackloom::Project project("Song");
    project.createTrack("Piano", trackloom::TrackType::Instrument);
    project.createTrack("Vocal", trackloom::TrackType::Audio);

    const auto saved = trackloom::saveProjectToText(project);
    const auto loaded = trackloom::loadProjectFromText(saved);

    require(loaded.project.has_value(), "saved project should load");
    require(loaded.project->name() == "Song", "loaded project should keep name");
    require(loaded.project->tracks().size() == 2, "loaded project should keep tracks");
    require(loaded.project->tracks()[0].type == trackloom::TrackType::Instrument, "first track type should survive");
    require(loaded.project->tracks()[1].type == trackloom::TrackType::Audio, "second track type should survive");
}

void projectCanRoundTripNamesWithSpaces()
{
    trackloom::Project project("Demo Song");
    project.createTrack("Lead Piano", trackloom::TrackType::Instrument);

    const auto saved = trackloom::saveProjectToText(project);
    const auto loaded = trackloom::loadProjectFromText(saved);

    require(loaded.project.has_value(), "project names with spaces should load");
    require(loaded.project->name() == "Demo Song", "project name should keep spaces");
    require(loaded.project->tracks().front().name == "Lead Piano", "track name should keep spaces");
}

void invalidTextIsRejected()
{
    const auto loaded = trackloom::loadProjectFromText("broken 99\n");

    require(!loaded.project.has_value(), "invalid project should not load");
    require(!loaded.error.empty(), "invalid project should report an error");
}

void projectCanSaveAndLoadFromFile()
{
    const auto directory = makeTestDirectory("save_and_load");
    const auto path = directory / "song.tlproj";

    trackloom::Project project("Saved Song");
    project.createTrack("Lead Piano", trackloom::TrackType::Instrument);

    const auto saved = trackloom::saveProjectToFileAtomically(project, path);
    const auto loaded = trackloom::loadProjectFromFile(path);

    require(saved.success, "saving project to file should succeed");
    require(loaded.project.has_value(), "saved project file should load");
    require(loaded.project->name() == "Saved Song", "loaded file should keep project name");
    require(loaded.project->tracks().front().name == "Lead Piano", "loaded file should keep track name");
}

void saveCreatesParentDirectories()
{
    const auto directory = makeTestDirectory("nested_parent");
    const auto path = directory / "level1" / "level2" / "song.tlproj";

    trackloom::Project project("Nested Song");
    const auto saved = trackloom::saveProjectToFileAtomically(project, path);

    require(saved.success, "saving should create missing parent directories");
    require(std::filesystem::exists(path), "project file should exist after save");
}

void saveReplacesExistingFile()
{
    const auto directory = makeTestDirectory("replace_existing");
    const auto path = directory / "song.tlproj";

    trackloom::Project first("Old Song");
    trackloom::Project second("New Song");

    require(trackloom::saveProjectToFileAtomically(first, path).success, "initial save should succeed");
    require(trackloom::saveProjectToFileAtomically(second, path).success, "replacement save should succeed");

    const auto loaded = trackloom::loadProjectFromFile(path);
    require(loaded.project.has_value(), "replaced project should load");
    require(loaded.project->name() == "New Song", "replacement should store new content");
}

void loadingMissingFileReportsError()
{
    const auto directory = makeTestDirectory("missing_file");
    const auto loaded = trackloom::loadProjectFromFile(directory / "missing.tlproj");

    require(!loaded.project.has_value(), "missing file should not load");
    require(!loaded.error.empty(), "missing file should report an error");
}

void savingEmptyPathReportsError()
{
    trackloom::Project project("No Path");
    const auto saved = trackloom::saveProjectToFileAtomically(project, {});

    require(!saved.success, "saving to empty path should fail");
    require(!saved.error.empty(), "empty path save should report an error");
}

}

int main()
{
    try {
        projectStartsEmpty();
        addTrackCommandSupportsUndoAndRedo();
        invalidCommandDoesNotModifyProject();
        projectCanRoundTripThroughText();
        projectCanRoundTripNamesWithSpaces();
        invalidTextIsRejected();
        projectCanSaveAndLoadFromFile();
        saveCreatesParentDirectories();
        saveReplacesExistingFile();
        loadingMissingFileReportsError();
        savingEmptyPathReportsError();
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "All core tests passed.\n";
    return 0;
}
