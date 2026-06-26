#include "Project.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string_view>
#include <system_error>
#include <utility>

namespace trackloom {
namespace {

bool trackCanOwnClip(const Track& track, ClipType clipType)
{
    // 片段类型和轨道类型先保持严格对应，避免把 MIDI 放到音频轨或把音频放到乐器轨。
    if (track.type == TrackType::Instrument) {
        return clipType == ClipType::Midi;
    }
    if (track.type == TrackType::Audio) {
        return clipType == ClipType::Audio;
    }

    // 文件夹轨只负责分类和折叠，不能承载真实时间线片段。
    return false;
}

}

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

const std::vector<TimelineClip>& Project::clips() const
{
    return clips_;
}

std::optional<TimelineClip> Project::findClipById(const std::string& id) const
{
    const auto it = std::find_if(clips_.begin(), clips_.end(), [&](const TimelineClip& clip) {
        return clip.id == id;
    });

    if (it == clips_.end()) {
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

    if (tracks_.size() == oldSize) {
        return false;
    }

    // 轨道是片段的父对象；删除轨道时必须清理它的片段，避免保存出无法解析的工程文件。
    clips_.erase(
        std::remove_if(clips_.begin(), clips_.end(), [&](const TimelineClip& clip) {
            return clip.trackId == id;
        }),
        clips_.end());

    return true;
}

std::optional<TimelineClip> Project::createClip(
    std::string trackId,
    std::string name,
    ClipType type,
    std::int64_t startTick,
    std::int64_t lengthTick)
{
    TimelineClip clip {
        "clip-" + std::to_string(nextClipNumber_),
        std::move(trackId),
        std::move(name),
        type,
        startTick,
        lengthTick
    };

    if (!insertExistingClip(clip)) {
        return std::nullopt;
    }

    return clip;
}

bool Project::insertExistingClip(const TimelineClip& clip)
{
    if (clip.id.empty() || clip.name.empty() || !isValidClipTiming(clip.startTick, clip.lengthTick)) {
        return false;
    }

    if (findClipById(clip.id).has_value()) {
        return false;
    }

    const auto track = findTrackById(clip.trackId);
    if (!track.has_value() || !trackCanOwnClip(*track, clip.type)) {
        return false;
    }

    clips_.push_back(clip);
    observeClipId(clip.id);
    return true;
}

bool Project::removeClipById(const std::string& id)
{
    const auto oldSize = clips_.size();
    clips_.erase(
        std::remove_if(clips_.begin(), clips_.end(), [&](const TimelineClip& clip) {
            return clip.id == id;
        }),
        clips_.end());

    return clips_.size() != oldSize;
}

bool Project::renameClipById(const std::string& id, std::string name)
{
    if (name.empty()) {
        return false;
    }

    const auto it = std::find_if(clips_.begin(), clips_.end(), [&](const TimelineClip& clip) {
        return clip.id == id;
    });

    if (it == clips_.end()) {
        return false;
    }

    it->name = std::move(name);
    return true;
}

bool Project::setClipTiming(const std::string& id, std::int64_t startTick, std::int64_t lengthTick)
{
    if (!isValidClipTiming(startTick, lengthTick)) {
        return false;
    }

    const auto it = std::find_if(clips_.begin(), clips_.end(), [&](const TimelineClip& clip) {
        return clip.id == id;
    });

    if (it == clips_.end()) {
        return false;
    }

    it->startTick = startTick;
    it->lengthTick = lengthTick;
    return true;
}

bool Project::moveClipToTrack(const std::string& clipId, std::string targetTrackId)
{
    const auto clipIt = std::find_if(clips_.begin(), clips_.end(), [&](const TimelineClip& clip) {
        return clip.id == clipId;
    });

    if (clipIt == clips_.end()) {
        return false;
    }

    const auto targetTrack = findTrackById(targetTrackId);
    if (!targetTrack.has_value() || !trackCanOwnClip(*targetTrack, clipIt->type)) {
        return false;
    }

    clipIt->trackId = std::move(targetTrackId);
    return true;
}

std::optional<TimelineClip> Project::splitClipAtTick(const std::string& clipId, std::int64_t splitTick)
{
    const auto clipIt = std::find_if(clips_.begin(), clips_.end(), [&](const TimelineClip& clip) {
        return clip.id == clipId;
    });

    if (clipIt == clips_.end()) {
        return std::nullopt;
    }

    const auto originalEndTick = clipIt->startTick + clipIt->lengthTick;
    if (splitTick <= clipIt->startTick || splitTick >= originalEndTick) {
        return std::nullopt;
    }

    const auto leftLength = splitTick - clipIt->startTick;
    const auto rightLength = originalEndTick - splitTick;
    if (!isValidClipTiming(clipIt->startTick, leftLength) || !isValidClipTiming(splitTick, rightLength)) {
        return std::nullopt;
    }

    TimelineClip rightClip {
        "clip-" + std::to_string(nextClipNumber_),
        clipIt->trackId,
        clipIt->name,
        clipIt->type,
        splitTick,
        rightLength
    };

    // 先缩短左段，再追加右段。右段使用 observeClipId 推进计数器，保证后续新片段不撞 ID。
    clipIt->lengthTick = leftLength;
    clips_.push_back(rightClip);
    observeClipId(rightClip.id);
    return rightClip;
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

void Project::observeClipId(const std::string& id)
{
    constexpr std::string_view prefix = "clip-";
    if (id.rfind(prefix, 0) != 0) {
        return;
    }

    int parsedNumber = 0;
    const auto numberPart = std::string_view(id).substr(prefix.size());
    const auto* first = numberPart.data();
    const auto* last = first + numberPart.size();
    const auto result = std::from_chars(first, last, parsedNumber);

    if (result.ec == std::errc{} && result.ptr == last && parsedNumber >= nextClipNumber_) {
        nextClipNumber_ = parsedNumber + 1;
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

std::string toString(ClipType type)
{
    switch (type) {
    case ClipType::Midi:
        return "Midi";
    case ClipType::Audio:
        return "Audio";
    }

    return "Unknown";
}

std::optional<ClipType> clipTypeFromString(const std::string& value)
{
    if (value == "Midi") {
        return ClipType::Midi;
    }
    if (value == "Audio") {
        return ClipType::Audio;
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

bool isValidClipTiming(std::int64_t startTick, std::int64_t lengthTick)
{
    // 时间线位置使用音乐 tick。起点允许为 0，但长度必须大于 0，避免零长度片段干扰后续调度。
    return startTick >= 0 && lengthTick > 0;
}

}
