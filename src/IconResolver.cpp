#include "IconResolver.h"

#include "IniFile.h"
#include "Utils.h"
#include "Xdg.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>

namespace Kohiko
{

namespace
{

namespace fs = std::filesystem;

// Tries `base/name.ext` for every extension in preference order
// (scalable SVG first, then PNG, then the legacy XPM format some
// older themes still ship) and returns the first that actually
// exists on disk.
std::string FirstExistingWithExtension(
    const fs::path& base,
    const std::string& name)
{
    static constexpr std::array<const char*, 3> kExtensions = {".svg", ".png", ".xpm"};

    for (const char* ext : kExtensions)
    {
        fs::path candidate = base / (name + ext);

        std::error_code ec;

        if (fs::exists(candidate, ec) && !ec)
            return candidate.string();
    }

    return "";
}

}

IconResolver::IconResolver(
    std::string themeNameOverride)
    :
    m_themeNameOverride(std::move(themeNameOverride))
{
}

std::string IconResolver::DetectSystemThemeName()
{
    // gtk-3.0's settings.ini is the closest thing to a portable
    // "what icon theme is the user using" signal on a system that
    // otherwise has no desktop environment of its own (Kohiko is a
    // WM, not a DE) - most icon-theme-aware GTK/Qt apps end up
    // reading (or at least respecting overrides for) the same file.
    if (fs::path home = Xdg::Home(); !home.empty())
    {
        IniFile settings;

        if (settings.Load(home / ".config" / "gtk-3.0" / "settings.ini"))
        {
            std::string theme = settings.Get("Settings", "gtk-icon-theme-name");

            if (!theme.empty())
                return theme;
        }
    }

    return "hicolor";
}

void IconResolver::EnsureChainLoaded()
{
    if (m_chainLoaded)
        return;

    m_chainLoaded = true;

    std::string themeName = m_themeNameOverride.empty() ?
        DetectSystemThemeName() :
        m_themeNameOverride;

    std::vector<std::string> visited;
    LoadThemeChain(themeName, visited);

    // The spec requires every theme lookup to fall back to "hicolor"
    // last, regardless of what the active theme Inherits= - most
    // themes already do this explicitly, but this makes sure it
    // happens even for the ones that don't.
    if (std::find(visited.begin(), visited.end(), "hicolor") == visited.end())
        LoadThemeChain("hicolor", visited);
}

void IconResolver::LoadThemeChain(
    const std::string& themeName,
    std::vector<std::string>& visited)
{
    if (std::find(visited.begin(), visited.end(), themeName) != visited.end())
        return; // already loaded (or mid-loading) - avoids an inheritance cycle recursing forever

    visited.push_back(themeName);

    for (const fs::path& iconBase : Xdg::IconThemeDirs())
    {
        fs::path themeBase = iconBase / themeName;
        fs::path indexPath = themeBase / "index.theme";

        std::error_code ec;

        if (!fs::exists(indexPath, ec))
            continue;

        IniFile index;

        if (!index.Load(indexPath))
            continue;

        Theme theme;
        theme.name = themeName;
        theme.base = themeBase;

        // Prefer the author's own preferred search order (Directories=
        // in [Icon Theme]) when present; a handful of very old/minimal
        // themes omit it, in which case every other group in the file
        // is itself one directory entry, just in file order instead.
        std::vector<std::string> directoryNames = index.GetList("Icon Theme", "Directories", ',');

        if (directoryNames.empty())
        {
            for (const auto& group : index.Groups())
            {
                if (group != "Icon Theme")
                    directoryNames.push_back(group);
            }
        }

        for (const auto& dirName : directoryNames)
        {
            ThemeDir dir;
            dir.relativePath = dirName;
            dir.size = std::atoi(index.Get(dirName, "Size", "48").c_str());
            dir.type = index.Get(dirName, "Type", "Threshold");

            std::string minSize = index.Get(dirName, "MinSize");
            std::string maxSize = index.Get(dirName, "MaxSize");
            std::string threshold = index.Get(dirName, "Threshold");

            dir.minSize = minSize.empty() ? dir.size : std::atoi(minSize.c_str());
            dir.maxSize = maxSize.empty() ? dir.size : std::atoi(maxSize.c_str());
            dir.threshold = threshold.empty() ? 2 : std::atoi(threshold.c_str());

            theme.dirs.push_back(std::move(dir));
        }

        m_chain.push_back(std::move(theme));

        // Recurse into whatever this theme inherits from *after*
        // adding it, keeping the chain in the right priority order
        // (this theme's own icons first, then its parents').
        for (const auto& parent : index.GetList("Icon Theme", "Inherits", ','))
            LoadThemeChain(parent, visited);

        return; // found this theme's index.theme - no need to check the remaining base directories for it too
    }
}

bool IconResolver::DirectoryMatchesSize(
    const ThemeDir& dir,
    int size)
{
    if (dir.type == "Fixed")
        return dir.size == size;

    if (dir.type == "Scalable")
        return size >= dir.minSize && size <= dir.maxSize;

    // "Threshold", also the spec's own default when Type= is absent.
    return size >= dir.size - dir.threshold &&
           size <= dir.size + dir.threshold;
}

int IconResolver::DirectorySizeDistance(
    const ThemeDir& dir,
    int size)
{
    if (dir.type == "Fixed")
        return std::abs(dir.size - size);

    if (dir.type == "Scalable")
    {
        if (size < dir.minSize) return dir.minSize - size;
        if (size > dir.maxSize) return size - dir.maxSize;
        return 0;
    }

    if (size < dir.size - dir.threshold) return (dir.size - dir.threshold) - size;
    if (size > dir.size + dir.threshold) return size - (dir.size + dir.threshold);
    return 0;
}

std::string IconResolver::SearchChain(
    const std::string& iconName,
    int size) const
{
    // Pass 1: an exact/threshold size match, in each theme's own
    // preferred directory order - this is what makes a 48px lookup
    // actually prefer a theme's "48x48" directory over its "256x256"
    // one even though both technically contain the icon.
    for (const auto& theme : m_chain)
    {
        for (const auto& dir : theme.dirs)
        {
            if (!DirectoryMatchesSize(dir, size))
                continue;

            std::string found = FirstExistingWithExtension(theme.base / dir.relativePath, iconName);

            if (!found.empty())
                return found;
        }
    }

    // Pass 2: no directory matched the requested size exactly -
    // per the spec, fall back to whichever directory that *does*
    // contain the icon is numerically closest to the requested size.
    std::string best;
    int bestDistance = std::numeric_limits<int>::max();

    for (const auto& theme : m_chain)
    {
        for (const auto& dir : theme.dirs)
        {
            std::string found = FirstExistingWithExtension(theme.base / dir.relativePath, iconName);

            if (found.empty())
                continue;

            int distance = DirectorySizeDistance(dir, size);

            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = std::move(found);
            }
        }
    }

    return best;
}

std::string IconResolver::SearchPixmaps(
    const std::string& iconName) const
{
    return FirstExistingWithExtension(Xdg::PixmapsDir(), iconName);
}

std::string IconResolver::Resolve(
    const std::string& iconName)
{
    if (iconName.empty())
        return "";

    if (iconName.front() == '/')
    {
        std::error_code ec;
        return fs::exists(iconName, ec) ? iconName : "";
    }

    if (auto cached = m_cache.find(iconName); cached != m_cache.end())
        return cached->second;

    EnsureChainLoaded();

    std::string result = SearchChain(iconName, m_preferredSize);

    // See this function's own header comment (IconResolver.h) for why
    // "-symbolic" specifically.
    if (result.empty() && !iconName.ends_with("-symbolic"))
        result = SearchChain(iconName + "-symbolic", m_preferredSize);

    if (result.empty())
        result = SearchPixmaps(iconName);

    m_cache.emplace(iconName, result); // cached even when empty - a confirmed miss shouldn't be re-searched either
    return result;
}

}
