#pragma once

#include "Project.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace trackloom {

// CommandResult 让命令明确报告成功或失败原因。
// 失败结果必须在修改 Project 前返回，避免留下半完成状态。
struct CommandResult {
    bool success = false;
    std::string message;

    static CommandResult ok();
    static CommandResult fail(std::string message);
};

// Command 是所有工程修改的统一入口。
// 这个接口以后会同时服务用户操作和 AI 操作，所以必须支持验证、执行和撤销。
class Command {
public:
    virtual ~Command() = default;

    virtual std::string name() const = 0;
    virtual CommandResult validate(const Project& project) const = 0;
    virtual CommandResult execute(Project& project) = 0;
    virtual void undo(Project& project) = 0;
};

// CommandStack 维护撤销栈和重做栈。
// 执行新命令后会清空重做栈，这是多数编辑器一致的用户预期。
class CommandStack {
public:
    CommandResult execute(Project& project, std::unique_ptr<Command> command);
    bool undo(Project& project);
    bool redo(Project& project);

    bool canUndo() const;
    bool canRedo() const;

private:
    std::vector<std::unique_ptr<Command>> undoStack_;
    std::vector<std::unique_ptr<Command>> redoStack_;
};

// AddTrackCommand 是第一条真实工程命令。
// 它保存创建出的 Track，因此撤销后再重做时能恢复同一个稳定 ID。
class AddTrackCommand final : public Command {
public:
    AddTrackCommand(std::string trackName, TrackType trackType);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string trackName_;
    TrackType trackType_;
    std::optional<Track> createdTrack_;
};

// RenameTrackCommand 修改轨道名称，并保存旧名称用于撤销。
// 轨道名称是用户、AI 和导入器都会展示的标识，因此必须进入统一命令历史。
class RenameTrackCommand final : public Command {
public:
    RenameTrackCommand(std::string trackId, std::string newName);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string trackId_;
    std::string newName_;
    std::optional<std::string> oldName_;
};

// DeleteTrackCommand 删除轨道及其片段，并保存完整状态用于撤销。
// 当前不删除外部素材文件；后续音频素材生命周期需要单独设计和测试。
class DeleteTrackCommand final : public Command {
public:
    explicit DeleteTrackCommand(std::string trackId);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string trackId_;
    std::optional<Track> deletedTrack_;
    std::optional<std::size_t> deletedTrackIndex_;
    std::vector<TimelineClip> deletedClips_;
};

// MoveTrackCommand 调整轨道顺序，并保存旧索引用于撤销。
// 它只移动轨道在列表中的位置，不改变轨道 ID、片段归属、路由或混音状态。
class MoveTrackCommand final : public Command {
public:
    MoveTrackCommand(std::string trackId, std::size_t targetIndex);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string trackId_;
    std::size_t targetIndex_ = 0;
    std::optional<std::size_t> oldIndex_;
};

// AddClipCommand 负责把新的时间线片段加入工程。
// 它保存创建出的 TimelineClip，因此撤销后再重做时能恢复同一个稳定 ID。
class AddClipCommand final : public Command {
public:
    AddClipCommand(
        std::string trackId,
        std::string clipName,
        ClipType clipType,
        std::int64_t startTick,
        std::int64_t lengthTick);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string trackId_;
    std::string clipName_;
    ClipType clipType_;
    std::int64_t startTick_ = 0;
    std::int64_t lengthTick_ = 0;
    std::optional<TimelineClip> createdClip_;
};

// RenameClipCommand 修改片段名称，并保存旧名称用于撤销。
// 片段名称是用户和 AI 都会读写的可见标识，必须通过命令系统进入历史记录。
class RenameClipCommand final : public Command {
public:
    RenameClipCommand(std::string clipId, std::string newName);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string clipId_;
    std::string newName_;
    std::optional<std::string> oldName_;
};

// SetClipTimingCommand 修改片段在音乐时间线上的起点和长度。
// 它只处理片段外壳的 tick 范围，不负责 MIDI 事件、音频素材或播放调度。
class SetClipTimingCommand final : public Command {
public:
    SetClipTimingCommand(std::string clipId, std::int64_t startTick, std::int64_t lengthTick);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string clipId_;
    std::int64_t startTick_ = 0;
    std::int64_t lengthTick_ = 0;
    std::optional<std::int64_t> oldStartTick_;
    std::optional<std::int64_t> oldLengthTick_;
};

