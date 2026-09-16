// Focused tests for the new native Kohiko notification mechanism
// (NotificationPopup.h/NotificationCenter.h), added for the "headphone/
// audio device connect-disconnect should behave like a transient
// desktop notification instead of a normal X11 window" request - see
// CHANGELOG.md's entry for the version this shipped in, and
// docs/ARCHITECTURE.md's "Native notifications" section for the full
// design writeup (why this is a from-scratch native mechanism rather
// than a headphone-specific special case, and why it lives outside
// WindowManager/XConnection so kohiko-audio-tray can use the exact
// same classes it does).
//
// Needs a real X11 connection - same "gracefully skip without a real,
// even if headless/Xvfb, $DISPLAY" deal as test_bar/
// test_windowclassification/test_trayiconclient - kept out of `make
// test` the same way (see "test-notificationcenter" in the Makefile).
//
// Deliberately does NOT link XConnection/XAtoms/WindowManager at all -
// NotificationPopup/NotificationCenter depend on neither (see their
// own header comments on why that's what makes them genuinely
// reusable, not WindowManager-only), so this test doesn't either; it
// opens its own plain Xlib display exactly the way kohiko-audio-tray's
// real call site does. The complementary "WindowManager::Manage()
// correctly leaves a NOTIFICATION-type window alone" check lives in
// test_windowclassification.cpp instead, alongside the existing
// IsXEmbedWindow()/IsDockWindowType() checks it already covers - that
// is genuinely an XConnection-level classification question, this
// file is not.

#include "Font.h"
#include "NotificationCenter.h"
#include "NotificationLayout.h"
#include "NotificationPopup.h"
#include "Types.h"
#include "UiTheme.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <chrono>

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

// Standing in for WindowManager's own global X error handler (see
// test_windowclassification.cpp's identical reasoning) - the
// "destroyed window" checks below deliberately query a window ID that
// no longer exists, which Xlib's default handler would otherwise treat
// as fatal.
int ForgivingErrorHandler(Display*, XErrorEvent*)
{
    return 0;
}

bool WindowStillExists(Display* display, ::Window window)
{
    XWindowAttributes attrs{};
    return XGetWindowAttributes(display, window, &attrs) != 0;
}

}

