#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

// A from-scratch implementation of the lookup algorithm in the
// freedesktop Icon Theme Specification
// (https://specifications.freedesktop.org/icon-theme-spec/latest/),
// used instead of GtkIconTheme so the launcher no longer needs to
// link the whole of GTK3 just to answer "where is the file for icon
// name X" (see CMakeLists.txt/Makefile - GTK3 has been dropped
// entirely now that this is the only thing that ever used it).
//
// Deliberately pure filesystem + string work, no Xlib/Imlib2 calls -
// that's what lets AppIndex resolve every application's icon path
// once, up front, on its background rebuild thread (see AppIndex.h),
// instead of the UI thread doing filesystem lookups while the user is
// typing.
namespace Kohiko
{

class IconResolver
{
public:

    // `themeNameOverride`, when non-empty, always wins (this is what
    // `launcher.icon_theme=` in Config plugs into). Otherwise the
    // resolver auto-detects the active GTK icon theme from
    // ~/.config/gtk-3.0/settings.ini, falling back to "hicolor" (the
    // one theme every conformant icon set - and therefore every
    // Linux system - is required to ship).
    explicit IconResolver(
        std::string themeNameOverride = ""
    );

    // Resolves an icon name (e.g. "firefox") or an already-absolute
    // path to a concrete file on disk. Returns "" if nothing could be
    // found anywhere - callers should fall back to a generic
    // "application-x-executable" lookup (or nothing at all) rather
    // than treat that as an error; plenty of third-party .desktop
    // files reference an icon name no installed theme actually ships.
    //
    // If the exact name isn't found anywhere in the chain, also tries
    // it with a "-symbolic" suffix before giving up - a plain status-
    // icon name like "network-wireless-signal-excellent" is standard,
    // real freedesktop icon-naming-spec content, but most actively-
    // maintained themes (Adwaita included) only actually ship it as
    // "network-wireless-signal-excellent-symbolic" these days, full-
    // colour status icons having mostly fallen out of use. Skipped
    // when the requested name already ends in "-symbolic", so this
    // never doubles up into a "...-symbolic-symbolic" search.
    //
    // Resolved names are cached in memory for the lifetime of this
    // object, including negative (not-found) results, so looking up
    // the same icon name twice (extremely common - many apps share
    // one generic icon) never touches the filesystem a second time.
    std::string Resolve(
        const std::string& iconName
    );

private:

    struct ThemeDir
    {
        std::string relativePath; // e.g. "48x48/apps", relative to the theme's own base directory
        int size = 48;
        int minSize = 48;
        int maxSize = 48;
        int threshold = 2;
        std::string type = "Threshold"; // Fixed | Scalable | Threshold
    };

    struct Theme
    {
        std::string name;
        std::filesystem::path base; // .../icons/<name>
        std::vector<ThemeDir> dirs;
    };

    void EnsureChainLoaded();

    // Appends `themeName`'s directories to m_chain, then recurses into
    // whatever it Inherits=, skipping any theme already in `visited`
    // so a (malformed) inheritance cycle can't loop forever.
    void LoadThemeChain(
        const std::string& themeName,
        std::vector<std::string>& visited
    );

    std::string SearchChain(
        const std::string& iconName,
        int size
    ) const;

    std::string SearchPixmaps(
        const std::string& iconName
    ) const;

    static bool DirectoryMatchesSize(
        const ThemeDir& dir,
        int size
    );

    // Per the spec's own DirectorySizeDistance() - how far `size` is
    // from what `dir` actually provides, used for the "closest size"
    // fallback pass when no directory is an exact/threshold match.
    static int DirectorySizeDistance(
        const ThemeDir& dir,
        int size
    );

    static std::string DetectSystemThemeName();

    std::string m_themeNameOverride;
    std::vector<Theme> m_chain;
    bool m_chainLoaded = false;

    int m_preferredSize = 48;

    std::unordered_map<std::string, std::string> m_cache;
};

}
