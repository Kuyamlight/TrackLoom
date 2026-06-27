#include "Project.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iterator>
#include <string_view>
#include <system_error>
#include <utility>

namespace trackloom {
namespace {

double ticksToSeconds(std::int64_t ticks, double beatsPerMinute)
{
    return (static_cast<double>(ticks) / static_cast<double>(Project::ticksPerQuarterNote))
        * (60.0 / beatsPerMinute);
}

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
    tempoEvents_.push_back({ "tempo-1", 0, 120.0 });
    observeTempoEventId("tempo-1");
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

const std::vector<TimelineMarker>& Project::markers() const
{
    return markers_;
}

std::optional<TimelineMarker> Project::findMarkerById(const std::string& id) const
{
    const auto it = std::find_if(markers_.begin(), markers_.end(), [&](const TimelineMarker& marker) {
        return marker.id == id;
    });

    if (it == markers_.end()) {
        return std::nullopt;
    }

    return *it;
}

const std::vector<TempoEvent>& Project::tempoEvents() const
{
    return tempoEvents_;
}

std::optional<TempoEvent> Project::findTempoEventById(const std::string& id) const
{
    const auto it = std::find_if(tempoEvents_.begin(), tempoEvents_.end(), [&](const TempoEvent& event) {
        return event.id == id;
    });

    if (it == tempoEvents_.end()) {
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

bool Project::setTrackViewState(const std::string& id, TrackViewState state)
{
    const auto it = std::find_if(tracks_.begin(), tracks_.end(), [&](const Track& track) {
        return track.id == id;
    });

    if (it == tracks_.end() || !isValidTrackViewState(it->type, state)) {
        return false;
    }

    it->view = state;
    return true;
}

bool Project::insertExistingTrack(const Track& track)
{
    return insertExistingTrackAt(track, tracks_.size());
}

bool Project::insertExistingTrackAt(const Track& track, std::size_t index)
{
    if (!isValidTrackMixState(track.mix) || !isValidTrackViewState(track.type, track.view)) {
        return false;
    }

    if (index > tracks_.size()) {
        return false;
    }

    if (findTrackById(track.id).has_value()) {
        return false;
    }

    tracks_.insert(tracks_.begin() + static_cast<std::vector<Track>::difference_type>(index), track);
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

bool Project::renameTrackById(const std::string& id, std::string name)
{
    if (name.empty()) {
        return false;
    }

    const auto it = std::find_if(tracks_.begin(), tracks_.end(), [&](const Track& track) {
        return track.id == id;
    });

    if (it == tracks_.end()) {
        return false;
    }

    it->name = std::move(name);
    return true;
}

std::optional<std::size_t> Project::trackIndexById(const std::string& id) const
{
    const auto it = std::find_if(tracks_.begin(), tracks_.end(), [&](const Track& track) {
        return track.id == id;
    });

    if (it == tracks_.end()) {
        return std::nullopt;
    }

    return static_cast<std::size_t>(std::distance(tracks_.begin(), it));
}

std::vector<TimelineClip> Project::clipsForTrack(const std::string& trackId) const
{
    std::vector<TimelineClip> result;
    for (const auto& clip : clips_) {
        if (clip.trackId == trackId) {
            result.push_back(clip);
        }
    }

    return result;
}

bool Project::moveTrackToIndex(const std::string& id, std::size_t targetIndex)
{
    if (targetIndex >= tracks_.size()) {
        return false;
    }

    const auto it = std::find_if(tracks_.begin(), tracks_.end(), [&](const Track& track) {
        return track.id == id;
    });

    if (it == tracks_.end()) {
        return false;
    }

    const auto oldIndex = static_cast<std::size_t>(std::distance(tracks_.begin(), it));
    if (oldIndex == targetIndex) {
        return false;
    }

    // 轨道顺序只影响工程里的排列；片段使用 trackId 关联轨道，所以不需要重写片段。
    auto movedTrack = std::move(*it);
    tracks_.erase(it);
    tracks_.insert(tracks_.begin() + static_cast<std::vector<Track>::difference_type>(targetIndex), std::move(movedTrack));
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

std::optional<TimelineClip> Project::duplicateClipToTrackAtTick(
    const std::string& clipId,
    std::string targetTrackId,
    std::int64_t startTick)
{
    const auto sourceClip = findClipById(clipId);
    if (!sourceClip.has_value() || startTick < 0 || !isValidClipTiming(startTick, sourceClip->lengthTick)) {
        return std::nullopt;
    }

    const auto targetTrack = findTrackById(targetTrackId);
    if (!targetTrack.has_value() || !trackCanOwnClip(*targetTrack, sourceClip->type)) {
        return std::nullopt;
    }

    TimelineClip duplicate {
        "clip-" + std::to_string(nextClipNumber_),
        std::move(targetTrackId),
        sourceClip->name,
        sourceClip->type,
        startTick,
        sourceClip->lengthTick
    };

    // 复制只产生新的片段外壳，不修改源片段；未来素材引用复制需要在更高层单独定义。
    clips_.push_back(duplicate);
    observeClipId(duplicate.id);
    return duplicate;
}

bool Project::trimClipStartToTick(const std::string& clipId, std::int64_t startTick)
{
    const auto it = std::find_if(clips_.begin(), clips_.end(), [&](const TimelineClip& clip) {
        return clip.id == clipId;
    });

    if (it == clips_.end()) {
        return false;
    }

    const auto oldEndTick = it->startTick + it->lengthTick;
    if (startTick <= it->startTick || startTick >= oldEndTick) {
        return false;
    }

    const auto newLength = oldEndTick - startTick;
    if (!isValidClipTiming(startTick, newLength)) {
        return false;
    }

    // 当前没有素材偏移模型，所以修剪只向内缩短片段外壳，不表达向外扩展。
    it->startTick = startTick;
    it->lengthTick = newLength;
    return true;
}

bool Project::trimClipEndToTick(const std::string& clipId, std::int64_t endTick)
{
    const auto it = std::find_if(clips_.begin(), clips_.end(), [&](const TimelineClip& clip) {
        return clip.id == clipId;
    });

    if (it == clips_.end()) {
        return false;
    }

    const auto oldEndTick = it->startTick + it->lengthTick;
    if (endTick <= it->startTick || endTick >= oldEndTick) {
        return false;
    }

    const auto newLength = endTick - it->startTick;
    if (!isValidClipTiming(it->startTick, newLength)) {
        return false;
    }

    it->lengthTick = newLength;
    return true;
}

std::optional<TimelineMarker> Project::createMarker(std::string name, std::int64_t tick)
{
    TimelineMarker marker {
        "marker-" + std::to_string(nextMarkerNumber_),
        std::move(name),
        tick
    };

    if (!insertExistingMarker(marker)) {
        return std::nullopt;
    }

    return marker;
}

bool Project::insertExistingMarker(const TimelineMarker& marker)
{
    if (marker.id.empty() || marker.name.empty() || !isValidMarkerTick(marker.tick)) {
        return false;
    }

    if (findMarkerById(marker.id).has_value()) {
        return false;
    }

    // 多个标记允许放在同一 tick；它们可能表达不同结构语义，例如“副歌”和“高潮”同点开始。
    markers_.push_back(marker);
    observeMarkerId(marker.id);
    return true;
}

bool Project::removeMarkerById(const std::string& id)
{
    const auto oldSize = markers_.size();
    markers_.erase(
        std::remove_if(markers_.begin(), markers_.end(), [&](const TimelineMarker& marker) {
            return marker.id == id;
        }),
        markers_.end());

    return markers_.size() != oldSize;
}

bool Project::renameMarkerById(const std::string& id, std::string name)
{
    if (name.empty()) {
        return false;
    }

    const auto it = std::find_if(markers_.begin(), markers_.end(), [&](const TimelineMarker& marker) {
        return marker.id == id;
    });

    if (it == markers_.end()) {
        return false;
    }

    it->name = std::move(name);
    return true;
}

bool Project::moveMarkerToTick(const std::string& id, std::int64_t tick)
{
    if (!isValidMarkerTick(tick)) {
        return false;
    }

    const auto it = std::find_if(markers_.begin(), markers_.end(), [&](const TimelineMarker& marker) {
        return marker.id == id;
    });

    if (it == markers_.end() || it->tick == tick) {
        return false;
    }

    it->tick = tick;
    return true;
}

std::optional<TempoEvent> Project::createTempoEvent(std::int64_t tick, double beatsPerMinute)
{
    TempoEvent event {
        "tempo-" + std::to_string(nextTempoNumber_),
        tick,
        beatsPerMinute
    };

    if (!insertExistingTempoEvent(event)) {
        return std::nullopt;
    }

    return event;
}

bool Project::insertExistingTempoEvent(const TempoEvent& event)
{
    if (event.id.empty() || !isValidMarkerTick(event.tick) || !isValidTempoBpm(event.beatsPerMinute)) {
        return false;
    }

    if (findTempoEventById(event.id).has_value()) {
        return false;
    }

    const auto duplicateTick = std::find_if(tempoEvents_.begin(), tempoEvents_.end(), [&](const TempoEvent& existingEvent) {
        return existingEvent.tick == event.tick;
    });
    if (duplicateTick != tempoEvents_.end()) {
        return false;
    }

    tempoEvents_.push_back(event);
    std::sort(tempoEvents_.begin(), tempoEvents_.end(), [](const TempoEvent& left, const TempoEvent& right) {
        return left.tick < right.tick;
    });
    observeTempoEventId(event.id);
    return true;
}

bool Project::removeTempoEventById(const std::string& id)
{
    const auto it = std::find_if(tempoEvents_.begin(), tempoEvents_.end(), [&](const TempoEvent& event) {
        return event.id == id;
    });

    if (it == tempoEvents_.end() || it->tick == 0 || tempoEvents_.size() <= 1) {
        return false;
    }

    tempoEvents_.erase(it);
    return true;
}

bool Project::setTempoEventBpm(const std::string& id, double beatsPerMinute)
{
    if (!isValidTempoBpm(beatsPerMinute)) {
        return false;
    }

    const auto it = std::find_if(tempoEvents_.begin(), tempoEvents_.end(), [&](const TempoEvent& event) {
        return event.id == id;
    });

    if (it == tempoEvents_.end()) {
        return false;
    }

    it->beatsPerMinute = beatsPerMinute;
    return true;
}

bool Project::moveTempoEventToTick(const std::string& id, std::int64_t tick)
{
    if (!isValidMarkerTick(tick)) {
        return false;
    }

    const auto it = std::find_if(tempoEvents_.begin(), tempoEvents_.end(), [&](const TempoEvent& event) {
        return event.id == id;
    });

    if (it == tempoEvents_.end() || it->tick == 0 || it->tick == tick) {
        return false;
    }

    const auto duplicateTick = std::find_if(tempoEvents_.begin(), tempoEvents_.end(), [&](const TempoEvent& event) {
        return event.id != id && event.tick == tick;
    });
    if (duplicateTick != tempoEvents_.end()) {
        return false;
    }

    it->tick = tick;
    std::sort(tempoEvents_.begin(), tempoEvents_.end(), [](const TempoEvent& left, const TempoEvent& right) {
        return left.tick < right.tick;
    });
    return true;
}

double Project::tempoAtTick(std::int64_t tick) const
{
    if (tempoEvents_.empty()) {
        return 120.0;
    }

    double currentTempo = tempoEvents_.front().beatsPerMinute;
    for (const auto& event : tempoEvents_) {
        if (event.tick > tick) {
            break;
        }
        currentTempo = event.beatsPerMinute;
    }

    return currentTempo;
}

double Project::tickToSeconds(std::int64_t tick) const
{
    if (tick <= 0 || tempoEvents_.empty()) {
        return 0.0;
    }

    double seconds = 0.0;
    std::int64_t segmentStartTick = 0;
    double segmentBpm = tempoEvents_.front().beatsPerMinute;

    for (std::size_t index = 1; index < tempoEvents_.size(); ++index) {
        const auto& event = tempoEvents_[index];
        if (event.tick >= tick) {
            break;
        }

        seconds += ticksToSeconds(event.tick - segmentStartTick, segmentBpm);
        segmentStartTick = event.tick;
        segmentBpm = event.beatsPerMinute;
    }

    seconds += ticksToSeconds(tick - segmentStartTick, segmentBpm);
    return seconds;
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

void Project::observeMarkerId(const std::string& id)
{
    constexpr std::string_view prefix = "marker-";
    if (id.rfind(prefix, 0) != 0) {
        return;
    }

    int parsedNumber = 0;
    const auto numberPart = std::string_view(id).substr(prefix.size());
    const auto* first = numberPart.data();
    const auto* last = first + numberPart.size();
    const auto result = std::from_chars(first, last, parsedNumber);

    if (result.ec == std::errc{} && result.ptr == last && parsedNumber >= nextMarkerNumber_) {
        nextMarkerNumber_ = parsedNumber + 1;
    }
}

void Project::observeTempoEventId(const std::string& id)
{
    constexpr std::string_view prefix = "tempo-";
    if (id.rfind(prefix, 0) != 0) {
        return;
    }

    int parsedNumber = 0;
    const auto numberPart = std::string_view(id).substr(prefix.size());
    const auto* first = numberPart.data();
    const auto* last = first + numberPart.size();
    const auto result = std::from_chars(first, last, parsedNumber);

    if (result.ec == std::errc{} && result.ptr == last && parsedNumber >= nextTempoNumber_) {
        nextTempoNumber_ = parsedNumber + 1;
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

bool isValidTrackViewState(TrackType type, TrackViewState state)
{
    // hidden 只是显示过滤，不影响轨道能否播放；collapsed 只属于文件夹轨。
    return !state.collapsed || type == TrackType::Folder;
}

bool isValidClipTiming(std::int64_t startTick, std::int64_t lengthTick)
{
    // 时间线位置使用音乐 tick。起点允许为 0，但长度必须大于 0，避免零长度片段干扰后续调度。
    return startTick >= 0 && lengthTick > 0;
}

bool isValidMarkerTick(std::int64_t tick)
{
    // 标记是时间线上的点，因此允许 0；负数没有明确音乐含义，读取坏文件或 AI 命令时必须拒绝。
    return tick >= 0;
}

bool isValidTempoBpm(double beatsPerMinute)
{
    // 20-300 BPM 覆盖常见创作范围，同时能挡住坏文件、无穷值和明显错误的 AI 命令。
    return std::isfinite(beatsPerMinute)
        && beatsPerMinute >= 20.0
        && beatsPerMinute <= 300.0;
}

}
