#pragma once

#include "ProjectPlaybackSession.h"
#include "Transport.h"

#include <cstdint>
#include <string>
#include <vector>

namespace trackloom {

// PlaybackControlResult 是播放控制命令的统一返回值。
// transportControl 和 midiOutputRebuild 保留底层细节，方便 UI 或日志解释失败原因。
struct PlaybackControlResult {
    bool success = false;
    std::string message;
    ProjectPlaybackControlResult transportControl;
    ProjectPlaybackMidiOutputRebuildResult midiOutputRebuild;
};

// PlaybackControlCommand 是运行态播放控制命令，不进入工程撤销栈。
// 它面向 UI、快捷键、设备层和 AI 工具，统一调用 ProjectPlaybackSession 的安全入口。
class PlaybackControlCommand {
public:
    virtual ~PlaybackControlCommand() = default;

    virtual std::string name() const = 0;
    virtual PlaybackControlResult execute(
        ProjectPlaybackSession& session,
        Transport& transport,
        const Project& project) const = 0;
};

// StopPlaybackCommand 先释放活动 MIDI 音符，再停止播放头。
class StopPlaybackCommand final : public PlaybackControlCommand {
public:
    explicit StopPlaybackCommand(int releaseSampleOffset);

    std::string name() const override;
    PlaybackControlResult execute(
        ProjectPlaybackSession& session,
        Transport& transport,
        const Project& project) const override;

private:
    int releaseSampleOffset_ = 0;
};

// SeekPlaybackCommand 先释放活动 MIDI 音符，再移动播放头。
class SeekPlaybackCommand final : public PlaybackControlCommand {
public:
    SeekPlaybackCommand(std::int64_t targetSample, int releaseSampleOffset);

    std::string name() const override;
    PlaybackControlResult execute(
        ProjectPlaybackSession& session,
        Transport& transport,
        const Project& project) const override;

private:
    std::int64_t targetSample_ = 0;
    int releaseSampleOffset_ = 0;
};

// RebuildMidiOutputCommand 先通过旧路由释放活动音符，再应用新的 MIDI 输出绑定。
class RebuildMidiOutputCommand final : public PlaybackControlCommand {
public:
    RebuildMidiOutputCommand(
        std::vector<MidiTrackReceiverBinding> bindings,
        int releaseSampleOffset);

    std::string name() const override;
    PlaybackControlResult execute(
        ProjectPlaybackSession& session,
        Transport& transport,
        const Project& project) const override;

private:
    std::vector<MidiTrackReceiverBinding> bindings_;
    int releaseSampleOffset_ = 0;
};

}
