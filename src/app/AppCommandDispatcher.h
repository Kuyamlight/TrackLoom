#pragma once

#include "AppMainMenu.h"

#include <cstddef>
#include <functional>

namespace trackloom {

// AppCommandKind 是应用层统一命令名称。
// 它比 JUCE 菜单 id 更容易测试，也能被后续快捷键、命令面板和 AI 工具复用。
enum class AppCommandKind {
    Unknown,
    NewProject,
    OpenProject,
    SaveProject,
    SaveProjectAs,
    UndoProject,
    RedoProject,
    AddInstrumentTrack,
    AddAudioTrack,
    AddFolderTrack,
    PlayProject,
    StopProject,
    RewindProject,
    OpenRecentProject
};

// AppCommandDispatchResultKind 描述分发过程的稳定结果。
// UI 可根据 kind 判断未知命令或缺失回调，不需要解析 message 文案。
enum class AppCommandDispatchResultKind {
    Executed,
    UnknownCommand,
    MissingHandler
};

struct AppCommandDispatchResult {
    bool executed = false;
    AppCommandDispatchResultKind kind = AppCommandDispatchResultKind::UnknownCommand;
    AppCommandKind command = AppCommandKind::Unknown;
    std::size_t recentProjectNumber = 0;
};

// AppCommandHandlers 是命令分发器能调用的具体动作集合。
// 分发器只负责“哪个命令调用哪个回调”，不直接保存工程、弹窗口或控制 JUCE。
struct AppCommandHandlers {
    std::function<void()> newProject;
    std::function<void()> openProject;
    std::function<void()> saveProject;
    std::function<void()> saveProjectAs;
    std::function<void()> undoProject;
    std::function<void()> redoProject;
    // 轨道创建回调复用 AppTrackActions；分发器不直接命名轨道或改工程。
    std::function<void()> addInstrumentTrack;
    std::function<void()> addAudioTrack;
    std::function<void()> addFolderTrack;
    std::function<void()> playProject;
    std::function<void()> stopProject;
    std::function<void()> rewindProject;
    std::function<void(std::size_t)> openRecentProject;
};

// dispatchAppCommand 解析稳定 command id，并执行已绑定的回调。
// 返回值让调用方知道命令是否真的执行；未知命令和缺失回调都不会静默成功。
AppCommandDispatchResult dispatchAppCommand(
    int commandId,
    const AppCommandHandlers& handlers);

}
