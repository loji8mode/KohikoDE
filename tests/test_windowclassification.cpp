// Regression test for the tray-window classification fix: a window
// that sets _XEMBED_INFO before being mapped (the freedesktop XEmbed
// spec's own requirement for any client that wants to be embedded
// rather than managed as a normal top-level window - see
// TrayIconClient::Create(), the only thing in this codebase that
// currently does this) must be recognized by
// XConnection::IsXEmbedWindow() so that WindowManager::Manage() skips
// it - not tiled into the BSP, not added to the taskbar (_NET_
// CLIENT_LIST), not eligible for normal focus, not persisted by
// session restore. See that function's own comment (XConnection.h)
// and WindowManager::Manage()'s own comment for the full "why" - in
// short, without this check, a race between TrayIconClient mapping
// its window immediately after requesting a dock (rather than
// waiting for the embedder to reparent it first) and Kohiko's own
// ordinary MapRequest handling meant tray icon windows could get
// fully managed before SystemTray ever got a chance to reparent them
// away, leaving a phantom BSP tile and taskbar entry rendering
// nothing once the window's actual content moved into the tray.
//
// This is a real-X11-connection test - like test_monitormanager, not
// test_desktopentry - since IsXEmbedWindow() calls XGetWindowProperty()
// against a real window and a real property. Connects to whatever
// $DISPLAY points at; if none is set (a genuinely headless CI box with
// no Xvfb either), prints a note and exits 0 rather than failing the
// build, the same as test_monitormanager does.
//
// Deliberately does NOT try to construct a full WindowManager and
// drive a real MapRequest through Manage() end to end - that would
// mean standing up a second X connection acting as a window manager
// (SubstructureRedirectMask on the root window can only be selected
// by one client at a time), which is a much heavier, more fragile
// test than this bug actually needs: the fix is entirely in
// IsXEmbedWindow()'s own answer, and Manage()'s use of that answer is
// a two-line early return, simple enough that testing the predicate
// directly - the same "test the decision, not the whole pipeline
// around it" approach test_desktopentry already takes for
// ShouldAutostart() - is enough to catch a regression in the part
// that actually has logic in it.

#include "XAtoms.h"
#include "XConnection.h"

#include <X11/Xatom.h>

#include <cstdio>
#include <cstdlib>

using namespace Kohiko;

namespace
{

int g_pass = 0;

void Check(bool condition, const char* what)
{
    if (condition)
    {
        ++g_pass;
        std::printf("  PASS: %s\n", what);
    }
    else
    {
        std::printf("  FAIL: %s\n", what);
        std::exit(1);
    }
}

// A forgiving error handler, scoped to this test only - not calling
// XConnection::BecomeWindowManager() here (which is what installs the
// real one production uses - see its own comment) on purpose: that
// selects SubstructureRedirectMask on the root window, which only one
// client can hold at a time, and this test has no business
// contending with an actual Kohiko session that might already be
// running on the same $DISPLAY. Xlib's own *default* handler treats
// most protocol errors as fatal (exit()), which is exactly the
// scenario the third check below deliberately creates (a property
// query against a window ID that doesn't exist) - in real production
// use this exact class of error is already caught by the WM's own
// global handler (installed once, in BecomeWindowManager(), active
// for the entire time Manage() could ever be called - a MapRequest
// literally cannot happen before that), so this local handler is
// standing in for that, not testing something IsXEmbedWindow() itself
// needs to additionally guard against.
int ForgivingErrorHandler(Display*, XErrorEvent*)
{
    return 0;
}

}

