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

    void HandleKeyPress(
        const XKeyEvent& event
    );

    // True when this key event is one of the configured `bind=`
    // shortcuts - exactly the test HandleKeyPress() applies, without
    // running anything. A KeyPress for a grabbed combination reaches
    // Kohiko *because* it was grabbed globally (the X server delivers
    // it to the grabbing client instead of the focused window), so
    // this is also the question "was that a global shortcut, or text
    // typed into the focused Launcher?" - answered from the user's own
    // configuration rather than from a list of special keys.
    bool IsGlobalShortcut(
        const XKeyEvent& event
    ) const;

private:

    struct Binding
    {
        unsigned int modifiers = 0;
        KeyCode keycode = 0;
        Command command;
    };

    const Binding* FindBinding(
        const XKeyEvent& event
    ) const;

    KeySym ResolveKeysym(
        const std::string& token
    ) const;

private:

    XConnection& m_connection;
    WindowManager& m_windowManager;

    std::vector<Binding> m_bindings;

};

}
