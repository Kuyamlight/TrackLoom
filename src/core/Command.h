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
// 当前只切分时间范围，不切分 MIDI 事件、音频文件或自动化数据。
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
// 当前只复制 TimelineClip 元数据，不复制 MIDI 事件、音频文件或素材引用。
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