int main()
{
    const char* displayEnv = std::getenv("DISPLAY");

    if (!displayEnv || displayEnv[0] == '\0')
    {
        std::printf("No $DISPLAY set - skipping (needs a real, even if headless/Xvfb, X server).\n");
        return 0;
    }

    XConnection connection;

    if (!connection.Connect())
    {
        std::printf("Could not open display \"%s\" - skipping.\n", displayEnv);
        return 0;
    }

    XSetErrorHandler(ForgivingErrorHandler);

    XAtoms atoms(connection);
    atoms.Initialize();

    Display* display = connection.GetDisplay();
    Window root = connection.Root();

    std::printf("XConnection::IsXEmbedWindow() tests (real X connection: %s):\n", displayEnv);

    std::printf("-- An ordinary window, no _XEMBED_INFO set --\n");
    {
        Window plain = XCreateSimpleWindow(display, root, 0, 0, 10, 10, 0, 0, 0);
        Check(!connection.IsXEmbedWindow(plain, atoms),
              "a plain window with no _XEMBED_INFO property is not treated as an XEmbed window");
        XDestroyWindow(display, plain);
    }

    std::printf("\n-- A window with _XEMBED_INFO set, exactly like TrayIconClient::Create() sets it --\n");
    {
        Window embedder = XCreateSimpleWindow(display, root, 0, 0, 22, 22, 0, 0, 0);

        long xembedInfo[2] = {0, 1}; // version 0, XEMBED_MAPPED flag set - see TrayIconClient::Create()'s own comment for why these exact values
        XChangeProperty(display, embedder, atoms.XEMBED_INFO, atoms.XEMBED_INFO, 32,
            PropModeReplace, reinterpret_cast<unsigned char*>(xembedInfo), 2);
        XSync(display, False);

        Check(connection.IsXEmbedWindow(embedder, atoms),
              "a window with _XEMBED_INFO set (TrayIconClient's own exact convention) is recognized as an XEmbed window");

        XDestroyWindow(display, embedder);
    }

    std::printf("\n-- A destroyed/nonexistent window --\n");
    {
        Window bogus = 0x7ffffff0; // astronomically unlikely to collide with anything real on this display
        Check(!connection.IsXEmbedWindow(bogus, atoms),
              "a nonexistent window ID is treated as \"not XEmbed\" rather than crashing or throwing");
    }

    // Regression test for the native-notification feature: before this
    // existed, Kohiko had a Manage() classification entry for DOCK and
    // for XEmbed but none at all for _NET_WM_WINDOW_TYPE_NOTIFICATION -
    // see XConnection::IsNotificationWindowType()'s own comment for
    // why that's a real, general gap (not just an audio-device-toast
    // one) this closes. NotificationPopup::Create() is the only thing
    // in this codebase that currently sets this type, and it also sets
    // override-redirect (so its own windows never reach Manage() far
    // enough to need this check at all) - these tests exist for the
    // "some *other* notification window, not override-redirect" case
    // this check is actually the backstop for, the same reasoning
    // IsXEmbedWindow()'s own tests above already established for the
    // XEmbed side of Manage().
    std::printf("\nXConnection::IsNotificationWindowType() tests:\n");

    std::printf("-- An ordinary window, no _NET_WM_WINDOW_TYPE set --\n");
    {
        Window plain = XCreateSimpleWindow(display, root, 0, 0, 10, 10, 0, 0, 0);
        Check(!connection.IsNotificationWindowType(plain, atoms),
              "a plain window with no _NET_WM_WINDOW_TYPE property at all is not treated as a notification");
        XDestroyWindow(display, plain);
    }

    std::printf("\n-- A window declaring _NET_WM_WINDOW_TYPE_DOCK (a different type entirely) --\n");
    {
        Window dock = XCreateSimpleWindow(display, root, 0, 0, 200, 24, 0, 0, 0);

        Atom dockType = atoms.NET_WM_WINDOW_TYPE_DOCK;
        XChangeProperty(display, dock, atoms.NET_WM_WINDOW_TYPE, XA_ATOM, 32,
            PropModeReplace, reinterpret_cast<unsigned char*>(&dockType), 1);
        XSync(display, False);

        Check(!connection.IsNotificationWindowType(dock, atoms),
              "a window declaring some other _NET_WM_WINDOW_TYPE (DOCK here) is not mistaken for a notification "
              "- confirms this checks the specific type value, not merely \"any type is set\"");
        Check(connection.IsDockWindowType(dock, atoms),
              "...and is still correctly recognized as a DOCK by the existing, unrelated check - this feature "
              "did not regress that classification");

        XDestroyWindow(display, dock);
    }

    std::printf("\n-- A window declaring _NET_WM_WINDOW_TYPE_NOTIFICATION, exactly like NotificationPopup::"
                "Create() sets it --\n");
    {
        Window toast = XCreateSimpleWindow(display, root, 0, 0, 240, 48, 0, 0, 0);

        Atom notificationType = atoms.NET_WM_WINDOW_TYPE_NOTIFICATION;
        XChangeProperty(display, toast, atoms.NET_WM_WINDOW_TYPE, XA_ATOM, 32,
            PropModeReplace, reinterpret_cast<unsigned char*>(&notificationType), 1);
        XSync(display, False);

        Check(connection.IsNotificationWindowType(toast, atoms),
              "a window with _NET_WM_WINDOW_TYPE_NOTIFICATION set is recognized as one, so WindowManager::"
              "Manage() will skip it - never tiled, never in the taskbar, never focus-eligible - even in the "
              "hypothetical case where such a window isn't also override-redirect");

        XDestroyWindow(display, toast);
    }

    std::printf("\n-- A destroyed/nonexistent window (notification check) --\n");
    {
        Window bogus = 0x7ffffff0;
        Check(!connection.IsNotificationWindowType(bogus, atoms),
              "a nonexistent window ID is treated as \"not a notification\" rather than crashing or throwing");
    }

    connection.Disconnect();

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    return 0;
}
