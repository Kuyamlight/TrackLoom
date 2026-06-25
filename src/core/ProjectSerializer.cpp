#include "ProjectSerializer.h"

#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace trackloom {
namespace {

bool startsWith(std::string_view value, std::string_view prefix)
{
    return value.substr(0, prefix.size()) == prefix;
}

bool parseFlagToken(const std::string& token, std::string_view key, bool& value)
{
    const std::string expectedPrefix = std::string(key) + "=";
    if (!startsWith(token, expectedPrefix)) {
        return false;
    }

    const auto rawValue = token.substr(expectedPrefix.size());
    if (rawValue == "0") {
        value = false;
        return true;
    }
    if (rawValue == "1") {
        value = true;
        return true;
    }

    return false;
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
        output << "track_playback_state " << track.id
               << " muted=" << (track.playback.muted ? 1 : 0)
               << " soloed=" << (track.playback.soloed ? 1 : 0)
               << " disabled=" << (track.playback.disabled ? 1 : 0)
               << '\n';
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
    // v2 增加轨道播放状态；v1 仍可读取，并在内存中迁移为默认播放状态。
    if (!(header >> keyword >> version) || keyword != "trackloom_project" || version < 1 || version > Project::currentFormatVersion) {
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

        // 播放状态单独成行，避免破坏已有 track 行里“轨道名可以包含空格”的规则。
        if (startsWith(line, "track_playback_state ")) {
            if (version < 2) {
                return LoadProjectResult::fail("Track playback state requires project version 2.");
            }

            std::istringstream stateLine(line);
            std::string trackId;
            std::string mutedToken;
            std::string soloedToken;
            std::string disabledToken;
            TrackPlaybackState state;

            if (!(stateLine >> keyword >> trackId >> mutedToken >> soloedToken >> disabledToken)) {
                return LoadProjectResult::fail("Invalid track playback state record.");
            }
            if (!parseFlagToken(mutedToken, "muted", state.muted)
                || !parseFlagToken(soloedToken, "soloed", state.soloed)
                || !parseFlagToken(disabledToken, "disabled", state.disabled)) {
                return LoadProjectResult::fail("Invalid track playback state value.");
            }
            if (!project.setTrackPlaybackState(trackId, state)) {
                return LoadProjectResult::fail("Track playback state references unknown track.");
            }

            continue;
        }

        if (startsWith(line, "track ")) {
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

            continue;
        }

        return LoadProjectResult::fail("Unknown project record.");
    }

    return LoadProjectResult::ok(std::move(project));
}

}
