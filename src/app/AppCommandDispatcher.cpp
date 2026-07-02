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
    case AppMainMenuCommand::PlayProject:
        return AppCommandKind::PlayProject;
    case AppMainMenuCommand::StopProject:
        return AppCommandKind::StopProject;
    case AppMainMenuCommand::RewindProject:
        return AppCommandKind::RewindProject;
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
    case AppCommandKind::PlayProject:
        return runSimpleCommand(command, handlers.playProject);
    case AppCommandKind::StopProject:
        return runSimpleCommand(command, handlers.stopProject);
    case AppCommandKind::RewindProject:
        return runSimpleCommand(command, handlers.rewindProject);
    case AppCommandKind::OpenRecentProject:
    case AppCommandKind::Unknown:
        break;
    }

    return unknownCommand();
}

}
