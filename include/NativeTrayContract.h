#pragma once

#include "AppInstanceLock.h"
#include "TrayIconClient.h"

#include <functional>
#include <string>

// The ONE interaction contract for Kohiko's own tray icons (audio,
// network, Bluetooth - anything that is a Kohiko utility with a main
// window and a quick-actions menu):
//
//   Left click   -> open, or raise/restore, the utility's MAIN WINDOW.
//                   Never a second window: AppInstanceLock::LaunchOrRaise()
//                   raises the running instance, and a launch that is
//                   already starting is not repeated.
//   Right click  -> the compact context menu of quick actions.
//
// Before this every tool wired its own handlers, and they disagreed:
// the audio tray used left = main window / right = menu, while the
// network and Bluetooth trays had the two swapped. One helper, used by
// all of them, is what keeps them from drifting apart again.
//
// This is for Kohiko's OWN icons only. Third-party tray icons are never
// touched by it: their clicks go straight to the application that owns
// them, as the XEmbed protocol says.
namespace Kohiko
{

// What a left click does. Exposed on its own so tests can drive the
// exact handler a tray tool installs.
inline std::function<void()> NativeTrayOpenMainWindow(const std::string& appId)
{
    return [appId] { AppInstanceLock::LaunchOrRaise(appId, appId); };
}

inline void InstallNativeTrayContract(
    TrayIconClient& tray,
    const std::string& appId,
    std::function<void()> showMenu)
{
    tray.SetLeftClickHandler(NativeTrayOpenMainWindow(appId));

    // The menu runs its own short event loop and may cover (then
    // uncover) the icon; ask for a redraw once it has closed so the
    // icon is never left stale underneath where the popup was. (A
    // redraw only ever happens because something asked for one - this
    // adds no timer.)
    tray.SetRightClickHandler([&tray, showMenu = std::move(showMenu)]
    {
        showMenu();
        tray.RequestRedraw();
    });
}

}
