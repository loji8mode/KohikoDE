#include "EventLoop.h"

#include "EventDispatcher.h"
#include "IPCServer.h"
#include "WindowManager.h"
#include "XConnection.h"

#include <X11/Xlib.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>

namespace Kohiko
{

namespace
{

// Set (async-signal-safe: a plain sig_atomic_t, nothing else touched)
// by HandleTerminationSignal() below; checked once per Run() loop
// iteration. A file-local flag rather than anything routed through
// WindowManager/EventLoop instance state, since a signal handler
// registered via std::signal() can only ever be a plain function
// pointer - it has no way to reach a particular object instance.
volatile std::sig_atomic_t g_terminateRequested = 0;

extern "C" void HandleTerminationSignal(int)
{
    g_terminateRequested = 1;
}

}

void EventLoop::InstallSignalHandlers()
{
    std::signal(SIGTERM, HandleTerminationSignal);
    std::signal(SIGINT, HandleTerminationSignal);
}

EventLoop::EventLoop(
    XConnection& connection,
    WindowManager& windowManager)
    :
    m_connection(connection),
    m_windowManager(windowManager)
{
}

void EventLoop::Run()
{
    m_running = true;

    Display* display = m_connection.GetDisplay();
    int xfd = m_connection.ConnectionFd();

    // Tick() (the clock, idle-lock/sleep-inhibition checks, launcher/
    // notepad cursor blink, notification expiry - see its own
    // comment) is scheduled off this absolute deadline, not off
    // select() itself timing out. The two look equivalent at first
    // glance but aren't: with the timeout reset fresh on every loop
    // iteration (the old approach), *any* unrelated fd going ready
    // before the full second is up - IPC, the app-dir watcher, the
    // lock bridge, the wallpaper file watch, and especially
    // ScreenSaverInhibitor's own fd, which subscribes bus-wide to
    // every NameOwnerChanged signal on the session bus, not just ones
    // it cares about, so it can drop a crashed inhibitor's cookie
    // automatically (see that class's own comment) - restarts the
    // wait from a full second again, indefinitely. That's easy to
    // miss testing somewhere quiet (a fresh sandbox with almost
    // nothing else on the bus), and easy to hit on an actual desktop
    // session, `startx`-launched or not, where routine background
    // D-Bus traffic is normal and frequent - the WM keeps servicing
    // every real X11 event throughout, so nothing *looks* stuck, but
    // the clock (and everything else Tick() drives) can go a long time
    // without actually updating, however busy the bus gets. Tracking
    // an absolute deadline instead means every loop iteration counts
    // down the *same* target regardless of how many times select()
    // returns early in between - other fd traffic can only delay a
    // tick by the time it takes to service it, never indefinitely.
    auto nextTick = std::chrono::steady_clock::now();

    while (m_running && m_windowManager.IsRunning() && !g_terminateRequested)
    {
        // Drain everything Xlib already has buffered before going
        // back to select() - XPending() does a non-blocking read
        // itself if the fd looks readable but the buffer is empty.
        while (XPending(display) > 0)
        {
            XEvent event;
            XNextEvent(display, &event);

            // Motion-event compression: a slow-to-process WM (or a
            // fast mouse) can leave several MotionNotify events queued
            // up behind each other. Working through them one at a time
            // means Super+RMB resize and the Super+LMB drag-follow
            // always lag a little behind the *actual* pointer, which
            // reads as jerky. Instead, whenever the next queued event
            // is another MotionNotify for the same window, skip
            // straight to it and keep only the newest point - the
            // motion handlers below always compute their delta from
            // absolute coordinates, so collapsing a run of motion
            // events down to the last one loses no information.
            if (event.type == MotionNotify)
            {
                while (XPending(display) > 0)
                {
                    XEvent next;
                    XPeekEvent(display, &next);

                    if (next.type != MotionNotify ||
                        next.xmotion.window != event.xmotion.window)
                        break;

                    XNextEvent(display, &event);
                }
            }

            m_windowManager.Dispatcher().Dispatch(event);

            if (!m_windowManager.IsRunning())
                break;
        }

        if (!m_windowManager.IsRunning())
            break;

        int ipcFd = m_windowManager.Ipc().ListenFd();
        int inhibitFd = m_windowManager.SleepInhibitor().Fd();
        int appDirFd = m_windowManager.AppWatcher().Fd();
        int lockBridgeFd = m_windowManager.LockBridge().Fd();
        int wallpaperFd = m_windowManager.Wallpaper().Fd();

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(xfd, &readSet);

        int maxFd = xfd;

        if (ipcFd >= 0)
        {
            FD_SET(ipcFd, &readSet);

            if (ipcFd > maxFd)
                maxFd = ipcFd;
        }

        if (inhibitFd >= 0)
        {
            FD_SET(inhibitFd, &readSet);

            if (inhibitFd > maxFd)
                maxFd = inhibitFd;
        }

        if (appDirFd >= 0)
        {
            FD_SET(appDirFd, &readSet);

            if (appDirFd > maxFd)
                maxFd = appDirFd;
        }

        if (lockBridgeFd >= 0)
        {
            FD_SET(lockBridgeFd, &readSet);

            if (lockBridgeFd > maxFd)
                maxFd = lockBridgeFd;
        }

        if (wallpaperFd >= 0)
        {
            FD_SET(wallpaperFd, &readSet);

            if (wallpaperFd > maxFd)
                maxFd = wallpaperFd;
        }

        // A window sliding into place after a Swap needs Tick() to run
        // at something like frame rate, not once a second - but only
        // while an animation is actually in flight, so an idle Kohiko
        // still spends almost all its time asleep in select().
        auto tickInterval = m_windowManager.HasActiveAnimation()
            ? std::chrono::steady_clock::duration(std::chrono::microseconds(8000)) // ~125Hz
            : std::chrono::steady_clock::duration(std::chrono::seconds(1));

        auto now = std::chrono::steady_clock::now();

        nextTick = TickSchedule::PullForward(nextTick, now, tickInterval);

        if (TickSchedule::IsDue(now, nextTick))
        {
            m_windowManager.Tick();
            now = std::chrono::steady_clock::now();
            nextTick = now + tickInterval;
        }

        auto remainingUs = std::chrono::duration_cast<std::chrono::microseconds>(
            TickSchedule::TimeUntilDue(now, nextTick));

        timeval timeout{};
        timeout.tv_sec = static_cast<time_t>(remainingUs.count() / 1000000);
        timeout.tv_usec = static_cast<suseconds_t>(remainingUs.count() % 1000000);

        int ready = select(maxFd + 1, &readSet, nullptr, nullptr, &timeout);

        if (ready < 0)
        {
            if (errno == EINTR)
                continue;

            break;
        }

        if (ready == 0)
            continue; // nextTick is due (or past due) - the top of the next iteration picks it up unconditionally, no separate check needed here

        if (ipcFd >= 0 && FD_ISSET(ipcFd, &readSet))
            m_windowManager.Ipc().Poll();

        if (inhibitFd >= 0 && FD_ISSET(inhibitFd, &readSet))
            m_windowManager.SleepInhibitor().Dispatch();

        if (appDirFd >= 0 && FD_ISSET(appDirFd, &readSet))
            m_windowManager.HandleAppDirChanged();

        if (lockBridgeFd >= 0 && FD_ISSET(lockBridgeFd, &readSet))
            m_windowManager.LockBridge().Dispatch();

        if (wallpaperFd >= 0 && FD_ISSET(wallpaperFd, &readSet))
            m_windowManager.HandleWallpaperFileChanged();
    }
}

void EventLoop::Stop()
{
    m_running = false;
}

}
