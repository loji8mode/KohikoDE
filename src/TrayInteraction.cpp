#include "TrayInteraction.h"

#include "AppInstanceLock.h"
#include "TrayIconClient.h"

#include <X11/Xlib.h>

namespace Kohiko
{

void TrayInteraction::OpenMainWindow(const Spec& spec)
{
    AppInstanceLock::LaunchOrRaise(spec.appId, spec.executable);
}

void TrayInteraction::ShowContextMenu(TrayIconClient& tray, const Spec& spec)
{
    PopupMenu menu;

    if (spec.buildMenu)
        spec.buildMenu(menu);

    Spec specCopy = spec;
    menu.AddItem(spec.openLabel, [specCopy] { OpenMainWindow(specCopy); });

    Window rootReturn, childReturn;
    int rootX, rootY, winX, winY;
    unsigned int mask;
    XQueryPointer(tray.GetDisplay(), tray.GetWindow(), &rootReturn, &childReturn, &rootX, &rootY, &winX, &winY, &mask);

    menu.Show(tray.GetDisplay(), tray.GetScreen(), tray.GetVisual(), tray.GetColormap(), tray.GetFont(), tray.Theme(), rootX, rootY);
}

void TrayInteraction::Install(TrayIconClient& tray, Spec spec)
{
    tray.SetLeftClickHandler([spec] { OpenMainWindow(spec); });
    tray.SetRightClickHandler([&tray, spec] { ShowContextMenu(tray, spec); });
}

}
