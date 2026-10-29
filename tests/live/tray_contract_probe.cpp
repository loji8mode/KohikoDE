// A native tray utility stand-in for the live tray tests - not part of Kohiko.
//
// Built from the REAL pieces the three native tray tools are built from:
// TrayIconClient (XEmbed docking, redraw), TrayInteraction (the
// left-click/right-click contract), PopupMenu, AppInstanceLock. Only the
// icon's artwork and the menu's actions are fakes.
//
//   tray_contract_probe --log FILE --app SCRIPT [--color RRGGBB] [--plain]
//
//   default  installs TrayInteraction (the native contract): left =
//            LaunchOrRaise(SCRIPT), right = a menu with "Quick action",
//            "Slow action" (blocks 700 ms inside its callback) and the
//            contract's final "Open ..." item.
//   --plain  behaves like a THIRD-PARTY tray icon: plain left/right
//            handlers that only log (no contract, no popup) - proves a
//            native popup/instance machinery does not sit in the way of
//            other XEmbed clients.
//
// Prints "CENTER=<rootX>,<rootY>" (the icon's centre on screen) once docked.

#include "TrayIconClient.h"
#include "TrayInteraction.h"
#include "UiPopupMenu.h"

#include <X11/Xlib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <ctime>
#include <unistd.h>

using namespace Kohiko;

static std::string g_log;

static void Log(const std::string& line)
{
    FILE* f = std::fopen(g_log.c_str(), "a");
    if (!f)
        return;
    std::fprintf(f, "%s\n", line.c_str());
    std::fclose(f);
}

int main(int argc, char** argv)
{
    std::string app;
    unsigned long color = 0xCC2222;
    bool plain = false;

    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--log") && i + 1 < argc) g_log = argv[++i];
        else if (!std::strcmp(argv[i], "--app") && i + 1 < argc) app = argv[++i];
        else if (!std::strcmp(argv[i], "--color") && i + 1 < argc) color = std::strtoul(argv[++i], nullptr, 16);
        else if (!std::strcmp(argv[i], "--plain")) plain = true;
    }

    TrayIconClient tray;
    if (!tray.Create(22))
        return 1;

    tray.SetDrawCallback([color](Display* display, Window window, GC gc, Visual*, Colormap, int size)
    {
        if (std::getenv("TRAYPROBE_TRACE"))
        {
            timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            Log("DRAW abs=" + std::to_string(ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6));
        }

        XSetForeground(display, gc, color);
        XFillRectangle(display, window, gc, 0, 0, size, size);
    });

    if (plain)
    {
        tray.SetLeftClickHandler([] { Log("PLAIN_LEFT"); });
        tray.SetRightClickHandler([] { Log("PLAIN_RIGHT"); });
    }
    else
    {
        TrayInteraction::Spec spec;
        spec.appId = "fakeapp";
        spec.executable = app;
        spec.openLabel = "Open Fake App";
        spec.buildMenu = [&tray](PopupMenu& menu)
        {
            menu.AddItem("Quick action", [] { Log("ACTION_QUICK"); });
            menu.AddItem("Slow action", []
            {
                // A slow action - the kind that used to be a synchronous
                // BlueZ/NetworkManager call. The test observes the
                // desktop from ANOTHER client while this sleeps.
                Log("ACTION_SLOW_BEGIN");
                usleep(700 * 1000);
                Log("ACTION_SLOW_END");
            });
            menu.AddSeparator();
        };

        TrayInteraction::Install(tray, spec);
    }

    // Report where the icon is - and again whenever the bar moves it (the
    // tray lays its icons out after they dock, and again when another
    // one joins), so the LAST "CENTER=" line is always the current spot.
    tray.SetInterval(100, [&tray]
    {
        static int lastX = -1, lastY = -1;

        Display* display = tray.GetDisplay();
        Window root = DefaultRootWindow(display);
        int rx = 0, ry = 0;
        Window child;

        XWindowAttributes attrs;
        if (!XGetWindowAttributes(display, tray.GetWindow(), &attrs) || attrs.map_state != IsViewable)
            return;

        XTranslateCoordinates(display, tray.GetWindow(), root, attrs.width / 2, attrs.height / 2, &rx, &ry, &child);

        if (rx <= 0 || rx >= DisplayWidth(display, DefaultScreen(display)) || (rx == lastX && ry == lastY))
            return;

        lastX = rx;
        lastY = ry;
        std::printf("CENTER=%d,%d\n", rx, ry);
        std::fflush(stdout);
    });

    tray.Run();
    return 0;
}