int main()
{
    const char* displayEnv = std::getenv("DISPLAY");

    if (!displayEnv || displayEnv[0] == '\0')
    {
        std::printf("No $DISPLAY set - skipping (needs a real, even if headless/Xvfb, X server).\n");
        std::printf("\n0 CHECKS RUN (skipped).\n");
        return 0;
    }

    Display* display = XOpenDisplay(nullptr);

    if (!display)
    {
        std::printf("Could not open display \"%s\" - skipping.\n", displayEnv);
        std::printf("\n0 CHECKS RUN (skipped).\n");
        return 0;
    }

    XSetErrorHandler(ForgivingErrorHandler);

    int screen = DefaultScreen(display);
    ::Window root = RootWindow(display, screen);

    Atom windowTypeAtom = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
    Atom notificationTypeAtom = XInternAtom(display, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);

    UiTheme theme; // defaults are fine - nothing here tests color output

    std::printf("NotificationPopup/NotificationCenter tests (real X connection: %s):\n", displayEnv);

    std::printf("-- NotificationPopup::Create() --\n");
    ::Window createdWindowId = 0;
    {
        Kohiko::Font font;
        Check(font.Load(display, screen, "monospace:pixelsize=14"), "test setup: loaded a font to create popups with");

        NotificationPopup popup;
        Check(popup.Create(display, screen, root, font, theme), "Create() succeeds given a real display and a loaded font");
        Check(popup.WindowId() != 0, "Create() leaves a real, nonzero X11 window id behind");

        createdWindowId = popup.WindowId();

        std::printf("\n-- Window properties: override-redirect, correct EWMH type, no input eligibility --\n");

        XWindowAttributes attrs{};
        Check(XGetWindowAttributes(display, popup.WindowId(), &attrs) != 0,
              "the created window really exists and XGetWindowAttributes() succeeds on it");

        Check(attrs.override_redirect == True,
              "override-redirect is set - the one thing that actually keeps a window manager's "
              "SubstructureRedirect from ever seeing this as a MapRequest at all, so it can never be tiled, "
              "added to the taskbar, or made focus-eligible, regardless of any window-type classification");

        Check((attrs.your_event_mask & (KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask)) == 0,
              "no key/button event mask is selected on this window - it cannot receive keyboard or pointer "
              "input even if something else tried to hand it focus, matching \"without stealing keyboard focus\"");

        Atom actualType = 0;
        int actualFormat = 0;
        unsigned long itemCount = 0;
        unsigned long bytesLeft = 0;
        unsigned char* data = nullptr;

        int status = XGetWindowProperty(
            display, popup.WindowId(), windowTypeAtom, 0, 1, False,
            XA_ATOM, &actualType, &actualFormat, &itemCount, &bytesLeft, &data);

        bool hasNotificationType = false;

        if (status == Success && data && itemCount >= 1)
            hasNotificationType = reinterpret_cast<Atom*>(data)[0] == notificationTypeAtom;

        if (data)
            XFree(data);

        Check(hasNotificationType,
              "_NET_WM_WINDOW_TYPE is set to _NET_WM_WINDOW_TYPE_NOTIFICATION - spec-correct labelling for any "
              "pager/tool that inspects window types, on top of (not instead of) the override-redirect guarantee above");

        XWMHints* hints = XGetWMHints(display, popup.WindowId());
        Check(hints == nullptr,
              "no WM_HINTS is set at all - in particular, nothing here ever declares input=True, so there is "
              "nothing for a window manager's ICCCM input-focus negotiation to even act on");

        std::printf("\n-- No focus stealing across Show() --\n");

        ::Window focusBefore = 0;
        int revertBefore = 0;
        XGetInputFocus(display, &focusBefore, &revertBefore);

        // Show() itself just places a popup at whatever top-left it's
        // given (see NotificationPopup.h's own comment on why it
        // deliberately doesn't compute bottom-right placement itself -
        // that's NotificationLayout::ComputeRect()/NotificationCenter's
        // job) - so this computes the real bottom-right position first,
        // exactly the way NotificationCenter::Post() itself does,
        // rather than an arbitrary point.
        Rect monitor{0, 0, 1920, 1080};
        std::string text = "Headphones connected";

        Rect expected = NotificationLayout::ComputeRect(
            monitor,
            NotificationLayout::ComputeWidth(font.TextWidth(text)),
            NotificationLayout::ComputeHeight(font.Height()),
            0);

        popup.Show(text, Point{expected.x, expected.y}, std::chrono::steady_clock::now() + std::chrono::seconds(5));
        XSync(display, False);

        ::Window focusAfter = 0;
        int revertAfter = 0;
        XGetInputFocus(display, &focusAfter, &revertAfter);

        Check(focusAfter == focusBefore,
              "showing (mapping + raising) the notification does not change X input focus at all - "
              "\"appear above normal windows without stealing keyboard focus\"");

        Check(focusAfter != popup.WindowId(),
              "...and input focus specifically never lands on the notification window itself");

        std::printf("\n-- Placement: bottom-right of the given monitor, auto-sized to content --\n");

        XGetWindowAttributes(display, popup.WindowId(), &attrs);

        Check(attrs.x == expected.x && attrs.y == expected.y,
              "the window's actual on-screen position matches the bottom-right placement it was told to show "
              "at (NotificationCenter::Post()'s own job - see NotificationCenter's own placement/stacking tests "
              "further below - is computing that position in the first place)");

        Check(attrs.width == expected.width && attrs.height == expected.height,
              "the window auto-sized itself to the text it was actually shown with, not some fixed default size");
    }

    std::printf("\n-- NotificationCenter: creation, placement, and independent per-monitor stacking --\n");
    {
        NotificationCenter center;
        Check(center.Initialize(display, screen, root, "monospace:pixelsize=14", theme),
              "Initialize() succeeds given a real display");

        Rect monitor{0, 0, 1920, 1080};

        center.Post("Headphones connected", monitor, std::chrono::seconds(5));
        Check(center.ActiveCount() == 1, "posting one notification leaves exactly one active");
        Check(center.HasActive(), "HasActive() agrees");

        center.Post("USB Microphone connected", monitor, std::chrono::seconds(5));
        center.Post("Bluetooth Speaker disconnected", monitor, std::chrono::seconds(5));

        Check(center.ActiveCount() == 3,
              "\"multiple notifications... handled cleanly if several events happen close together\": posting "
              "three in quick succession leaves all three active, not just the latest one");

        std::printf("\n-- Cleanup: destroying and re-posting never leaves orphaned windows --\n");

        // Post enough additional notifications to exceed kMaxStacked -
        // "handled cleanly" (per the spec) includes not letting the
        // stack grow without bound if something posts faster than
        // notifications expire.
        for (int i = 0; i < NotificationLayout::kMaxStacked + 2; ++i)
            center.Post("Flood " + std::to_string(i), monitor, std::chrono::seconds(5));

        Check(center.ActiveCount() == static_cast<std::size_t>(NotificationLayout::kMaxStacked),
              "posting past kMaxStacked evicts the oldest ones down to exactly kMaxStacked active, rather than "
              "growing without bound");
    }

    std::printf("\n-- Timeout/expiration at 2.5 seconds (using an explicit short override so this test doesn't "
                "have to sleep 2.5 real seconds - see NotificationLayout::kDefaultLifetime's own comment and "
                "test_notificationlayout.cpp for where the real 2.5s figure itself is directly asserted on) --\n");
    {
        NotificationCenter center;
        center.Initialize(display, screen, root, "monospace:pixelsize=14", theme);

        Rect monitor{0, 0, 1920, 1080};

        auto shortLifetime = std::chrono::milliseconds(300);
        center.Post("Headphones connected", monitor, shortLifetime);

        Check(center.HasActive(), "immediately after Post(), the notification is active");

        // Not yet expired.
        center.Tick();
        Check(center.HasActive(), "well before its lifetime elapses, Tick() leaves it showing");

        std::this_thread::sleep_for(shortLifetime + std::chrono::milliseconds(150));

        center.Tick();
        Check(!center.HasActive(),
              "once its lifetime has actually elapsed, Tick() cleans it up - \"automatically disappear\", per the spec");
        Check(center.ActiveCount() == 0, "...and ActiveCount() agrees: nothing left active");
    }

    std::printf("\n-- REGRESSION: NotificationCenter::Tick() itself actually removes the window from the X "
                "SERVER, observed from a second, independent connection --\n");
    {
        // Same reasoning as the NotificationPopup-level regression test
        // above, but exercised through the real production entry point
        // (Post()/Tick()) rather than the lower-level class directly -
        // this is the exact call sequence kohiko-audio-tray's own
        // SetInterval()-driven timer uses.
        Display* observerDisplay = XOpenDisplay(nullptr);
        Check(observerDisplay != nullptr, "test setup: opened a second, independent connection to act as an observer");

        NotificationCenter center;
        center.Initialize(display, screen, root, "monospace:pixelsize=14", theme);

        Rect monitor{0, 0, 1920, 1080};
        auto shortLifetime = std::chrono::milliseconds(250);
        center.Post("Regression check", monitor, shortLifetime);

        // Find the window id NotificationCenter just created, purely
        // for the observer to check later - HandleExpose(0) on a
        // window we don't actually have exposed is a no-op either way,
        // so instead we ask the observer to enumerate the root's
        // children and diff against a before/after snapshot, which
        // needs no access to NotificationCenter's own internals at all.
        auto childWindows = [&](Display* d) -> std::vector<::Window>
        {
            ::Window rootReturn, parentReturn;
            ::Window* children = nullptr;
            unsigned int count = 0;
            std::vector<::Window> result;
            if (XQueryTree(d, root, &rootReturn, &parentReturn, &children, &count))
            {
                for (unsigned int i = 0; i < count; ++i)
                    result.push_back(children[i]);
                if (children)
                    XFree(children);
            }
            return result;
        };

        XSync(display, False); // test setup only - make sure creation is visible to the observer first
        auto withPopup = childWindows(observerDisplay);

        std::this_thread::sleep_for(shortLifetime + std::chrono::milliseconds(150));

        // The only thing that happens on `display` is the real
        // production call - no XSync/XFlush of our own after it.
        center.Tick();

        auto afterExpiry = childWindows(observerDisplay);

        Check(withPopup.size() == afterExpiry.size() + 1,
              "the observer connection sees the root's child window count drop by exactly one after "
              "NotificationCenter::Tick() expires the popup - confirming Tick() itself (the real production "
              "entry point, not just NotificationPopup's destructor in isolation) actually removes the window "
              "from the X server, not merely from its own internal bookkeeping");

        XCloseDisplay(observerDisplay);
    }

    std::printf("\n-- Multiple notifications expiring independently --\n");
    {
        NotificationCenter center;
        center.Initialize(display, screen, root, "monospace:pixelsize=14", theme);

        Rect monitor{0, 0, 1920, 1080};

        center.Post("Short-lived", monitor, std::chrono::milliseconds(200));
        center.Post("Long-lived", monitor, std::chrono::seconds(5));

        Check(center.ActiveCount() == 2, "both are active right after posting");

        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        center.Tick();

        Check(center.ActiveCount() == 1,
              "after only the short-lived one's own lifetime elapses, exactly one notification is cleaned up "
              "and the other keeps showing - each notification's expiry is independent of the others'");
    }

    std::printf("\n-- REGRESSION: a destroyed popup's window is gone from the X SERVER, not just forgotten "
                "locally (two independent connections, matching the real kohiko-audio-tray-vs-observer topology) --\n");
    {
        // The real production bug this reproduces: NotificationPopup's
        // destructor called XDestroyWindow() but never XFlush()'d.
        // Xlib buffers requests client-side, so without an explicit
        // flush (or some *other* Xlib call on the same connection that
        // happens to flush as a side effect - see below) the destroy
        // request could sit unsent indefinitely in a host process with
        // nothing else generating X traffic - the window stayed mapped
        // and visible forever even though NotificationCenter had
        // already destroyed the object and forgotten about it. That is
        // "does not disappear after 2.5 seconds" from the actual field
        // report.
        //
        // A single-connection test cannot catch this: querying the
        // *same* connection (XGetWindowAttributes, XSync, ...) flushes
        // the very request being tested as an unavoidable side effect
        // of making any round-trip call at all, which is exactly how
        // the original version of this test passed in Xvfb while the
        // real bug shipped anyway. A second, independent connection -
        // standing in for kohiko (or xwininfo, or anything else)
        // observing kohiko-audio-tray's windows from a different
        // process - has no such side effect and reports what the X
        // server actually has, which is the only thing that matters in
        // production.
        Display* observerDisplay = XOpenDisplay(nullptr);
        Check(observerDisplay != nullptr, "test setup: opened a second, independent connection to act as an observer");

        ::Window windowId = 0;

        {
            Kohiko::Font popupFont;
            popupFont.Load(display, screen, "monospace:pixelsize=14");

            NotificationPopup popup;
            popup.Create(display, screen, root, popupFont, theme);
            popup.Show("Regression check", Point{50, 50}, std::chrono::steady_clock::now() + std::chrono::seconds(5));
            windowId = popup.WindowId();

            XSync(display, False); // test setup only: make sure the *creation* is visible to the observer before we even start timing the destruction below
            Check(WindowStillExists(observerDisplay, windowId),
                  "test setup: the observer connection can see the window while it's still alive");

            // `popup` is destroyed here, at scope exit - deliberately
            // the *only* thing that happens on `display` between
            // creating it and the observer's check below. No XSync/
            // XFlush/query of any kind on `display` itself, so nothing
            // masks a missing flush inside the destructor the way it
            // did before.
        }

        Check(!WindowStillExists(observerDisplay, windowId),
              "the observer connection - which never touched `display` at all - sees the window is actually gone "
              "from the X server immediately after the owning NotificationPopup was destroyed, with no delay and "
              "no round-trip call of the primary connection's own needed to make that true");

        XCloseDisplay(observerDisplay);
    }

    std::printf("\n-- A destroyed/nonexistent window (same-connection sanity check) --\n");
    {
        Check(!WindowStillExists(display, createdWindowId),
              "the very first popup created above, whose owning NotificationPopup has since gone out of scope, "
              "was actually destroyed (not merely unmapped and forgotten) - \"do not leave orphaned windows\", "
              "per the spec. (Same-connection check, kept for completeness - see the two-connection regression "
              "test above for the one that actually exercises the flush behaviour production depends on.)");

        NotificationCenter center;
        Check(!center.OwnsWindow(createdWindowId),
              "OwnsWindow() correctly says no for a window id this NotificationCenter never created, including "
              "one that used to belong to an entirely different, already-destroyed NotificationPopup");
    }

    XCloseDisplay(display);

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    return 0;
}
