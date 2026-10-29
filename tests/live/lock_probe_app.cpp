// A stand-in "application" for the single-instance tests - not part of Kohiko.
//
//   lock_probe_app --id ID --log FILE [--startup-ms N] [--stay-ms M]
//
// Logs "STARTED <pid>", waits N ms (a slow-starting program), then asks
// AppInstanceLock::AcquireOrListen(ID): "LOCKED <pid>" if it is the one
// instance (it then serves raise requests - "RAISED <pid>" - for M ms),
// "LOST <pid>" and exit if another instance holds the lock. No X11.

#include "AppInstanceLock.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <poll.h>
#include <time.h>
#include <unistd.h>

using namespace Kohiko;

static void Log(const std::string& file, const char* what)
{
    FILE* f = std::fopen(file.c_str(), "a");
    if (!f)
        return;
    std::fprintf(f, "%s %d\n", what, static_cast<int>(getpid()));
    std::fclose(f); // O_APPEND-style single small write: lines never interleave
}

static long NowMs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(int argc, char** argv)
{
    std::string id = "probe", log;
    int startupMs = 0, stayMs = 3000;

    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--id") && i + 1 < argc) id = argv[++i];
        else if (!std::strcmp(argv[i], "--log") && i + 1 < argc) log = argv[++i];
        else if (!std::strcmp(argv[i], "--startup-ms") && i + 1 < argc) startupMs = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--stay-ms") && i + 1 < argc) stayMs = std::atoi(argv[++i]);
    }

    Log(log, "STARTED");

    if (startupMs > 0)
        usleep(static_cast<useconds_t>(startupMs) * 1000);

    AppInstanceLock lock;
    lock.SetRaiseHandler([&] { Log(log, "RAISED"); });

    if (!lock.AcquireOrListen(id))
    {
        Log(log, "LOST");
        return 0;
    }

    Log(log, "LOCKED");

    long end = NowMs() + stayMs;
    while (NowMs() < end)
    {
        pollfd pfd{ lock.Fd(), POLLIN, 0 };
        int remaining = static_cast<int>(end - NowMs());

        if (poll(&pfd, 1, remaining > 0 ? remaining : 0) > 0)
            lock.Dispatch();
    }

    return 0;
}
