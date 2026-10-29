#pragma once

#include "Types.h"

#include <string>

namespace Kohiko
{

enum class CommandType
{
    NoCommand,
    Exec,
    Close,
    ToggleFloating,
    ToggleFullscreen,
    ScratchpadToggle,
    Workspace,
    MoveToWorkspace,
    FocusDirection,

    // `move <left|right|up|down>` - swap the focused tiled window
    // with its neighbor in that direction; see Command::Parse().
    MoveDirection,

    // `focus next` / `focus prev` - cycle real focus through every
    // visible window on the current workspace, direction-agnostic;
    // see Command::Parse().
    FocusCycle,

    Rotate,
    Flip,
    Reload,
    Quit,
    LauncherToggle,
    NotepadToggle,

    // Open kohiko-choose-window for the focused window - the dialog
    // that sets its explicit window rule (workspace, side, mode).
    ChooseWindow,

    // `lock` - shows the native lock screen immediately, unless
    // lockscreen.after=never. See LockScreen.h; also triggered
    // automatically before Suspend (see PowerMenu's suspend callback)
    // when lockscreen.after is "suspend" or "always".
    Lock,

    // Multi-monitor: `focusmonitor <left|right|up|down|N>` and
    // `movetomonitor <left|right|up|down|N>` - see Command::Parse()
    // and WindowManager::FocusMonitorCommand()/
    // MoveFocusedToMonitorCommand() for what the argument means.
    FocusMonitor,
    MoveToMonitor,

    // Rebuilds the Launcher's cached application list and file index
    // from disk right now, without restarting Kohiko - see Launcher's
    // ReloadDesktopEntries() for details.
    LauncherReload
};

// A parsed, ready-to-run action.
//
// Config turns raw config/IPC text such as
//
//     bind = SUPER+Q close
//     bind = SUPER+RETURN exec terminal
//     bind = SUPER+SHIFT+1 movetoworkspace 1
//
// into one of these instead of passing strings all the way down to
// the input layer, so nothing downstream of parsing ever has to
// tokenize text again. The same parser backs `kohikoctl dispatch ...`.
struct Command
{
    CommandType type = CommandType::NoCommand;

    // Exec's program/command line for CommandType::Exec; the raw,
    // lower-cased argument word ("left"/"right"/"up"/"down"/a digit
    // string) for FocusMonitor/MoveToMonitor - see WindowManager::
    // FocusMonitorCommand() for why that one's left as text instead
    // of being parsed here (it can mean either a direction or a
    // 1-based monitor index, and only WindowManager's MonitorManager
    // handle actually knows which monitors exist to resolve it against).
    std::string stringArg;

    int intArg = 0;
    Direction directionArg = Direction::Left;

    static Command Parse(const std::string& text);
};

}
