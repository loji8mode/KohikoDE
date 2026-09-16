#pragma once

namespace Kohiko
{

class XConnection;
class WindowManager;

// Multiplexes the X11 connection, the IPC socket, and a handful of
// other subsystems' file descriptors (ScreenSaverInhibitor's D-Bus
// connection, AppDirWatcher's inotify fd) with select(). Every pass
// drains whatever X events are already pending through WindowManager's
// EventDispatcher, then polls whichever of the others have something
// waiting. A short timeout keeps the bar's clock ticking even when
// nothing else happens - no threads anywhere in Kohiko.
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

}
