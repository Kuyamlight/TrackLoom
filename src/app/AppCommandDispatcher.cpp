#include "AppCommandDispatcher.h"

namespace trackloom {
namespace {

AppCommandDispatchResult unknownCommand()
{
    return {};
}

AppCommandDispatchResult missingHandler(AppCommandKind command, std::size_t recentProjectNumber = 0)
{
    AppCommandDispatchResult result;
    result.kind = AppCommandDispatchResultKind::MissingHandler;
    result.command = command;
    result.recentProjectNumber = recentProjectNumber;
    return result;
}

AppCommandDispatchResult executed(AppCommandKind command, std::size_t recentProjectNumber = 0)
{
    AppCommandDispatchResult result;
    result.executed = true;
    result.kind = AppCommandDispatchResultKind::Executed;
    result.command = command;
    result.recentProjectNumber = recentProjectNumber;
    return result;
}

AppCommandKind appCommandKindFromMainMenuCommand(AppMainMenuCommand command)
{
    switch (command) {
    case AppMainMenuCommand::NewProject:
        return AppCommandKind::NewProject;
    case AppMainMenuCommand::OpenProject:
        return AppCommandKind::OpenProject;
    case AppMainMenuCommand::SaveProject:
        return AppCommandKind::SaveProject;
    case AppMainMenuCommand::SaveProjectAs:
        return AppCommandKind::SaveProjectAs;
    case AppMainMenuCommand::UndoProject:
        return AppCommandKind::UndoProject;
    case AppMainMenuCommand::RedoProject:
        return AppCommandKind::RedoProject;
    case AppMainMenuCommand::AddInstrumentTrack:
        return AppCommandKind::AddInstrumentTrack;
    case AppMainMenuCommand::AddAudioTrack:
        return AppCommandKind::AddAudioTrack;
    case AppMainMenuCommand::AddFolderTrack:
        return AppCommandKind::AddFolderTrack;
    case AppMainMenuCommand::RenameSelectedInstrumentTrack:
        return AppCommandKind::RenameSelectedInstrumentTrack;
    case AppMainMenuCommand::DeleteSelectedInstrumentTrack:
        return AppCommandKind::DeleteSelectedInstrumentTrack;
    case AppMainMenuCommand::MoveSelectedInstrumentTrackUp:
        return AppCommandKind::MoveSelectedInstrumentTrackUp;
    case AppMainMenuCommand::MoveSelectedInstrumentTrackDown:
        return AppCommandKind::MoveSelectedInstrumentTrackDown;
    case AppMainMenuCommand::ToggleSelectedInstrumentTrackMute:
        return AppCommandKind::ToggleSelectedInstrumentTrackMute;
    case AppMainMenuCommand::ToggleSelectedInstrumentTrackSolo:
        return AppCommandKind::ToggleSelectedInstrumentTrackSolo;
    case AppMainMenuCommand::ToggleSelectedInstrumentTrackDisabled:
        return AppCommandKind::ToggleSelectedInstrumentTrackDisabled;
    case AppMainMenuCommand::ToggleSelectedInstrumentTrackHidden:
        return AppCommandKind::ToggleSelectedInstrumentTrackHidden;
    case AppMainMenuCommand::DeleteSelectedAudioTrack:
        return AppCommandKind::DeleteSelectedAudioTrack;
    case AppMainMenuCommand::MoveSelectedAudioTrackUp:
        return AppCommandKind::MoveSelectedAudioTrackUp;
    case AppMainMenuCommand::MoveSelectedAudioTrackDown:
        return AppCommandKind::MoveSelectedAudioTrackDown;
    case AppMainMenuCommand::ToggleSelectedAudioTrackMute:
        return AppCommandKind::ToggleSelectedAudioTrackMute;
    case AppMainMenuCommand::ToggleSelectedAudioTrackSolo:
        return AppCommandKind::ToggleSelectedAudioTrackSolo;
    case AppMainMenuCommand::ToggleSelectedAudioTrackDisabled:
        return AppCommandKind::ToggleSelectedAudioTrackDisabled;
    case AppMainMenuCommand::ToggleSelectedAudioTrackHidden:
        return AppCommandKind::ToggleSelectedAudioTrackHidden;
    case AppMainMenuCommand::RenameSelectedMidiClip:
        return AppCommandKind::RenameSelectedMidiClip;
    case AppMainMenuCommand::RenameSelectedAudioClip:
        return AppCommandKind::RenameSelectedAudioClip;
    case AppMainMenuCommand::DeleteSelectedMidiClip:
        return AppCommandKind::DeleteSelectedMidiClip;
    case AppMainMenuCommand::DeleteSelectedAudioClip:
        return AppCommandKind::DeleteSelectedAudioClip;
    case AppMainMenuCommand::DuplicateSelectedMidiClip:
        return AppCommandKind::DuplicateSelectedMidiClip;
    case AppMainMenuCommand::DuplicateSelectedAudioClip:
        return AppCommandKind::DuplicateSelectedAudioClip;
    case AppMainMenuCommand::SplitSelectedMidiClip:
        return AppCommandKind::SplitSelectedMidiClip;
    case AppMainMenuCommand::SplitSelectedAudioClip:
        return AppCommandKind::SplitSelectedAudioClip;
    case AppMainMenuCommand::MoveSelectedMidiClipToTargetTrack:
        return AppCommandKind::MoveSelectedMidiClipToTargetTrack;
    case AppMainMenuCommand::MoveSelectedAudioClipToTargetTrack:
        return AppCommandKind::MoveSelectedAudioClipToTargetTrack;
    case AppMainMenuCommand::MoveSelectedMidiClipLeft:
        return AppCommandKind::MoveSelectedMidiClipLeft;
    case AppMainMenuCommand::MoveSelectedMidiClipRight:
        return AppCommandKind::MoveSelectedMidiClipRight;
    case AppMainMenuCommand::TrimSelectedMidiClipEnd:
        return AppCommandKind::TrimSelectedMidiClipEnd;
    case AppMainMenuCommand::ExtendSelectedMidiClipEnd:
        return AppCommandKind::ExtendSelectedMidiClipEnd;
    case AppMainMenuCommand::TrimSelectedMidiClipStart:
        return AppCommandKind::TrimSelectedMidiClipStart;
    case AppMainMenuCommand::ExtendSelectedMidiClipStart:
        return AppCommandKind::ExtendSelectedMidiClipStart;
    case AppMainMenuCommand::MoveSelectedAudioClipLeft:
        return AppCommandKind::MoveSelectedAudioClipLeft;
    case AppMainMenuCommand::MoveSelectedAudioClipRight:
        return AppCommandKind::MoveSelectedAudioClipRight;
    case AppMainMenuCommand::TrimSelectedAudioClipEnd:
        return AppCommandKind::TrimSelectedAudioClipEnd;
    case AppMainMenuCommand::ExtendSelectedAudioClipEnd:
        return AppCommandKind::ExtendSelectedAudioClipEnd;
    case AppMainMenuCommand::TrimSelectedAudioClipStart:
        return AppCommandKind::TrimSelectedAudioClipStart;
    case AppMainMenuCommand::ExtendSelectedAudioClipStart:
        return AppCommandKind::ExtendSelectedAudioClipStart;
    case AppMainMenuCommand::PlayProject:
        return AppCommandKind::PlayProject;
    case AppMainMenuCommand::StopProject:
        return AppCommandKind::StopProject;
    case AppMainMenuCommand::RewindProject:
        return AppCommandKind::RewindProject;
    case AppMainMenuCommand::OpenCommandPalette:
        return AppCommandKind::OpenCommandPalette;
    case AppMainMenuCommand::OpenShortcutStatus:
        return AppCommandKind::OpenShortcutStatus;
    }

    return AppCommandKind::Unknown;
}

template <typename Callback>
AppCommandDispatchResult runSimpleCommand(
    AppCommandKind command,
    const Callback& callback)
{
    if (!callback) {
        return missingHandler(command);
    }

    callback();
    return executed(command);
}

}

AppCommandDispatchResult dispatchAppCommand(
    int commandId,
    const AppCommandHandlers& handlers)
{
    if (const auto recentProjectNumber = appMainMenuRecentProjectNumberFromCommandId(commandId)) {
        if (!handlers.openRecentProject) {
            return missingHandler(AppCommandKind::OpenRecentProject, *recentProjectNumber);
        }

        handlers.openRecentProject(*recentProjectNumber);
        return executed(AppCommandKind::OpenRecentProject, *recentProjectNumber);
    }

    const auto command = appCommandKindFromMainMenuCommand(static_cast<AppMainMenuCommand>(commandId));
    switch (command) {
    case AppCommandKind::NewProject:
        return runSimpleCommand(command, handlers.newProject);
    case AppCommandKind::OpenProject:
        return runSimpleCommand(command, handlers.openProject);
    case AppCommandKind::SaveProject:
        return runSimpleCommand(command, handlers.saveProject);
    case AppCommandKind::SaveProjectAs:
        return runSimpleCommand(command, handlers.saveProjectAs);
    case AppCommandKind::UndoProject:
        // 撤销/重做也走同一个分发器，后续快捷键或命令面板就不会再复制一套执行规则。
        return runSimpleCommand(command, handlers.undoProject);
    case AppCommandKind::RedoProject:
        return runSimpleCommand(command, handlers.redoProject);
    case AppCommandKind::AddInstrumentTrack:
        // 轨道创建也只分发到应用层动作；命名、dirty 和撤销历史继续由 AppTrackActions 处理。
        return runSimpleCommand(command, handlers.addInstrumentTrack);
    case AppCommandKind::AddAudioTrack:
        return runSimpleCommand(command, handlers.addAudioTrack);
    case AppCommandKind::AddFolderTrack:
        return runSimpleCommand(command, handlers.addFolderTrack);
    case AppCommandKind::RenameSelectedInstrumentTrack:
        return runSimpleCommand(command, handlers.renameSelectedInstrumentTrack);
    case AppCommandKind::DeleteSelectedInstrumentTrack:
        return runSimpleCommand(command, handlers.deleteSelectedInstrumentTrack);
    case AppCommandKind::MoveSelectedInstrumentTrackUp:
        return runSimpleCommand(command, handlers.moveSelectedInstrumentTrackUp);
    case AppCommandKind::MoveSelectedInstrumentTrackDown:
        return runSimpleCommand(command, handlers.moveSelectedInstrumentTrackDown);
    case AppCommandKind::ToggleSelectedInstrumentTrackMute:
        return runSimpleCommand(command, handlers.toggleSelectedInstrumentTrackMute);
    case AppCommandKind::ToggleSelectedInstrumentTrackSolo:
        return runSimpleCommand(command, handlers.toggleSelectedInstrumentTrackSolo);
    case AppCommandKind::ToggleSelectedInstrumentTrackDisabled:
        return runSimpleCommand(command, handlers.toggleSelectedInstrumentTrackDisabled);
    case AppCommandKind::ToggleSelectedInstrumentTrackHidden:
        return runSimpleCommand(command, handlers.toggleSelectedInstrumentTrackHidden);
    case AppCommandKind::DeleteSelectedAudioTrack:
        return runSimpleCommand(command, handlers.deleteSelectedAudioTrack);
    case AppCommandKind::MoveSelectedAudioTrackUp:
        return runSimpleCommand(command, handlers.moveSelectedAudioTrackUp);
    case AppCommandKind::MoveSelectedAudioTrackDown:
        return runSimpleCommand(command, handlers.moveSelectedAudioTrackDown);
    case AppCommandKind::ToggleSelectedAudioTrackMute:
        return runSimpleCommand(command, handlers.toggleSelectedAudioTrackMute);
    case AppCommandKind::ToggleSelectedAudioTrackSolo:
        return runSimpleCommand(command, handlers.toggleSelectedAudioTrackSolo);
    case AppCommandKind::ToggleSelectedAudioTrackDisabled:
        return runSimpleCommand(command, handlers.toggleSelectedAudioTrackDisabled);
    case AppCommandKind::ToggleSelectedAudioTrackHidden:
        return runSimpleCommand(command, handlers.toggleSelectedAudioTrackHidden);
    case AppCommandKind::RenameSelectedMidiClip:
        return runSimpleCommand(command, handlers.renameSelectedMidiClip);
    case AppCommandKind::RenameSelectedAudioClip:
        return runSimpleCommand(command, handlers.renameSelectedAudioClip);
    case AppCommandKind::DeleteSelectedMidiClip:
        return runSimpleCommand(command, handlers.deleteSelectedMidiClip);
    case AppCommandKind::DeleteSelectedAudioClip:
        return runSimpleCommand(command, handlers.deleteSelectedAudioClip);
    case AppCommandKind::DuplicateSelectedMidiClip:
        return runSimpleCommand(command, handlers.duplicateSelectedMidiClip);
    case AppCommandKind::DuplicateSelectedAudioClip:
        return runSimpleCommand(command, handlers.duplicateSelectedAudioClip);
    case AppCommandKind::SplitSelectedMidiClip:
        return runSimpleCommand(command, handlers.splitSelectedMidiClip);
    case AppCommandKind::SplitSelectedAudioClip:
        return runSimpleCommand(command, handlers.splitSelectedAudioClip);
    case AppCommandKind::MoveSelectedMidiClipToTargetTrack:
        return runSimpleCommand(command, handlers.moveSelectedMidiClipToTargetTrack);
    case AppCommandKind::MoveSelectedAudioClipToTargetTrack:
        return runSimpleCommand(command, handlers.moveSelectedAudioClipToTargetTrack);
    case AppCommandKind::MoveSelectedMidiClipLeft:
        return runSimpleCommand(command, handlers.moveSelectedMidiClipLeft);
    case AppCommandKind::MoveSelectedMidiClipRight:
        return runSimpleCommand(command, handlers.moveSelectedMidiClipRight);
    case AppCommandKind::TrimSelectedMidiClipEnd:
        return runSimpleCommand(command, handlers.trimSelectedMidiClipEnd);
    case AppCommandKind::ExtendSelectedMidiClipEnd:
        return runSimpleCommand(command, handlers.extendSelectedMidiClipEnd);
    case AppCommandKind::TrimSelectedMidiClipStart:
        return runSimpleCommand(command, handlers.trimSelectedMidiClipStart);
    case AppCommandKind::ExtendSelectedMidiClipStart:
        return runSimpleCommand(command, handlers.extendSelectedMidiClipStart);
    case AppCommandKind::MoveSelectedAudioClipLeft:
        return runSimpleCommand(command, handlers.moveSelectedAudioClipLeft);
    case AppCommandKind::MoveSelectedAudioClipRight:
        return runSimpleCommand(command, handlers.moveSelectedAudioClipRight);
    case AppCommandKind::TrimSelectedAudioClipEnd:
        return runSimpleCommand(command, handlers.trimSelectedAudioClipEnd);
    case AppCommandKind::ExtendSelectedAudioClipEnd:
        return runSimpleCommand(command, handlers.extendSelectedAudioClipEnd);
    case AppCommandKind::TrimSelectedAudioClipStart:
        return runSimpleCommand(command, handlers.trimSelectedAudioClipStart);
    case AppCommandKind::ExtendSelectedAudioClipStart:
        return runSimpleCommand(command, handlers.extendSelectedAudioClipStart);
    case AppCommandKind::PlayProject:
        return runSimpleCommand(command, handlers.playProject);
    case AppCommandKind::StopProject:
        return runSimpleCommand(command, handlers.stopProject);
    case AppCommandKind::RewindProject:
        return runSimpleCommand(command, handlers.rewindProject);
    case AppCommandKind::OpenCommandPalette:
        return runSimpleCommand(command, handlers.openCommandPalette);
    case AppCommandKind::OpenShortcutStatus:
        return runSimpleCommand(command, handlers.openShortcutStatus);
    case AppCommandKind::OpenRecentProject:
    case AppCommandKind::Unknown:
        break;
    }

    return unknownCommand();
}

}
