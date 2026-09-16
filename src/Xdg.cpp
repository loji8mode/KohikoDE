#include "Xdg.h"

#include <cstdlib>
#include <system_error>

namespace Kohiko::Xdg
{

namespace
{

namespace fs = std::filesystem;

// Empty-string env vars are treated as "unset", per the spec's own
// wording ("If ... is either not set or empty, a default equal to
// ... should be used").
std::string EnvOrEmpty(
    const char* name)
{
    const char* value = std::getenv(name);
    return (value && value[0] != '\0') ? std::string(value) : std::string();
}

fs::path EnsureExists(
    fs::path path)
{
    std::error_code ec;
    fs::create_directories(path, ec); // best-effort - callers just get write failures later if this didn't work
    return path;
}

// Splits a ':'-separated XDG dir-list env var into paths, dropping
// empty entries (a leading/trailing/doubled ':') - shared by
// XDG_DATA_DIRS below and XDG_CONFIG_DIRS in AutostartDirs(), which
// both use exactly this same list syntax.
std::vector<fs::path> SplitPathList(
    const std::string& raw)
{
    std::vector<fs::path> dirs;

    std::size_t pos = 0;

    while (pos <= raw.size())
    {
        std::size_t end = raw.find(':', pos);

        if (end == std::string::npos)
            end = raw.size();

        if (end > pos)
            dirs.emplace_back(raw.substr(pos, end - pos));

        pos = end + 1;
    }

    return dirs;
}

// $XDG_DATA_DIRS, split on ':', defaulting to the spec's own
// "/usr/local/share/:/usr/share/" when unset/empty.
std::vector<fs::path> DataDirs()
{
    std::string raw = EnvOrEmpty("XDG_DATA_DIRS");

    if (raw.empty())
        raw = "/usr/local/share/:/usr/share/";

    return SplitPathList(raw);
}

}

std::filesystem::path Home()
{
    return fs::path(EnvOrEmpty("HOME"));
}

std::filesystem::path CacheDir()
{
    std::string xdgCache = EnvOrEmpty("XDG_CACHE_HOME");

    fs::path base = xdgCache.empty() ?
        Home() / ".cache" :
        fs::path(xdgCache);

    return EnsureExists(base / "kohiko");
}

std::filesystem::path DataDir()
{
    std::string xdgData = EnvOrEmpty("XDG_DATA_HOME");

    fs::path base = xdgData.empty() ?
        Home() / ".local" / "share" :
        fs::path(xdgData);

    return EnsureExists(base / "kohiko");
}

std::filesystem::path ConfigDir()
{
    std::string xdgConfig = EnvOrEmpty("XDG_CONFIG_HOME");

    fs::path base = xdgConfig.empty() ?
        Home() / ".config" :
        fs::path(xdgConfig);

    return EnsureExists(base / "kohiko");
}

std::vector<std::filesystem::path> ApplicationDirs()
{
    std::vector<fs::path> dirs;

    std::string xdgData = EnvOrEmpty("XDG_DATA_HOME");

    fs::path userData = xdgData.empty() ?
        Home() / ".local" / "share" :
        fs::path(xdgData);

    if (!userData.empty())
        dirs.push_back(userData / "applications");

    for (const auto& dir : DataDirs())
        dirs.push_back(dir / "applications");

    return dirs;
}

std::vector<std::filesystem::path> AutostartDirs()
{
    std::vector<fs::path> dirs;

    std::string xdgConfig = EnvOrEmpty("XDG_CONFIG_HOME");

    fs::path userConfig = xdgConfig.empty() ?
        Home() / ".config" :
        fs::path(xdgConfig);

    if (!userConfig.empty())
        dirs.push_back(userConfig / "autostart");

    std::string rawConfigDirs = EnvOrEmpty("XDG_CONFIG_DIRS");

    if (rawConfigDirs.empty())
        rawConfigDirs = "/etc/xdg";

    for (const auto& dir : SplitPathList(rawConfigDirs))
        dirs.push_back(dir / "autostart");

    return dirs;
}

std::vector<std::filesystem::path> AppStreamDirs()
{
    std::vector<fs::path> dirs;

    std::string xdgData = EnvOrEmpty("XDG_DATA_HOME");

    fs::path userData = xdgData.empty() ?
        Home() / ".local" / "share" :
        fs::path(xdgData);

    if (!userData.empty())
        dirs.push_back(userData / "metainfo");

    for (const auto& dir : DataDirs())
    {
        dirs.push_back(dir / "metainfo");
        dirs.push_back(dir / "app-info" / "xmls"); // older appstream-generator layout, still shipped by several distros
    }

    return dirs;
}

std::vector<std::filesystem::path> IconThemeDirs()
{
    std::vector<fs::path> dirs;

    if (fs::path home = Home(); !home.empty())
        dirs.push_back(home / ".icons");

    std::string xdgData = EnvOrEmpty("XDG_DATA_HOME");

    fs::path userData = xdgData.empty() ?
        Home() / ".local" / "share" :
        fs::path(xdgData);

    if (!userData.empty())
        dirs.push_back(userData / "icons");

    for (const auto& dir : DataDirs())
        dirs.push_back(dir / "icons");

    return dirs;
}

std::filesystem::path PixmapsDir()
{
    return "/usr/share/pixmaps";
}

}
