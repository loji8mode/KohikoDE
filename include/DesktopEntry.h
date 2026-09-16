#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Parses freedesktop .desktop application entries
// (https://specifications.freedesktop.org/desktop-entry-spec/latest/)
// into a plain data struct, and decides whether one deserves to show
// up in the launcher at all. Used by AppIndex to build the persistent
// application index; kept as pure filesystem/string code (no Xlib, no
// XConnection) so it's safe to run from AppIndex's background rebuild
// thread.
namespace Kohiko
{

struct DesktopEntry
{
    std::string desktopId;   // filename without ".desktop" (freedesktop's own notion of "the" ID for a given entry)
    std::string sourcePath;  // absolute path to the file this was parsed from

    // Lower sourcePriority means "found in a directory the user/admin
    // is more likely to have deliberately customised" - see
    // Xdg::ApplicationDirs(). AppIndex's dedup pass uses this to
    // decide which of several candidates for the "same" application
    // wins.
    int sourcePriority = 0;

    std::string name;
    std::string genericName;
    std::string comment;

    std::string exec;        // Exec=, with %f/%F/%u/%U/... field codes stripped (nothing left to substitute them with here)
    std::string tryExec;     // TryExec= - a binary that must exist on PATH for this entry to be valid
    std::string icon;        // Icon= - a bare theme icon name, or an absolute path
    std::string startupWMClass;

    std::vector<std::string> categories;
    std::vector<std::string> keywords;
    std::vector<std::string> onlyShowIn;
    std::vector<std::string> notShowIn;

    std::string type = "Application"; // Type= - Application/Link/Directory
    bool noDisplay = false;
    bool hidden = false;
    bool terminal = false;

    // X-GNOME-Autostart-enabled - not part of the formal Desktop Entry
    // Specification, but a de-facto extension the Desktop Application
    // Autostart Specification explicitly carves out room for ("desktop
    // environments... may... support additional methods for enabling/
    // disabling autostart"), near-universally honoured, and how e.g. a
    // desktop's own settings UI commonly disables one specific
    // autostart entry without deleting or hand-editing the file. Only
    // ever meaningful to ShouldAutostart() below - distinct from
    // hidden (the spec's own key, checked there too). Defaults to
    // true: absence means enabled, matching every desktop's own
    // interpretation (and every entry Kohiko itself ships, none of
    // which sets this to false).
    bool autostartEnabled = true;

    // First whitespace-separated token of `exec`, with any leading
    // path stripped - used both for dedup (two desktop files that
    // Exec the same binary are almost certainly the same app under
    // two names) and as a fallback search field.
    std::string ExecBinary() const;
};

// Parses a single .desktop file. Returns std::nullopt if the file
// couldn't be read, or has no [Desktop Entry] group at all (some
// stray non-entry .desktop-suffixed files do exist in the wild).
std::optional<DesktopEntry> ParseDesktopFile(
    const std::filesystem::path& path,
    int sourcePriority
);

// True if this entry both wants to be shown (NoDisplay/Hidden) and
// makes sense to show given the current desktop environment
// (OnlyShowIn/NotShowIn against $XDG_CURRENT_DESKTOP), and has
// something to actually launch. `includeHiddenNoDisplay` mirrors the
// task's "unless explicitly enabled in configuration" carve-out for
// Hidden=true/NoDisplay=true (see `launcher.show_hidden=` in
// default.conf).
bool ShouldDisplay(
    const DesktopEntry& entry,
    bool includeHiddenNoDisplay
);

// True if this entry should be launched once at session startup, per
// the freedesktop Desktop Application Autostart Specification
// (https://specifications.freedesktop.org/autostart-spec/latest/) -
// answers a different question from ShouldDisplay() above, and in
// particular treats NoDisplay= oppositely: an autostart helper is
// *expected* to set NoDisplay=true (it has no menu presence at all)
// while still very much wanting to run, which is exactly what
// Kohiko's own desktop/kohiko-*-tray.desktop entries do. Checks, in
// order: Type=Application; not Hidden=true (the spec's own kill
// switch); Exec= actually set to something; not disabled via
// X-GNOME-Autostart-enabled=false; TryExec=, if set, resolves to a
// real executable; and OnlyShowIn=/NotShowIn= against
// $XDG_CURRENT_DESKTOP, reusing the exact same rule ShouldDisplay()
// applies for the same keys.
bool ShouldAutostart(
    const DesktopEntry& entry
);

// A light heuristic layer on top of ShouldDisplay(): most non-app
// helper entries (MIME handlers, updater UIs, uninstallers, desktop
// integration helpers...) already ship with NoDisplay=true, which
// ShouldDisplay() alone screens out - this only catches the handful
// that don't, by matching well-known, narrow patterns in the desktop
// ID or Exec line. Deliberately conservative: it is much better to
// occasionally show a stray helper than to hide a real application by
// an over-eager guess. Controlled by `launcher.strict_filtering=`
// (default on).
bool LooksLikeHelperEntry(
    const DesktopEntry& entry
);

}
