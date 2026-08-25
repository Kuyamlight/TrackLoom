#pragma once

#include "LoopRange.h"

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

// MidiNoteEvent 是 MIDI 片段内部最小的音符事件。
// startTick 和 lengthTick 都是相对片段起点的音乐 tick，片段整体移动时音符相对位置不变。
struct MidiNoteEvent {
    std::string id;
    std::int64_t startTick = 0;
    std::int64_t lengthTick = 0;
    int noteNumber = 60;
    int velocity = 100;
    int channel = 1;

    bool operator==(const MidiNoteEvent&) const = default;
};

// TimelineClip 是放在工程时间线上的最小片段外壳。
// MIDI 音符保存在 MIDI 片段内部；音频文件路径和素材引用会在后续阶段单独扩展。
struct TimelineClip {
    std::string id;
    std::string trackId;
    std::string name;
    ClipType type = ClipType::Midi;
    std::int64_t startTick = 0;
    std::int64_t lengthTick = 0;
    std::vector<MidiNoteEvent> midiNotes;

    bool operator==(const TimelineClip&) const = default;
};

// TimelineMarker 是工程级时间线提示点，不属于任何轨道或片段。
// 它只记录结构位置和名称；播放、速度图、拍号图和 MIDI 导出会在后续模块单独处理。
struct TimelineMarker {
    std::string id;
    std::string name;
    std::int64_t tick = 0;

    bool operator==(const TimelineMarker&) const = default;
};

// TempoEvent 是工程级速度事件，表示从某个 tick 开始使用新的 BPM。
// 当前只支持阶梯式速度变化；曲线速度、拍号和节拍器会在更高层单独建模。
struct TempoEvent {
    std::string id;
    std::int64_t tick = 0;
    double beatsPerMinute = 120.0;

    bool operator==(const TempoEvent&) const = default;
};

// TimeSignatureEvent 是工程级拍号事件，表示从某个 tick 开始使用新的小节拍号。
// 本阶段只保存分子和分母；小节编号、节拍器、网格吸附和 MIDI meta event 会在后续模块单独实现。
struct TimeSignatureEvent {
    std::string id;
    std::int64_t tick = 0;
    int numerator = 4;
    int denominator = 4;

    bool operator==(const TimeSignatureEvent&) const = default;
};

// Project 是 TrackLoom 自有工程格式的最小核心状态。
// 后续 UI、AI 和导入器都应通过命令系统修改它，避免绕过验证、撤销和历史记录。
class Project {
public:
    static constexpr int currentFormatVersion = 10;
    static constexpr std::int64_t ticksPerQuarterNote = 960;

    explicit Project(std::string name = "Untitled");

    int formatVersion() const;
    const std::string& name() const;
    void rename(std::string newName);
    const std::optional<PlaybackLoopRange>& playbackLoopRange() const noexcept;

    // setPlaybackLoopRange 保存工程级循环范围；无范围表示关闭，非法非空范围保持工程不变。
    bool setPlaybackLoopRange(std::optional<PlaybackLoopRange> range);

    const std::vector<Track>& tracks() const;
    std::optional<Track> findTrackById(const std::string& id) const;
    const std::vector<TimelineClip>& clips() const;
    std::optional<TimelineClip> findClipById(const std::string& id) const;
    std::optional<MidiNoteEvent> findMidiNoteById(const std::string& id) const;
    const std::vector<TimelineMarker>& markers() const;
    std::optional<TimelineMarker> findMarkerById(const std::string& id) const;
    const std::vector<TempoEvent>& tempoEvents() const;
    std::optional<TempoEvent> findTempoEventById(const std::string& id) const;
    const std::vector<TimeSignatureEvent>& timeSignatureEvents() const;
    std::optional<TimeSignatureEvent> findTimeSignatureEventById(const std::string& id) const;

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

    // createMidiNote 在 MIDI 片段中创建相对片段起点的音符事件。
    std::optional<MidiNoteEvent> createMidiNote(
        const std::string& clipId,
        std::int64_t startTick,
        std::int64_t lengthTick,
        int noteNumber,
        int velocity,
        int channel);

    // insertExistingMidiNote 用于撤销重做或读取文件时恢复已有音符 ID。
    bool insertExistingMidiNote(const std::string& clipId, const MidiNoteEvent& note);
    bool removeMidiNoteById(const std::string& id);

    // 音符编辑只修改音符自身，不移动片段或轨道。
    bool setMidiNoteTiming(const std::string& id, std::int64_t startTick, std::int64_t lengthTick);
    bool setMidiNotePitch(const std::string& id, int noteNumber);
    bool setMidiNoteVelocity(const std::string& id, int velocity);
    bool setMidiNoteChannel(const std::string& id, int channel);

