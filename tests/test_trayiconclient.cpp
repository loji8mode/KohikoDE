// Regression test for the 0.20.8 tray-icon finding (see CHANGELOG.md):
// Kohiko's own tray tools (kohiko-audio-tray, kohiko-network-tray,
// kohiko-bluetooth-tray - all built on the shared TrayIconClient) look
// up freedesktop-standard status icon names (e.g. "audio-volume-high")
// via UiIconCache/IconResolver, and drew nothing at all beyond the
// plain background fill whenever that lookup failed - which is the
// common case on a system whose active icon theme chain doesn't
// happen to carry those specific names (confirmed live: on the
// reporter's real desktop, and independently reproduced in the
// sandbox used for this fix, which has no configured GTK icon theme
// and so falls back to the near-empty "hicolor" theme - see
// IconResolver::DetectSystemThemeName()'s own comment). The result was
// indistinguishable from a broken icon: a plain background-colored
// square, "only rendering when affected by something" in the sense
// that a DIFFERENT icon name (a different volume/signal/connection
// state) might happen to resolve while another doesn't, with nothing
// about the window's own content ever refreshing regardless of how
// many times it got redrawn.
//
// This is explicitly NOT the same mechanism as 0.20.6's/0.20.7's
// Bar::Redraw() fixes - the icon pixmap is never successfully created
// in the first place, so there was nothing stale to invalidate. Fixed
// by giving TrayIconClient a DrawText() method (mirroring
// Bar::DrawText()) and having each tray tool's DrawCallback draw a
// small, guaranteed-to-render fallback glyph whenever
// UiIconCache::Get() returns false.
//
// Also covers the later (0.21.2) "tray icons render on a visibly
// wrong-coloured square" finding: every one of Kohiko's own tray icons
// painted its own opaque background using tray.Theme().background -
// UiTheme::Default()'s built-in constant, deliberately not read from
// Kohiko's config (see UiTheme.h's own comment on why) - rather than
// the real, live background of whatever it's actually docked into.
// That's a reasonable guess for a standalone control-panel window
// (AudioWindow and friends), which is what UiTheme::Default() is
// really for, but a tray icon isn't one: it's reparented flush against
// Kohiko's own Bar, whose real background is whatever `bar.background`
// is actually configured to, and the two happened to coincide only on
// whichever desktop first shipped this. On any real desktop where they
// don't - confirmed live, matching this constant exactly - every
// Kohiko tray icon sat on a small, visibly wrong-coloured square, easy
// to mistake for a fallback-glyph rendering bug (this file's own
// section above) rather than what it actually was. Fixed by switching
// TrayIconClient's own window to a ParentRelative background (so it
// always shows whatever its real parent - the tray host's container,
// itself chained to the Bar's own real background_pixel - actually
// has) and having Redraw() clear to that before every draw, rather
// than each tray tool separately painting a solid guess of its own.
//
// Also covers the (0.21.3) "tray icon corrupts immediately after its
// own popup closes" finding: PopupMenu::Show() (the dropdown each tray
// tool opens on click) runs its own blocking loop directly on
// TrayIconClient's own shared X connection - XNextEvent(display, ...),
// not filtered to the popup's own window - because that connection is
// also what the tray icon's own window lives on. Confirmed live and
// reproduced here exactly: an Expose meant for the tray icon (the
// ordinary result of the popup, which necessarily overlaps the icon it
// dropped down from, being destroyed and uncovering it again) that
// happens to arrive while Show()'s loop still owns the connection was
// silently misattributed to "the popup needs a redraw" (Show()'s old
// `case Expose: redraw();` never checked which window the event was
// actually for) and lost for good - the tray icon was left showing
// only its own correct, live background (0.21.2's fix), with no icon
// glyph on top, until some *unrelated* later event on the same
// connection happened to drag the stranded one out along with it (a
// real symptom on the reporter's machine: clicking the same icon again
// to open its full GUI window - not its dropdown - restored it).
// TrayIconClient::Run()'s own poll()-gated event drain compounded this:
// it only ever checked XPending() when poll() reported the raw socket
// readable, but Xlib keeps its own internal queue in front of that
// socket, and any blocking round-trip call anywhere in the process can
// silently drain bytes into it as a side effect - invisible to poll(),
// which only ever sees genuinely new, still-unread socket data. Fixed
// two ways, each closing a different half of this: PopupMenu::Show()
// now waits specifically for events addressed to its own window
// (XIfEvent with a window-matching predicate) rather than blindly
// taking whatever XNextEvent() hands back next, so anything else stays
// exactly where it is in Xlib's queue rather than being consumed or
// reordered; and TrayIconClient::Run() now checks XPending() every
// iteration unconditionally, rather than only when poll() said the
// socket had new data, so anything left waiting there is never more
// than one loop iteration from being noticed. (A third, related but
// independently-confirmed-harmless finding from the same investigation:
// SystemTray::Reposition() called XMoveResizeWindow() on the tray
// container unconditionally on every single Bar redraw, identical
// geometry or not - real, wasteful X11 traffic, but proven not to be
// what caused this specific symptom - given its own change-detection
// guard anyway, matching the pattern Bar::Redraw() already uses for
// itself.)
//
// Needs a real X11 connection (TrayIconClient creates a real X
// window). Same "gracefully skip without a real, even if headless/
// Xvfb, $DISPLAY" deal as test_bar.cpp - kept out of `make test` the
// same way (see "test-trayiconclient" in the Makefile).
//
// Per the fix's own request: reproduces the actual failure condition
// (a real icon-name lookup against the real resolver, genuinely
// failing) and reads back real pixels from the tray icon's own live
// window with XGetImage, rather than only asserting that DrawText()
// was called.

