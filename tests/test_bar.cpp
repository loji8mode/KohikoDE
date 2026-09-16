// Regression/performance test for the idle-CPU finding from the
// 0.20.6 performance audit (see CHANGELOG.md): Bar::Redraw() used to
// unconditionally repaint the *entire* bar - background fill, every
// workspace label, scratchpad/notepad indicators, tray reposition,
// power button, and the clock - on every single call, including the
// once-a-second call WindowManager::Tick() makes purely to keep the
// clock live. In the idle steady state (by far the most common case
// Redraw() runs for) everything except the clock's own digits is
// pixel-identical to what's already on screen, so that was a real,
// measured, unconditional X11 round trip every second for the entire
// life of the process, for content that didn't need to change.
//
// Needs a real X11 connection (Bar creates real X windows/pixmaps),
// same "gracefully skip without a real, even if headless/Xvfb,
// $DISPLAY" deal as test_monitormanager/test_svgrenderer - kept out
// of `make test` the same way (see "test-bar" in the Makefile).
//
// This can't see Bar's private m_lastDrawn*/m_hasDrawnOnce bookkeeping
// directly, so it verifies the property that actually matters from
// the outside: how many X11 protocol requests a Redraw() call
// generates, via XNextRequest()'s request-sequence counter (a plain
// Xlib mechanism for exactly this - not a private test hook). A tick
// where nothing but the clock changed must issue markedly fewer
// requests than a full repaint; a tick where something real changed
// (a workspace switch, or the bar having been hidden and shown again,
// where X11 doesn't guarantee prior window content survived) must
// still issue a full repaint's worth - proving both that the fast
// path actually engages AND that it never accidentally papers over a
// real change.

#include "Bar.h"

#include "Config.h"
#include "Types.h"
#include "XConnection.h"

#include <X11/Xlib.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
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

Config LoadConfigFromText(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream file(path, std::ios::trunc);
    file << text;
    file.close();

    Config config;
    config.Load(path.string());
    return config;
}

// Requests issued by exactly one Redraw() call, measured via the
// display's own request-sequence counter (XNextRequest() - "the
// request number that will be assigned by the next request", so the
// delta across a call is exactly how many requests that call made -
// no private access into Bar or Xlib internals needed).
unsigned long RequestsForOneRedraw(Display* display, Bar& bar)
{
    unsigned long before = XNextRequest(display);
    bar.Redraw();
    XSync(display, False);
    unsigned long after = XNextRequest(display);
    return after - before;
}