    // createMarker 创建工程级时间线标记。标记只表达结构位置，不影响播放或片段调度。
    std::optional<TimelineMarker> createMarker(std::string name, std::int64_t tick);

    // insertExistingMarker 用于撤销重做或读取文件时恢复已有 ID，避免重做后引用失效。
    bool insertExistingMarker(const TimelineMarker& marker);
    bool removeMarkerById(const std::string& id);

    // renameMarkerById 和 moveMarkerToTick 只修改标记自身，不移动片段或轨道。
    bool renameMarkerById(const std::string& id, std::string name);
    bool moveMarkerToTick(const std::string& id, std::int64_t tick);

    // createTempoEvent 创建非默认速度事件。默认 tempo-1 始终留在 tick 0。
    std::optional<TempoEvent> createTempoEvent(std::int64_t tick, double beatsPerMinute);

    // insertExistingTempoEvent 用于撤销重做或读取文件时恢复已有 ID。
    bool insertExistingTempoEvent(const TempoEvent& event);
    bool removeTempoEventById(const std::string& id);

    // setTempoEventBpm 只修改 BPM；moveTempoEventToTick 只移动非默认速度事件。
    bool setTempoEventBpm(const std::string& id, double beatsPerMinute);
    bool moveTempoEventToTick(const std::string& id, std::int64_t tick);

    // tempoAtTick 和 tickToSeconds 提供确定性音乐时间换算，不读取实时音频状态。
    double tempoAtTick(std::int64_t tick) const;
    double tickToSeconds(std::int64_t tick) const;

    // createTimeSignatureEvent 创建非默认拍号事件。默认 meter-1 始终留在 tick 0。
    std::optional<TimeSignatureEvent> createTimeSignatureEvent(
        std::int64_t tick,
        int numerator,
        int denominator);

    // insertExistingTimeSignatureEvent 用于撤销重做或读取文件时恢复已有 ID。
    bool insertExistingTimeSignatureEvent(const TimeSignatureEvent& event);
    bool removeTimeSignatureEventById(const std::string& id);

    // setTimeSignature 只修改拍号值；moveTimeSignatureEventToTick 只移动非默认拍号事件。
    bool setTimeSignature(const std::string& id, int numerator, int denominator);
    bool moveTimeSignatureEventToTick(const std::string& id, std::int64_t tick);

    // timeSignatureAtTick 和 ticksPerMeasureAtTick 提供确定性小节结构查询，不读取实时音频状态。
    TimeSignatureEvent timeSignatureAtTick(std::int64_t tick) const;
    std::int64_t ticksPerMeasureAtTick(std::int64_t tick) const;

private:
    int formatVersion_ = currentFormatVersion;
    std::string name_;
    std::optional<PlaybackLoopRange> playbackLoopRange_;
    std::vector<Track> tracks_;
    std::vector<TimelineClip> clips_;
    std::vector<TimelineMarker> markers_;
    std::vector<TempoEvent> tempoEvents_;
    std::vector<TimeSignatureEvent> timeSignatureEvents_;
    int nextTrackNumber_ = 1;
    int nextClipNumber_ = 1;
    int nextMarkerNumber_ = 1;
    int nextTempoNumber_ = 1;
    int nextTimeSignatureNumber_ = 1;
    int nextMidiNoteNumber_ = 1;

    // 读取旧轨道 ID 后推进计数器，避免下一次新建轨道撞上已有 ID。
    void observeTrackId(const std::string& id);

    // 读取旧片段 ID 后推进计数器，避免下一次新建片段撞上已有 ID。
    void observeClipId(const std::string& id);

    // 读取旧音符 ID 后推进计数器，避免下一次新建音符撞上已有 ID。
    void observeMidiNoteId(const std::string& id);

    // 读取旧标记 ID 后推进计数器，避免下一次新建标记撞上已有 ID。
    void observeMarkerId(const std::string& id);

    // 读取旧速度事件 ID 后推进计数器，避免下一次新建速度事件撞上已有 ID。
    void observeTempoEventId(const std::string& id);

    // 读取旧拍号事件 ID 后推进计数器，避免下一次新建拍号事件撞上已有 ID。
    void observeTimeSignatureEventId(const std::string& id);
};

std::string toString(TrackType type);
std::optional<TrackType> trackTypeFromString(const std::string& value);
std::string toString(ClipType type);
std::optional<ClipType> clipTypeFromString(const std::string& value);
bool isValidTrackMixState(TrackMixState state);
bool isValidTrackViewState(TrackType type, TrackViewState state);
bool isValidClipTiming(std::int64_t startTick, std::int64_t lengthTick);
bool isValidMidiNoteValues(const MidiNoteEvent& note);
bool isValidMarkerTick(std::int64_t tick);
bool isValidTempoBpm(double beatsPerMinute);
bool isValidTimeSignature(int numerator, int denominator);

}
