// Regression test for the standard interaction contract of Kohiko's own tray
// icons (include/NativeTrayContract.h): LEFT click opens/raises the main
// window, RIGHT click opens the compact menu - the same for audio, network
// and Bluetooth. Real TrayIconClient, real shared contract, real PopupMenu,
// real AppInstanceLock; a forked "clicker" process with its own X connection
// delivers the events, the way tests/test_trayiconclient.cpp does. Needs an
// X display (run under xvfb-run; skips cleanly without one).
//
// What it pins down:
//   - left click launches the main window ONCE even when clicked repeatedly
//     while it is still starting, and later clicks RAISE the running
//     instance (no duplicate windows, existing state reused);
//   - right click opens the menu; a middle click does nothing;
//   - the menu action runs only AFTER the popup has released its global
//     pointer and keyboard grabs (before: it ran inside the grabbed loop, so
//     a slow action froze every window's input system-wide);
//   - the icon is redrawn after the menu closes (never left stale).
//
// argv[1]: directory containing the fake-native-app binary (default "build").

#include "NativeTrayContract.h"
#include "TrayIconClient.h"
#include "UiPopupMenu.h"

#include <X11/Xlib.h>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace Kohiko;

namespace
{

int g_pass = 0, g_fail = 0;

void Check(bool ok, const std::string& what)
{
    if (ok) { ++g_pass; std::printf("  PASS: %s\n", what.c_str()); }
    else    { ++g_fail; std::printf("  FAIL: %s\n", what.c_str()); }
}

int Count(const std::string& path, const std::string& prefix)
{
    std::ifstream in(path);
    std::string line;
    int n = 0;
    while (std::getline(in, line))
        n += line.rfind(prefix, 0) == 0;
    return n;
}

void SendButton(Display* d, ::Window window, int button, int rootX, int rootY, int x, int y)
{
    XEvent press{};
    press.xbutton.type = ButtonPress;
    press.xbutton.display = d;
    press.xbutton.window = window;
    press.xbutton.root = RootWindow(d, DefaultScreen(d));
    press.xbutton.x = x; press.xbutton.y = y;
    press.xbutton.x_root = rootX + x; press.xbutton.y_root = rootY + y;
    press.xbutton.button = button;
    press.xbutton.time = CurrentTime;
    press.xbutton.same_screen = True;
    XSendEvent(d, window, False, ButtonPressMask, &press);

    XEvent release = press;
    release.xbutton.type = ButtonRelease;
    XSendEvent(d, window, False, ButtonReleaseMask, &release);
    XFlush(d);
}

}

