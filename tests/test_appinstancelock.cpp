// Regression test for AppInstanceLock - the single-instance guard behind
// every Kohiko native app's tray icon ("left click: open or raise the main
// window"). No X server needed. What it pins down:
//
//   - STARTUP RACE: any number of instances starting at the same instant
//     produce exactly ONE winner. (Before: probe -> unlink -> bind let two
//     starters both "win", i.e. two main windows.)
//   - REPEATED CLICKS: LaunchOrRaise() called many times while the app is
//     still starting forks it exactly ONCE. (Before: each call forked
//     another copy, because a starting app is not listening yet.)
//   - REUSE: once the app is up, a further click raises the running
//     instance - it does not launch another.
//   - RETRY: a launch that failed (the process exited) can be tried again.
//   - NO STALE LOCK: if the holder is killed hard, the next starter wins.
//
// The "native app" is this very binary, run through a symlink named
// fake-native-app (see FakeApp()). Timings are generous (an app that takes
// 400 ms to start vs clicks 25 ms apart) so the test is not flaky.

#include "AppInstanceLock.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using Kohiko::AppInstanceLock;

namespace
{

int g_pass = 0;
int g_fail = 0;

void Check(bool ok, const std::string& what)
{
    if (ok) { ++g_pass; std::printf("  PASS: %s\n", what.c_str()); }
    else    { ++g_fail; std::printf("  FAIL: %s\n", what.c_str()); }
}

void Log(const std::string& line)
{
    const char* path = std::getenv("FAKE_LOG");
    if (!path) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return;
    std::string text = line + "\n";
    if (write(fd, text.c_str(), text.size()) < 0) {}
    close(fd);
}

// What a native app does: log that it was launched, take its time starting,
// then either become THE instance (listen, log raises) or quit.
int FakeApp()
{
    Log("LAUNCH " + std::to_string(getpid()));

    if (std::getenv("FAKE_EXIT"))
        return 127; // a launch that fails outright

    usleep(std::atoi(std::getenv("FAKE_DELAY_MS") ? std::getenv("FAKE_DELAY_MS") : "0") * 1000);

    AppInstanceLock lock;
    if (!lock.AcquireOrListen("fake-native-app"))
    {
        Log("LOST " + std::to_string(getpid()));
        return 0;
    }

    Log("START " + std::to_string(getpid()));
    lock.SetRaiseHandler([] { Log("RAISE"); });

    int lifetimeMs = std::atoi(std::getenv("FAKE_LIFETIME_MS") ? std::getenv("FAKE_LIFETIME_MS") : "2000");

    for (int waited = 0; waited < lifetimeMs;)
    {
        pollfd pfd{ lock.Fd(), POLLIN, 0 };
        if (poll(&pfd, 1, 50) > 0)
            lock.Dispatch();
        waited += 50;
    }

    return 0;
}

int Count(const std::string& logPath, const std::string& prefix)
{
    std::ifstream in(logPath);
    std::string line;
    int n = 0;
    while (std::getline(in, line))
        n += line.rfind(prefix, 0) == 0;
    return n;
}

void Sleep(int ms) { usleep(ms * 1000); }

}

