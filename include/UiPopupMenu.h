#pragma once

#include "Font.h"
#include "UiTheme.h"

#include <X11/Xlib.h>

#include <functional>
#include <string>
#include <vector>

namespace Kohiko
{

// A small override-redirect popup menu - what every tray widget's
// right-click (or, for the audio tray, left-click) quick menu is, and
// available to kohiko-audio/network/bluetooth themselves for any
// future right-click context menu. Runs its own short, self-contained
// modal loop (grabs the pointer/keyboard, processes X events itself
// until something dismisses it, then returns) rather than integrating
// into UiWindow's own poll() loop - a context menu is a brief, modal
// interaction nothing else needs to stay responsive during, exactly
// as blocking as the plain Xlib round-trips the rest of Kohiko
// already makes (see DBusClient's header comment for the same
// reasoning applied to D-Bus calls).
class PopupMenu
{
public:

    // `enabled=false` items are drawn muted and can't be clicked.
    // `onClick` is ignored for a separator.
    void AddItem(const std::string& label, std::function<void()> onClick, bool enabled = true);
    void AddSeparator();

    // Shows the menu with its top-left corner at (x, y) in root
    // window coordinates, and blocks until it's dismissed - by
    // clicking an item (which runs that item's onClick before
    // returning), clicking outside the menu, or pressing Escape.
    void Show(
        Display* display,
        int screen,
        Visual* visual,
        Colormap colormap,
        Font& font,
        const UiTheme& theme,
        int x,
        int y
    );

private:

    struct Item
    {
        std::string label;
        std::function<void()> onClick;
        bool enabled = true;
        bool separator = false;
    };

    std::vector<Item> m_items;
};

}
