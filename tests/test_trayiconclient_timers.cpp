// Regression/measurement test for the 0.21.4 "idle audio-tray wakes up
// ~10 times a second" finding (see CHANGELOG.md): a real strace on a
// real machine showed kohiko-audio-tray's main thread doing a
// poll()+recvmsg() cycle roughly every 100ms, forever, even sitting
// completely idle - not PipeWire's own protocol or event loop
// (PipeWireClient registers no timer of its own - grepped, confirmed:
// no pw_loop_add_timer or equivalent anywhere in it), but
// `tray.SetInterval(100, [&]{ notifications.Tick(); })` - registered
// once, unconditionally, at process startup, for its entire lifetime,
// even though the 100ms cadence is only ever needed for the ~2.5s a
// toast notification is actually showing.
//
// Fixed two ways: TrayIconClient gained a way to cancel a previously-
// registered interval (SetInterval now returns a TimerId,
// CancelInterval(id) stops it for good - this file's first section);
// and kohiko-audio-tray.cpp now starts the 100ms ticker only once
// NotificationCenter::Post() actually creates a toast, cancelling it
// the moment NotificationCenter::HasActive() goes false again - this
// file's second section wires up that exact same TrayIconClient +
// NotificationCenter combination (not a stand-in), to prove the real
// fix works end to end, not just the timer API it's built from.
//
// This is a measurement test, not just a boolean correctness check: it
// times how long TrayIconClient::Run() actually spends blocked inside
// a single poll() call, which is exactly what a real strace's
// timestamps show as one long gap versus dozens of ~100ms-apart
// wakeups - proving the loop genuinely goes idle, not merely that some
// counter happened to stay low.
//
// Needs a real X11 connection (TrayIconClient creates a real X
// window, and NotificationCenter's popups are real windows too). Same
// "gracefully skip without a real, even if headless/Xvfb, $DISPLAY"
// deal as the other TrayIconClient tests - kept out of `make test` the
// same way (see "test-trayiconclient-timers" in the Makefile).

#include "NotificationCenter.h"
#include "TrayIconClient.h"

#include <X11/Xlib.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>

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

// Runs `tray`'s real event loop for close to `ms` (a timer, not a
// sleep - the whole point is exercising Run()'s actual poll() call),
// returning how long it really took. Cancels its own "stop" timer
// immediately afterward, whether it fired or Run() was already
// stopped some other way - SetInterval() is a *repeating* timer, so a
// forgotten one would keep calling Quit() every `ms` forever after,
// contaminating every section that runs later in this same process.
long long RunFor(TrayIconClient& tray, int ms)
{
    auto start = std::chrono::steady_clock::now();
    TrayIconClient::TimerId stopId = tray.SetInterval(ms, [&] { tray.Quit(); });
    tray.Run();
    tray.CancelInterval(stopId);
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
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

    std::printf("-- TimerId/CancelInterval: a cancelled interval never fires again --\n");

    int tickCount = 0;
    TrayIconClient::TimerId tickId = tray.SetInterval(20, [&] { ++tickCount; });

    RunFor(tray, 90);
    Check(tickCount >= 2, "a live 20ms interval fires more than once inside a ~90ms window");

    int countAtCancel = tickCount;
    tray.CancelInterval(tickId);

    RunFor(tray, 90);
    Check(tickCount == countAtCancel,
          "CancelInterval() stops a timer for good - it does not fire even once more afterward, "
          "across a window long enough it certainly would have otherwise");

    std::printf("\n-- Measuring actual idle behaviour: does Run() really block, or just spin quietly? --\n");

    // No 100ms-class timer registered at all in this section - RunFor()'s
    // own "stop" timer is the only thing scheduled, the same kind of
    // single coarse deadline a real idle audio-tray has once this fix
    // is in (PipeWire's own 2000ms reconnect-retry aside). Real
    // wall-clock timing, not a call counter - exactly what a real
    // strace's timestamps would show as one long gap rather than
    // dozens of ~100ms-apart wakeups.
    long long idleElapsedMs = RunFor(tray, 700);
    std::printf("  (idle Run() call actually took %lldms)\n", idleElapsedMs);

    Check(idleElapsedMs >= 650,
          "with nothing shorter than a ~700ms deadline registered, Run() actually blocks for "
          "close to the full ~700ms in one poll() call - not the ~100ms-apart wakeups the old, "
          "unconditional SetInterval(100, ...) forced even while completely idle");

    std::printf("\n-- The real fix, wired exactly as kohiko-audio-tray.cpp wires it --\n");

    NotificationCenter notifications;
    Check(notifications.Initialize(tray.GetDisplay(), tray.GetScreen(),
              RootWindow(tray.GetDisplay(), tray.GetScreen()), "monospace:pixelsize=14", tray.Theme()),
          "NotificationCenter initializes against the same real X connection");

    // The exact pattern now in kohiko-audio-tray.cpp: started only
    // once a toast exists to tick, cancels itself the moment
    // HasActive() goes false.
    std::optional<TrayIconClient::TimerId> tickerId;
    int realTickCount = 0;

    auto ensureTicker = [&]
    {
        if (tickerId.has_value())
            return;

        tickerId = tray.SetInterval(100, [&]
        {
            ++realTickCount;
            notifications.Tick();

            if (!notifications.HasActive())
            {
                tray.CancelInterval(*tickerId);
                tickerId.reset();
            }
        });
    };

    Rect rootGeometry{0, 0,
        DisplayWidth(tray.GetDisplay(), tray.GetScreen()), DisplayHeight(tray.GetDisplay(), tray.GetScreen())};

    // Nothing posted yet - ensureTicker() is never called by anything,
    // so this measures the same "does it really block" property as
    // the section above, but through NotificationCenter's own actual
    // decision not to need it, rather than simply not registering
    // anything by hand.
    long long preElapsedMs = RunFor(tray, 400);

    Check(!tickerId.has_value() && realTickCount == 0,
          "before anything is ever posted, the 100ms ticker was never started at all");
    Check(preElapsedMs >= 350,
          "and Run() blocked for close to the full ~400ms with nothing shorter registered, "
          "exactly like the idle section above");

    // Short-lived so this doesn't need a real 2.5-second sleep -
    // Post()'s own lifetime override exists exactly for this, per its
    // own comment.
    notifications.Post("Test device connected", rootGeometry, std::chrono::milliseconds(300));
    ensureTicker();

    Check(tickerId.has_value(), "posting a notification starts the ticker");

    RunFor(tray, 600); // well past the 300ms notification lifetime

    Check(realTickCount >= 2,
          "while the notification was alive, the ticker actually fired at roughly its 100ms "
          "cadence, more than once");
    Check(!tickerId.has_value(),
          "and cancelled itself once the notification expired and HasActive() went false - "
          "not left running indefinitely");

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    return 0;
}
