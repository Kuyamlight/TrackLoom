#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trackloom {

// TrackType 描述轨道在工程模型中的基本职责。
// 文件夹轨只负责分类和折叠，不代表音频总线；这个边界会影响后续混音和路由设计。
enum class TrackType {
    Instrument,
    Audio,
    Folder
};

// TrackPlaybackState 只保存会影响播放处理开关的轨道状态。
// 隐藏、冻结和隐私权限语义不同，后续应作为独立状态扩展。
struct TrackPlaybackState {
    bool muted = false;
    bool soloed = false;
    bool disabled = false;

    bool operator==(const TrackPlaybackState&) const = default;
};

// TrackMixState 保存轨道混音参数，不与静音、独奏、禁用等播放开关混在一起。
// 当前只包含线性音量；后续声像、自动化和总线发送可以沿着这个独立结构继续扩展。
struct TrackMixState {
    float gain = 1.0f;
    float pan = 0.0f;

    bool operator==(const TrackMixState&) const = default;
};

// TrackViewState 保存只影响界面显示的轨道状态。
// hidden 不参与播放判断；collapsed 只允许文件夹轨使用，用来表达折叠显示。
struct TrackViewState {
    bool hidden = false;
    bool collapsed = false;

    bool operator==(const TrackViewState&) const = default;
};

// Track 是工程里最小的轨道数据。
// 第一阶段暂不保存插件、片段或自动化，避免在核心边界稳定前过早扩大模型。
struct Track {
    std::string id;
    std::string name;
    TrackType type;
    TrackPlaybackState playback;
    TrackMixState mix;
    TrackViewState view;
};

// ClipType 描述时间线片段承载的素材类型。
// 当前只区分 MIDI 与音频，后续自动化片段、模式片段等应继续扩展这个边界。
enum class ClipType {
    Midi,
    Audio
};

// TimelineClip 是放在工程时间线上的最小片段外壳。
// 它暂不保存 MIDI 事件或音频文件路径，只先稳定“哪个轨道、从哪里开始、持续多久”。
struct TimelineClip {
    std::string id;
    std::string trackId;
    std::string name;
    ClipType type = ClipType::Midi;
    std::int64_t startTick = 0;
    std::int64_t lengthTick = 0;

    bool operator==(const TimelineClip&) const = default;
};

// Project 是 TrackLoom 自有工程格式的最小核心状态。
// 后续 UI、AI 和导入器都应通过命令系统修改它，避免绕过验证、撤销和历史记录。
class Project {
public:
    static constexpr int currentFormatVersion = 6;

    explicit Project(std::string name = "Untitled");

    int formatVersion() const;
    const std::string& name() const;
    void rename(std::string newName);

    const std::vector<Track>& tracks() const;
    std::optional<Track> findTrackById(const std::string& id) const;
    const std::vector<TimelineClip>& clips() const;
    std::optional<TimelineClip> findClipById(const std::string& id) const;

    // createTrack 用于创建全新轨道，并分配本工程内稳定的可读 ID。
    Track createTrack(std::string name, TrackType type);

    // setTrackPlaybackState 只按轨道 ID 更新播放状态；轨道不存在时保持工程不变。
    bool setTrackPlaybackState(const std::string& id, TrackPlaybackState state);

    // setTrackMixState 只更新轨道混音参数；非法 gain 会被拒绝，避免坏文件或 AI 命令污染工程。
    bool setTrackMixState(const std::string& id, TrackMixState state);

    // setTrackViewState 只更新轨道显示状态；隐藏不能影响播放，折叠只允许文件夹轨使用。
    bool setTrackViewState(const std::string& id, TrackViewState state);

    // insertExistingTrack 用于撤销重做或读取文件时恢复已有 ID 的轨道。
    bool insertExistingTrack(const Track& track);
    bool insertExistingTrackAt(const Track& track, std::size_t index);
    bool removeTrackById(const std::string& id);

    // renameTrackById 只修改轨道显示名称；轨道不存在或名称为空时保持工程不变。
    bool renameTrackById(const std::string& id, std::string name);

    // trackIndexById 返回轨道在工程轨道列表中的当前位置，用于撤销时恢复原顺序。
    std::optional<std::size_t> trackIndexById(const std::string& id) const;

    // clipsForTrack 返回轨道当前拥有的片段副本，删除轨道命令用它保存可撤销状态。
    std::vector<TimelineClip> clipsForTrack(const std::string& trackId) const;

    // moveTrackToIndex 调整轨道显示顺序；片段仍通过稳定 trackId 归属轨道。
    bool moveTrackToIndex(const std::string& id, std::size_t targetIndex);

    // createClip 创建新的时间线片段，并验证片段类型是否允许放在目标轨道上。
    std::optional<TimelineClip> createClip(
        std::string trackId,
        std::string name,
        ClipType type,
        std::int64_t startTick,
        std::int64_t lengthTick);

    // insertExistingClip 用于撤销重做或读取文件时恢复已有 ID 的片段。
    bool insertExistingClip(const TimelineClip& clip);
    bool removeClipById(const std::string& id);

    // renameClipById 只修改片段显示名称；片段不存在或名称为空时保持工程不变。
    bool renameClipById(const std::string& id, std::string name);

    // setClipTiming 只修改片段音乐时间范围；非法 tick 会被拒绝，避免坏命令污染工程。
    bool setClipTiming(const std::string& id, std::int64_t startTick, std::int64_t lengthTick);

    // moveClipToTrack 只修改片段所属轨道；目标轨道必须存在并兼容片段类型。
    bool moveClipToTrack(const std::string& clipId, std::string targetTrackId);

    // splitClipAtTick 在片段内部切开时间范围；左段复用原 ID，右段获得新的稳定 ID。
    std::optional<TimelineClip> splitClipAtTick(const std::string& clipId, std::int64_t splitTick);

    // duplicateClipToTrackAtTick 复制片段外壳；新片段获得新 ID，并放到指定兼容轨道和起点。
    std::optional<TimelineClip> duplicateClipToTrackAtTick(
        const std::string& clipId,
        std::string targetTrackId,
        std::int64_t startTick);

    // trimClipStartToTick 向内移动片段左边界，并保持旧终点不变。
    bool trimClipStartToTick(const std::string& clipId, std::int64_t startTick);

    // trimClipEndToTick 向内移动片段右边界，并保持旧起点不变。
    bool trimClipEndToTick(const std::string& clipId, std::int64_t endTick);

private:
    int formatVersion_ = currentFormatVersion;
    std::string name_;
    std::vector<Track> tracks_;
    std::vector<TimelineClip> clips_;
    int nextTrackNumber_ = 1;
    int nextClipNumber_ = 1;

    // 读取旧轨道 ID 后推进计数器，避免下一次新建轨道撞上已有 ID。
    void observeTrackId(const std::string& id);

    // 读取旧片段 ID 后推进计数器，避免下一次新建片段撞上已有 ID。
    void observeClipId(const std::string& id);
};

std::string toString(TrackType type);
std::optional<TrackType> trackTypeFromString(const std::string& value);
std::string toString(ClipType type);
std::optional<ClipType> clipTypeFromString(const std::string& value);
bool isValidTrackMixState(TrackMixState state);
bool isValidTrackViewState(TrackType type, TrackViewState state);
bool isValidClipTiming(std::int64_t startTick, std::int64_t lengthTick);

}