#include "TrayIconClient.h"
#include "UiIconCache.h"
#include "UiPopupMenu.h"

#include <X11/Xlib.h>

#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>

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

// True if any pixel in the given rectangle differs from `expected` -
// robust against exactly where within the icon a glyph's baseline/
// advance width happen to put its ink, unlike checking one fixed
// pixel would be.
bool AnyPixelDiffers(Display* display, Window window, int x, int y, int w, int h, unsigned long expected)
{
    XImage* image = XGetImage(display, window, x, y, w, h, AllPlanes, ZPixmap);

    if (!image)
        return false;

    bool differs = false;

    for (int py = 0; py < h && !differs; ++py)
        for (int px = 0; px < w; ++px)
            if ((XGetPixel(image, px, py) & 0xFFFFFF) != (expected & 0xFFFFFF))
            {
                differs = true;
                break;
            }

    XDestroyImage(image);
    return differs;
}

// The raw pixel value at one point, masked to 24 bits so it compares
// directly against the 0xRRGGBB values every Kohiko colour config key
// (bar.background included) is written as.
unsigned long SamplePixel(Display* display, Window window, int x, int y)
{
    XImage* image = XGetImage(display, window, x, y, 1, 1, AllPlanes, ZPixmap);
    if (!image)
        return 0;

    unsigned long pixel = XGetPixel(image, 0, 0) & 0xFFFFFF;
    XDestroyImage(image);
    return pixel;
}

