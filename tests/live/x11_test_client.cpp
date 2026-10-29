// Minimal, configurable X11 client for Task 9's investigation
// (CHANGELOG.md's 0.20.4 entry) - not part of Kohiko itself, just a
// tool for constructing precise, real window-creation scenarios
// (a plain top-level window, a window carrying _XEMBED_INFO, a
// WM_TRANSIENT_FOR child, override-redirect, a chosen
// _NET_WM_WINDOW_TYPE) to run against the real WindowManager code
// rather than simulating or guessing at its behavior.
//
// Prints "WINDOW=0x<hex> PID=<pid>" on its own stdout once mapped, so
// a driving shell script can find it, then blocks handling events
// (Expose only - it just paints a solid color) until killed.
//
// Usage:
//   x11_test_client [options]
//     --title NAME            WM_NAME / _NET_WM_NAME (default "TestClient")
//     --class NAME            WM_CLASS class (default "TestClient")
//     --xembed                Set _XEMBED_INFO on this window (mimics
//                              a Qt system-tray-icon window)
//     --transient-for HEXID   Set WM_TRANSIENT_FOR to this window ID
//     --override-redirect     Create with override_redirect=True
//                              (mimics a Qt tooltip/menu popup)
//     --window-type ATOM      Set _NET_WM_WINDOW_TYPE to this atom
//                              name (e.g. _NET_WM_WINDOW_TYPE_DIALOG)
//     --width W / --height H  Window size (default 400x300)
//     --color r,g,b           Fill color, 0-255 each (default a
//                              distinct color per instance so
//                              screenshots are easy to tell apart)
//     --send-dock-request     After mapping, send a real
//                              SYSTEM_TRAY_REQUEST_DOCK for this window
//                              to whoever owns _NET_SYSTEM_TRAY_S<n>
//     --dock-delay-ms N       Wait N ms between the map and the dock
//                              request (default 0 - just the one X
//                              round trip the request itself needs).
//                              Lets a test place the dock request
//                              inside/outside the WM's grace window.
//     --set-pid               Set _NET_WM_PID to this process's pid, like a
//                              real application does. Needed to test
//                              anything that matches a window to the
//                              process that launched it (the
//                              workspace<N>= autostart placement).
//     --report-events         Print "EVENT=<MapNotify|UnmapNotify|
//                              ReparentNotify> parent=0x<id>" lines for
//                              this window, in arrival order - lets a
//                              test see whether a window was ever
//                              *mapped at root level* before being
//                              reparented into a tray (the "flash")
//     --instance NAME         WM_CLASS instance (default: same as --class)
//     --role ROLE             Set WM_WINDOW_ROLE (what Firefox/Zen/Qt/GTK
//                              apps use to tell their own window kinds
//                              apart, e.g. "browser" vs
//                              "PictureInPicture")
//     --map-delay-ms N        Create the window, set every property, then
//                              wait N ms before XMapWindow() - a slow-
//                              starting application (its window is
//                              created long after the process, and the
//                              WM only sees a MapRequest at the end)
//     --report-map-latency    Also print "MAP_LATENCY_MS=<x>" once the
//                              window actually gets mapped, measured
//                              from this client's own XMapWindow() to
//                              the MapNotify it receives (i.e. how long
//                              the window manager sat on it)

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unistd.h>

