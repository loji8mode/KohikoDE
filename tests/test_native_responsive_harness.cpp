// Harness for tests/test_native_responsive_live.sh: drives the REAL
// BluetoothWindow (real UiWindow, real BluezClient, real D-Bus to the mock
// bluetoothd in tests/mock_bluez.py) with REAL X input events, and measures
// whether the window stays responsive while a slow Bluetooth operation is in
// flight. Prints "RESULT <name> <value>" lines for the shell test.
//
// Modes (argv[1]):
//   pending    click Pair (the mock holds it for ~4 s), then - while it is
//              still pending - click an independent control ("Scan for
//              devices") and then ask the window to CLOSE. Measures how long
//              each takes to be honoured.
//   close      click Pair, then ask the window to close 300 ms later, while the
//              Pair is certainly still pending: how long until it closes?
//   cancel     click Pair, then click the same button again (it is now
//              "Cancel"): the mock must see a real CancelPairing.
//   client     BluezClient API on its own (no window): async completion,
//              busy tracking, duplicate refusal, failure reporting, cancel
//              semantics.
//
// The window runs on the main thread exactly as in tools/kohiko-bluetooth.cpp;
// a driver thread with its OWN X connection sends the events (XSendEvent).
// The driver only reads widget geometry BEFORE the window starts running.

#include "BluetoothWindow.h"
#include "BluezClient.h"

#include <X11/Xlib.h>

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

using namespace Kohiko;
using Clock = std::chrono::steady_clock;

