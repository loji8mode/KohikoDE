// Functional test for AppDirWatcher - exercises real Linux inotify
// against throwaway temp directories (redirecting XDG_DATA_HOME/
// XDG_DATA_DIRS, same idea as the other Xdg-based tests). No X11
// needed. Build & run: see the "test-appdirwatcher" target in the
// Makefile.

#include "AppDirWatcher.h"

#include <sys/select.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace Kohiko;

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

// Waits up to `timeoutMs` for `fd` to become readable - inotify
// events on a local filesystem are effectively immediate, but this
// avoids the test being flaky under load rather than assuming Poll()
// would already see it with zero wait.
bool WaitReadable(int fd, int timeoutMs)
{
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(fd, &readSet);

    timeval timeout{};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;

    return select(fd + 1, &readSet, nullptr, nullptr, &timeout) > 0;
}

}

int main()
{
    std::filesystem::path tempDir =
        std::filesystem::temp_directory_path() / "kohiko-test-appdirwatcher";
    std::filesystem::remove_all(tempDir);

    std::filesystem::path userApps = tempDir / "user-data" / "applications";
    std::filesystem::create_directories(userApps);

    setenv("XDG_DATA_HOME", (tempDir / "user-data").string().c_str(), 1);
    // No system applications directories exist under this fake root -
    // AppDirWatcher should skip them silently (see Initialize()'s
    // comment) rather than failing outright.
    setenv("XDG_DATA_DIRS", (tempDir / "no-such-system-dir").string().c_str(), 1);

    std::printf("-- Initialize() against a real (if empty) directory --\n");
    AppDirWatcher watcher;
    watcher.Initialize();

    Check(watcher.Available(), "inotify initialized successfully");
    Check(watcher.Fd() >= 0, "Fd() returns a valid descriptor");

    std::printf("\n-- Nothing pending yet --\n");
    Check(!watcher.Poll(), "Poll() returns false with no filesystem activity");

    std::printf("\n-- Creating a .desktop file is detected --\n");
    {
        std::ofstream file(userApps / "firefox.desktop");
        file << "[Desktop Entry]\nName=Firefox\nExec=firefox\nType=Application\n";
    }

    bool readable = WaitReadable(watcher.Fd(), 2000);
    Check(readable, "the watched fd became readable after a file was created");
    Check(watcher.Poll(), "Poll() reports the change");
    Check(!watcher.Poll(), "...and a second immediate Poll() has nothing left to drain");

    std::printf("\n-- Editing an existing file is detected --\n");
    {
        std::ofstream file(userApps / "firefox.desktop", std::ios::app);
        file << "Comment=Browse the web\n";
    }

    Check(WaitReadable(watcher.Fd(), 2000), "fd became readable after an edit");
    Check(watcher.Poll(), "Poll() reports the edit");

    std::printf("\n-- Deleting a file is detected --\n");
    std::filesystem::remove(userApps / "firefox.desktop");

    Check(WaitReadable(watcher.Fd(), 2000), "fd became readable after a deletion");
    Check(watcher.Poll(), "Poll() reports the deletion");

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    std::filesystem::remove_all(tempDir);
    return 0;
}