static double NowMs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char** argv)
{
    std::string title = "TestClient";
    std::string className = "TestClient";
    bool xembed = false;
    Window transientFor = 0;
    bool overrideRedirect = false;
    std::string windowType;
    int width = 400, height = 300;
    int r = 100, g = 120, b = 200;
    bool sendDockRequest = false;
    int dockDelayMs = 0;
    bool reportMapLatency = false;
    bool reportEvents = false;
    bool setPid = false;
    std::string instanceName;
    std::string role;
    int mapDelayMs = 0;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };

        if (arg == "--title") title = next();
        else if (arg == "--class") className = next();
        else if (arg == "--xembed") xembed = true;
        else if (arg == "--transient-for") transientFor = strtoul(next().c_str(), nullptr, 0);
        else if (arg == "--override-redirect") overrideRedirect = true;
        else if (arg == "--window-type") windowType = next();
        else if (arg == "--width") width = atoi(next().c_str());
        else if (arg == "--height") height = atoi(next().c_str());
        else if (arg == "--color")
        {
            std::string c = next();
            sscanf(c.c_str(), "%d,%d,%d", &r, &g, &b);
        }
        else if (arg == "--send-dock-request") sendDockRequest = true;
        else if (arg == "--dock-delay-ms") dockDelayMs = atoi(next().c_str());
        else if (arg == "--report-map-latency") reportMapLatency = true;
        else if (arg == "--report-events") reportEvents = true;
        else if (arg == "--set-pid") setPid = true;
        else if (arg == "--instance") instanceName = next();
        else if (arg == "--role") role = next();
        else if (arg == "--map-delay-ms") mapDelayMs = atoi(next().c_str());
    }

    Display* display = XOpenDisplay(nullptr);
    if (!display)
    {
        std::fprintf(stderr, "x11_test_client: could not open display\n");
        return 1;
    }

    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);

    XSetWindowAttributes attrs{};
    attrs.background_pixel = (r << 16) | (g << 8) | b;
    attrs.override_redirect = overrideRedirect ? True : False;
    attrs.event_mask = ExposureMask | StructureNotifyMask;

    unsigned long valueMask = CWBackPixel | CWOverrideRedirect | CWEventMask;

    Window win = XCreateWindow(
        display, root, 100, 100, width, height, 0,
        CopyFromParent, InputOutput, CopyFromParent,
        valueMask, &attrs
    );

    XStoreName(display, win, title.c_str());

    XClassHint classHint{};
    classHint.res_name = const_cast<char*>(instanceName.empty() ? className.c_str() : instanceName.c_str());
    classHint.res_class = const_cast<char*>(className.c_str());
    XSetClassHint(display, win, &classHint);

    // WM_HINTS with a real, non-null input flag - matching what a
    // genuine toolkit-created top-level window sets (a WM checking
    // "does this window even want input" is a separate, real
    // consideration this client shouldn't accidentally leave
    // ambiguous by omission).
    XWMHints wmHints{};
    wmHints.flags = InputHint;
    wmHints.input = True;
    XSetWMHints(display, win, &wmHints);

    Atom wmProtocols = XInternAtom(display, "WM_PROTOCOLS", False);
    Atom wmDeleteWindow = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, win, &wmDeleteWindow, 1);
    (void)wmProtocols;

    if (transientFor != 0)
        XSetTransientForHint(display, win, transientFor);

    if (!windowType.empty())
    {
        Atom netWmWindowType = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
        Atom typeAtom = XInternAtom(display, windowType.c_str(), False);
        XChangeProperty(display, win, netWmWindowType, XA_ATOM, 32, PropModeReplace,
                         reinterpret_cast<unsigned char*>(&typeAtom), 1);
    }

    if (xembed)
    {
        // Real shape per the XEmbed spec (and matching
        // TrayIconClient::Create(), which is what a real Qt tray icon
        // window does): two CARD32s, [0]=spec version, [1]=flags with
        // XEMBED_MAPPED (bit 0) set.
        Atom xembedInfo = XInternAtom(display, "_XEMBED_INFO", False);
        long data[2] = { 0, 1 };
        XChangeProperty(display, win, xembedInfo, xembedInfo, 32, PropModeReplace,
                         reinterpret_cast<unsigned char*>(data), 2);
    }

    if (!role.empty())
    {
        Atom windowRole = XInternAtom(display, "WM_WINDOW_ROLE", False);
        XChangeProperty(display, win, windowRole, XA_STRING, 8, PropModeReplace,
                         reinterpret_cast<const unsigned char*>(role.c_str()), static_cast<int>(role.size()));
    }

    if (setPid)
    {
        Atom netWmPid = XInternAtom(display, "_NET_WM_PID", False);
        unsigned long pid = static_cast<unsigned long>(getpid());
        XChangeProperty(display, win, netWmPid, XA_CARDINAL, 32, PropModeReplace,
                        reinterpret_cast<unsigned char*>(&pid), 1);
    }

    if (mapDelayMs > 0)
    {
        // The window already exists with all its properties; only the
        // MapRequest is late - exactly what a slow-starting application
        // looks like to a window manager.
        std::printf("CREATED=0x%lx PID=%d\n", win, getpid());
        std::fflush(stdout);
        usleep(static_cast<useconds_t>(mapDelayMs) * 1000);
    }

    double mapRequestedAt = NowMs();
    XMapWindow(display, win);
    XFlush(display);

    if (sendDockRequest)
    {
        if (dockDelayMs > 0)
            usleep(static_cast<useconds_t>(dockDelayMs) * 1000);

        // The real freedesktop system tray protocol: find whoever
        // owns _NET_SYSTEM_TRAY_S<screen> (Kohiko's SystemTray, once
        // Initialize()'d) and ask them, via a ClientMessage, to dock
        // this window - the same request a genuine tray-icon
        // application (a NetworkManager/volume/chat applet) sends.
        std::string selectionName = "_NET_SYSTEM_TRAY_S" + std::to_string(screen);
        Atom selectionAtom = XInternAtom(display, selectionName.c_str(), False);
        Window trayOwner = XGetSelectionOwner(display, selectionAtom);

        if (trayOwner != None)
        {
            Atom opcodeAtom = XInternAtom(display, "_NET_SYSTEM_TRAY_OPCODE", False);

            XClientMessageEvent dock{};
            dock.type = ClientMessage;
            dock.window = trayOwner;
            dock.message_type = opcodeAtom;
            dock.format = 32;
            dock.data.l[0] = CurrentTime;
            dock.data.l[1] = 0; // SYSTEM_TRAY_REQUEST_DOCK
            dock.data.l[2] = static_cast<long>(win);

            XSendEvent(display, trayOwner, False, NoEventMask, reinterpret_cast<XEvent*>(&dock));
            XFlush(display);
            std::fprintf(stderr, "x11_test_client: sent SYSTEM_TRAY_REQUEST_DOCK to 0x%lx\n", trayOwner);
        }
        else
        {
            std::fprintf(stderr, "x11_test_client: no _NET_SYSTEM_TRAY_S%d owner found - dock request not sent\n", screen);
        }
    }

    std::printf("WINDOW=0x%lx PID=%d\n", win, getpid());
    std::fflush(stdout);

    GC gc = XCreateGC(display, win, 0, nullptr);

    while (true)
    {
        XEvent event;
        XNextEvent(display, &event);

        if (reportMapLatency && event.type == MapNotify && event.xmap.window == win)
        {
            std::printf("MAP_LATENCY_MS=%.1f\n", NowMs() - mapRequestedAt);
            std::fflush(stdout);
            reportMapLatency = false;
        }

        if (reportEvents && event.xany.window == win)
        {
            if (event.type == MapNotify)
                std::printf("EVENT=MapNotify\n");
            else if (event.type == UnmapNotify)
                std::printf("EVENT=UnmapNotify\n");
            else if (event.type == ReparentNotify)
                std::printf("EVENT=ReparentNotify parent=0x%lx root=%d\n", event.xreparent.parent,
                            event.xreparent.parent == root ? 1 : 0);
            std::fflush(stdout);
        }

        if (event.type == Expose)
        {
            XSetForeground(display, gc, attrs.background_pixel);
            XFillRectangle(display, win, gc, 0, 0, width, height);
            XFlush(display);
        }
        else if (event.type == ClientMessage)
        {
            // Only actually exit for a real WM_DELETE_WINDOW - any
            // other ClientMessage (XEMBED_EMBEDDED_NOTIFY from a tray
            // host after a successful dock, for instance - a real,
            // spec-compliant thing for Kohiko's own DockIcon() to
            // send) must NOT be treated as a close request, or this
            // client would misreport a successful dock as itself
            // having crashed/exited.
            if (static_cast<Atom>(event.xclient.data.l[0]) == wmDeleteWindow)
                break;
        }
    }

    return 0;
}
