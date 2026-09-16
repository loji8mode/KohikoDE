#include "DesktopEntry.h"

#include "IniFile.h"
#include "Utils.h"

#include <unistd.h>

#include <array>
#include <cstdlib>

namespace Kohiko
{

namespace
{

constexpr const char* kGroup = "Desktop Entry";

// Every field code the spec defines - none of them can be usefully
// substituted here (there's no file/URL list to hand the launched
// program yet), so they're just stripped, same as the code this
// replaces already did, just with the full set instead of a partial
// one.
std::string StripFieldCodes(
    std::string exec)
{
    static constexpr std::array<const char*, 11> kCodes =
    {
        "%f", "%F", "%u", "%U", "%d", "%D",
        "%n", "%N", "%i", "%c", "%k"
    };

    for (const char* code : kCodes)
    {
        std::size_t pos = exec.find(code);

        if (pos != std::string::npos)
            exec.erase(pos);
    }

    while (!exec.empty() &&
           std::isspace(static_cast<unsigned char>(exec.back())))
    {
        exec.pop_back();
    }

    return exec;
}

}

std::string DesktopEntry::ExecBinary() const
{
    if (exec.empty())
        return "";

    std::vector<std::string> tokens = Utils::SplitWhitespace(exec);

    if (tokens.empty())
        return "";

    std::string token = tokens.front();

    // A quoted program path ("/opt/My App/app" arg) keeps its leading
    // quote through the whitespace split above - strip it before
    // taking the basename, or every quoted Exec= dedups as unique.
    if (token.front() == '"' || token.front() == '\'')
        token.erase(0, 1);

    return std::filesystem::path(token).filename().string();
}

std::optional<DesktopEntry> ParseDesktopFile(
    const std::filesystem::path& path,
    int sourcePriority)
{
    IniFile ini;

    if (!ini.Load(path) || !ini.HasGroup(kGroup))
        return std::nullopt;

    DesktopEntry entry;

    entry.desktopId = path.stem().string();
    entry.sourcePath = path.string();
    entry.sourcePriority = sourcePriority;

    entry.type = ini.Get(kGroup, "Type", "Application");
    entry.name = ini.Get(kGroup, "Name");
    entry.genericName = ini.Get(kGroup, "GenericName");
    entry.comment = ini.Get(kGroup, "Comment");
    entry.icon = ini.Get(kGroup, "Icon");
    entry.startupWMClass = ini.Get(kGroup, "StartupWMClass");
    entry.tryExec = ini.Get(kGroup, "TryExec");

    entry.exec = StripFieldCodes(ini.Get(kGroup, "Exec"));

    entry.noDisplay = ini.GetBool(kGroup, "NoDisplay", false);
    entry.hidden = ini.GetBool(kGroup, "Hidden", false);
    entry.terminal = ini.GetBool(kGroup, "Terminal", false);
    entry.autostartEnabled = ini.GetBool(kGroup, "X-GNOME-Autostart-enabled", true);

    entry.categories = ini.GetList(kGroup, "Categories");
    entry.keywords = ini.GetList(kGroup, "Keywords");
    entry.onlyShowIn = ini.GetList(kGroup, "OnlyShowIn");
    entry.notShowIn = ini.GetList(kGroup, "NotShowIn");

    return entry;
}

namespace
{

// $XDG_CURRENT_DESKTOP is a colon-separated list (e.g.
// "ubuntu:GNOME") - Kohiko itself never sets it, so on most Kohiko
// sessions this comes back empty, which PassesShowIn() below treats
// as "unknown environment, don't filter by OnlyShowIn/NotShowIn"
// rather than hiding every entry that specifies either key.
std::vector<std::string> CurrentDesktopNames()
{
    const char* raw = std::getenv("XDG_CURRENT_DESKTOP");

    if (!raw || raw[0] == '\0')
        return {};

    return Utils::Split(raw, ':');
}

bool ListContainsAny(
    const std::vector<std::string>& haystack,
    const std::vector<std::string>& needles)
{
    for (const auto& hay : haystack)
    {
        for (const auto& needle : needles)
        {
            if (Utils::Lower(hay) == Utils::Lower(needle))
                return true;
        }
    }

    return false;
}

bool PassesShowIn(
    const DesktopEntry& entry)
{
    std::vector<std::string> current = CurrentDesktopNames();

    if (current.empty())
        return true; // can't tell what desktop this is - don't second-guess the entry

    if (!entry.onlyShowIn.empty() && !ListContainsAny(entry.onlyShowIn, current))
        return false;

    if (!entry.notShowIn.empty() && ListContainsAny(entry.notShowIn, current))
        return false;

    return true;
}

// TryExec=, per spec: "a generic name of an executable... or its full
// path. If the path is not absolute, the file is looked up in the
// $PATH environment variable. If the file is not present or if it is
// not executable, the entry may be ignored." An empty tryExec means
// the key was never set at all, which is not the same thing as it
// being set-but-unresolvable - callers check emptiness separately
// (see ShouldAutostart()), so this only has to handle the "some value
// was given" case.
bool TryExecFound(
    const std::string& tryExec)
{
    if (tryExec.find('/') != std::string::npos)
        return ::access(tryExec.c_str(), X_OK) == 0; // absolute (or otherwise slash-containing) path - check directly, no PATH search

    const char* pathEnv = std::getenv("PATH");
    std::string path = (pathEnv && pathEnv[0] != '\0') ?
        pathEnv :
        "/usr/local/bin:/usr/bin:/bin"; // glibc's own confstr(_CS_PATH) fallback for a missing $PATH

    std::size_t pos = 0;

    while (pos <= path.size())
    {
        std::size_t end = path.find(':', pos);

        if (end == std::string::npos)
            end = path.size();

        if (end > pos)
        {
            std::filesystem::path candidate =
                std::filesystem::path(path.substr(pos, end - pos)) / tryExec;

            if (::access(candidate.c_str(), X_OK) == 0)
                return true;
        }

        pos = end + 1;
    }

    return false;
}

}

bool ShouldDisplay(
    const DesktopEntry& entry,
    bool includeHiddenNoDisplay)
{
    if (entry.type != "Application")
        return false;

    if (!includeHiddenNoDisplay && (entry.noDisplay || entry.hidden))
        return false;

    if (entry.exec.empty())
        return false; // nothing to launch - not a real, runnable application

    if (!PassesShowIn(entry))
        return false;

    return true;
}

bool ShouldAutostart(
    const DesktopEntry& entry)
{
    if (entry.type != "Application")
        return false;

    if (entry.hidden)
        return false; // deliberately NOT noDisplay too - see this function's header comment

    if (entry.exec.empty())
        return false; // nothing to launch

    if (!entry.autostartEnabled)
        return false;

    if (!entry.tryExec.empty() && !TryExecFound(entry.tryExec))
        return false;

    if (!PassesShowIn(entry))
        return false;

    return true;
}

bool LooksLikeHelperEntry(
    const DesktopEntry& entry)
{
    // Narrow, deliberately conservative substring patterns for the
    // handful of non-app helper .desktop files that are still
    // NoDisplay=false in the wild (most of the ones the task lists -
    // MIME handlers, Electron auto-updaters, uninstallers - already
    // ship with NoDisplay=true and are caught by ShouldDisplay()
    // instead). Matched against the desktop ID and the Exec binary
    // name, both lower-cased.
    static constexpr std::array<const char*, 12> kPatterns =
    {
        "uninstall",
        "-updater",
        "update-notifier",
        "mimeinfo",
        "mimehandler",
        "-migration",
        "software-properties",
        "-firstrun",
        "-first-run",
        "print-applet",
        "-crash-report",
        "xdg-desktop-portal",
    };

    std::string haystack =
        Utils::Lower(entry.desktopId) + " " +
        Utils::Lower(entry.ExecBinary());

    for (const char* pattern : kPatterns)
    {
        if (haystack.find(pattern) != std::string::npos)
            return true;
    }

    return false;
}

}
