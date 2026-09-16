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
    const MonitorManager& monitors)
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

    for (const auto& monitor : monitors.All())
    {
        Resolved resolved = ResolveFor(*monitor);
        const Rect& geometry = monitor->Geometry();

        if (resolved.path.empty() || geometry.width <= 0 || geometry.height <= 0)
            continue; // background color fill above already covers this monitor's area

        Pixmap rendered = ImageRenderer::Render(
            connection, root, resolved.path,
            geometry.width, geometry.height, resolved.mode, m_backgroundColor);

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

    RefreshWatches(monitors);
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

        sawEvent = true;
    }

    return sawEvent;
}

void WallpaperManager::RefreshWatches(
    const MonitorManager& monitors)
{
    if (m_inotifyFd < 0)
    {
        m_inotifyFd = inotify_init1(IN_NONBLOCK);

        if (m_inotifyFd < 0)
            return; // not fatal - see AppDirWatcher's identical reasoning
    }

    for (int watch : m_watchDescriptors)
        inotify_rm_watch(m_inotifyFd, watch);

    m_watchDescriptors.clear();

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
    std::unordered_set<std::string> directories;

    for (const auto& monitor : monitors.All())
    {
        Resolved resolved = ResolveFor(*monitor);

        if (resolved.path.empty())
            continue;

        std::error_code error;
        std::filesystem::path parent = std::filesystem::path(resolved.path).parent_path();

        if (!parent.empty() && std::filesystem::is_directory(parent, error))
            directories.insert(parent.string());
    }

    for (const std::string& directory : directories)
    {
        int watch = inotify_add_watch(
            m_inotifyFd, directory.c_str(),
            IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE);

        if (watch >= 0)
            m_watchDescriptors.push_back(watch);
    }
}

}
