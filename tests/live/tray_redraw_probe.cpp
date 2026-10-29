// Live measurement/regression probe for tray-icon redraw latency
// (CHANGELOG.md 0.22.0) - NOT a mock: it links the real TrayIconClient,
// docks into whatever real tray host owns _NET_SYSTEM_TRAY_S<screen>
// (a real `kohiko` under Xvfb), and measures from the outside how long
// a state change takes to become *visible pixels* on the server.
//
// The state change is delivered exactly the way a real tray tool gets
// one: a file descriptor registered with TrayIconClient::WatchFd()
// becomes readable (kohiko-audio-tray's PipeWire fd, DBus fds, ...),
// the callback flips some state and calls RequestRedraw(). A second,
// independent X connection (the parent process) watches the icon
// window's pixels via XGetImage and records when the new colour first
// appears.
//
// Usage: tray_redraw_probe [--trials N] [--reconnect-timer]
//   --reconnect-timer  also register a 2000 ms SetInterval() no-op,
//                      mimicking kohiko-audio-tray's PipeWire
//                      reconnect timer - the only periodic wakeup that
//                      tool has once its 100 ms ticker was removed.
// Prints one `trial <n> latency_ms=<x>` line per trial and a final
// `summary max_ms=<x> median_ms=<x>` line. A trial that never shows the
// new colour within 4 s reports latency_ms=4000 (== "still stale").
// Exit status is 0 when it could run at all, regardless of latency -
// the shell test applies the thresholds.

#include "TrayIconClient.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

static double NowMs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static const unsigned long kColors[2] = { 0xCC2222, 0x22CC22 }; // red, green

// ---------------------------------------------------------------- child
static int RunTrayClient(int triggerReadFd, int reportWriteFd, bool reconnectTimer)
{
    Kohiko::TrayIconClient tray;

    if (!tray.Create(22))
        return 2;

    static int state = 0;

    tray.SetDrawCallback([](Display* display, Window window, GC gc, Visual*, Colormap, int size)
    {
        XSetForeground(display, gc, kColors[state]);
        XFillRectangle(display, window, gc, 0, 0, size, size);
    });

    tray.WatchFd(triggerReadFd, [&]
    {
        char b;
        if (read(triggerReadFd, &b, 1) == 1)
        {
            state = (b == 'g') ? 1 : 0;
            tray.RequestRedraw();
        }
    });

    if (reconnectTimer)
        tray.SetInterval(2000, [] {});

    char line[64];
    int n = std::snprintf(line, sizeof line, "WINDOW=0x%lx\n", static_cast<unsigned long>(tray.GetWindow()));
    if (write(reportWriteFd, line, static_cast<size_t>(n)) < 0)
        return 3;

    tray.Run();
    return 0;
}

// --------------------------------------------------------------- parent
static int IgnoreXError(Display*, XErrorEvent*) { return 0; }

int main(int argc, char** argv)
{
    int trials = 6;
    bool reconnectTimer = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--trials" && i + 1 < argc)      trials = std::atoi(argv[++i]);
        else if (a == "--reconnect-timer")        reconnectTimer = true;
    }

    int trigger[2], report[2];
    if (pipe(trigger) != 0 || pipe(report) != 0)
        return 1;

    pid_t child = fork();
    if (child == 0)
    {
        close(trigger[1]);
        close(report[0]);
        _exit(RunTrayClient(trigger[0], report[1], reconnectTimer));
    }

    close(trigger[0]);
    close(report[1]);

    Display* display = XOpenDisplay(nullptr);
    XSetErrorHandler(IgnoreXError); // a sample taken before the icon is viewable is just "no sample yet"
    if (!display)
    {
        std::fprintf(stderr, "tray_redraw_probe: cannot open display\n");
        kill(child, SIGTERM);
        return 1;
    }

    // Window id from the child.
    char buf[64] = {};
    pollfd pfd{ report[0], POLLIN, 0 };
    if (poll(&pfd, 1, 5000) <= 0 || read(report[0], buf, sizeof buf - 1) <= 0)
    {
        std::fprintf(stderr, "tray_redraw_probe: tray client did not start\n");
        kill(child, SIGTERM);
        return 1;
    }

    Window icon = std::strtoul(std::strchr(buf, '=') + 1, nullptr, 0);
    Window root = DefaultRootWindow(display);

    auto isDocked = [&]
    {
        Window r, parent, *kids = nullptr;
        unsigned int n = 0;
        if (!XQueryTree(display, icon, &r, &parent, &kids, &n))
            return false;
        if (kids) XFree(kids);
        return parent != root && parent != 0;
    };

    auto pixel = [&]() -> unsigned long
    {
        XImage* img = XGetImage(display, icon, 11, 11, 1, 1, AllPlanes, ZPixmap);
        if (!img)
            return 0xFFFFFFFFul;
        unsigned long p = XGetPixel(img, 0, 0);
        XDestroyImage(img);
        return p & 0xFFFFFF;
    };

    auto isViewable = [&]
    {
        XWindowAttributes a;
        return XGetWindowAttributes(display, icon, &a) && a.map_state == IsViewable;
    };

    // Wait to be docked AND viewable (TryDock retries every 3 s if the
    // host isn't up yet; the tray maps the icon only after reparenting).
    double giveUp = NowMs() + 10000;
    while (!(isDocked() && isViewable()) && NowMs() < giveUp)
        usleep(20000);

    if (!isDocked())
    {
        std::fprintf(stderr, "tray_redraw_probe: icon never docked - is a tray host running?\n");
        kill(child, SIGTERM);
        return 1;
    }

    usleep(600000); // let the initial draw settle

    std::vector<double> latencies;

    for (int t = 0; t < trials; ++t)
    {
        // Spread trial start times over the 2 s timer period so the
        // timer-bounded case is sampled at different phases.
        usleep(static_cast<useconds_t>(300000 + (t * 370000) % 1900000));

        int target = (t % 2 == 0) ? 1 : 0; // alternate green / red
        unsigned long want = kColors[target];

        double t0 = NowMs();
        char msg = target ? 'g' : 'r';
        if (write(trigger[1], &msg, 1) != 1)
            break;

        double latency = 4000;
        while (NowMs() - t0 < 4000)
        {
            if (pixel() == want)
            {
                latency = NowMs() - t0;
                break;
            }
            usleep(1000);
        }

        latencies.push_back(latency);
        std::printf("trial %d latency_ms=%.1f\n", t, latency);
        std::fflush(stdout);
    }

    kill(child, SIGTERM);
    waitpid(child, nullptr, 0);

    std::vector<double> sorted = latencies;
    std::sort(sorted.begin(), sorted.end());
    double mx = sorted.empty() ? -1 : sorted.back();
    double med = sorted.empty() ? -1 : sorted[sorted.size() / 2];
    std::printf("summary max_ms=%.1f median_ms=%.1f\n", mx, med);
    return 0;
}