// Reads back a single pixel actually on screen - used to verify a
// forced repaint restores *real* content, not just that some X11
// activity happened (which the request-count checks above already
// cover well, but can't tell correct pixels from merely-different
// ones).
unsigned long ReadPixel(Display* display, ::Window window, int x, int y)
{
    XImage* image = XGetImage(display, window, x, y, 1, 1, AllPlanes, ZPixmap);

    if (!image)
        return static_cast<unsigned long>(-1);

    unsigned long pixel = XGetPixel(image, 0, 0);
    XDestroyImage(image);
    return pixel;
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

    XConnection connection;

    if (!connection.Connect())
    {
        std::printf("Could not open display \"%s\" - skipping.\n", displayEnv);
        std::printf("\n0 CHECKS RUN (skipped).\n");
        return 0;
    }

    Display* display = connection.GetDisplay();

    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "kohiko-test-bar";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directories(tempDir);

    Config config = LoadConfigFromText(tempDir / "kohiko.conf",
        "general.bar_height=26\n"
        "general.font=monospace:pixelsize=14\n"
        "bar.background=0x1e1e2e\n"
        "bar.foreground=0xcdd6f4\n"
        "bar.active=0x89b4fa\n");

    Rect monitorGeometry{0, 0, 1920, 26};

    Bar bar(connection);
    bar.Configure(config, monitorGeometry);
    bar.SetWorkspaces(4, 1);
    bar.Show();

    std::printf("-- Bar::Redraw() request-count behaviour --\n");

    unsigned long firstRedrawRequests = RequestsForOneRedraw(display, bar);
    std::printf("    (first-ever redraw: %lu X11 requests - always a full repaint)\n", firstRedrawRequests);

    Check(firstRedrawRequests > 0, "the first-ever Redraw() call actually issues X11 requests (sanity check)");

    // Redraw() again with *nothing* changed. Depending on exactly
    // where within the current wall-clock second this runs, the clock
    // text may or may not have ticked over yet - either way this call
    // must never issue anywhere near a full repaint's worth of
    // requests, since nothing real changed.
    unsigned long immediateRepeatRequests = RequestsForOneRedraw(display, bar);
    std::printf("    (immediate repeat, same state: %lu X11 requests)\n", immediateRepeatRequests);

    Check(immediateRepeatRequests < firstRedrawRequests,
          "calling Redraw() again with nothing changed issues fewer requests than the first full repaint");

    // Sleep past a real wall-clock second boundary, guaranteeing the
    // clock text differs from what's currently on screen, with every
    // other part of the bar's state left exactly as it was - the
    // actual idle-tick scenario this optimization targets.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    unsigned long clockOnlyTickRequests = RequestsForOneRedraw(display, bar);
    std::printf("    (clock ticked over, nothing else changed: %lu X11 requests)\n", clockOnlyTickRequests);

    Check(clockOnlyTickRequests > 0,
          "a tick where the clock genuinely changed still redraws something (live-reload of the clock is preserved)");
    Check(clockOnlyTickRequests * 2 < firstRedrawRequests,
          "...but a clock-only tick costs less than half of a full repaint's request count - "
          "the fast path is actually engaging, not silently falling back to a full redraw every time");

    // A *real* change: switch the active workspace. This must go back
    // to a full repaint's magnitude - proving the fast path never
    // papers over an actual state change.
    bar.SetWorkspaces(4, 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    unsigned long realChangeRequests = RequestsForOneRedraw(display, bar);
    std::printf("    (real change - workspace switched: %lu X11 requests)\n", realChangeRequests);

    Check(realChangeRequests >= firstRedrawRequests / 2,
          "a genuine workspace-switch redraw is back to full-repaint magnitude, not the cheap clock-only path");
    Check(realChangeRequests > clockOnlyTickRequests,
          "...clearly more than a clock-only tick costs, confirming the fast path didn't quietly \"win\" here");

    // Immediately after that real change, nothing changed again - back
    // to the cheap path.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    unsigned long afterRealChangeRequests = RequestsForOneRedraw(display, bar);
    std::printf("    (tick right after the real change, nothing further changed: %lu X11 requests)\n",
                afterRealChangeRequests);

    Check(afterRealChangeRequests * 2 < firstRedrawRequests,
          "the fast path re-engages on the very next tick after a real change, not just once ever");

    // Hide()/Show() - X11 doesn't guarantee a plain window's prior
    // pixels survive an unmap/remap, so this must force a full repaint
    // even though none of SetWorkspaces()/SetScratchpadActive()/etc.
    // were touched.
    bar.Hide();
    bar.Show();
    unsigned long afterShowRequests = RequestsForOneRedraw(display, bar);
    std::printf("    (redraw right after Hide()+Show(), same logical state: %lu X11 requests)\n",
                afterShowRequests);

    Check(afterShowRequests >= firstRedrawRequests / 2,
          "Hide() then Show() forces a full repaint on the next Redraw() - window content isn't assumed to "
          "have survived being unmapped, even though nothing else about the bar's state changed");

    // Configure() (a monitor resize/hotplug or config reload, in
    // practice) must equally force a full repaint on the next call.
    Rect widerGeometry{0, 0, 2560, 26};
    bar.Configure(config, widerGeometry);
    unsigned long afterConfigureRequests = RequestsForOneRedraw(display, bar);
    std::printf("    (redraw right after Configure() with a new width: %lu X11 requests)\n",
                afterConfigureRequests);

    Check(afterConfigureRequests >= firstRedrawRequests / 2,
          "Configure() (geometry change) forces a full repaint on the next Redraw() too");

    std::printf("\n-- A live bug report, reproduced directly: Expose-style external window damage --\n");
    std::printf("   (WindowManager::HandleExpose() calling Redraw(forceFullRepaint=true) is what fixes this)\n");
    {
        // A background-only spot, comfortably clear of any text
        // glyph's anti-aliased edges - an unambiguous solid-color
        // check rather than one sensitive to font rendering
        // specifics. Deliberately chosen well inside the bar's width
        // (300px, against a 1920px-wide bar) so it's background
        // regardless of exactly how many workspace labels/indicators
        // are drawn.
        int probeX = 300;
        int probeY = 5;
        Display* rawDisplay = display;

        bar.Redraw(); // a known-correct full repaint to establish a baseline
        unsigned long correctPixel = ReadPixel(rawDisplay, bar.WindowId(), probeX, probeY);
        Check(correctPixel != static_cast<unsigned long>(-1),
              "can read back a real pixel from the bar's window (sanity check for this whole section)");

        // Simulates exactly what an Expose event's damage looks like -
        // some other window had been overlapping this exact spot and
        // just moved away, and the X server makes no promise about
        // what pixels are left behind. Drawn directly on the *window*
        // (m_backing, the off-screen source of truth, is completely
        // untouched by this - same as a real overlap would leave it).
        GC scratchGc = XCreateGC(rawDisplay, bar.WindowId(), 0, nullptr);
        XSetForeground(rawDisplay, scratchGc, 0xFF00FF); // a color nothing in this bar would ever legitimately draw
        XFillRectangle(rawDisplay, bar.WindowId(), scratchGc, probeX - 2, probeY - 2, 4, 4);
        XSync(rawDisplay, False);

        unsigned long corruptedPixel = ReadPixel(rawDisplay, bar.WindowId(), probeX, probeY);
        Check(corruptedPixel != correctPixel,
              "the simulated external damage actually changed that pixel (sanity check on the simulation itself)");

        // The actual regression: an ordinary Redraw() (what
        // WindowManager::Tick() calls every second, and what
        // HandleExpose() called before this fix) with the bar's
        // logical state completely unchanged since the last full
        // repaint correctly - by design - takes the cheap clock-only
        // path, which never touches this spot. This demonstrates the
        // exact mechanism the live bug report hit: real content stays
        // stale here until something *else* happens to trigger a full
        // repaint.
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        bar.Redraw();
        unsigned long afterOrdinaryRedraw = ReadPixel(rawDisplay, bar.WindowId(), probeX, probeY);
        Check(afterOrdinaryRedraw == corruptedPixel,
              "...and confirms it: an ordinary Redraw() with nothing logically changed does NOT touch that "
              "spot - it stays damaged, exactly like the live report (tray icons/workspace highlight frozen "
              "until something unrelated changed)");

        // Re-damage the same spot, then redraw exactly the way
        // HandleExpose() now does.
        XFillRectangle(rawDisplay, bar.WindowId(), scratchGc, probeX - 2, probeY - 2, 4, 4);
        XSync(rawDisplay, False);
        Check(ReadPixel(rawDisplay, bar.WindowId(), probeX, probeY) != correctPixel,
              "re-damaged the same spot for the actual fix check below");

        bar.Redraw(/*forceFullRepaint=*/true);
        unsigned long afterForcedRedraw = ReadPixel(rawDisplay, bar.WindowId(), probeX, probeY);
        Check(afterForcedRedraw == correctPixel,
              "Redraw(forceFullRepaint=true) - what HandleExpose() calls - restores the real, correct pixel "
              "even though nothing about the bar's logical state changed: this is the actual fix");

        XFreeGC(rawDisplay, scratchGc);
    }

    // Sanity: PowerButtonRect()/Geometry() are still well-formed after
    // all of the above (nothing left the bar in a half-updated state).
    Check(bar.PowerButtonRect().width > 0, "PowerButtonRect() still has a sane width after all the above");
    Check(bar.Geometry().width == 2560, "Geometry() reflects the last Configure() call");

    std::filesystem::remove_all(tempDir);

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    return 0;
}
