// Regression test for the taskbar/clock "stuck until something else
// forces a repaint" bug: EventLoop::Run() used to reset its select()
// timeout to a fresh interval on *every* loop iteration, regardless of
// why that iteration happened. Any other file descriptor going ready
// before the full interval elapsed - IPC, the app-dir watcher, the
// wallpaper file watch, and especially ScreenSaverInhibitor's own fd,
// which subscribes bus-wide to every NameOwnerChanged signal on the
// session bus, not just ones it cares about (see that class's own
// comment) - restarted the wait, indefinitely. WindowManager::Tick()
// (which redraws every bar - the clock among everything else it
// drives) only ever ran when select() actually timed out with zero
// other activity, so on a real, ordinarily-busy D-Bus session -
// confirmed directly: a live Kohiko session, kept idle, with a
// background process claiming and releasing a D-Bus name every 150ms
// to simulate exactly that - the clock stayed frozen for 70+ real
// seconds. The same session with this fix in place updated correctly
// throughout, under the identical synthetic load.
//
// This test covers the pure scheduling logic behind that fix
// (TickSchedule::PullForward/IsDue/TimeUntilDue, in EventLoop.h/.cpp)
// directly - no X11 connection, no D-Bus, no real waiting on a clock;
// every timestamp here is synthetic, constructed by adding/subtracting
// durations from one reference point, so this runs in a fraction of a
// second and needs nothing beyond the C++ standard library. It cannot
// exercise EventLoop::Run() itself (a real, blocking, X11-connected
// loop - see this project's own established pattern of extracting
// pure logic into testable free functions specifically because the
// surrounding infrastructure can't be unit tested this way), but the
// live-session churn test described above, and repeatable via the
// same synthetic-D-Bus-traffic approach, is what actually confirms the
// fix end to end.

#include "EventLoop.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace Kohiko;
using Clock = std::chrono::steady_clock;

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

}

int main()
{
    // An arbitrary fixed reference point - steady_clock has no
    // meaningful "epoch", so every test below works in offsets from
    // this rather than real wall-clock time.
    Clock::time_point t0 = Clock::now();

    std::printf("TickSchedule::PullForward() tests:\n");

    std::printf("-- A tighter cadence (animation starting) pulls the deadline earlier --\n");
    {
        Clock::time_point farOffDeadline = t0 + std::chrono::seconds(1);
        Clock::time_point result = TickSchedule::PullForward(farOffDeadline, t0, std::chrono::microseconds(8000));

        Check(result == t0 + std::chrono::microseconds(8000),
              "a 1s-away deadline gets pulled in to 8ms when the desired interval tightens");
    }

    std::printf("\n-- THE regression guard: a longer cadence never pushes the deadline later --\n");
    {
        Clock::time_point nearDeadline = t0 + std::chrono::milliseconds(50);
        Clock::time_point result = TickSchedule::PullForward(nearDeadline, t0, std::chrono::seconds(1));

        Check(result == nearDeadline,
              "a deadline just 50ms away is left alone when this call's own interval is a full second - "
              "this is the exact property that was missing before the fix: the old code recomputed a "
              "fresh timeout from scratch every iteration instead of respecting an existing, sooner deadline");
    }

    std::printf("\n-- Repeated calls with only slightly-advancing \"now\" (simulating frequent unrelated "
                 "fd wakeups) never push the deadline past its original value --\n");
    {
        Clock::time_point deadline = t0 + std::chrono::seconds(1);
        Clock::time_point originalDeadline = deadline;

        // 200 simulated "select() returned early because some other
        // fd was ready" wakeups, 5ms apart (a busier cadence than the
        // 150ms D-Bus churn that reproduced this live), each one
        // re-running the scheduler exactly as EventLoop::Run()'s own
        // loop does.
        bool everMovedLater = false;

        for (int i = 0; i < 200; ++i)
        {
            Clock::time_point now = t0 + std::chrono::milliseconds(5 * i);
            Clock::time_point next = TickSchedule::PullForward(deadline, now, std::chrono::seconds(1));

            if (next > deadline)
                everMovedLater = true;

            deadline = next;
        }

        Check(!everMovedLater, "the deadline never moved later across 200 simulated early wakeups");
        Check(deadline == originalDeadline,
              "...and is still exactly the original 1-second deadline, not extended even once");
    }

    std::printf("\nTickSchedule::IsDue() tests:\n");
    Check(!TickSchedule::IsDue(t0, t0 + std::chrono::milliseconds(1)), "not due 1ms before the deadline");
    Check(TickSchedule::IsDue(t0, t0), "due exactly at the deadline");
    Check(TickSchedule::IsDue(t0 + std::chrono::milliseconds(1), t0), "due 1ms past the deadline");

    std::printf("\nTickSchedule::TimeUntilDue() tests:\n");
    {
        auto remaining = TickSchedule::TimeUntilDue(t0, t0 + std::chrono::milliseconds(700));
        Check(remaining == std::chrono::milliseconds(700), "700ms remaining computes correctly");
    }
    {
        // Tick() itself takes some nonzero time to run, so by the time
        // TimeUntilDue() is called again afterward, `now` can already
        // be slightly past `nextTick` - must clamp to zero, not go
        // negative (a negative timeval passed to select() is
        // undefined behaviour, not "wait zero time").
        auto remaining = TickSchedule::TimeUntilDue(t0 + std::chrono::milliseconds(5), t0);
        Check(remaining == std::chrono::steady_clock::duration::zero(),
              "clamped to zero rather than negative when already past due");
    }

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    return 0;
}