int main(int argc, char** argv)
{
    Display* display = XOpenDisplay(nullptr);

    if (!display)
    {
        std::printf("SKIP: no X display available.\n");
        return 0;
    }

    std::string fakeDir = argc > 1 ? argv[1] : "build";
    char tmpl[] = "/tmp/kohiko-test-traycontract-XXXXXX";
    std::string dir = mkdtemp(tmpl);
    setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
    std::string logPath = dir + "/app.log";
    setenv("FAKE_LOG", logPath.c_str(), 1);
    setenv("FAKE_DELAY_MS", "300", 1);
    setenv("FAKE_LIFETIME_MS", "4000", 1);
    setenv("PATH", (fakeDir + ":" + std::getenv("PATH")).c_str(), 1);

    TrayIconClient tray;
    if (!tray.Create(22))
    {
        std::printf("FAIL: could not create the tray client\n");
        return 1;
    }

    int screen = tray.GetScreen();
    int draws = 0;
    int drawsWhenActionRan = -1;

    tray.SetDrawCallback([&](Display* d, Window w, GC gc, Visual*, Colormap cm, int size)
    {
        XColor c{}; c.red = 0xFFFF; c.green = 0; c.blue = 0xFFFF; c.flags = DoRed | DoGreen | DoBlue;
        XAllocColor(d, cm, &c);
        XSetForeground(d, gc, c.pixel);
        XFillRectangle(d, w, gc, 0, 0, size, size);
        ++draws;
    });

    // Map it ourselves: there is no tray host in this test to dock and map it.
    XMapWindow(tray.GetDisplay(), tray.GetWindow());
    XFlush(tray.GetDisplay());
    ::Window trayWindow = tray.GetWindow();

    int menuShown = 0;
    int actionRan = 0;
    int pointerGrab = -1, keyboardGrab = -1;

    // Exactly what a native tray tool does: ONE call, shared by all of them.
    InstallNativeTrayContract(tray, "fake-native-app", [&]
    {
        PopupMenu menu;
        menu.AddItem("Quick action", [&]
        {
            // The action runs now. Is the screen's pointer/keyboard still held by the
            // popup? A second client trying to grab them finds out.
            Display* other = XOpenDisplay(nullptr);
            ::Window root = RootWindow(other, DefaultScreen(other));
            pointerGrab = XGrabPointer(other, root, False, ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
            keyboardGrab = XGrabKeyboard(other, root, False, GrabModeAsync, GrabModeAsync, CurrentTime);
            XUngrabPointer(other, CurrentTime);
            XUngrabKeyboard(other, CurrentTime);
            XCloseDisplay(other);

            drawsWhenActionRan = draws;
            ++actionRan;
        });

        ++menuShown;
        menu.Show(tray.GetDisplay(), screen, tray.GetVisual(), tray.GetColormap(), tray.GetFont(), tray.Theme(), 300, 300);
    });

    int rootX = 0, rootY = 0;
    ::Window ignoredChild;
    XTranslateCoordinates(tray.GetDisplay(), trayWindow, RootWindow(tray.GetDisplay(), screen), 0, 0, &rootX, &rootY, &ignoredChild);
    XSync(tray.GetDisplay(), False);

    pid_t clicker = fork();

    if (clicker == 0)
    {
        Display* d = XOpenDisplay(nullptr);
        if (!d) _exit(1);
        ::Window root = RootWindow(d, DefaultScreen(d));

        usleep(300 * 1000);

        // 1. Six LEFT clicks, 30 ms apart, while the app (300 ms startup) is still starting.
        for (int i = 0; i < 6; ++i)
        {
            SendButton(d, trayWindow, Button1, rootX, rootY, 11, 11);
            usleep(30 * 1000);
        }

        usleep(1000 * 1000); // the app is up and listening now

        // 2. One more LEFT click: must RAISE the running window, not launch another.
        SendButton(d, trayWindow, Button1, rootX, rootY, 11, 11);
        usleep(400 * 1000);

        // 3. A MIDDLE click: nothing.
        SendButton(d, trayWindow, Button2, rootX, rootY, 11, 11);
        usleep(200 * 1000);

        // 4. RIGHT click: the menu opens. Find the popup window and pick its item.
        ::Window before[1024];
        unsigned int nBefore = 0;
        {
            ::Window r, p, *kids = nullptr; unsigned int n = 0;
            XQueryTree(d, root, &r, &p, &kids, &n);
            nBefore = n < 1024 ? n : 1024;
            for (unsigned int i = 0; i < nBefore; ++i) before[i] = kids[i];
            if (kids) XFree(kids);
        }

        SendButton(d, trayWindow, Button3, rootX, rootY, 11, 11);

        ::Window popup = 0;
        for (int tries = 0; tries < 100 && !popup; ++tries)
        {
            usleep(10 * 1000);
            ::Window r, p, *kids = nullptr; unsigned int n = 0;
            XQueryTree(d, root, &r, &p, &kids, &n);
            for (unsigned int i = 0; i < n && !popup; ++i)
            {
                bool known = false;
                for (unsigned int j = 0; j < nBefore; ++j) if (kids[i] == before[j]) known = true;
                if (!known) popup = kids[i];
            }
            if (kids) XFree(kids);
        }

        if (!popup) _exit(2);

        usleep(100 * 1000);

        XEvent click{};
        click.xbutton.type = ButtonRelease;
        click.xbutton.display = d;
        click.xbutton.window = popup;
        click.xbutton.root = root;
        click.xbutton.x = 10; click.xbutton.y = 15;
        click.xbutton.x_root = 310; click.xbutton.y_root = 315;
        click.xbutton.button = Button1;
        click.xbutton.time = CurrentTime;
        click.xbutton.same_screen = True;
        XSendEvent(d, popup, False, ButtonReleaseMask, &click);
        XFlush(d);

        usleep(300 * 1000);
        XCloseDisplay(d);
        _exit(0);
    }

    tray.SetInterval(3500, [&] { tray.Quit(); }); // test-only: end the run
    tray.Run();

    int status = 0;
    waitpid(clicker, &status, 0);

    std::printf("-- LEFT click: open or raise the main window --\n");
    Check(Count(logPath, "LAUNCH") == 1, "six rapid left clicks launched the main window exactly once (got " + std::to_string(Count(logPath, "LAUNCH")) + ")");
    Check(Count(logPath, "START") == 1 && Count(logPath, "LOST") == 0, "exactly one instance is running; no second process lost a race");
    Check(Count(logPath, "RAISE") >= 1, "a later left click raised the running window instead of opening another");

    std::printf("-- RIGHT click: the compact menu --\n");
    Check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the popup appeared on a right click and its item could be picked");
    Check(menuShown == 1, "the menu was shown exactly once - by the right click only (not by the left clicks or the middle click)");
    Check(actionRan == 1, "the picked menu item's action ran exactly once");
    Check(Count(logPath, "LAUNCH") == 1, "the right click and the middle click did not launch anything");

    std::printf("-- The popup is fully gone before its action runs --\n");
    Check(pointerGrab == GrabSuccess, "when the action ran, the screen's POINTER was no longer grabbed by the popup (0.21.5: still held)");
    Check(keyboardGrab == GrabSuccess, "when the action ran, the KEYBOARD was no longer grabbed by the popup (0.21.5: still held)");

    std::printf("-- The icon is not left stale --\n");
    Check(drawsWhenActionRan >= 0 && draws > drawsWhenActionRan, "the icon was redrawn after the menu closed");

    XCloseDisplay(display);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
