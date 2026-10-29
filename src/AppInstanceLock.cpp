#include "AppInstanceLock.h"

#include <cstdlib>
#include <cstring>
#include <map>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <unistd.h>

namespace Kohiko
{

namespace
{

std::string SocketPathFor(const std::string& appId)
{
    const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    std::string dir = (runtimeDir && *runtimeDir) ? runtimeDir : "/tmp";
    return dir + "/kohiko-" + appId + ".sock";
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

    if (m_lockFd >= 0)
        close(m_lockFd); // releases the flock (the kernel would anyway at exit)
}

bool AppInstanceLock::AcquireOrListen(const std::string& appId)
{
    m_socketPath = SocketPathFor(appId);

    // Decide "who is THE instance" atomically, with the kernel, before
    // touching the socket at all. The old sequence - probe the socket,
    // unlink it, bind a fresh one - is a race: two instances starting
    // within one another's startup time (a double-click on a tray icon
    // does exactly that) both probe an absent listener, both unlink, both
    // bind, and the second silently steals the path - two main windows.
    // An exclusive flock() on a lock file has exactly one winner no
    // matter how the starts interleave, is released by the kernel when the
    // holder exits OR crashes (so there is never a stale lock to clean
    // up), and needs no polling. The lock file itself is deliberately
    // never deleted (deleting it would let a late opener lock a different
    // inode than the one the winner holds).
    m_lockFd = open((m_socketPath + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);

    if (m_lockFd >= 0 && flock(m_lockFd, LOCK_EX | LOCK_NB) != 0)
    {
        // Somebody else holds it: they are the instance (maybe still
        // starting up). Ask them to raise if they are already listening;
        // either way this process must not become a second one.
        close(m_lockFd);
        m_lockFd = -1;
        RequestRaise(appId);
        return false;
    }

    // First, check whether an instance is already listening - if so,
    // this process isn't the one that gets to run.
    {
        int probeFd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (probeFd >= 0)
        {
            sockaddr_un addr;
            std::memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, m_socketPath.c_str(), sizeof(addr.sun_path) - 1);

            bool connected = connect(probeFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
            close(probeFd);

            if (connected)
                return false;
        }
    }

    // Nobody's there (or the socket file is stale/nonexistent) -
    // become the instance. Unlinking first means a leftover file from
    // a previous, uncleanly-terminated run never blocks bind().
    unlink(m_socketPath.c_str());

    m_listenFd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
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
    // Launches this process has started and not yet seen the end of.
    // Without it, every click made while the app was still starting up
    // (it is not listening yet, so RequestRaise() fails) forked ANOTHER
    // copy. Reaping with WNOHANG here is the only housekeeping needed:
    // no timer, no blocking wait.
    static std::map<std::string, pid_t> launching;

    for (auto it = launching.begin(); it != launching.end();)
    {
        int status = 0;

        if (waitpid(it->second, &status, WNOHANG) == it->second)
            it = launching.erase(it); // exited (failed or finished): not "starting" any more
        else
            ++it;
    }

    if (RequestRaise(appId))
    {
        launching.erase(appId); // it is up and answering
        return;
    }

    if (launching.count(appId))
        return; // already started, still coming up: the window will appear by itself

    pid_t pid = fork();
    if (pid > 0)
        launching[appId] = pid;

    if (pid == 0)
    {
        // Detach fully so the app keeps running after this tray
        // widget process eventually exits - setsid() moves it to its
        // own session rather than staying a child of the tray widget.
        setsid();
        execlp(executableName.c_str(), executableName.c_str(), static_cast<char*>(nullptr));
        _exit(127); // only reached if exec itself failed (binary not on PATH, ...)
    }
}

}
