#include "Command.h"

#include "Utils.h"

#include <sstream>

namespace Kohiko
{

Command Command::Parse(const std::string& text)
{
    Command command;

    std::istringstream stream(Utils::Trim(text));

    std::string action;
    stream >> action;
    action = Utils::Lower(action);

    if (action.empty())
        return command;

    if (action == "exec")
    {
        command.type = CommandType::Exec;

        std::string rest;
        std::getline(stream, rest);
        command.stringArg = Utils::Trim(rest);
    }
    else if (action == "close" || action == "kill")
    {
        command.type = CommandType::Close;
    }
    else if (action == "toggle_floating" || action == "togglefloating")
    {
        command.type = CommandType::ToggleFloating;
    }
    else if (action == "fullscreen" || action == "togglefullscreen")
    {
        command.type = CommandType::ToggleFullscreen;
    }
    else if (action == "scratchpad_toggle" || action == "scratchpad")
    {
        command.type = CommandType::ScratchpadToggle;
    }
    else if (action == "workspace")
    {
        command.type = CommandType::Workspace;
        stream >> command.intArg;
    }
    else if (action == "movetoworkspace")
    {
        command.type = CommandType::MoveToWorkspace;
        stream >> command.intArg;
    }
    else if (action == "focus")
    {
        std::string dir;
        stream >> dir;
        dir = Utils::Lower(dir);

        if (dir == "next" || dir == "prev" || dir == "previous")
        {
            command.type = CommandType::FocusCycle;
            command.intArg = (dir == "next") ? 1 : -1;
        }
        else
        {
            command.type = CommandType::FocusDirection;

            if (dir == "left")       command.directionArg = Direction::Left;
            else if (dir == "right") command.directionArg = Direction::Right;
            else if (dir == "up")    command.directionArg = Direction::Up;
            else if (dir == "down")  command.directionArg = Direction::Down;
        }
    }
    else if (action == "move")
    {
        command.type = CommandType::MoveDirection;

        std::string dir;
        stream >> dir;
        dir = Utils::Lower(dir);

        if (dir == "left")       command.directionArg = Direction::Left;
        else if (dir == "right") command.directionArg = Direction::Right;
        else if (dir == "up")    command.directionArg = Direction::Up;
        else if (dir == "down")  command.directionArg = Direction::Down;
    }
    else if (action == "rotate")
    {
        command.type = CommandType::Rotate;
    }
    else if (action == "flip")
    {
        command.type = CommandType::Flip;
    }
    else if (action == "focusmonitor")
    {
        command.type = CommandType::FocusMonitor;

        std::string arg;
        stream >> arg;
        command.stringArg = Utils::Lower(arg);
    }
    else if (action == "movetomonitor")
    {
        command.type = CommandType::MoveToMonitor;

        std::string arg;
        stream >> arg;
        command.stringArg = Utils::Lower(arg);
    }
    else if (action == "reload")
    {
        command.type = CommandType::Reload;
    }
    else if (action == "quit" || action == "exit")
    {
        command.type = CommandType::Quit;
    }
    else if (action == "launcher_toggle" || action == "launcher")
    {
        command.type = CommandType::LauncherToggle;
    }
    else if (action == "notepad_toggle" || action == "notepad")
    {
        command.type = CommandType::NotepadToggle;
    }
    else if (action == "choose_window" || action == "choosewindow")
    {
        command.type = CommandType::ChooseWindow;

        // Optional scope: `choose_window` edits the rule for the whole
        // application (as always); `choose_window role` edits the rule
        // for just windows with the focused window's WM_WINDOW_ROLE,
        // `choose_window type` for its window type, `choose_window
        // role+type` for both. See WindowRuleStore / RuleScope.
        std::string scope;
        stream >> scope;
        command.stringArg = Utils::Lower(scope);
    }
    else if (action == "lock")
    {
        command.type = CommandType::Lock;
    }
    else if (action == "launcher_reload" || action == "reloadlauncher")
    {
        command.type = CommandType::LauncherReload;
    }

    return command;
}

}
