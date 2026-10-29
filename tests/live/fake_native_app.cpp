// A stand-in for one of Kohiko's native apps (kohiko-audio, -network,
// -bluetooth) for tests of the tray icons' click contract: it behaves like
// the real thing as far as single-instance handling goes (AppInstanceLock)
// and records what happened, without needing PipeWire/NetworkManager/BlueZ.
//
// Built as build/fake-native-app - the name is the app id the tray's
// LaunchOrRaise() launches through PATH. Configured by environment:
//   FAKE_LOG          file to append "LAUNCH <pid>", "START <pid>", "LOST <pid>", "RAISE" lines to
//   FAKE_DELAY_MS     how long it takes to start up before it listens (default 0)
//   FAKE_LIFETIME_MS  how long it stays alive (default 2000)

#include "AppInstanceLock.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

using Kohiko::AppInstanceLock;

static void Log(const std::string& line)
{
    const char* path = std::getenv("FAKE_LOG");
    if (!path) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return;
    std::string text = line + "\n";
    if (write(fd, text.c_str(), text.size()) < 0) {}
    close(fd);
}

int main()
{
    Log("LAUNCH " + std::to_string(getpid()));

    const char* delay = std::getenv("FAKE_DELAY_MS");
    usleep((delay ? std::atoi(delay) : 0) * 1000);

    AppInstanceLock lock;
    if (!lock.AcquireOrListen("fake-native-app"))
    {
        Log("LOST " + std::to_string(getpid()));
        return 0;
    }

    Log("START " + std::to_string(getpid()));
    lock.SetRaiseHandler([] { Log("RAISE"); });

    const char* lifetime = std::getenv("FAKE_LIFETIME_MS");
    int lifetimeMs = lifetime ? std::atoi(lifetime) : 2000;

    for (int waited = 0; waited < lifetimeMs; waited += 50)
    {
        pollfd pfd{ lock.Fd(), POLLIN, 0 };
        if (poll(&pfd, 1, 50) > 0)
            lock.Dispatch();
    }

    return 0;
}
