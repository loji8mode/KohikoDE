#include "WallpaperManager.h"

#include "Config.h"
#include "ImageRenderer.h"
#include "Monitor.h"
#include "MonitorManager.h"
#include "Types.h"
#include "Utils.h"
#include "Workspace.h"
#include "XConnection.h"

#include <sys/inotify.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <unordered_set>

namespace Kohiko
{

namespace
{


bool ParseWallpaperRule(
    const std::string& text,
    ImageScaleMode defaultMode,
    std::string& outId,
    std::string& outPath,
    ImageScaleMode& outMode)
{
    std::size_t firstComma = text.find(',');

    if (firstComma == std::string::npos)
        return false;

    std::string id = Utils::Trim(text.substr(0, firstComma));

    if (id.empty())
        return false;

    std::string path;
    ImageScaleMode mode = defaultMode;
    bool gotPath = false;

    std::string rest = text.substr(firstComma + 1);
    std::size_t pos = 0;

    while (pos <= rest.size())
    {
        std::size_t comma = rest.find(',', pos);
        std::string token = Utils::Trim(
            (comma == std::string::npos) ? rest.substr(pos) : rest.substr(pos, comma - pos));

        std::size_t eq = token.find('=');

        if (eq != std::string::npos)
        {
            std::string key = Utils::Trim(token.substr(0, eq));
            std::string value = Utils::Trim(token.substr(eq + 1));

            if (key == "path" && !value.empty())
            {
                path = value;
                gotPath = true;
            }
            else if (key == "mode")
            {
                mode = ImageRenderer::ParseMode(value, mode);
            }
        }

        if (comma == std::string::npos)
            break;

        pos = comma + 1;
    }

    if (!gotPath)
        return false;

    outId = id;
    outPath = path;
    outMode = mode;

    return true;
}

}

WallpaperManager::WallpaperManager() = default;

WallpaperManager::~WallpaperManager()
{
    // Closing the inotify fd itself automatically removes every watch
    // still associated with it (same as AppDirWatcher's own
    // destructor) - no need to inotify_rm_watch() each one in
    // m_watchedDirectories individually first, and doing so here would
    // just generate a burst of IN_IGNORED events nothing is left to
    // ever read anyway.
    if (m_inotifyFd >= 0)
        ::close(m_inotifyFd);

    // m_currentRootPixmap is deliberately NOT freed here: once set via
    // XSetWindowBackgroundPixmap, the root window keeps using it for
    // any exposed area until something else replaces it - the
    // standard convention every wallpaper-setting tool (feh, nitrogen,
    // xwallpaper) already relies on, precisely so the desktop doesn't
    // go blank/undefined the moment the tool that set it exits. Kohiko
    // is no different: this pixmap is meant to outlive this object.
}

void WallpaperManager::Configure(
    const Config& config)
{
    m_defaultPath = config.GetString("wallpaper.default", "");
    m_defaultMode = ImageRenderer::ParseMode(config.GetString("wallpaper.mode", "fill"), ImageScaleMode::Fill);
    m_backgroundColor = std::strtoul(
        config.GetString("wallpaper.background_color", "0x000000").c_str(), nullptr, 0);

    m_monitorRules.clear();
    m_workspaceRules.clear();

    for (const std::string& line : config.GetAll("wallpaper.monitor"))
    {
        std::string outputName, path;
        ImageScaleMode mode;

        if (ParseWallpaperRule(line, m_defaultMode, outputName, path, mode))
            m_monitorRules[outputName] = { path, mode }; // a later line for the same output wins
    }

    for (const std::string& line : config.GetAll("wallpaper.workspace"))
    {
        std::string idText, path;
        ImageScaleMode mode;

        if (!ParseWallpaperRule(line, m_defaultMode, idText, path, mode))
            continue;

        try
        {
            int id = std::stoi(idText);

            if (id >= 1)
                m_workspaceRules[id] = { path, mode }; // a later line for the same workspace wins
        }
        catch (const std::exception&)
        {
            // Not a valid integer - skip this one line rather than
            // the whole config, same future-proofing philosophy as
            // everywhere else this parses.
        }
    }
}

WallpaperManager::Resolved WallpaperManager::ResolveFor(
    const Monitor& monitor) const
{
    Workspace* workspace = monitor.ActiveWorkspace();

    if (workspace)
    {
        auto it = m_workspaceRules.find(workspace->Id());

        if (it != m_workspaceRules.end())
            return { it->second.path, it->second.mode };
    }

    auto it = m_monitorRules.find(monitor.Name());

    if (it != m_monitorRules.end())
        return { it->second.path, it->second.mode };

    return { m_defaultPath, m_defaultMode };
}

unsigned long WallpaperManager::BackgroundColor() const
{
    return m_backgroundColor;
}

void WallpaperManager::ApplyToRoot(
    XConnection& connection,
    const MonitorManager& monitors,
    bool forceRerender)
{
    Display* display = connection.GetDisplay();

    if (!display)
        return;

    // The bounding box of every connected monitor - exactly the X
    // screen's own dimensions under XRandR, since monitor geometries
    // are always non-overlapping sub-rectangles of it.
    int totalWidth = 0;
    int totalHeight = 0;

    for (const auto& monitor : monitors.All())
    {
        const Rect& geometry = monitor->Geometry();
        totalWidth = std::max(totalWidth, geometry.x + geometry.width);
        totalHeight = std::max(totalHeight, geometry.y + geometry.height);
    }

    if (totalWidth <= 0 || totalHeight <= 0)
        return;

    // What ResolveFor() currently says for every monitor - pure Config
    // lookups, no rendering/X11 work yet. Used both to decide whether
    // the render below can be skipped, and (via resolvedPaths) to keep
    // the inotify watch set current either way - see this function's
    // own header comment and RefreshWatches()'s.
    const auto& allMonitors = monitors.All();

    std::vector<CompositedMonitor> desired;
    std::vector<std::string> resolvedPaths;

    desired.reserve(allMonitors.size());

    for (const auto& monitor : allMonitors)
    {
        Resolved resolved = ResolveFor(*monitor);

        desired.push_back(CompositedMonitor{
            monitor->Id(), monitor->Geometry(), resolved.path, resolved.mode});

        if (!resolved.path.empty())
            resolvedPaths.push_back(resolved.path);
    }

    bool sameAsLastComposite =
        !forceRerender &&
        m_currentRootPixmap != 0 &&
        totalWidth == m_lastCompositedWidth &&
        totalHeight == m_lastCompositedHeight &&
        m_backgroundColor == m_lastCompositedBackgroundColor &&
        desired == m_lastComposited;

    if (!sameAsLastComposite)
    {
        int screen = connection.Screen();
        ::Window root = connection.Root();

        Pixmap composite = XCreatePixmap(
            display, root,
            static_cast<unsigned int>(totalWidth), static_cast<unsigned int>(totalHeight),
            DefaultDepth(display, screen));

        GC gc = XCreateGC(display, composite, 0, nullptr);

        // Fills the whole composite first - covers any area no monitor's
        // rectangle happens to reach (shouldn't normally exist, but
        // defensive) with something sane rather than undefined pixmap
        // contents.
        XSetForeground(display, gc, m_backgroundColor);
        XFillRectangle(display, composite, gc, 0, 0,
            static_cast<unsigned int>(totalWidth), static_cast<unsigned int>(totalHeight));

        for (std::size_t i = 0; i < allMonitors.size(); ++i)
        {
            const Rect& geometry = desired[i].geometry;
            const std::string& path = desired[i].path;

            if (path.empty() || geometry.width <= 0 || geometry.height <= 0)
                continue; // background color fill above already covers this monitor's area

            Pixmap rendered = ImageRenderer::Render(
                connection, root, path,
                geometry.width, geometry.height, desired[i].mode, m_backgroundColor);

            if (rendered)
            {
                XCopyArea(display, rendered, composite, gc,
                    0, 0,
                    static_cast<unsigned int>(geometry.width), static_cast<unsigned int>(geometry.height),
                    geometry.x, geometry.y);

                XFreePixmap(display, rendered);
            }
        }

        XFreeGC(display, gc);

        XSetWindowBackgroundPixmap(display, root, composite);
        XClearWindow(display, root);
        XFlush(display);

        if (m_currentRootPixmap)
            XFreePixmap(display, m_currentRootPixmap);

        m_currentRootPixmap = composite;

        m_lastComposited = desired;
        m_lastCompositedWidth = totalWidth;
        m_lastCompositedHeight = totalHeight;
        m_lastCompositedBackgroundColor = m_backgroundColor;
    }

    // RefreshWatches() itself decides what actually needs touching from
    // this - see its own comment for why calling it unconditionally on
    // every ApplyToRoot() (including a call the skip-check above just
    // turned into a no-op, or one Poll() itself triggered) is safe.
    RefreshWatches(resolvedPaths);
}

bool WallpaperManager::Available() const
{
    return m_inotifyFd >= 0;
}

int WallpaperManager::Fd() const
{
    return m_inotifyFd;
}

bool WallpaperManager::Poll()
{
    if (m_inotifyFd < 0)
        return false;

    bool sawEvent = false;
    std::array<char, 4096> buffer;

    while (true)
    {
        ssize_t bytesRead = ::read(m_inotifyFd, buffer.data(), buffer.size());

        if (bytesRead <= 0)
            break; // EAGAIN (non-blocking fd) - nothing left queued

        // read() on inotify never returns a partial event, so this
        // walks exactly `bytesRead` bytes' worth of complete
        // `struct inotify_event` records (each one's `len` covers its
        // own trailing name, possibly zero) - same layout every other
        // inotify consumer (including the kernel's own documentation)
        // assumes.
        std::size_t offset = 0;

        while (offset + sizeof(struct inotify_event) <= static_cast<std::size_t>(bytesRead))
        {
            const auto* event =
                reinterpret_cast<const struct inotify_event*>(buffer.data() + offset);

            // IN_IGNORED means a watch was torn down - explicitly, by
            // RefreshWatches() itself removing one, or because a
            // watched directory was deleted/unmounted out from under
            // it - and carries no information about any *file's
            // contents* having changed. Every watch RefreshWatches()
            // installs only ever asks for real content-change bits
            // (IN_CREATE/IN_DELETE/IN_MODIFY/IN_MOVED_FROM/
            // IN_MOVED_TO/IN_CLOSE_WRITE), so IN_IGNORED is the one
            // event kind that can arrive on this fd without any of
            // those - and specifically the one a watch-refresh's own
            // inotify_rm_watch() call generates on itself. Not
            // filtering this out here is what turned a
            // Poll()-triggered ApplyToRoot() -> RefreshWatches() into
            // a self-sustaining loop before this fix (each refresh's
            // own removal kept manufacturing the next "something
            // changed" wakeup) - see CHANGELOG.md's 0.20.5 entry for
            // the full writeup.
            if (!(event->mask & IN_IGNORED))
                sawEvent = true;

            offset += sizeof(struct inotify_event) + event->len;
        }
    }

    return sawEvent;
}

void WallpaperManager::RefreshWatches(
    const std::vector<std::string>& resolvedPaths)
{
    if (m_inotifyFd < 0)
    {
        m_inotifyFd = inotify_init1(IN_NONBLOCK);

        if (m_inotifyFd < 0)
            return; // not fatal - see AppDirWatcher's identical reasoning
    }

    // Watches each resolved file's *containing directory*, not the
    // file itself - inotify watches are attached to an inode, and a
    // common "save" pattern (write a new file, rename() it over the
    // original) replaces the inode at that path entirely, which a
    // watch on the old inode won't reliably see. Watching the
    // directory instead (the same approach AppDirWatcher already
    // uses, and for the same reason) catches every way the file
    // actually changes - an in-place edit, or a full replace - at the
    // cost of also seeing unrelated changes to sibling files in the
    // same directory, which just means an occasional harmless extra
    // Poll() -> ApplyToRoot() re-render (cheap; see this class's own
    // header comment on why that's fine).
    std::unordered_set<std::string> desiredDirectories;

    for (const std::string& path : resolvedPaths)
    {
        if (path.empty())
            continue;

        std::error_code error;
        std::filesystem::path parent = std::filesystem::path(path).parent_path();

        if (!parent.empty() && std::filesystem::is_directory(parent, error))
            desiredDirectories.insert(parent.string());
    }

    // Drop only the watches for directories that are no longer
    // desired - a directory that's still wanted keeps its existing
    // watch descriptor untouched, with no inotify_rm_watch() call (and
    // therefore no IN_IGNORED) at all. This is the actual fix for the
    // idle-CPU regression: previously every watch was torn down and
    // rebuilt on every single call, including ones this method's own
    // previous run indirectly caused via Poll() - see Poll()'s and
    // this method's own header comments, and CHANGELOG.md's 0.20.5
    // entry, for the full loop this closes.
    for (auto it = m_watchedDirectories.begin(); it != m_watchedDirectories.end(); )
    {
        if (desiredDirectories.find(it->first) == desiredDirectories.end())
        {
            inotify_rm_watch(m_inotifyFd, it->second);
            it = m_watchedDirectories.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Add watches only for directories that aren't already watched.
    for (const std::string& directory : desiredDirectories)
    {
        if (m_watchedDirectories.find(directory) != m_watchedDirectories.end())
            continue; // already watching this one - leave its watch descriptor alone

        int watch = inotify_add_watch(
            m_inotifyFd, directory.c_str(),
            IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE);

        if (watch >= 0)
            m_watchedDirectories[directory] = watch;
    }
}

}
