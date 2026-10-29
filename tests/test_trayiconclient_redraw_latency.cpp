// Regression + measurement test: a real state change must become
// VISIBLE on a Kohiko tray icon promptly, not whenever the tray tool's
// event loop happens to wake up next for some unrelated reason.
//
// The bug (0.21.4 and earlier): TrayIconClient::Redraw() issued its
// XClearWindow + draw calls and returned without flushing Xlib's output
// buffer, and Run() then went straight back into a blocking poll(). So
// a state change - say a PipeWire callback on a watched fd calling
// RequestRedraw() - redrew into a client-side buffer that nothing sent
// to the X server until the NEXT wake-up: the tools' 2 s reconnect/
// state timer, an unrelated X event, or a click. The icon stayed stale
// for that long ("a second or several"). It was hidden for years by
// kohiko-audio-tray's permanent 100 ms ticker, which woke the loop (and
// therefore flushed) ten times a second whether or not anything had
// changed; removing that ticker in 0.21.4 exposed it. The fix is
// event-driven: flush when a redraw has been drawn, and nothing else.
//
// How this measures it - the production path, end to end:
//   - a real TrayIconClient on its own X connection, running Run()
//     on a worker thread;
//   - a pipe fd registered with WatchFd() whose callback flips state
//     and calls RequestRedraw() - exactly what the audio/network/
//     bluetooth tools do for PipeWire/D-Bus activity;
//   - optionally a no-op 2 s SetInterval(), like the real tools'
//     reconnect/state timers (the pre-fix wake-up that eventually
//     flushed the stale draw);
//   - an INDEPENDENT second X connection (this thread) that reads the
//     window's real pixels back from the X server with XGetImage, so
//     "visible" means the server has the new pixels, not that the
//     client merely issued the draw.
//
// Thresholds are deliberately generous (500 ms) against the failure
// being measured (~2000 ms or never) and the fixed behaviour
// (low single-digit ms), so it cannot be flaky on a loaded machine.
//
// Also asserts the fix did not "solve" latency with polling: while idle
// (no state change) the icon is never redrawn.
//
// Needs an X display (run under xvfb-run, as `make test` does); skips
// cleanly with exit 0 if there isn't one.

#include "TrayIconClient.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace Kohiko;
using Clock = std::chrono::steady_clock;

