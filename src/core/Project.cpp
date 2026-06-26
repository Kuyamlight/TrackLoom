#include "Project.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string_view>
#include <system_error>
#include <utility>

namespace trackloom {

Project::Project(std::string name)
    : name_(std::move(name))
{
}

int Project::formatVersion() const
{
    return formatVersion_;
}

const std::string& Project::name() const
{
    return name_;
}

void Project::rename(std::string newName)
{
    name_ = std::move(newName);
}

const std::vector<Track>& Project::tracks() const
{
    return tracks_;
}

std::optional<Track> Project::findTrackById(const std::string& id) const
{
    const auto it = std::find_if(tracks_.begin(), tracks_.end(), [&](const Track& track) {
        return track.id == id;
    });

    if (it == tracks_.end()) {
        return std::nullopt;
    }

    return *it;
}

Track Project::createTrack(std::string name, TrackType type)
{
    Track track {
        "track-" + std::to_string(nextTrackNumber_++),
        std::move(name),
        type,
        {},
        {}
    };

    tracks_.push_back(track);
    return track;
}

bool Project::setTrackPlaybackState(const std::string& id, TrackPlaybackState state)
{
    const auto it = std::find_if(tracks_.begin(), tracks_.end(), [&](const Track& track) {
        return track.id == id;
    });

    if (it == tracks_.end()) {
        return false;
    }

    it->playback = state;
    return true;
}

bool Project::setTrackMixState(const std::string& id, TrackMixState state)
{
    if (!isValidTrackMixState(state)) {
        return false;
    }

    const auto it = std::find_if(tracks_.begin(), tracks_.end(), [&](const Track& track) {
        return track.id == id;
    });

    if (it == tracks_.end()) {
        return false;
    }

    it->mix = state;
    return true;
}

bool Project::insertExistingTrack(const Track& track)
{
    if (!isValidTrackMixState(track.mix)) {
        return false;
    }

    if (findTrackById(track.id).has_value()) {
        return false;
    }

    tracks_.push_back(track);
    observeTrackId(track.id);
    return true;
}

bool Project::removeTrackById(const std::string& id)
{
    const auto oldSize = tracks_.size();
    tracks_.erase(
        std::remove_if(tracks_.begin(), tracks_.end(), [&](const Track& track) {
            return track.id == id;
        }),
        tracks_.end());

    return tracks_.size() != oldSize;
}

void Project::observeTrackId(const std::string& id)
{
    constexpr std::string_view prefix = "track-";
    if (id.rfind(prefix, 0) != 0) {
        return;
    }

    int parsedNumber = 0;
    const auto numberPart = std::string_view(id).substr(prefix.size());
    const auto* first = numberPart.data();
    const auto* last = first + numberPart.size();
    const auto result = std::from_chars(first, last, parsedNumber);

    if (result.ec == std::errc{} && result.ptr == last && parsedNumber >= nextTrackNumber_) {
        nextTrackNumber_ = parsedNumber + 1;
    }
}

std::string toString(TrackType type)
{
    switch (type) {
    case TrackType::Instrument:
        return "Instrument";
    case TrackType::Audio:
        return "Audio";
    case TrackType::Folder:
        return "Folder";
    }

    return "Unknown";
}

std::optional<TrackType> trackTypeFromString(const std::string& value)
{
    if (value == "Instrument") {
        return TrackType::Instrument;
    }
    if (value == "Audio") {
        return TrackType::Audio;
    }
    if (value == "Folder") {
        return TrackType::Folder;
    }

    return std::nullopt;
}

bool isValidTrackMixState(TrackMixState state)
{
    return std::isfinite(state.gain)
        && state.gain >= 0.0f
        && std::isfinite(state.pan)
        && state.pan >= -1.0f
        && state.pan <= 1.0f;
}

}
