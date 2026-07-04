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
    RenameSelectedInstrumentTrack,
    DeleteSelectedInstrumentTrack,
    MoveSelectedInstrumentTrackUp,
    MoveSelectedInstrumentTrackDown,
    ToggleSelectedInstrumentTrackMute,
    ToggleSelectedInstrumentTrackSolo,
    ToggleSelectedInstrumentTrackDisabled,
    ToggleSelectedInstrumentTrackHidden,
    DeleteSelectedAudioTrack,
    MoveSelectedAudioTrackUp,
    MoveSelectedAudioTrackDown,
    ToggleSelectedAudioTrackMute,
    ToggleSelectedAudioTrackSolo,
    ToggleSelectedAudioTrackDisabled,
    ToggleSelectedAudioTrackHidden,
    RenameSelectedMidiClip,
    RenameSelectedAudioClip,
    DeleteSelectedMidiClip,
    DeleteSelectedAudioClip,
    DuplicateSelectedMidiClip,
    DuplicateSelectedAudioClip,
    SplitSelectedMidiClip,
    SplitSelectedAudioClip,
    MoveSelectedMidiClipToTargetTrack,
    MoveSelectedAudioClipToTargetTrack,
    MoveSelectedMidiClipLeft,
    MoveSelectedMidiClipRight,
    TrimSelectedMidiClipEnd,
    ExtendSelectedMidiClipEnd,
    TrimSelectedMidiClipStart,
    ExtendSelectedMidiClipStart,
    MoveSelectedAudioClipLeft,
    MoveSelectedAudioClipRight,
    TrimSelectedAudioClipEnd,
    ExtendSelectedAudioClipEnd,
    TrimSelectedAudioClipStart,
    ExtendSelectedAudioClipStart,
    PlayProject,
    StopProject,
    RewindProject,
    OpenCommandPalette,
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
    std::function<void()> renameSelectedInstrumentTrack;
    std::function<void()> deleteSelectedInstrumentTrack;
    std::function<void()> moveSelectedInstrumentTrackUp;
    std::function<void()> moveSelectedInstrumentTrackDown;
    std::function<void()> toggleSelectedInstrumentTrackMute;
    std::function<void()> toggleSelectedInstrumentTrackSolo;
    std::function<void()> toggleSelectedInstrumentTrackDisabled;
    std::function<void()> toggleSelectedInstrumentTrackHidden;
    std::function<void()> deleteSelectedAudioTrack;
    std::function<void()> moveSelectedAudioTrackUp;
    std::function<void()> moveSelectedAudioTrackDown;
    std::function<void()> toggleSelectedAudioTrackMute;
    std::function<void()> toggleSelectedAudioTrackSolo;
    std::function<void()> toggleSelectedAudioTrackDisabled;
    std::function<void()> toggleSelectedAudioTrackHidden;
    std::function<void()> renameSelectedMidiClip;
    std::function<void()> renameSelectedAudioClip;
    std::function<void()> deleteSelectedMidiClip;
    std::function<void()> deleteSelectedAudioClip;
    std::function<void()> duplicateSelectedMidiClip;
    std::function<void()> duplicateSelectedAudioClip;
    std::function<void()> splitSelectedMidiClip;
    std::function<void()> splitSelectedAudioClip;
    std::function<void()> moveSelectedMidiClipToTargetTrack;
    std::function<void()> moveSelectedAudioClipToTargetTrack;
    std::function<void()> moveSelectedMidiClipLeft;
    std::function<void()> moveSelectedMidiClipRight;
    std::function<void()> trimSelectedMidiClipEnd;
    std::function<void()> extendSelectedMidiClipEnd;
    std::function<void()> trimSelectedMidiClipStart;
    std::function<void()> extendSelectedMidiClipStart;
    std::function<void()> moveSelectedAudioClipLeft;
    std::function<void()> moveSelectedAudioClipRight;
    std::function<void()> trimSelectedAudioClipEnd;
    std::function<void()> extendSelectedAudioClipEnd;
    std::function<void()> trimSelectedAudioClipStart;
    std::function<void()> extendSelectedAudioClipStart;
    std::function<void()> playProject;
    std::function<void()> stopProject;
    std::function<void()> rewindProject;
    std::function<void()> openCommandPalette;
    std::function<void(std::size_t)> openRecentProject;
};

// dispatchAppCommand 解析稳定 command id，并执行已绑定的回调。
// 返回值让调用方知道命令是否真的执行；未知命令和缺失回调都不会静默成功。
AppCommandDispatchResult dispatchAppCommand(
    int commandId,
    const AppCommandHandlers& handlers);

}
