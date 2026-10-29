#include "AppInstanceLock.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace Kohiko
{

namespace
{

std::string RuntimeDir()
{
    const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    return (runtimeDir && *runtimeDir) ? runtimeDir : "/tmp";
}

std::string SocketPathFor(const std::string& appId)
{
    return RuntimeDir() + "/kohiko-" + appId + ".sock";
}

std::string LockPathFor(const std::string& appId)
{
    return RuntimeDir() + "/kohiko-" + appId + ".lock";
}

bool BindAddress(int fd, const std::string& path, sockaddr_un& addr)
{
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    // Truncation here would mean two different app IDs collide on the
    // same socket path, which never happens in practice (every caller
    // passes a short literal like "kohiko-audio") - not worth a
    // runtime check for a path length constraint that can only be
    // violated by a coding mistake, not user input.
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    return bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
}

}

AppInstanceLock::AppInstanceLock() = default;

AppInstanceLock::~AppInstanceLock()
{
    if (m_listenFd >= 0)
    {
        close(m_listenFd);
        unlink(m_socketPath.c_str());
    }

    // Closing the descriptor releases the flock. The lock FILE is left
    // in place on purpose: unlinking it would let a new instance lock a
    // fresh inode while an old one still holds the previous one.
    if (m_lockFd >= 0)
        close(m_lockFd);
}

bool AppInstanceLock::AcquireOrListen(const std::string& appId)
{
    m_socketPath = SocketPathFor(appId);

    // The decision "am I the one instance?" is made with flock(), which
    // is atomic. This used to be "connect to the socket; if nobody
    // answers, unlink it and bind a new one" - two copies started within
    // the same few milliseconds (a double click on a tray icon, a
    // tray click racing a Launcher start) both saw an empty socket, both
    // unlinked, both bound, and BOTH ran: two windows. flock() has no
    // such gap, is released by the kernel the instant the holder exits
    // for any reason (so a crash never leaves a stale lock), and only
    // the holder ever touches the socket file.
    m_lockFd = open(LockPathFor(appId).c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);

    if (m_lockFd >= 0 && flock(m_lockFd, LOCK_EX | LOCK_NB) != 0)
    {
        bool heldByAnother = errno == EWOULDBLOCK;

        close(m_lockFd);
        m_lockFd = -1;

        if (heldByAnother)
        {
            // The running instance wins; tell it somebody wanted the
            // window (it may still be starting up and not listening
            // yet - then the user simply gets the window it is about
            // to show).
            RequestRaise(appId);
            return false;
        }
    }

    // We hold the lock (or locking is unavailable - then fall back to
    // running as a normal, unguarded instance, as before).
    // Nobody else can be listening now, so a leftover socket file from
    // an uncleanly terminated run is safe to remove.
    unlink(m_socketPath.c_str());

    m_listenFd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (m_listenFd < 0)
        return true; // no locking available, but still safe to just run as a normal (unguarded) instance

    sockaddr_un addr;
    if (!BindAddress(m_listenFd, m_socketPath, addr))
    {
        close(m_listenFd);
        m_listenFd = -1;
        return true;
    }

    listen(m_listenFd, 4);
    return true;
}

int AppInstanceLock::Fd() const
{
    return m_listenFd;
}

void AppInstanceLock::Dispatch()
{
    if (m_listenFd < 0)
        return;

    for (;;)
    {
        int clientFd = accept(m_listenFd, nullptr, nullptr);
        if (clientFd < 0)
            break;

        // The content of the message doesn't matter - any connection
        // at all means "please raise the window"; this app has
        // exactly one action a peer could possibly want. The byte
        // count read() returns is equally irrelevant, hence the
        // explicit discard rather than leaving the warning in place.
        char buffer[64];
        ssize_t bytesRead = read(clientFd, buffer, sizeof(buffer));
        (void)bytesRead;
        close(clientFd);

        if (m_handler)
            m_handler();
    }
}

void AppInstanceLock::SetRaiseHandler(RaiseHandler handler)
{
    m_handler = std::move(handler);
}

bool AppInstanceLock::RequestRaise(const std::string& appId)
{
    std::string path = SocketPathFor(appId);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return false;

    sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    bool connected = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    if (connected)
    {
        ssize_t bytesWritten = write(fd, "raise\n", 6);
        (void)bytesWritten;
    }

    close(fd);
    return connected;
}

void AppInstanceLock::LaunchOrRaise(const std::string& appId, const std::string& executableName)
{
    if (RequestRaise(appId))
        return;

    // The application is not listening yet. Either it is not running,
    // or we started it a moment ago and it is still opening its window
    // (a few hundred milliseconds - longer on a loaded machine). Starting
    // it again in the second case is how a quick second click made two
    // processes; the extra one then had to notice it had lost the
    // instance lock and exit, but until it did the user could see two
    // windows' worth of startup work. So remember what this process
    // started, and do not start the same application while that child
    // is still alive and has not yet become the listening instance.
    static std::map<std::string, pid_t> started;

    // Reap every child that has finished (no zombies pile up, and a
    // finished child no longer blocks a new start).
    for (auto it = started.begin(); it != started.end();)
    {
        int status = 0;
        pid_t result = waitpid(it->second, &status, WNOHANG);

        if (result == it->second || (result < 0 && errno == ECHILD))
            it = started.erase(it);
        else
            ++it;
    }

    if (started.count(appId))
        return; // still starting: the window will appear on its own

    pid_t pid = fork();
    if (pid == 0)
    {
        // Detach fully so the app keeps running after this tray
        // widget process eventually exits - setsid() moves it to its
        // own session rather than staying a child of the tray widget.
        setsid();
        execlp(executableName.c_str(), executableName.c_str(), static_cast<char*>(nullptr));
        _exit(127); // only reached if exec itself failed (binary not on PATH, ...)
    }

    if (pid > 0)
        started[appId] = pid;
}

}
