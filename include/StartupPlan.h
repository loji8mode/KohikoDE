#pragma once

#include <string>
#include <vector>

// The single place that decides WHICH programs Kohiko launches at
// startup. Three configuration sources can name startup programs:
//
//   1. `workspace<N>=`        - launch + land on workspace N
//   2. `auto_start_programs=` - launch
//   3. XDG autostart entries  - launch (~/.config/autostart/*.desktop,
//                               /etc/xdg/autostart/*.desktop)
//
// Before this existed each source launched independently, so one
// logical program named in two places ran twice - and only the
// `workspace<N>` copy was tracked for placement, so the two instances
// then fought (one on workspace N, one on whatever was focused).
//
// ---------------------------------------------------------------
// Responsibility model
// ---------------------------------------------------------------
//
//   - Startup LAUNCHES come from exactly one decision: BuildStartupPlan().
//   - `workspace<N>=` is "launch, and place the window on workspace N".
//     It is the most specific source, so it wins.
//   - `auto_start_programs` is "launch, no placement".
//   - XDG entries are the least specific (often written by the
//     applications themselves) and yield to both of the above.
//   - Adaptive placement never launches anything and never decides
//     what to launch: it only learns from user-directed placement.
//
// ---------------------------------------------------------------
// What counts as "the same logical startup program"
// ---------------------------------------------------------------
//
// Two commands are the same program iff they have the same executable
// (path stripped: `/usr/bin/firefox` == `firefox`) AND the same
// arguments, compared exactly. Arguments are what distinguishes
// legitimate separate instances (`firefox -P work` vs
// `firefox -P home`, `chromium --user-data-dir=a` ...), so commands
// that differ in any argument are never treated as duplicates.
//
// Cross-source "claiming": every entry a higher-priority source
// declares claims ONE entry of the same program from each
// lower-priority source (that lower entry is dropped, since the
// higher one already launches that instance). Anything left over is a
// genuinely additional instance and still launches.
//
// Within a single source nothing is ever removed: `workspace1=xterm
// xterm` is two xterms on purpose, and so is `workspace1=xterm`
// together with `workspace2=xterm`.
namespace Kohiko
{

enum class StartupSource
{
    WorkspaceLine = 0, // highest priority
    AutoStartList = 1,
    XdgEntry = 2       // lowest priority
};

struct StartupEntry
{
    std::string command;
    int workspace = 0;       // 0 = no placement requested
    StartupSource source = StartupSource::AutoStartList;
};

struct StartupPlanInput
{
    // Index 0 is workspace 1.
    std::vector<std::vector<std::string>> workspacePrograms;
    std::vector<std::string> autoStartPrograms;
    std::vector<std::string> xdgCommands;
};

struct StartupPlan
{
    // In launch order: auto_start_programs, XDG, then workspace<N>
    // lines (the historical order, kept so nothing else changes).
    std::vector<StartupEntry> launches;

    // Entries dropped because a higher-priority source already
    // launches that same program. Reported so the user can see why.
    struct Skipped
    {
        StartupEntry entry;
        StartupSource claimedBy;
    };
    std::vector<Skipped> skipped;
};

// Normalized identity of one startup command (see above):
// "<executable basename> <arg1> <arg2> ...", whitespace-collapsed.
std::string StartupProgramKey(const std::string& command);

StartupPlan BuildStartupPlan(const StartupPlanInput& input);

const char* StartupSourceName(StartupSource source);

}