namespace
{

int g_pass = 0;
int g_fail = 0;

void Check(bool ok, const char* what)
{
    if (ok) { ++g_pass; std::printf("  PASS: %s\n", what); }
    else    { ++g_fail; std::printf("  FAIL: %s\n", what); }
}

double MsSince(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// The pixel at the window's center as the X SERVER sees it.
unsigned long ReadCenterPixel(Display* observer, Window window)
{
    XWindowAttributes attrs;

    if (!XGetWindowAttributes(observer, window, &attrs) || attrs.map_state != IsViewable)
        return ~0UL;

    XImage* image = XGetImage(observer, window, attrs.width / 2, attrs.height / 2, 1, 1, AllPlanes, ZPixmap);

    if (!image)
        return ~0UL;

    unsigned long pixel = XGetPixel(image, 0, 0);
    XDestroyImage(image);

    return pixel & 0xFFFFFFUL; // ignore any alpha channel
}

// Milliseconds until the server shows `wanted` at the center, or -1.
double WaitForPixel(Display* observer, Window window, unsigned long wanted, int timeoutMs)
{
    Clock::time_point start = Clock::now();

    while (MsSince(start) < timeoutMs)
    {
        if (ReadCenterPixel(observer, window) == wanted)
            return MsSince(start);

        usleep(2000);
    }

    return -1.0;
}

struct Scenario
{
    double initialPaintMs = -1;
    double changeMs = -1;
    int idleRedraws = -1;
};

Scenario RunScenario(Display* observer, bool withTwoSecondTimer)
{
    Scenario result;

    int pipefd[2];

    if (pipe(pipefd) != 0)
        return result;

    std::atomic<int> state{0}; // 0 -> red, 1 -> green
    std::atomic<int> redraws{0};
    std::atomic<unsigned long> pixelRed{~0UL};
    std::atomic<unsigned long> pixelGreen{~0UL};

    TrayIconClient client;

    if (!client.Create(22))
        return result;

    client.SetDrawCallback(
        [&](Display* display, Window window, GC gc, Visual*, Colormap colormap, int size)
        {
            XColor red{};   red.red = 0xFFFF;   red.flags = DoRed | DoGreen | DoBlue;
            XColor green{}; green.green = 0xFFFF; green.flags = DoRed | DoGreen | DoBlue;
            XAllocColor(display, colormap, &red);
            XAllocColor(display, colormap, &green);
            pixelRed = red.pixel & 0xFFFFFFUL;
            pixelGreen = green.pixel & 0xFFFFFFUL;

            XSetForeground(display, gc, state == 0 ? red.pixel : green.pixel);
            XFillRectangle(display, window, gc, 0, 0, size, size);
            ++redraws;
        });

    // The production path: a watched fd's callback changes state and
    // asks for a redraw.
    client.WatchFd(pipefd[0], [&]
    {
        char byte = 0;

        if (read(pipefd[0], &byte, 1) != 1)
            return;

        if (byte == 'q')
        {
            client.Quit();
            return;
        }

        state = 1;
        client.RequestRedraw();
    });

    // The real tools always have at least one slow timer (PipeWire/
    // D-Bus reconnect, state refresh). It is the wake-up that used to
    // eventually flush the stale draw.
    if (withTwoSecondTimer)
        client.SetInterval(2000, [] {});

    // No tray host exists in this test, so nothing will dock/map the
    // icon: map it ourselves as a root-level window, then let the
    // client's own Run() loop take over its connection.
    XMapWindow(client.GetDisplay(), client.GetWindow());
    XFlush(client.GetDisplay());

    std::thread worker([&] { client.Run(); });

    // (1) The very first paint after the window is mapped (Expose ->
    // redraw): time from the map until the server actually shows red.
    // (`pixelRed` only becomes known once the draw callback has run
    // once; until then WaitForPixel() cannot match, which is correct -
    // nothing was drawn yet.)
    Clock::time_point mapped = Clock::now();

    while (pixelRed == ~0UL && MsSince(mapped) < 3000)
        usleep(1000);

    double firstPaint = WaitForPixel(observer, client.GetWindow(), pixelRed, 4000);
    result.initialPaintMs = firstPaint < 0 ? -1.0 : MsSince(mapped);

    // (2) A state change arrives on the watched fd.
    Clock::time_point changed = Clock::now();
    char go = 'x';
    if (write(pipefd[1], &go, 1) != 1)
        return result;

    double waitedForGreen = WaitForPixel(observer, client.GetWindow(), pixelGreen, 4000);
    result.changeMs = waitedForGreen < 0 ? -1.0 : MsSince(changed);

    // (3) Event-driven, not polled: with nothing changing, the icon
    // must not be redrawn at all.
    int before = redraws;
    usleep(1200 * 1000);
    result.idleRedraws = redraws - before;

    char quit = 'q';
    if (write(pipefd[1], &quit, 1) != 1)
        client.Quit();

    worker.join();
    close(pipefd[0]);
    close(pipefd[1]);

    return result;
}

}

int main()
{
    Display* observer = XOpenDisplay(nullptr);

    if (!observer)
    {
        std::printf("SKIP: no X display available.\n");
        return 0;
    }

    struct Case { const char* name; bool timer; };
    const Case cases[] = {
        { "with the tools' usual 2 s timer (the pre-fix wake-up that eventually flushed)", true },
        { "with NO timer at all (pre-fix: stale until some unrelated X event)", false },
    };

    for (const Case& c : cases)
    {
        std::printf("-- State change on a watched fd, %s --\n", c.name);
        Scenario r = RunScenario(observer, c.timer);

        std::printf("  (first paint visible after %.1f ms; state change visible after %.1f ms; "
                    "redraws while idle for 1.2 s: %d)\n",
                    r.initialPaintMs, r.changeMs, r.idleRedraws);

        Check(r.initialPaintMs >= 0 && r.initialPaintMs < 500,
              "the first paint after mapping reaches the X server promptly (< 500 ms)");
        Check(r.changeMs >= 0 && r.changeMs < 500,
              "a real state change is visible on screen promptly (< 500 ms), not on the next unrelated wake-up");
        Check(r.idleRedraws == 0,
              "event-driven: nothing is redrawn while nothing changes (no polling was introduced)");
    }

    XCloseDisplay(observer);

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
