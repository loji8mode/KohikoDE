#pragma once

#include "UiPopupMenu.h"

#include <functional>
#include <string>

namespace Kohiko
{

class TrayIconClient;

// The ONE mouse contract of Kohiko's own tray utilities (audio, network,
// Bluetooth - and any native tray icon added later):
//
//   Left click   open the application's main control window - or, if it
//                is already open, raise it with its state exactly as it
//                was. Never a menu.
//   Right click  the compact quick-action menu: the utility's own few
//                actions, then "Open <application>" as the last item.
//
// Before this existed each tool wired its own two handlers, and they
// disagreed: audio did left = window / right = menu, network and
// Bluetooth did the opposite. Every native tray now installs the same
// pair through Install(), so the convention cannot drift again.
//
// What the contract guarantees beyond the button mapping:
//   * Opening the window goes through AppInstanceLock::LaunchOrRaise():
//     a running window is raised, an application that is still starting
//     is not started a second time, and at most one process ever holds
//     the instance lock.
//   * The menu is built when it is shown (current state) and an action
//     the user picks runs only after the menu is gone and its pointer
//     and keyboard grabs are released (PopupMenu::Show()). Actions must
//     not block - the native clients' *Async methods exist for this.
//   * Scroll and any other handlers a tool installs are untouched.
//   * Only native icons are affected. Third-party tray icons are
//     ordinary XEmbed clients that receive their clicks directly from
//     the X server; nothing here sits in their path.
class TrayInteraction
{
public:

    struct Spec
    {
        std::string appId;      // AppInstanceLock id of the main window, e.g. "kohiko-bluetooth"
        std::string executable; // what to launch if it is not running
        std::string openLabel;  // last menu item, e.g. "Open Bluetooth Settings"

        // Adds the utility's quick actions to the (empty) menu. May add
        // nothing. Called each time the menu is about to be shown.
        std::function<void(PopupMenu&)> buildMenu;
    };

    static void Install(TrayIconClient& tray, Spec spec);

    // Left-click behaviour, exposed so tests and tools can invoke the
    // exact code path the click handler runs.
    static void OpenMainWindow(const Spec& spec);

    // Right-click behaviour (builds and shows the menu at the pointer).
    static void ShowContextMenu(TrayIconClient& tray, const Spec& spec);
};

}