// DeleteClipCommand 删除时间线片段外壳，并保存完整片段用于撤销。
// 当前片段还没有外部素材引用；后续如果加入音频文件或缓存，删除素材必须单独设计。
class DeleteClipCommand final : public Command {
public:
    explicit DeleteClipCommand(std::string clipId);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string clipId_;
    std::optional<TimelineClip> deletedClip_;
};

// MoveClipToTrackCommand 把片段移动到另一条兼容轨道，并保存旧轨道用于撤销。
// 它只改变片段归属，不改变片段时间、长度、类型或内容。
class MoveClipToTrackCommand final : public Command {
public:
    MoveClipToTrackCommand(std::string clipId, std::string targetTrackId);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string clipId_;
    std::string targetTrackId_;
    std::optional<std::string> oldTrackId_;
};

// SplitClipCommand 把一个片段外壳切成左右两段，并保存原始片段与右段用于撤销重做。
// MIDI 音符会按片段内相对 tick 分配到左右片段；跨切点音符当前先拒绝，避免隐式拆音。
class SplitClipCommand final : public Command {
public:
    SplitClipCommand(std::string clipId, std::int64_t splitTick);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string clipId_;
    std::int64_t splitTick_ = 0;
    std::optional<TimelineClip> originalClip_;
    std::optional<TimelineClip> rightClip_;
};

// DuplicateClipCommand 复制片段外壳到兼容轨道和指定起点，并保存新片段用于撤销重做。
// MIDI 音符会复制为新 ID；音频文件或素材引用复制规则会在后续阶段单独定义。
class DuplicateClipCommand final : public Command {
public:
    DuplicateClipCommand(std::string clipId, std::string targetTrackId, std::int64_t startTick);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string clipId_;
    std::string targetTrackId_;
    std::int64_t startTick_ = 0;
    std::optional<TimelineClip> createdClip_;
};

// TrimClipStartCommand 向内修剪片段左边界，并保存旧时间范围用于撤销。
class TrimClipStartCommand final : public Command {
public:
    TrimClipStartCommand(std::string clipId, std::int64_t startTick);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string clipId_;
    std::int64_t startTick_ = 0;
    std::optional<std::int64_t> oldStartTick_;
    std::optional<std::int64_t> oldLengthTick_;
};

// SetMidiClipStartKeepingNoteTimesCommand 修改 MIDI 片段左边界，并同步平移内部音符相对 tick。
// 目标是保持保留下来的音符绝对播放时间不变，同时让整个片头动作只占用一次撤销记录。
class SetMidiClipStartKeepingNoteTimesCommand final : public Command {
public:
    SetMidiClipStartKeepingNoteTimesCommand(
        std::string clipId,
        std::int64_t startTick,
        std::int64_t lengthTick);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string clipId_;
    std::int64_t startTick_ = 0;
    std::int64_t lengthTick_ = 0;
    std::optional<std::int64_t> oldStartTick_;
    std::optional<std::int64_t> oldLengthTick_;
    std::vector<MidiNoteEvent> oldMidiNotes_;
};

// TrimClipEndCommand 向内修剪片段右边界，并保存旧时间范围用于撤销。
class TrimClipEndCommand final : public Command {
public:
    TrimClipEndCommand(std::string clipId, std::int64_t endTick);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string clipId_;
    std::int64_t endTick_ = 0;
    std::optional<std::int64_t> oldStartTick_;
    std::optional<std::int64_t> oldLengthTick_;
};

// AddMidiNoteCommand 在 MIDI 片段里创建音符，并保存新音符用于撤销重做。
// 音符时间使用片段内相对 tick，片段移动时不需要重写音符事件。
class AddMidiNoteCommand final : public Command {
public:
    AddMidiNoteCommand(
        std::string clipId,
        std::int64_t startTick,
        std::int64_t lengthTick,
        int noteNumber,
        int velocity,
        int channel);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string clipId_;
    std::int64_t startTick_ = 0;
    std::int64_t lengthTick_ = 0;
    int noteNumber_ = 60;
    int velocity_ = 100;
    int channel_ = 1;
    std::optional<MidiNoteEvent> createdNote_;
};

