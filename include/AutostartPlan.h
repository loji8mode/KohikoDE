#pragma once

#include <string>
#include <utility>
#include <vector>

namespace Kohiko
{

// Decides exactly which programs Kohiko launches at startup, given
// everything that can ask it to launch something. Pure (no X11, no
// processes, no files) so the rules below can be tested directly.
//
// Three independent mechanisms can name a program to start:
//
//   workspace<N>=...      launch it AND send its first windows to
//                         workspace N
//   auto_start_programs   launch it
//   XDG autostart         ~/.config/autostart/*.desktop etc. - entries
//                         that applications and the distribution
//                         install by themselves
//
// Before this existed they were three unrelated loops, so a program
// mentioned by two of them (an app that installs its own XDG autostart
// entry AND is listed in auto_start_programs; or listed under
// workspace2= AND in auto_start_programs) simply started twice.
//
// The rule: ONE launch per program ACROSS mechanisms, and the more
// explicit mechanism keeps it -
//
//     workspace<N>=  >  auto_start_programs  >  XDG autostart
//
// (a workspace line says the most - it also says where the window goes;
// an XDG entry says the least - the user never wrote it).
//
// What this deliberately does NOT do is deduplicate by executable name
// globally. Listing the same program twice WITHIN one mechanism
// (`auto_start_programs=xterm xterm`, or the same program under two
// different workspaces) is an explicit request for several instances
// and is honoured exactly as written. Only a program that a LESS
// explicit mechanism would launch again is dropped.
//
// "Same program" means the same executable name (path and case
// ignored, leading `env`/VAR=value words skipped) - see
// ProgramIdentity(). Two differently-named launchers for one
// application (`Telegram` vs `telegram-desktop`) are not recognised as
// the same program; list an application in one place.
struct AutostartItem
{
    enum class Source
    {
        WorkspaceLine,   // workspace<N>=
        ProgramList,     // auto_start_programs
        Xdg              // an XDG autostart .desktop entry
    };

    Source source = Source::ProgramList;

    // Exactly what is handed to Process::Spawn().
    std::string command;

    // Only for Source::WorkspaceLine: 1-based target workspace.
    int workspace = 0;

    // Only for Source::Xdg: the desktop file's id (its basename), for
    // log messages.
    std::string desktopId;
};

struct AutostartInputs
{
    // (workspace, program) in the order the user wrote them.
    std::vector<std::pair<int, std::string>> workspacePrograms;

    // auto_start_programs, one entry per whitespace-separated word.
    std::vector<std::string> programs;

    // (desktop id, Exec command) of every XDG entry that already
    // passed ShouldAutostart() (Hidden/OnlyShowIn/TryExec/... all
    // applied) - this does not re-judge them.
    std::vector<std::pair<std::string, std::string>> xdgEntries;
};

struct AutostartSkipped
{
    AutostartItem item;
    std::string reason; // human-readable, for the log
};

struct AutostartPlan
{
    // In launch order: auto_start_programs, then XDG entries, then
    // workspace<N> lines (the order they were always started in).
    std::vector<AutostartItem> launch;

    std::vector<AutostartSkipped> skipped;
};

// The program's identity for the cross-mechanism comparison: the
// lower-cased basename of the executable in `command`, after skipping
// a leading `env` and any VAR=value words. "" if there is nothing to
// compare (an empty command).
std::string ProgramIdentity(const std::string& command);

AutostartPlan PlanAutostart(const AutostartInputs& inputs);

}
