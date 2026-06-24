#include "ProjectSerializer.h"

#include <sstream>
#include <string_view>
#include <utility>

namespace trackloom {
namespace {

bool startsWith(std::string_view value, std::string_view prefix)
{
    return value.substr(0, prefix.size()) == prefix;
}

}

LoadProjectResult LoadProjectResult::ok(Project project)
{
    return { std::move(project), "" };
}

LoadProjectResult LoadProjectResult::fail(std::string message)
{
    return { std::nullopt, std::move(message) };
}

std::string saveProjectToText(const Project& project)
{
    std::ostringstream output;
    output << "trackloom_project " << project.formatVersion() << '\n';
    output << "name " << project.name() << '\n';

    for (const auto& track : project.tracks()) {
        output << "track " << track.id << ' ' << toString(track.type) << ' ' << track.name << '\n';
    }

    return output.str();
}

LoadProjectResult loadProjectFromText(const std::string& text)
{
    std::istringstream input(text);
    std::string line;
    std::string keyword;
    int version = 0;

    if (!std::getline(input, line)) {
        return LoadProjectResult::fail("Project header is missing.");
    }

    std::istringstream header(line);
    if (!(header >> keyword >> version) || keyword != "trackloom_project" || version != Project::currentFormatVersion) {
        return LoadProjectResult::fail("Unsupported or invalid project header.");
    }

    if (!std::getline(input, line) || !startsWith(line, "name ")) {
        return LoadProjectResult::fail("Project name is missing.");
    }

    const std::string projectName = line.substr(std::string_view("name ").size());
    Project project(projectName);

    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }

        if (!startsWith(line, "track ")) {
            return LoadProjectResult::fail("Unknown project record.");
        }

        std::istringstream trackLine(line);
        Track track;
        std::string typeName;
        if (!(trackLine >> keyword >> track.id >> typeName)) {
            return LoadProjectResult::fail("Invalid track record.");
        }

        std::getline(trackLine, track.name);
        if (!track.name.empty() && track.name.front() == ' ') {
            track.name.erase(0, 1);
        }
        if (track.name.empty()) {
            return LoadProjectResult::fail("Track name is missing.");
        }

        const auto type = trackTypeFromString(typeName);
        if (!type.has_value()) {
            return LoadProjectResult::fail("Unknown track type.");
        }

        track.type = *type;
        if (!project.insertExistingTrack(track)) {
            return LoadProjectResult::fail("Duplicate track id.");
        }
    }

    return LoadProjectResult::ok(std::move(project));
}

}