namespace
{

double MsSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
void SleepMs(int ms) { usleep(ms * 1000); }

std::string LogText(const char* path)
{
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Milliseconds until `needle` appears in the mock's call log, or -1.
double WaitForLog(const char* path, const std::string& needle, int timeoutMs)
{
    Clock::time_point start = Clock::now();
    while (MsSince(start) < timeoutMs)
    {
        if (LogText(path).find(needle) != std::string::npos)
            return MsSince(start);
        SleepMs(10);
    }
    return -1;
}

struct Found { bool ok = false; int x = 0; int y = 0; };

// Centre of the Button labelled `label`. In this toolkit a widget's bounds
// are WINDOW-ABSOLUTE (pages build them from window coordinates), not
// relative to their parent, so no offsets are summed.
Found FindButton(Widget& widget, const std::string& label)
{
    if (auto* button = dynamic_cast<Button*>(&widget); button && button->label == label)
        return { true, button->bounds.x + button->bounds.width / 2, button->bounds.y + button->bounds.height / 2 };

    for (const auto& child : widget.Children())
    {
        Found found = FindButton(*child, label);
        if (found.ok)
            return found;
    }

    return {};
}

void Click(Display* display, ::Window window, int x, int y)
{
    for (int type : { ButtonPress, ButtonRelease })
    {
        XEvent event{};
        event.xbutton.type = type;
        event.xbutton.display = display;
        event.xbutton.window = window;
        event.xbutton.root = DefaultRootWindow(display);
        event.xbutton.x = x;
        event.xbutton.y = y;
        event.xbutton.button = Button1;
        event.xbutton.same_screen = True;
        event.xbutton.state = type == ButtonRelease ? Button1Mask : 0;
        XSendEvent(display, window, False, ButtonPressMask | ButtonReleaseMask, &event);
        XFlush(display);
        usleep(20 * 1000);
    }
}

void RequestClose(Display* display, ::Window window)
{
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = XInternAtom(display, "WM_PROTOCOLS", False);
    event.xclient.format = 32;
    event.xclient.data.l[0] = static_cast<long>(XInternAtom(display, "WM_DELETE_WINDOW", False));
    event.xclient.data.l[1] = CurrentTime;
    XSendEvent(display, window, False, NoEventMask, &event);
    XFlush(display);
}

#ifndef KOHIKO_HARNESS_WINDOW_ONLY // (the client modes need the async API; window modes build against 0.21.5 too)
// ---- client mode: the BluezClient API without a window -------------------------

int RunClientMode(const char* logPath)
{
    (void)logPath;
    BluezClient bluez;

    if (!bluez.Connect() || !bluez.Available())
    {
        std::printf("RESULT error bluez-unavailable\n");
        return 2;
    }

    bool changed = false;
    bluez.SetChangeHandler([&] { changed = true; });

    auto pump = [&](int ms)
    {
        Clock::time_point start = Clock::now();
        while (MsSince(start) < ms)
        {
            pollfd pfd{ bluez.Fd(), POLLIN, 0 };
            if (poll(&pfd, 1, 20) > 0)
                bluez.Dispatch();
        }
    };

    const std::string device = "/org/bluez/hci0/dev_10";

    // Starting a slow Pair returns at once and marks the device busy.
    Clock::time_point t0 = Clock::now();
    bool pairDone = false, pairOk = true;
    std::string pairErr;
    bluez.PairDevice(device, [&](bool ok, const std::string& e) { pairDone = true; pairOk = ok; pairErr = e; });
    std::printf("RESULT start_returned_ms %.1f\n", MsSince(t0));
    std::printf("RESULT busy_while_pending %d\n", bluez.IsBusy(device) ? 1 : 0);
    std::printf("RESULT busy_label %s\n", bluez.BusyLabel(device).c_str());

    // A duplicate request while one is in flight is refused, not queued.
    bool dupDone = false, dupOk = true; std::string dupErr;
    bluez.PairDevice(device, [&](bool ok, const std::string& e) { dupDone = true; dupOk = ok; dupErr = e; });
    std::printf("RESULT duplicate_refused %d\n", (dupDone && !dupOk && dupErr == "already in progress") ? 1 : 0);

    // Meanwhile the client still processes other work (a fast call completes).
    bool fastDone = false;
    Clock::time_point fastStart = Clock::now();
    bluez.StartDiscovery("/org/bluez/hci0");
    pump(300);
    std::printf("RESULT pending_calls_during_pair %zu\n", static_cast<size_t>(bluez.IsBusy(device) ? 1 : 0));
    (void)fastDone; (void)fastStart;

    // Cancel the Pair: a real CancelPairing goes to the service, and the
    // outcome is reported (not silently dropped).
    bool cancelled = bluez.CancelPending(device);
    std::printf("RESULT cancel_accepted %d\n", cancelled ? 1 : 0);
    pump(300);
    std::printf("RESULT not_busy_after_cancel %d\n", bluez.IsBusy(device) ? 0 : 1);
    std::printf("RESULT last_error_after_cancel %s\n", bluez.LastError(device).c_str());
    std::printf("RESULT done_called_after_cancel %d\n", pairDone ? 1 : 0);
    std::printf("RESULT change_handler_fired %d\n", changed ? 1 : 0);
    (void)pairOk; (void)pairErr;

    // CancelPending on an idle object is a no-op that says so.
    std::printf("RESULT cancel_idle_returns_false %d\n", bluez.CancelPending(device) ? 0 : 1);
    return 0;
}

// ---- client-ok / client-fail: the two ways an action can finish -----------------

int RunOutcomeMode(bool expectFailure)
{
    BluezClient bluez;
    if (!bluez.Connect() || !bluez.Available()) { std::printf("RESULT error bluez-unavailable\n"); return 2; }

    bool changed = false;
    bluez.SetChangeHandler([&] { changed = true; });

    const std::string device = "/org/bluez/hci0/dev_10";
    bool done = false, ok = false;
    std::string error;

    if (expectFailure)
        bluez.PairDevice(device, [&](bool o, const std::string& e) { done = true; ok = o; error = e; });
    else
        bluez.PairAndConnectDevice(device); // Pair, then (only once Pair finished) Connect

    Clock::time_point start = Clock::now();
    while (MsSince(start) < 2500)
    {
        pollfd pfd{ bluez.Fd(), POLLIN, 0 };
        if (poll(&pfd, 1, 20) > 0)
            bluez.Dispatch();
    }

    std::printf("RESULT not_busy_at_end %d\n", bluez.IsBusy(device) ? 0 : 1);
    std::printf("RESULT change_handler_fired %d\n", changed ? 1 : 0);

    if (expectFailure)
    {
        std::printf("RESULT done_called %d\n", done ? 1 : 0);
        std::printf("RESULT reported_failure %d\n", (done && !ok) ? 1 : 0);
        std::printf("RESULT last_error_recorded %d\n", bluez.LastError(device).find("Pair failed:") == 0 ? 1 : 0);
        std::printf("RESULT last_error_has_reason %d\n", bluez.LastError(device).find("Authentication Failed") != std::string::npos ? 1 : 0);
    }

    return 0;
}

#endif // KOHIKO_HARNESS_WINDOW_ONLY

}

int main(int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "pending";
    const char* logPath = std::getenv("MOCK_CALL_LOG");

    if (!logPath)
    {
        std::printf("RESULT error MOCK_CALL_LOG-not-set\n");
        return 2;
    }

#ifndef KOHIKO_HARNESS_WINDOW_ONLY
    if (mode == "client")
        return RunClientMode(logPath);

    if (mode == "client-ok")
        return RunOutcomeMode(false);

    if (mode == "client-fail")
        return RunOutcomeMode(true);
#endif

    BluetoothWindow window;

    if (!window.Initialize())
    {
        std::printf("RESULT error window-initialize-failed\n");
        return 2;
    }

    UiWindow& ui = window.WindowForTesting();
    ::Window xwindow = ui.GetWindow();
    std::atomic<bool> runReturned{false};
    std::atomic<double> closeRequestedAt{0};
    Clock::time_point epoch = Clock::now();

    std::thread driver([&]
    {
        Display* display = XOpenDisplay(nullptr);
        SleepMs(900); // Run() has built the page; the loop is now idle, so the tree is stable to read

        // (The widget tree only exists once Run() has built the chrome, so the
        // buttons are located here, at a quiet moment, not before Run().)
        Found pair, scan;
        if (Widget* root = ui.Root())
        {
            pair = FindButton(*root, "Pair");
            scan = FindButton(*root, "Scan for devices");
        }

        if (!pair.ok)
        {
            std::printf("RESULT error pair-button-not-found\n");
            std::fflush(stdout);
            _exit(2);
        }

        Click(display, xwindow, pair.x, pair.y);
        Clock::time_point clicked = Clock::now();
        double sent = WaitForLog(logPath, "CALL Pair", 3000);
        std::printf("RESULT pair_reached_service_ms %.1f\n", sent);

        if (mode == "cancel")
        {
            SleepMs(500); // the window has rebuilt: the same button now reads "Cancel"
            Click(display, xwindow, pair.x, pair.y);
            double cancelled = WaitForLog(logPath, "CALL CancelPairing", 3000);
            std::printf("RESULT cancel_reached_service_ms %.1f\n", cancelled);
            double done = WaitForLog(logPath, "DONE Pair path=/org/bluez/hci0/dev_10 cancelled", 3000);
            std::printf("RESULT pair_cancelled_at_service %d\n", done >= 0 ? 1 : 0);
        }
        else if (mode == "close")
        {
            // Ask to close 300 ms after the click, while the Pair (4 s) is certainly pending.
            SleepMs(300);
            double stillPending = LogText(logPath).find("DONE Pair path=/org/bluez/hci0/dev_10") == std::string::npos ? 1 : 0;
            std::printf("RESULT pair_still_pending_when_closing %d\n", static_cast<int>(stillPending));
        }
        else
        {
            // Pair is now pending at the service for ~4 s. Use another control...
            double scanMs = -1;
            if (scan.ok)
            {
                Clock::time_point scanClick = Clock::now();
                Click(display, xwindow, scan.x, scan.y);
                scanMs = WaitForLog(logPath, "CALL StartDiscovery", 6000);
                (void)scanClick;
            }
            std::printf("RESULT other_control_honoured_ms %.1f\n", scanMs);
            std::printf("RESULT ms_since_pair_click_at_scan %.1f\n", MsSince(clicked));

            // ...then close the window while the Pair is (still) in flight.
            double stillPending = LogText(logPath).find("DONE Pair path=/org/bluez/hci0/dev_10") == std::string::npos ? 1 : 0;
            std::printf("RESULT pair_still_pending_when_closing %d\n", static_cast<int>(stillPending));
        }

        closeRequestedAt = MsSince(epoch);
        Clock::time_point closeStart = Clock::now();
        RequestClose(display, xwindow);

        // Watchdog: a window that cannot honour its own close request is blocked.
        while (!runReturned && MsSince(closeStart) < 15000)
            SleepMs(10);

        if (!runReturned)
        {
            std::printf("RESULT close_honoured_ms -1\n");
            std::fflush(stdout);
            _exit(1);
        }

        XCloseDisplay(display);
    });

    window.Run(); // exactly what tools/kohiko-bluetooth.cpp does
    runReturned = true;

    double closeMs = MsSince(epoch) - closeRequestedAt;
    driver.join();
    std::printf("RESULT close_honoured_ms %.1f\n", closeMs);
    std::fflush(stdout);

    // Skip the destructors: tearing the window down after its display is
    // closed trips an Xft font-unload crash that has nothing to do with what
    // this harness measures (and would discard the results above).
    _exit(0);
}
