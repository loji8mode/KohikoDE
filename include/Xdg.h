#pragma once

#include <filesystem>
#include <vector>

// XDG Base Directory Specification helpers, used by the launcher so
// none of its persistent state (application index cache, launch
// history, icon lookups) hardcodes a path under the user's home
// directory or - worse - a shared, world-readable spot like /tmp.
//
// https://specifications.freedesktop.org/basedir-spec/latest/
namespace Kohiko::Xdg
{

// $HOME, or "" if genuinely unset (callers should treat that as "no
// per-user paths are available" rather than guessing one).
std::filesystem::path Home();

// $XDG_CACHE_HOME/kohiko, falling back to ~/.cache/kohiko. Created
// (including parents) if it doesn't exist yet - callers can write
// into it immediately.
std::filesystem::path CacheDir();

// $XDG_DATA_HOME/kohiko, falling back to ~/.local/share/kohiko.
// Created if missing, same as CacheDir().
std::filesystem::path DataDir();

// $XDG_CONFIG_HOME/kohiko, falling back to ~/.config/kohiko. Kohiko's
// main config already lives directly under ~/.config/kohiko (see
// Application::Run()) - this just names that same directory so new
// code doesn't have to hardcode it a second time.
std::filesystem::path ConfigDir();

// Every directory that can hold .desktop application entries, in the
// exact priority order a launcher must respect: a file in an earlier
// directory here overrides one with the same desktop ID in a later
// one. Per the XDG spec this is $XDG_DATA_HOME/applications first,
// then applications/ under each $XDG_DATA_DIRS entry - which in
// practice is almost always exactly
// ~/.local/share/applications, /usr/local/share/applications,
// /usr/share/applications, in that order, plus whatever else a
// distro/session adds to XDG_DATA_DIRS (e.g. Flatpak/Snap paths).
// Non-existent directories are still included - callers just find
// them empty when they try to iterate.
std::vector<std::filesystem::path> ApplicationDirs();

// Every directory that can hold autostart .desktop entries, per the
// freedesktop Desktop Application Autostart Specification - NOT the
// same directories or spec as ApplicationDirs() above, despite both
// being ".desktop entries somewhere under XDG base dirs": autostart
// entries live under the *config* hierarchy ($XDG_CONFIG_HOME,
// $XDG_CONFIG_DIRS), not the *data* one ($XDG_DATA_HOME,
// $XDG_DATA_DIRS), and the subdirectory name itself differs
// ("autostart", not "applications"). Same priority-order contract as
// ApplicationDirs(): $XDG_CONFIG_HOME/autostart first (in practice
// ~/.config/autostart), then autostart/ under each $XDG_CONFIG_DIRS
// entry (in practice just /etc/xdg/autostart, since that's the
// spec's own default when $XDG_CONFIG_DIRS is unset) - a file with
// the same basename in an earlier directory here is meant to
// completely override (not merge with) one of the same name in a
// later one, letting a user's own ~/.config/autostart/foo.desktop
// disable or replace a system-wide /etc/xdg/autostart/foo.desktop.
// Non-existent directories are still included, same as
// ApplicationDirs().
std::vector<std::filesystem::path> AutostartDirs();

// AppStream metadata directories to scan for a richer name/summary/
// icon than a bare .desktop file provides - $XDG_DATA_HOME/metainfo
// then metainfo/ (and the older, still-common app-info/xmls/) under
// each $XDG_DATA_DIRS entry, same priority order as ApplicationDirs().
std::vector<std::filesystem::path> AppStreamDirs();

// Base directories to search for icon themes, in the order the Icon
// Theme Specification expects: $HOME/.icons, then $XDG_DATA_HOME/icons
// and icons/ under each $XDG_DATA_DIRS entry, then the /usr/share/
// pixmaps flat fallback last.
std::vector<std::filesystem::path> IconThemeDirs();

std::filesystem::path PixmapsDir();

}
