#pragma once

#include <vector>

namespace Kohiko
{

// Watches every directory AppIndex::Build() scans
// (Xdg::ApplicationDirs() - /usr/share/applications,
// ~/.local/share/applications, and anything else XDG_DATA_DIRS adds)
// for .desktop files being added, edited, or removed, so Launcher can
// pick up the change automatically instead of needing `kohikoctl
// reloadlauncher` or a restart. Fd() is meant to be folded into
// EventLoop::Run()'s existing select() set - the exact same treatment
// ScreenSaverInhibitor::Fd() already gets there - so this needs no
// polling timer and no extra thread of its own.
//
// Uses Linux's inotify(7): watching a *directory* (not each
// .desktop file individually - which would also miss a file created
// after Initialize() ran) is enough to catch every way a package
// manager or a manually-dropped file actually changes it. Gracefully
// unavailable (Available() stays false, Fd() stays -1) if inotify
// couldn't be initialized - never expected to fail on Linux, but not
// something Kohiko should ever crash over; the launcher simply keeps
// working exactly as it did before this class existed; a manual
// `kohikoctl reloadlauncher` still works either way.
class AppDirWatcher
{
public:

    AppDirWatcher();
    ~AppDirWatcher();

    AppDirWatcher(const AppDirWatcher&) = delete;
    AppDirWatcher& operator=(const AppDirWatcher&) = delete;

    // Opens the inotify fd and adds a watch on every currently-listed
    // Xdg::ApplicationDirs() directory that exists yet - one that
    // XDG_DATA_DIRS lists but doesn't exist (e.g. a Flatpak
    // installation path when Flatpak isn't installed) is simply
    // skipped, the same as AppIndex::Build() already treats a missing
    // directory as "just empty" rather than an error.
    void Initialize();

    bool Available() const;

    // The inotify fd itself, for EventLoop's select() set - see this
    // class's own header comment. -1 if Available() is false.
    int Fd() const;

    // Drains every pending inotify event (Fd() being readable only
    // means "at least one event is queued", not "exactly one") and
    // returns true if anything was actually read - i.e. the caller
    // should treat the application list as possibly stale. Always
    // safe to call even when nothing's pending (returns false).
    //
    // Deliberately doesn't look at *which* file changed or how -
    // every event here means "re-check", and AppIndex::IsFresh()
    // already does the real, precise "did anything actually change"
    // comparison (directory mtimes) before Launcher decides whether
    // to actually re-scan/re-parse anything, so a spurious wakeup
    // here (e.g. a directory's own mtime touched by something that
    // isn't really a .desktop file change) costs nothing beyond that
    // one cheap mtime check.
    bool Poll();

private:

    int m_inotifyFd = -1;
    std::vector<int> m_watchDescriptors;

};

}