// Blocks (briefly) until the server actually reports `window` as
// viewable - XSync() alone only guarantees the map *request* was
// processed, not that every downstream piece of server-side window
// state (this included) has settled yet. Shared by both sections of
// this file.
void WaitUntilViewable(Display* display, Window window)
{
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        XWindowAttributes attrs{};
        XGetWindowAttributes(display, window, &attrs);

        if (attrs.map_state == IsViewable)
            return;

        XSync(display, False);
        usleep(10000);
    }
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

    TrayIconClient tray;

    if (!tray.Create(22))
    {
        std::printf("Could not create a tray icon window against display \"%s\" - skipping.\n", displayEnv);
        std::printf("\n0 CHECKS RUN (skipped).\n");
        return 0;
    }

    Display* display = tray.GetDisplay();

    // This test is specifically about drawing/pixel correctness, not
    // the XEmbed docking protocol (SystemTray.cpp's own concern, and
    // already exercised live by the running desktop) - map the window
    // directly rather than going through TryDock()/Run(), which needs
    // an actual host and blocks in an event loop. Polls for the
    // server to actually report the window viewable before touching
    // it with XGetImage - XSync() alone only guarantees the map
    // *request* was processed, not that every downstream piece of
    // server-side window state has settled yet.
    XMapWindow(display, tray.GetWindow());
    WaitUntilViewable(display, tray.GetWindow());

    UiIconCache iconCache(tray.GetDisplay(), tray.GetVisual(), tray.GetColormap(),
        RootWindow(tray.GetDisplay(), tray.GetScreen()));

    std::printf("-- Reproducing the actual failure condition: a real, failed icon lookup --\n");

    // Deliberately not a real freedesktop icon name anywhere - this
    // reproduces, exactly, what every one of Kohiko's own tray tools
    // hits for names like "audio-volume-high" on a system whose icon
    // theme chain doesn't carry them (see this file's own header
    // comment) - a real lookup against the real resolver, genuinely
    // failing, not a stubbed-out or asserted result.
    const std::string missingIconName = "kohiko-test-definitely-nonexistent-icon-xyz789";

    Pixmap pixmap = None, mask = None;
    bool resolved = iconCache.Get(missingIconName, 18, pixmap, mask);
    Check(!resolved, "a genuinely nonexistent icon name fails to resolve via the real IconResolver/UiIconCache");

    std::printf("\n-- The bug, reproduced directly: no fallback leaves the window unchanged --\n");

    GC gc = XCreateGC(display, tray.GetWindow(), 0, nullptr);
    XSetForeground(display, gc, tray.Theme().background);
    XFillRectangle(display, tray.GetWindow(), gc, 0, 0, 22, 22);
    XSync(display, False);

    // Exactly what every tray tool's DrawCallback used to do when
    // Get() returned false: nothing further at all. The window is
    // left showing only the plain background fill from above.
    Check(!AnyPixelDiffers(display, tray.GetWindow(), 0, 0, 22, 22, tray.Theme().background),
          "without a fallback, a failed icon resolution leaves the whole icon showing only the plain "
          "background fill - genuinely indistinguishable from a broken/blank icon (the bug itself, "
          "reproduced on a real window, not merely inferred from Get() returning false)");

    std::printf("\n-- The fix: TrayIconClient::DrawText() actually changes real pixels --\n");

    // Exactly what each tray tool's DrawCallback now does in its
    // `else` branch - see e.g. kohiko-audio-tray.cpp's FallbackForOutput().
    int textWidth = tray.GetFont().TextWidth("\u00d7");
    int baseline = (22 + tray.GetFont().Ascent()) / 2;
    tray.DrawText((22 - textWidth) / 2, baseline, "\u00d7", tray.Theme().muted);
    XSync(display, False);

    Check(AnyPixelDiffers(display, tray.GetWindow(), 0, 0, 22, 22, tray.Theme().background),
          "TrayIconClient::DrawText() - the fix - actually paints real, non-background content onto "
          "the live window, where the no-fallback case above provably did not");

    XFreeGC(display, gc);

    std::printf("\n-- The 0.21.2 bug: a real host whose background differs from UiTheme::Default() --\n");

    // A second, independent TrayIconClient/connection - `tray` above
    // is already mapped directly as a child of root (deliberately, to
    // stay out of the real XEmbed dance - see its own comment), which
    // this section needs to actually reparent into something.
    //
    // Deliberately NOT UiTheme::Default().background (0x121318, see
    // UiTheme.h): this is standing in for a real Bar whose
    // bar.background is actually configured to something else, which
    // is the ordinary case (0x1e1e2e is this project's own
    // config/default.conf value) - exactly the case the old
    // implementation got wrong.
    constexpr unsigned long kFakeBarBackground = 0x1e1e2e;
    constexpr unsigned long kFakeBarBackgroundAfterReload = 0x2a1a3a;

    TrayIconClient tray2;
    Check(tray2.Create(22), "a second TrayIconClient/X connection can be created for the docking section");

    Display* display2 = tray2.GetDisplay();
    int screen2 = tray2.GetScreen();

    // Stands in for Kohiko's own Bar window (Bar.cpp reads
    // bar.background from config and sets exactly this attribute on
    // its own window - see Bar::ApplyTheme()/its constructor) - a
    // real, solid background_pixel, which is what a ParentRelative
    // chain has to eventually bottom out on to mean anything.
    XSetWindowAttributes barAttrs{};
    barAttrs.background_pixel = kFakeBarBackground;
    Window fakeBar = XCreateWindow(
        display2, RootWindow(display2, screen2),
        0, 0, 300, 30, 0,
        DefaultDepth(display2, screen2), InputOutput, DefaultVisual(display2, screen2),
        CWBackPixel, &barAttrs);
    XMapWindow(display2, fakeBar);

    // Stands in for SystemTray's own container window (see
    // SystemTray::Initialize()/DockIcon() - already ParentRelative,
    // chained to the Bar exactly like this) - not the thing under
    // test here, just faithfully reproducing what a docked tray icon
    // is actually reparented into on a real desktop.
    XSetWindowAttributes containerAttrs{};
    containerAttrs.background_pixmap = ParentRelative;
    Window fakeContainer = XCreateWindow(
        display2, fakeBar,
        0, 0, 22, 22, 0,
        DefaultDepth(display2, screen2), InputOutput, DefaultVisual(display2, screen2),
        CWBackPixmap, &containerAttrs);
    XMapWindow(display2, fakeContainer);

    // What SystemTray::DockIcon() actually does to a real tray icon
    // window once it accepts a dock request: reparent it in, then map
    // it again in its new parent.
    XReparentWindow(display2, tray2.GetWindow(), fakeContainer, 0, 0);
    XMapWindow(display2, tray2.GetWindow());
    WaitUntilViewable(display2, tray2.GetWindow());

    // No DrawCallback set at all - this section is purely about what
    // Redraw() leaves the window showing on its own, before any icon
    // or fallback glyph is ever drawn on top.
    tray2.Redraw();
    XSync(display2, False);

    unsigned long dockedBackground = SamplePixel(display2, tray2.GetWindow(), 11, 11);

    Check(dockedBackground == kFakeBarBackground,
          "a tray icon docked into a host whose real background is 0x1e1e2e shows that real colour, "
          "not some other constant of its own");

    Check(dockedBackground != tray2.Theme().background,
          "that real colour is not UiTheme::Default().background (0x121318) - this is exactly the "
          "assertion the pre-fix implementation could never pass once a real host's background "
          "differed from this struct's built-in constant, which is the ordinary case (see this "
          "file's own header comment)");

    std::printf("\n-- Confirming this tracks the host live, not just at dock time --\n");

    // A live bar.background change - WindowManager::ReloadConfig()'s
    // own real effect on a running Bar - without touching the already-
    // docked tray icon window at all: no re-dock, no new attributes on
    // it, nothing. If this still shows the fake bar's new colour
    // afterwards, the ParentRelative chain (Create()'s fix) is doing
    // real, live work here, not just something that happened to look
    // right once at dock time and would drift out of sync on the next
    // config reload exactly like the old hardcoded fill did.
    XSetWindowBackground(display2, fakeBar, kFakeBarBackgroundAfterReload);
    XClearWindow(display2, fakeBar);
    tray2.Redraw();
    XSync(display2, False);

    Check(SamplePixel(display2, tray2.GetWindow(), 11, 11) == kFakeBarBackgroundAfterReload,
          "the docked tray icon's background follows a live change to the host's own background "
          "(a config reload changing bar.background) with no re-dock and no changes of its own");

    // No explicit XDestroyWindow for fakeContainer/fakeBar here:
    // they're on the same connection (display2) tray2's own
    // destructor is about to close, and destroying a connection tears
    // down every window it owns together - tray2.GetWindow() included,
    // since XReparentWindow above made it a descendant of fakeBar.
    // Destroying fakeBar explicitly first would recursively destroy
    // tray2's window as a side effect, then tray2's own destructor
    // destroying that already-gone window a second time is a BadWindow
    // - harmless (Xlib's default handler doesn't treat it as fatal),
    // but avoided entirely by just not doing either explicitly.

    std::printf("\n-- The 0.21.3 bug: an Expose for the tray icon, arriving while its own "
                "popup is still open, used to be lost for good --\n");

    // A third, independent TrayIconClient/connection, docked into its
    // own fake Bar/container pair the same way as the section above -
    // this one is specifically for exercising the real click ->
    // PopupMenu::Show() -> dismiss -> Run() lifecycle, so it needs its
    // own real event loop rather than being drawn into directly.
    constexpr unsigned long kThirdFakeBarBackground = 0x1e1e2e;

    TrayIconClient tray3;
    Check(tray3.Create(22), "a third TrayIconClient/X connection can be created for the popup-lifecycle section");

    Display* display3 = tray3.GetDisplay();
    int screen3 = tray3.GetScreen();
    Window trayWindow3 = tray3.GetWindow();

    XSetWindowAttributes bar3Attrs{};
    bar3Attrs.background_pixel = kThirdFakeBarBackground;
    Window fakeBar3 = XCreateWindow(
        display3, RootWindow(display3, screen3), 0, 0, 300, 30, 0,
        DefaultDepth(display3, screen3), InputOutput, DefaultVisual(display3, screen3),
        CWBackPixel, &bar3Attrs);
    XMapWindow(display3, fakeBar3);

    XSetWindowAttributes container3Attrs{};
    container3Attrs.background_pixmap = ParentRelative;
    Window fakeContainer3 = XCreateWindow(
        display3, fakeBar3, 0, 0, 22, 22, 0,
        DefaultDepth(display3, screen3), InputOutput, DefaultVisual(display3, screen3),
        CWBackPixmap, &container3Attrs);
    XMapWindow(display3, fakeContainer3);

    XReparentWindow(display3, trayWindow3, fakeContainer3, 0, 0);
    XMapWindow(display3, trayWindow3);
    WaitUntilViewable(display3, trayWindow3);

    // A distinctive "icon" - solid magenta in the middle, leaving a
    // border so the background is independently sampleable. What
    // matters for this section is purely whether this gets (re)drawn
    // after the popup closes, not what a real icon looks like.
    tray3.SetDrawCallback([](Display* d, Window w, GC gc, Visual*, Colormap, int size)
    {
        XSetForeground(d, gc, 0xFF00FF);
        XFillRectangle(d, w, gc, 4, 4, size - 8, size - 8);
    });
    tray3.Redraw();
    XSync(display3, False);

    Check(SamplePixel(display3, trayWindow3, 11, 11) == 0xFF00FF,
          "the tray icon renders normally before any popup interaction");

    XSync(display3, True); // drain setup-generated events, isolate what follows

    Window dummyChild3;
    int rootX3 = 0, rootY3 = 0;
    XTranslateCoordinates(display3, trayWindow3, RootWindow(display3, screen3), 0, 0, &rootX3, &rootY3, &dummyChild3);

    // What every real tray tool's click handler does: build the popup
    // and call Show() from inside the handler itself, which blocks
    // until the popup is dismissed. Positioned well away from the
    // tray icon - deliberately *not* overlapping it - so the only
    // thing that can possibly cause the tray icon to need redrawing
    // during this section is the synthetic Expose sent below, with no
    // second, naturally-occurring one (from the popup itself covering
    // and later uncovering the icon) able to mask whether that one
    // survived on its own.
    tray3.SetLeftClickHandler([&]
    {
        PopupMenu menu;
        menu.AddItem("Test Item", [] {});
        menu.Show(display3, screen3, tray3.GetVisual(), tray3.GetColormap(), tray3.GetFont(), tray3.Theme(),
                  rootX3 + 200, rootY3 + 200);
    });

    pid_t clickerPid = fork();

    if (clickerPid == 0)
    {
        // --- "clicker" process: independent X connection, standing in
        // for real user input arriving concurrently with the popup's
        // own event loop. ---
        Display* d4 = XOpenDisplay(nullptr);
        if (!d4) _exit(1);
        Window root = RootWindow(d4, DefaultScreen(d4));

        usleep(200000); // let the parent get into Run()'s poll() first

        Window before[512];
        unsigned int nBefore = 0;
        {
            Window r, p, *kids = nullptr;
            unsigned int n = 0;
            XQueryTree(d4, root, &r, &p, &kids, &n);
            nBefore = n < 512 ? n : 512;
            for (unsigned int i = 0; i < nBefore; ++i) before[i] = kids[i];
            if (kids) XFree(kids);
        }

        XEvent press{};
        press.xbutton.type = ButtonPress;
        press.xbutton.display = d4;
        press.xbutton.window = trayWindow3;
        press.xbutton.root = root;
        press.xbutton.x = 11; press.xbutton.y = 11;
        press.xbutton.x_root = rootX3 + 11; press.xbutton.y_root = rootY3 + 11;
        press.xbutton.button = Button1;
        press.xbutton.time = CurrentTime;
        press.xbutton.same_screen = True;
        XSendEvent(d4, trayWindow3, False, ButtonPressMask, &press);

        XEvent release = press;
        release.xbutton.type = ButtonRelease;
        XSendEvent(d4, trayWindow3, False, ButtonReleaseMask, &release);
        XFlush(d4);

        Window popup = 0;
        for (int tries = 0; tries < 500 && !popup; ++tries)
        {
            usleep(10000);
            Window r, p, *kids = nullptr;
            unsigned int n = 0;
            XQueryTree(d4, root, &r, &p, &kids, &n);
            for (unsigned int i = 0; i < n; ++i)
            {
                bool known = false;
                for (unsigned int j = 0; j < nBefore; ++j)
                    if (kids[i] == before[j]) known = true;
                if (!known) { popup = kids[i]; break; }
            }
            if (kids) XFree(kids);
        }

        if (!popup)
            _exit(2);

        // The crux of this test: a synthetic Expose for the tray
        // icon's own window, sent while PopupMenu::Show()'s loop is
        // confirmed still running - standing in for any of the real
        // reasons an Expose can arrive for the tray icon during a
        // popup's lifetime (most commonly the popup itself covering
        // and later uncovering the icon - deliberately not the case
        // here, see the popup's own position above, so there's no
        // second, naturally-occurring Expose able to mask whether
        // *this* one survived on its own). A real Expose is always
        // paired with the server having already repainted that region
        // from the window's background attribute *before* notifying
        // the client - that's what actually erases the previously-
        // drawn icon glyph in the real bug, not the event by itself -
        // so this clears the window the same way here, right before
        // sending the event, rather than just sending a notification
        // about content that was never actually touched.
        XClearWindow(d4, trayWindow3);
        XFlush(d4);

        XEvent expose{};
        expose.xexpose.type = Expose;
        expose.xexpose.display = d4;
        expose.xexpose.window = trayWindow3;
        expose.xexpose.width = 22;
        expose.xexpose.height = 22;
        expose.xexpose.count = 0;
        XSendEvent(d4, trayWindow3, False, ExposureMask, &expose);
        XFlush(d4);
        usleep(50000);

        XEvent click{};
        click.xbutton.type = ButtonRelease;
        click.xbutton.display = d4;
        click.xbutton.window = popup;
        click.xbutton.root = root;
        click.xbutton.x = 10; click.xbutton.y = 15;
        click.xbutton.x_root = rootX3 + 10; click.xbutton.y_root = rootY3 + 15;
        click.xbutton.button = Button1;
        click.xbutton.time = CurrentTime;
        click.xbutton.same_screen = True;
        XSendEvent(d4, popup, False, ButtonReleaseMask, &click);
        XFlush(d4);

        XCloseDisplay(d4);
        _exit(0);
    }

    tray3.SetInterval(500, [&] { tray3.Quit(); });
    tray3.Run();

    waitpid(clickerPid, nullptr, 0);

    Check(SamplePixel(display3, trayWindow3, 11, 11) == 0xFF00FF,
          "after a real click opens the popup, an Expose for the tray icon arrives while the popup is "
          "still open, and the popup is then dismissed, the tray icon's own icon glyph is still there "
          "afterward - this is exactly the assertion the pre-fix PopupMenu::Show() (misattributing any "
          "Expose to itself) and TrayIconClient::Run() (only checking XPending() when poll() saw new "
          "socket data, blind to anything Xlib already buffered internally) could not pass");

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    return 0;
}

