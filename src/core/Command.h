#pragma once

#include "Project.h"

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
// 它保存创建出来的 Track，因此撤销后再重做时能恢复同一个稳定 ID。
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

}
