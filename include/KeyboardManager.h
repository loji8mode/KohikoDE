#pragma once

#include "Command.h"

#include <X11/Xlib.h>

#include <string>
#include <vector>

namespace Kohiko
{

class XConnection;
class WindowManager;
class Config;

// Turns Config's `bind=` entries into grabbed keys, and grabbed
// KeyPress events back into Commands - no other logic lives here, as
// the spec asks for ("ніякої логіки").
class KeyboardManager
{
public:

    KeyboardManager(
        XConnection& connection,
        WindowManager& windowManager
    );

    // Parses every `bind=` entry and grabs each one (with NumLock/
    // CapsLock variants) on the root window. Safe to call again later
    // (e.g. on `reload`) - it ungrabs everything first.
    void Configure(
        const Config& config
    );

    // Runs the command bound to `event`'s key combination, if any.
    // Returns true if a configured binding matched (and was run),
    // false if this key press is not a shortcut at all - which is
    // what lets a modal (the Launcher, the Notepad) decide whether a
    // key is for it: a combination the user bound is a global
    // shortcut wherever focus is; anything else is ordinary input.
    bool HandleKeyPress(
        const XKeyEvent& event
    );

private:

    struct Binding
    {
        unsigned int modifiers = 0;
        KeyCode keycode = 0;
        Command command;
    };

    KeySym ResolveKeysym(
        const std::string& token
    ) const;

private:

    XConnection& m_connection;
    WindowManager& m_windowManager;

    std::vector<Binding> m_bindings;

};

}
