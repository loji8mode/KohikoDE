#include "EventLoop.h"

#include "EventDispatcher.h"
#include "IPCServer.h"
#include "WindowManager.h"
#include "XConnection.h"

#include <X11/Xlib.h>

#include <cerrno>
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

        // A window sliding into place after a Swap needs Tick() to run
        // at something like frame rate, not once a second - but only
        // while an animation is actually in flight, so an idle Kohiko
        // still spends almost all its time asleep in select().
        timeval timeout{};

        if (m_windowManager.HasActiveAnimation())
        {
            timeout.tv_sec = 0;
            timeout.tv_usec = 8000; // ~125Hz
        }
        else
        {
            timeout.tv_sec = 1;
            timeout.tv_usec = 0;
        }

        int ready = select(maxFd + 1, &readSet, nullptr, nullptr, &timeout);

        if (ready < 0)
        {
            if (errno == EINTR)
                continue;

            break;
        }

        if (ready == 0)
        {
            m_windowManager.Tick();
            continue;
        }

        if (ipcFd >= 0 && FD_ISSET(ipcFd, &readSet))
            m_windowManager.Ipc().Poll();

        if (inhibitFd >= 0 && FD_ISSET(inhibitFd, &readSet))
            m_windowManager.SleepInhibitor().Dispatch();
    }
}

void EventLoop::Stop()
{
    m_running = false;
}

}
