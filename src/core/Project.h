#pragma once

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

// Track 是工程里最小的轨道数据。
// 第一阶段暂不保存插件、片段或自动化，避免在核心边界稳定前过早扩大模型。
struct Track {
    std::string id;
    std::string name;
    TrackType type;
    TrackPlaybackState playback;
    TrackMixState mix;
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
    static constexpr int currentFormatVersion = 5;

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

    // insertExistingTrack 用于撤销重做或读取文件时恢复已有 ID 的轨道。
    bool insertExistingTrack(const Track& track);
    bool removeTrackById(const std::string& id);

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
bool isValidClipTiming(std::int64_t startTick, std::int64_t lengthTick);

}
