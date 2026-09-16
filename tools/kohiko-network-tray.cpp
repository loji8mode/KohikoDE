#include "AppInstanceLock.h"
#include "NetworkManagerClient.h"
#include "TrayIconClient.h"
#include "UiIconCache.h"
#include "UiPopupMenu.h"

#include <X11/Xlib.h>

using namespace Kohiko;

namespace
{

std::string CurrentIcon(const NetworkManagerClient& nm)
{
    if (!nm.Available())
        return "network-offline";

    if (!nm.NetworkingEnabled())
        return "network-offline";

    const NetworkDevice* ethernet = nullptr;
    const NetworkDevice* wifi = nullptr;

    for (auto& device : nm.Devices())
    {
        if (device.kind == NetworkDeviceKind::Ethernet && device.connected)
            ethernet = &device;
        else if (device.kind == NetworkDeviceKind::WiFi)
            wifi = &device;
    }

    if (ethernet)
        return "network-wired";

    if (wifi && wifi->connected)
    {
        std::uint8_t s = wifi->activeSignalStrength;
        if (s >= 80) return "network-wireless-signal-excellent";
        if (s >= 55) return "network-wireless-signal-good";
        if (s >= 30) return "network-wireless-signal-ok";
        return "network-wireless-signal-weak";
    }

    return "network-wireless-offline";
}

}

int main()
{
    TrayIconClient tray;
    if (!tray.Create(22))
        return 1;

    NetworkManagerClient networkManager;
    networkManager.Connect();

    UiIconCache iconCache(tray.GetDisplay(), tray.GetVisual(), tray.GetColormap(),
        RootWindow(tray.GetDisplay(), tray.GetScreen()));

    networkManager.SetChangeHandler([&] { tray.RequestRedraw(); });

    if (networkManager.Available())
        tray.WatchFd(networkManager.Fd(), [&] { networkManager.Dispatch(); });

    tray.SetInterval(2000, [&]
    {
        if (!networkManager.Available() && networkManager.Connect())
            tray.WatchFd(networkManager.Fd(), [&] { networkManager.Dispatch(); });
    });

    tray.SetDrawCallback([&](Display* display, Window window, GC gc, Visual*, Colormap, int size)
    {
        XSetForeground(display, gc, tray.Theme().background);
        XFillRectangle(display, window, gc, 0, 0, size, size);

        Pixmap pixmap = None, mask = None;
        if (iconCache.Get(CurrentIcon(networkManager), size - 4, pixmap, mask))
        {
            int offset = (size - (size - 4)) / 2;
            if (mask != None)
            {
                XSetClipMask(display, gc, mask);
                XSetClipOrigin(display, gc, offset, offset);
            }
            XCopyArea(display, pixmap, window, gc, 0, 0, size - 4, size - 4, offset, offset);
            if (mask != None)
                XSetClipMask(display, gc, None);
        }
    });

    tray.SetRightClickHandler([]
    {
        AppInstanceLock::LaunchOrRaise("kohiko-network", "kohiko-network");
    });

    tray.SetLeftClickHandler([&]
    {
        PopupMenu menu;

        if (!networkManager.Available())
        {
            menu.AddItem("NetworkManager is not available", nullptr, false);
        }
        else
        {
            bool wirelessEnabled = networkManager.WirelessEnabled();
            menu.AddItem(wirelessEnabled ? "Turn Wi-Fi off" : "Turn Wi-Fi on",
                [&networkManager, wirelessEnabled] { networkManager.SetWirelessEnabled(!wirelessEnabled); });
            menu.AddSeparator();

            const NetworkDevice* wifiDevice = nullptr;
            for (auto& device : networkManager.Devices())
                if (device.kind == NetworkDeviceKind::WiFi) { wifiDevice = &device; break; }

            if (wifiDevice && wirelessEnabled)
            {
                int shown = 0;
                for (auto& ap : wifiDevice->accessPoints)
                {
                    if (shown++ >= 6)
                        break;

                    std::string label = (ap.isActiveConnection ? "\u2713 " : "   ") + ap.ssid;
                    std::string devicePath = wifiDevice->objectPath;
                    std::string apPath = ap.objectPath;
                    std::string ssid = ap.ssid;
                    bool secured = ap.secured;
                    bool isActive = ap.isActiveConnection;

                    menu.AddItem(label, [&networkManager, devicePath, apPath, ssid, secured, isActive]
                    {
                        if (isActive || secured)
                            return; // opening the full password prompt from the tray's quick menu isn't worth the complexity - open the app instead
                        networkManager.ConnectToAccessPoint(devicePath, apPath, ssid, "");
                    });
                }

                menu.AddSeparator();
            }
        }

        menu.AddItem("Open Network Settings", [] { AppInstanceLock::LaunchOrRaise("kohiko-network", "kohiko-network"); });

        Window rootReturn, childReturn;
        int rootX, rootY, winX, winY;
        unsigned int mask;
        XQueryPointer(tray.GetDisplay(), tray.GetWindow(), &rootReturn, &childReturn, &rootX, &rootY, &winX, &winY, &mask);

        menu.Show(tray.GetDisplay(), tray.GetScreen(), tray.GetVisual(), tray.GetColormap(), tray.GetFont(), tray.Theme(), rootX, rootY);
    });

    tray.Run();
    return 0;
}