int main(int argc, char** argv)
{
    std::string self = argv[0];
    if (self.substr(self.find_last_of('/') + 1) == "fake-native-app")
        return FakeApp();

    (void)argc;

    char tmpl[] = "/tmp/kohiko-test-instancelock-XXXXXX";
    std::string dir = mkdtemp(tmpl);
    setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
    std::string logPath = dir + "/app.log";
    setenv("FAKE_LOG", logPath.c_str(), 1);

    // ---------------------------------------------------------------------
    std::printf("-- Eight instances start at the same instant: exactly one wins --\n");
    {
        int barrier[2], results[2];
        if (pipe(barrier) || pipe(results)) return 2;

        const int kInstances = 8;
        for (int i = 0; i < kInstances; ++i)
        {
            if (fork() == 0)
            {
                close(barrier[1]); close(results[0]);
                char c; if (read(barrier[0], &c, 1) < 0) {} // blocks until the parent closes its end: all go at once
                AppInstanceLock lock;
                bool won = lock.AcquireOrListen("race-app");
                char verdict = won ? 'W' : 'L';
                if (write(results[1], &verdict, 1) < 0) {}
                if (won) Sleep(1500); // hold it while the others try
                _exit(0);
            }
        }

        close(barrier[0]); close(results[1]);
        close(barrier[1]); // release everyone
        int winners = 0, losers = 0;
        for (int i = 0; i < kInstances; ++i)
        {
            char verdict = 0;
            if (read(results[0], &verdict, 1) == 1)
                (verdict == 'W' ? winners : losers)++;
        }
        while (wait(nullptr) > 0) {}
        Check(winners == 1, "exactly one of eight simultaneous starters became the instance (got " + std::to_string(winners) + ")");
        Check(losers == kInstances - 1, "and the other seven all backed off");
    }

    // ---------------------------------------------------------------------
    // The fake app: a symlink to this binary, found through PATH like a real one.
    std::string exe(4096, '\0');
    ssize_t n = readlink("/proc/self/exe", exe.data(), exe.size());
    exe.resize(n > 0 ? n : 0);
    std::string bin = dir + "/bin";
    mkdir(bin.c_str(), 0700);
    if (symlink(exe.c_str(), (bin + "/fake-native-app").c_str())) return 2;
    setenv("PATH", (bin + ":" + std::getenv("PATH")).c_str(), 1);

    // ---------------------------------------------------------------------
    std::printf("-- Repeated clicks while the app is still starting launch it ONCE --\n");
    {
        setenv("FAKE_DELAY_MS", "400", 1);
        setenv("FAKE_LIFETIME_MS", "4000", 1);
        unlink(logPath.c_str());

        for (int click = 0; click < 6; ++click)
        {
            AppInstanceLock::LaunchOrRaise("fake-native-app", "fake-native-app");
            Sleep(25);
        }

        Sleep(900); // the app has had time to finish starting
        Check(Count(logPath, "LAUNCH") == 1, "six rapid clicks forked the app exactly once (got " + std::to_string(Count(logPath, "LAUNCH")) + ")");
        Check(Count(logPath, "START") == 1, "and exactly one instance is running");
        Check(Count(logPath, "LOST") == 0, "(so there was never a second process to lose the race)");

        std::printf("-- ...and a click once it is up RAISES it rather than launching again --\n");
        AppInstanceLock::LaunchOrRaise("fake-native-app", "fake-native-app");
        Sleep(300);
        Check(Count(logPath, "RAISE") >= 1, "the running instance was asked to raise its window");
        Check(Count(logPath, "LAUNCH") == 1 && Count(logPath, "START") == 1, "and no new process was started - the existing window is reused");

        AppInstanceLock::LaunchOrRaise("fake-native-app", "fake-native-app");
        AppInstanceLock::LaunchOrRaise("fake-native-app", "fake-native-app");
        Sleep(300);
        Check(Count(logPath, "LAUNCH") == 1, "further clicks keep reusing it");
        Sleep(3500); // let it exit
        while (waitpid(-1, nullptr, WNOHANG) > 0) {}
    }

    // ---------------------------------------------------------------------
    std::printf("-- A launch that failed can be tried again --\n");
    {
        setenv("FAKE_EXIT", "1", 1);
        unlink(logPath.c_str());

        AppInstanceLock::LaunchOrRaise("fake-native-app", "fake-native-app");
        Sleep(300); // it launches, fails and exits
        AppInstanceLock::LaunchOrRaise("fake-native-app", "fake-native-app");
        Sleep(300);
        Check(Count(logPath, "LAUNCH") == 2, "after the first attempt died, the next click launched again (got " + std::to_string(Count(logPath, "LAUNCH")) + ")");
        unsetenv("FAKE_EXIT");
        while (waitpid(-1, nullptr, WNOHANG) > 0) {}
    }

    // ---------------------------------------------------------------------
    std::printf("-- No stale lock after the instance is killed --\n");
    {
        int ready[2];
        if (pipe(ready)) return 2;

        pid_t holder = fork();
        if (holder == 0)
        {
            close(ready[0]);
            AppInstanceLock lock;
            char verdict = lock.AcquireOrListen("crash-app") ? 'W' : 'L';
            if (write(ready[1], &verdict, 1) < 0) {}
            pause(); // wait to be killed
            _exit(0);
        }

        close(ready[1]);
        char verdict = 0;
        if (read(ready[0], &verdict, 1) < 0) {}
        Check(verdict == 'W', "the first instance acquired it");

        AppInstanceLock second;
        Check(!second.AcquireOrListen("crash-app"), "while it is alive, a second instance cannot");

        kill(holder, SIGKILL);
        waitpid(holder, nullptr, 0);

        AppInstanceLock third;
        Check(third.AcquireOrListen("crash-app"), "after it was killed (no cleanup ran), the next instance acquires it - no stale lock");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