// SetMidiNoteTimingCommand 只修改音符在片段内的相对起点和长度。
class SetMidiNoteTimingCommand final : public Command {
public:
    SetMidiNoteTimingCommand(std::string noteId, std::int64_t startTick, std::int64_t lengthTick);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string noteId_;
    std::int64_t startTick_ = 0;
    std::int64_t lengthTick_ = 0;
    std::optional<std::int64_t> oldStartTick_;
    std::optional<std::int64_t> oldLengthTick_;
};

// SetMidiNotePitchCommand 只修改 MIDI 音高，不改 velocity、channel 或时间。
class SetMidiNotePitchCommand final : public Command {
public:
    SetMidiNotePitchCommand(std::string noteId, int noteNumber);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string noteId_;
    int noteNumber_ = 60;
    std::optional<int> oldNoteNumber_;
};

// SetMidiNoteVelocityCommand 只修改 MIDI 力度，velocity 0 不作为发声音符保存。
class SetMidiNoteVelocityCommand final : public Command {
public:
    SetMidiNoteVelocityCommand(std::string noteId, int velocity);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string noteId_;
    int velocity_ = 100;
    std::optional<int> oldVelocity_;
};

// SetMidiNoteChannelCommand 只修改 MIDI 通道，范围保持 MIDI 1.0 的 1-16。
class SetMidiNoteChannelCommand final : public Command {
public:
    SetMidiNoteChannelCommand(std::string noteId, int channel);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string noteId_;
    int channel_ = 1;
    std::optional<int> oldChannel_;
};

// DeleteMidiNoteCommand 删除单个 MIDI 音符，并保存所属片段和完整音符用于撤销。
class DeleteMidiNoteCommand final : public Command {
public:
    explicit DeleteMidiNoteCommand(std::string noteId);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string noteId_;
    std::optional<std::string> owningClipId_;
    std::optional<MidiNoteEvent> deletedNote_;
};

// AddMarkerCommand 创建工程级时间线标记，并保存首次创建出的 ID 用于重做。
// 标记只表达歌曲结构位置，不改变播放、片段或轨道状态。
class AddMarkerCommand final : public Command {
public:
    AddMarkerCommand(std::string markerName, std::int64_t tick);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string markerName_;
    std::int64_t tick_ = 0;
    std::optional<TimelineMarker> createdMarker_;
};

// RenameMarkerCommand 修改标记显示名称，并保存旧名称用于撤销。
class RenameMarkerCommand final : public Command {
public:
    RenameMarkerCommand(std::string markerId, std::string newName);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string markerId_;
    std::string newName_;
    std::optional<std::string> oldName_;
};

// MoveMarkerCommand 修改标记在音乐时间线上的 tick，不移动任何片段内容。
class MoveMarkerCommand final : public Command {
public:
    MoveMarkerCommand(std::string markerId, std::int64_t tick);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string markerId_;
    std::int64_t tick_ = 0;
    std::optional<std::int64_t> oldTick_;
};

// DeleteMarkerCommand 删除标记时保存完整对象，撤销后能恢复相同 ID 和位置。
class DeleteMarkerCommand final : public Command {
public:
    explicit DeleteMarkerCommand(std::string markerId);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string markerId_;
    std::optional<TimelineMarker> deletedMarker_;
};

// AddTempoEventCommand 创建工程级速度事件，并保存首次创建出的 ID 用于重做。
// 速度事件影响音乐时间换算，但本阶段不直接驱动 Transport 或实时音频线程。
class AddTempoEventCommand final : public Command {
public:
    AddTempoEventCommand(std::int64_t tick, double beatsPerMinute);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::int64_t tick_ = 0;
    double beatsPerMinute_ = 120.0;
    std::optional<TempoEvent> createdEvent_;
};

