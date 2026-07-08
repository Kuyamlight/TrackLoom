#pragma once

#include "AppPlaybackActions.h"
#include "AppProjectSession.h"
#include "AppRecentProjects.h"

#include <optional>
#include <string>
#include <vector>

namespace trackloom {

// AppMainMenuCommand 是桌面主菜单里的稳定命令标识。
// JUCE 菜单、快捷键和后续 AI 工具可以共享这些 id，而不是各自发明编号。
enum class AppMainMenuCommand {
    NewProject = 1001,
    OpenProject = 1002,
    SaveProject = 1003,
    SaveProjectAs = 1004,
    UndoProject = 1051,
    RedoProject = 1052,
    PlayProject = 1101,
    StopProject = 1102,
    RewindProject = 1103,
    // 这三项只负责“创建轨道”，不依赖当前选中轨道或片段。
    AddInstrumentTrack = 1201,
    AddAudioTrack = 1202,
    AddFolderTrack = 1203,
    RenameSelectedInstrumentTrack = 1211,
    DeleteSelectedInstrumentTrack = 1212,
    MoveSelectedInstrumentTrackUp = 1213,
    MoveSelectedInstrumentTrackDown = 1214,
    ToggleSelectedInstrumentTrackMute = 1215,
    ToggleSelectedInstrumentTrackSolo = 1216,
    ToggleSelectedInstrumentTrackDisabled = 1217,
    ToggleSelectedInstrumentTrackHidden = 1218,
    DeleteSelectedAudioTrack = 1221,
    MoveSelectedAudioTrackUp = 1222,
    MoveSelectedAudioTrackDown = 1223,
    ToggleSelectedAudioTrackMute = 1224,
    ToggleSelectedAudioTrackSolo = 1225,
    ToggleSelectedAudioTrackDisabled = 1226,
    ToggleSelectedAudioTrackHidden = 1227,
    DeleteSelectedMidiClip = 1251,
    DeleteSelectedAudioClip = 1252,
    DuplicateSelectedMidiClip = 1253,
    DuplicateSelectedAudioClip = 1254,
    RenameSelectedMidiClip = 1255,
    RenameSelectedAudioClip = 1256,
    SplitSelectedMidiClip = 1257,
    SplitSelectedAudioClip = 1258,
    MoveSelectedMidiClipLeft = 1259,
    MoveSelectedMidiClipRight = 1260,
    MoveSelectedAudioClipLeft = 1261,
    MoveSelectedAudioClipRight = 1262,
    TrimSelectedMidiClipEnd = 1263,
    ExtendSelectedMidiClipEnd = 1264,
    TrimSelectedAudioClipEnd = 1265,
    ExtendSelectedAudioClipEnd = 1266,
    TrimSelectedMidiClipStart = 1267,
    ExtendSelectedMidiClipStart = 1268,
    TrimSelectedAudioClipStart = 1269,
    ExtendSelectedAudioClipStart = 1270,
    MoveSelectedMidiClipToTargetTrack = 1271,
    MoveSelectedAudioClipToTargetTrack = 1272,
    // 工具入口只打开本地 UI 状态，不直接修改工程。
    OpenCommandPalette = 1301,
    OpenShortcutStatus = 1302
};

struct AppMainMenuItem {
    int commandId = 0;
    bool separator = false;
    bool enabled = false;
    std::string label;
};

struct AppMainMenuGroup {
    std::string name;
    std::vector<AppMainMenuItem> items;
};

struct AppMainMenuStatus {
    std::vector<AppMainMenuGroup> groups;
};

struct AppMainMenuSelection {
    std::string selectedInstrumentTrackId;
    std::string selectedAudioTrackId;
    std::string selectedMidiClipId;
    std::string selectedAudioClipId;
};

// appMainMenuCommandId 把强类型命令转换成 JUCE PopupMenu 需要的正整数 id。
int appMainMenuCommandId(AppMainMenuCommand command);

// 最近工程是动态菜单项，编号来自用户可见的 1-based 最近工程行号。
int appMainMenuRecentProjectCommandId(std::size_t number);
std::optional<std::size_t> appMainMenuRecentProjectNumberFromCommandId(int commandId);

// describeAppMainMenu 只生成菜单快照，不执行命令。
// 菜单启用状态读取应用运行态；真正的新建、打开、播放仍由现有动作函数执行。
AppMainMenuStatus describeAppMainMenu(
    const AppProjectSession& session,
    const AppPlaybackController& playback,
    const AppRecentProjects& recentProjects);

// 带选择状态的重载用于桌面壳和命令面板暴露依赖目标轨道的菜单命令。
// 选择只作为启用状态输入；真正执行仍由 AppCommandDispatcher 调用已有 AppTrackActions。
AppMainMenuStatus describeAppMainMenu(
    const AppProjectSession& session,
    const AppPlaybackController& playback,
    const AppRecentProjects& recentProjects,
    const AppMainMenuSelection& selection);

}
