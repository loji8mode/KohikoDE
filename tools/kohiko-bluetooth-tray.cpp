#include "AppInstanceLock.h"
#include "BluezClient.h"
#include "NotificationClient.h"
#include "TrayIconClient.h"
#include "TrayInteraction.h"
#include "UiIconCache.h"
#include "UiPopupMenu.h"

#include <X11/Xlib.h>

using namespace Kohiko;

namespace
{

std::string CurrentIcon(const BluezClient& bluez)
{
    if (!bluez.Available() || bluez.Adapters().empty() || !bluez.Adapters().front().powered)
        return "bluetooth-disabled";

    for (auto& device : bluez.Devices())
        if (device.connected)
            return "bluetooth-active";

    return "bluetooth";
}

// Drawn instead whenever the installed icon theme doesn't actually
// have CurrentIcon()'s name - see TrayIconClient::DrawText()'s own
// comment for why this can never itself fail to render something, and
// kohiko-audio-tray.cpp's own IconFallback for why this mirrors
// CurrentIcon()'s branching independently rather than trying to
// derive one from the other. Same glyph throughout - color alone
// carries the state here, same as this tray's real icon set does
// (bluetooth-disabled/bluetooth-active/bluetooth are otherwise the
// same glyph in most themes too).
struct IconFallback { std::string glyph; unsigned long color; };

IconFallback FallbackFor(const TrayIconClient& tray, const BluezClient& bluez)
{
    if (!bluez.Available() || bluez.Adapters().empty() || !bluez.Adapters().front().powered)
        return { "B", tray.Theme().muted };

    for (auto& device : bluez.Devices())
        if (device.connected)
            return { "B", tray.Theme().success };

    return { "B", tray.Theme().foreground };
}

}

int main()
{
    TrayIconClient tray;
    if (!tray.Create(22))
        return 1;

    BluezClient bluez;
    bluez.Connect();

    UiIconCache iconCache(tray.GetDisplay(), tray.GetVisual(), tray.GetColormap(),
        RootWindow(tray.GetDisplay(), tray.GetScreen()));

    bluez.SetChangeHandler([&] { tray.RequestRedraw(); });

    if (bluez.Available())
        tray.WatchFd(bluez.Fd(), [&] { bluez.Dispatch(); });

    tray.SetInterval(2000, [&]
    {
        if (!bluez.Available() && bluez.Connect())
            tray.WatchFd(bluez.Fd(), [&] { bluez.Dispatch(); });
    });

    tray.SetDrawCallback([&](Display* display, Window window, GC gc, Visual*, Colormap, int size)
    {
        // TrayIconClient::Redraw() has already cleared `window` to the
        // tray host's real, live background (ParentRelative, chained
        // all the way to the Bar's own `bar.background`) before
        // calling this - see that function's own comment for why
        // painting a solid fill of our own here would be wrong.
        Pixmap pixmap = None, mask = None;
        if (iconCache.Get(CurrentIcon(bluez), size - 4, pixmap, mask))
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
        else
        {
            IconFallback fallback = FallbackFor(tray, bluez);
            int textWidth = tray.GetFont().TextWidth(fallback.glyph);
            int baseline = (size + tray.GetFont().Ascent()) / 2;
            tray.DrawText((size - textWidth) / 2, baseline, fallback.glyph, fallback.color);
        }
    });

    // The one mouse contract of Kohiko's native tray utilities (see
    // TrayInteraction.h): left click = open/raise the Bluetooth window,
    // right click = this quick menu. The menu's actions are all
    // asynchronous: they return at once, the result is applied when
    // BlueZ answers, and a failure is shown as a desktop notification
    // instead of silently doing nothing.
    tray.WatchFd(bluez.OperationFd(), [&] { bluez.DispatchOperations(); });

    NotificationClient notifications;
    notifications.Connect();

    auto report = [&notifications](const OperationResult& result)
    {
        if (!result.ok && notifications.Available())
            notifications.Notify("Kohiko Bluetooth", "bluetooth", "Bluetooth", result.message, 8000);
    };

    TrayInteraction::Spec spec;
    spec.appId = "kohiko-bluetooth";
    spec.executable = "kohiko-bluetooth";
    spec.openLabel = "Open Bluetooth Settings";
    spec.buildMenu = [&bluez, report](PopupMenu& menu)
    {
        if (!bluez.Available() || bluez.Adapters().empty())
        {
            menu.AddItem("Bluetooth is not available", nullptr, false);
            return;
        }

        const BluetoothAdapter& adapter = bluez.Adapters().front();
        std::string adapterPath = adapter.objectPath;
        bool powered = adapter.powered;

        menu.AddItem(powered ? "Turn Bluetooth off" : "Turn Bluetooth on",
            [&bluez, adapterPath, powered, report] { bluez.SetAdapterPoweredAsync(adapterPath, !powered, report); });

        if (powered)
        {
            menu.AddSeparator();

            bool anyDevice = false;
            for (auto& device : bluez.Devices())
            {
                if (!device.paired)
                    continue;

                anyDevice = true;
                std::string label = (device.connected ? "\u2713 " : "   ") + device.name;
                std::string devicePath = device.objectPath;
                bool connected = device.connected;

                menu.AddItem(label, [&bluez, devicePath, connected, report]
                {
                    if (connected)
                        bluez.DisconnectDeviceAsync(devicePath, report);
                    else
                        bluez.ConnectDeviceAsync(devicePath, report);
                });
            }

            if (!anyDevice)
                menu.AddItem("No paired devices", nullptr, false);
        }

        menu.AddSeparator();
    };

    TrayInteraction::Install(tray, spec);

    tray.Run();
    return 0;
}