// SetTempoEventBpmCommand 修改速度事件 BPM，并保存旧 BPM 用于撤销。
class SetTempoEventBpmCommand final : public Command {
public:
    SetTempoEventBpmCommand(std::string tempoId, double beatsPerMinute);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string tempoId_;
    double beatsPerMinute_ = 120.0;
    std::optional<double> oldBeatsPerMinute_;
};

// MoveTempoEventCommand 移动非默认速度事件；默认 tempo-1 必须固定在 tick 0。
class MoveTempoEventCommand final : public Command {
public:
    MoveTempoEventCommand(std::string tempoId, std::int64_t tick);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string tempoId_;
    std::int64_t tick_ = 0;
    std::optional<std::int64_t> oldTick_;
};

// DeleteTempoEventCommand 删除非默认速度事件，并保存完整事件用于撤销。
class DeleteTempoEventCommand final : public Command {
public:
    explicit DeleteTempoEventCommand(std::string tempoId);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string tempoId_;
    std::optional<TempoEvent> deletedEvent_;
};

// AddTimeSignatureEventCommand 创建工程级拍号事件，并保存首次创建出的 ID 用于重做。
// 拍号事件只影响小节结构查询，本阶段不驱动 Transport、UI 网格或实时音频线程。
class AddTimeSignatureEventCommand final : public Command {
public:
    AddTimeSignatureEventCommand(std::int64_t tick, int numerator, int denominator);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::int64_t tick_ = 0;
    int numerator_ = 4;
    int denominator_ = 4;
    std::optional<TimeSignatureEvent> createdEvent_;
};

// SetTimeSignatureCommand 修改拍号值，并保存旧分子和旧分母用于撤销。
class SetTimeSignatureCommand final : public Command {
public:
    SetTimeSignatureCommand(std::string eventId, int numerator, int denominator);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string eventId_;
    int numerator_ = 4;
    int denominator_ = 4;
    std::optional<int> oldNumerator_;
    std::optional<int> oldDenominator_;
};

// MoveTimeSignatureEventCommand 移动非默认拍号事件；默认 meter-1 必须固定在 tick 0。
class MoveTimeSignatureEventCommand final : public Command {
public:
    MoveTimeSignatureEventCommand(std::string eventId, std::int64_t tick);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string eventId_;
    std::int64_t tick_ = 0;
    std::optional<std::int64_t> oldTick_;
};

// DeleteTimeSignatureEventCommand 删除非默认拍号事件，并保存完整事件用于撤销。
class DeleteTimeSignatureEventCommand final : public Command {
public:
    explicit DeleteTimeSignatureEventCommand(std::string eventId);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string eventId_;
    std::optional<TimeSignatureEvent> deletedEvent_;
};

// SetTrackPlaybackStateCommand 修改轨道播放状态，并保存旧状态用于撤销。
// 静音、独奏和禁用属于播放开关，不负责表达音量或其他混音参数。
class SetTrackPlaybackStateCommand final : public Command {
public:
    SetTrackPlaybackStateCommand(std::string trackId, TrackPlaybackState newState);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string trackId_;
    TrackPlaybackState newState_;
    std::optional<TrackPlaybackState> oldState_;
};

// SetTrackViewStateCommand 修改轨道显示状态，并保存旧状态用于撤销。
// 它不改变播放、混音、片段归属或路由，避免把“隐藏”和“静音”混为一谈。
class SetTrackViewStateCommand final : public Command {
public:
    SetTrackViewStateCommand(std::string trackId, TrackViewState newState);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string trackId_;
    TrackViewState newState_;
    std::optional<TrackViewState> oldState_;
};

// SetTrackMixStateCommand 修改轨道混音状态，并保存旧状态用于撤销。
// 它与播放状态命令分开，避免把“音量为 0”和“轨道被静音”混为同一个概念。
class SetTrackMixStateCommand final : public Command {
public:
    SetTrackMixStateCommand(std::string trackId, TrackMixState newState);

    std::string name() const override;
    CommandResult validate(const Project& project) const override;
    CommandResult execute(Project& project) override;
    void undo(Project& project) override;

private:
    std::string trackId_;
    TrackMixState newState_;
    std::optional<TrackMixState> oldState_;
};

}
