#pragma once

#include <chrono>

namespace Kohiko
{

class XConnection;
class WindowManager;

// Multiplexes the X11 connection, the IPC socket, and a handful of
// other subsystems' file descriptors (ScreenSaverInhibitor's D-Bus
// connection, AppDirWatcher's inotify fd) with select(). Every pass
// drains whatever X events are already pending through WindowManager's
// EventDispatcher, then polls whichever of the others have something
// waiting. WindowManager::Tick() - the bar's clock among several other
// things it drives, see its own comment - runs off an absolute
// deadline tracked independently of whether select() itself times out
// (see the TickSchedule functions below, and Run()'s own comment on
// why: a naive "reset the timeout to a fresh second every time select()
// returns for any reason" approach lets ordinary background D-Bus
// traffic - not even unusual traffic; see ScreenSaverInhibitor's own
// comment on why it watches every NameOwnerChanged signal on the bus -
// starve Tick() indefinitely, without dropping a single X11 event in
// the meantime, which makes it look like nothing at all is wrong until
// specifically the clock, or the idle-lock timeout, or anything else
// Tick() drives, is what someone happens to be watching). No threads
// anywhere in Kohiko.
class EventLoop
{
public:

    EventLoop(
        XConnection& connection,
        WindowManager& windowManager
    );

    void Run();

    void Stop();

    // Installs SIGTERM/SIGINT handlers that make Run()'s loop exit
    // cleanly - the usual way a session manager or `pkill kohiko`
    // stops a window manager, and, unlike letting the default handler
    // just kill the process, one that still reaches
    // WindowManager::Shutdown() (session-restore's save-on-exit, among
    // other cleanup) instead of skipping it. Called once, from
    // Application::Run(), alongside the SIGCHLD/SIGPIPE handlers it
    // already installs.
    static void InstallSignalHandlers();

private:

    XConnection& m_connection;
    WindowManager& m_windowManager;

    bool m_running = true;

};

// The pure scheduling logic behind EventLoop::Run()'s Tick() timing -
// pulled out into free functions specifically so it's unit-testable
// (tests/test_eventloop.cpp) without needing a real X11/D-Bus
// connection or actually waiting on a clock, unlike Run() itself.
// Each one takes whatever time_points/durations it needs as plain
// arguments rather than reading any live clock or object state.
// Defined inline, right here, rather than in EventLoop.cpp: these are
// tiny, pure, and have zero dependency on anything else in this
// class, so keeping them header-only means a test can use them
// without linking EventLoop.cpp itself, which would otherwise drag in
// the entire WindowManager dependency graph for logic that needs none
// of it.
namespace TickSchedule
{

// Applies the one rule that makes an animation starting responsive
// without allowing the opposite (unrelated fd activity extending the
// wait): moves `nextTick` earlier if the current desired cadence
// would put it sooner than the existing deadline, but never moves it
// later just because this particular call happens to want a longer
// interval. See EventLoop.cpp's own comment on why "never later" is
// the entire fix.
inline std::chrono::steady_clock::time_point PullForward(
    std::chrono::steady_clock::time_point nextTick,
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::duration tickInterval)
{
    auto candidate = now + tickInterval;
    return candidate < nextTick ? candidate : nextTick;
}

// True once `now` has reached or passed `nextTick` - i.e. a tick is
// actually due, independent of *why* this particular call is
// happening (a real timeout, or just the loop checking again after
// dispatching some unrelated fd).
inline bool IsDue(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point nextTick)
{
    return now >= nextTick;
}

// How long the caller should ask select() to wait so it wakes up
// again no later than `nextTick` - clamped to zero rather than
// negative, since `nextTick` can already be slightly in the past by
// the time this is called (Tick() itself takes some nonzero time to
// run).
inline std::chrono::steady_clock::duration TimeUntilDue(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point nextTick)
{
    auto remaining = nextTick - now;
    return remaining < std::chrono::steady_clock::duration::zero()
        ? std::chrono::steady_clock::duration::zero()
        : remaining;
}

}

}
