#pragma once

#include "ImageRenderer.h"

#include <X11/Xlib.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace Kohiko
{

class Config;
class XConnection;
class MonitorManager;
class Monitor;

// Resolves and renders the desktop wallpaper - independently
// configurable per monitor and per workspace (see Configure()'s own
// comment for the exact priority order), rendered via the shared
// ImageRenderer (the same code LockScreen's own background image
// uses) and applied the standard X11 way: a composite Pixmap spanning
// every connected monitor's combined geometry, set as the root
// window's background. No separate "wallpaper window" to manage -
// this is exactly what feh/nitrogen/xwallpaper already do, and means
// gaps between tiled windows (and empty workspaces) show the
// wallpaper through automatically, for free.
//
// Also watches (via inotify) every currently-resolved wallpaper file
// for changes on disk - see Fd()/Poll() - and lets WindowManager know
// (by returning true from Poll()) exactly when to call ApplyToRoot()
// again. Deliberately does *not* try to detect every conceivable
// reason a re-render might be needed (that's WindowManager's job,
// same as everywhere else in this codebase - see its own
// Initialize()/HandleMonitorTopologyChanged()/
// SwitchWorkspaceOnMonitor() call sites), only "a file already in use
// as a wallpaper changed on disk".
class WallpaperManager
{
public:

    WallpaperManager();
    ~WallpaperManager();

    WallpaperManager(const WallpaperManager&) = delete;
    WallpaperManager& operator=(const WallpaperManager&) = delete;

    // Reads wallpaper.default/wallpaper.mode/wallpaper.background_color
    // and every wallpaper.monitor=/wallpaper.workspace= line - safe to
    // call again later (a config reload), same as every other
    // Configure() in this codebase. Does *not* itself call
    // ApplyToRoot() or touch the inotify watch set - see those
    // methods' own comments for why the caller drives both instead.
    void Configure(
        const Config& config
    );

    struct Resolved
    {
        // Empty means "nothing configured for this monitor - just
        // show BackgroundColor()", a normal, valid resolution rather
        // than a failure case.
        std::string path;

        ImageScaleMode mode = ImageScaleMode::Fill;
    };

    // The wallpaper that should currently be showing on `monitor`,
    // resolved via its *currently active* workspace. Priority, most
    // specific first:
    //
    //   1. wallpaper.workspace=<id>,... for the workspace `monitor` is
    //      currently showing
    //   2. wallpaper.monitor=<name>,... for `monitor` itself
    //   3. wallpaper.default
    //
    // A rule's own mode=, if it specified one, always wins; otherwise
    // falls back to wallpaper.mode (already resolved at Configure()
    // time, so there's nothing left to look up here).
    Resolved ResolveFor(
        const Monitor& monitor
    ) const;

    unsigned long BackgroundColor() const;

    // Renders ResolveFor() for every monitor in `monitors` into one
    // composite Pixmap spanning their combined geometry (the same
    // area the root window itself covers under XRandR) and sets it as
    // the root window's background, replacing (and freeing) whatever
    // this had previously set. This is the one and only place
    // anything is actually drawn - Configure() and ResolveFor() above
    // are pure bookkeeping.
    //
    // Cheap enough to call whenever anything that could change what
    // ResolveFor() returns for any monitor happens (a workspace switch
    // on any monitor, a monitor topology change, a config reload, or
    // Poll() below reporting a watched file changed) - WindowManager
    // is responsible for calling this at each of those points; this
    // class has no way to know about any of them itself. Also
    // refreshes the inotify watch set (see RefreshWatches()) to match
    // whatever was just rendered, so a later edit to any of *these*
    // files is still caught even if the rules themselves haven't
    // changed since the last Configure().
    void ApplyToRoot(
        XConnection& connection,
        const MonitorManager& monitors
    );

    bool Available() const;

    // For EventLoop's own select() set, exactly like AppDirWatcher::Fd().
    int Fd() const;

    // Drains every pending inotify event and returns true if anything
    // was actually read - i.e. the caller should call ApplyToRoot()
    // again (whatever changed on disk needs to be re-rendered). Same
    // "doesn't matter which specific file, just that something did"
    // reasoning as AppDirWatcher::Poll() - re-rendering is cheap
    // enough that a spurious wakeup costs little.
    bool Poll();

private:

    struct Rule
    {
        std::string path;
        ImageScaleMode mode = ImageScaleMode::Fill;
    };

    std::string m_defaultPath;
    ImageScaleMode m_defaultMode = ImageScaleMode::Fill;
    unsigned long m_backgroundColor = 0x000000;

    std::unordered_map<std::string, Rule> m_monitorRules;   // keyed by XRandr output name
    std::unordered_map<int, Rule> m_workspaceRules;         // keyed by workspace id

    Pixmap m_currentRootPixmap = 0;

    int m_inotifyFd = -1;
    std::vector<int> m_watchDescriptors;

    // (Re)points the inotify watch set at exactly the files
    // ResolveFor() could currently return across `monitors` - called
    // from ApplyToRoot() itself, right after rendering, so the watch
    // set always matches what was just actually drawn.
    void RefreshWatches(
        const MonitorManager& monitors
    );

};

}
