// Standalone test for StartupPlan - the single decision point for which
// programs Kohiko launches at startup. Pure logic: no X11, no
// processes spawned. See StartupPlan.h for the model.
//
// The two properties that matter, and that every case below serves:
//   1. ACCIDENTAL duplicates across configuration sources are removed
//      (one logical program named in workspace<N>= AND
//      auto_start_programs= AND an XDG entry launches once, on the
//      workspace).
//   2. INTENTIONAL multiples are never removed: the same executable
//      repeated within one source, or with different arguments
//      (profiles/instances), still launches every time.
//
// Build & run: see the "test-startupplan" target in the Makefile.

#include "StartupPlan.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Kohiko;

namespace
{

int g_pass = 0;

void Check(bool condition, const std::string& what)
{
    if (condition)
    {
        ++g_pass;
        std::printf("  PASS: %s\n", what.c_str());
    }
    else
    {
        std::printf("  FAIL: %s\n", what.c_str());
        std::exit(1);
    }
}

int CountLaunches(const StartupPlan& plan, const std::string& command)
{
    int n = 0;
    for (const StartupEntry& e : plan.launches)
        n += (e.command == command);
    return n;
}

// Workspace the (single) launch of `command` is placed on; -1 if it
// isn't launched exactly once.
int WorkspaceOf(const StartupPlan& plan, const std::string& command)
{
    int found = -1;
    int count = 0;
    for (const StartupEntry& e : plan.launches)
        if (e.command == command) { found = e.workspace; ++count; }
    return count == 1 ? found : -1;
}

}

int main()
{
    std::printf("-- Key: identity is executable basename + exact arguments --\n");
    {
        Check(StartupProgramKey("firefox") == "firefox", "a bare command is its own key");
        Check(StartupProgramKey("/usr/bin/firefox") == "firefox", "an absolute path is reduced to the executable");
        Check(StartupProgramKey("  firefox   -P   work ") == "firefox -P work", "whitespace is collapsed");
        Check(StartupProgramKey("firefox -P work") != StartupProgramKey("firefox -P home"), "different profile -> different program");
        Check(StartupProgramKey("Telegram") != StartupProgramKey("telegram"), "case matters (they are different executables)");
        Check(StartupProgramKey("   ").empty(), "a blank command has no key");
    }

    std::printf("-- ACCIDENTAL duplicate: the real migration-default scenario --\n");
    {
        // ConfigMigration appended the schema default to an existing config
        // that already had workspace lines:
        //   auto_start_programs=Telegram discord zen-browser flameshot
        //   workspace1=zen-browser   workspace2=discord Telegram   workspace3=steam
        StartupPlanInput in;
        in.autoStartPrograms = {"Telegram", "discord", "zen-browser", "flameshot"};
        in.workspacePrograms = {{"zen-browser"}, {"discord", "Telegram"}, {"steam"}};

        StartupPlan plan = BuildStartupPlan(in);

        Check(CountLaunches(plan, "Telegram") == 1, "Telegram launches exactly once (it used to launch twice)");
        Check(CountLaunches(plan, "discord") == 1, "discord launches exactly once");
        Check(CountLaunches(plan, "zen-browser") == 1, "zen-browser launches exactly once");
        Check(WorkspaceOf(plan, "Telegram") == 2, "the surviving Telegram is the workspace-2 launch (so it gets placed)");
        Check(WorkspaceOf(plan, "discord") == 2, "the surviving discord is the workspace-2 launch");
        Check(WorkspaceOf(plan, "zen-browser") == 1, "the surviving zen-browser is the workspace-1 launch");
        Check(CountLaunches(plan, "flameshot") == 1 && WorkspaceOf(plan, "flameshot") == 0,
              "flameshot, which no workspace line mentions, still launches (unplaced)");
        Check(CountLaunches(plan, "steam") == 1 && WorkspaceOf(plan, "steam") == 3, "steam on workspace 3, untouched");
        Check(plan.launches.size() == 5, "5 launches in total");
        Check(plan.skipped.size() == 3, "the 3 duplicates were reported as skipped");

        bool allClaimedByWorkspace = true;
        for (const auto& s : plan.skipped)
            allClaimedByWorkspace = allClaimedByWorkspace && s.claimedBy == StartupSource::WorkspaceLine
                                    && s.entry.source == StartupSource::AutoStartList;
        Check(allClaimedByWorkspace, "each skipped entry was an auto_start_programs entry claimed by a workspace<N> line");
    }

    std::printf("-- ACCIDENTAL duplicate: XDG entry vs config list --\n");
    {
        StartupPlanInput in;
        in.workspacePrograms = {{}, {"discord"}};
        in.xdgCommands = {"discord", "nm-applet"};
        StartupPlan plan = BuildStartupPlan(in);

        Check(CountLaunches(plan, "discord") == 1 && WorkspaceOf(plan, "discord") == 2,
              "discord launches once, on workspace 2 - the XDG copy yields");
        Check(CountLaunches(plan, "nm-applet") == 1, "an XDG-only entry still launches");

        StartupPlanInput in2;
        in2.autoStartPrograms = {"flameshot"};
        in2.xdgCommands = {"/usr/bin/flameshot"};
        StartupPlan plan2 = BuildStartupPlan(in2);
        Check(plan2.launches.size() == 1 && plan2.launches[0].source == StartupSource::AutoStartList,
              "an XDG entry with a full path is the same program as the bare name in auto_start_programs");
    }

    std::printf("-- ACCIDENTAL duplicate: the same program in all three sources --\n");
    {
        StartupPlanInput in;
        in.workspacePrograms = {{"app"}};
        in.autoStartPrograms = {"app"};
        in.xdgCommands = {"app"};
        StartupPlan plan = BuildStartupPlan(in);
        Check(plan.launches.size() == 1 && plan.launches[0].source == StartupSource::WorkspaceLine && plan.launches[0].workspace == 1,
              "all three name one program -> launched once, by the workspace line");
        Check(plan.skipped.size() == 2, "and the other two are reported as skipped");
    }

    std::printf("-- INTENTIONAL: repeats within ONE source are never removed --\n");
    {
        StartupPlanInput a;
        a.workspacePrograms = {{"xterm", "xterm"}};
        Check(CountLaunches(BuildStartupPlan(a), "xterm") == 2, "workspace1=xterm xterm -> two xterms");

        StartupPlanInput b;
        b.autoStartPrograms = {"xterm", "xterm", "xterm"};
        Check(CountLaunches(BuildStartupPlan(b), "xterm") == 3, "auto_start_programs=xterm xterm xterm -> three xterms");

        StartupPlanInput c;
        c.workspacePrograms = {{"xterm"}, {"xterm"}};
        StartupPlan planC = BuildStartupPlan(c);
        Check(CountLaunches(planC, "xterm") == 2, "workspace1=xterm + workspace2=xterm -> two xterms (one per workspace)");
        Check(planC.launches[0].workspace == 1 && planC.launches[1].workspace == 2, "each on its own workspace");
        Check(planC.skipped.empty(), "nothing skipped");

        StartupPlanInput d;
        d.xdgCommands = {"foot", "foot"};
        Check(CountLaunches(BuildStartupPlan(d), "foot") == 2, "two XDG entries with the same command both launch");
    }

    std::printf("-- INTENTIONAL: different arguments/profiles are different instances --\n");
    {
        StartupPlanInput in;
        in.workspacePrograms = {{"firefox"}};
        in.xdgCommands = {"firefox -P work", "firefox -P home", "firefox"};
        StartupPlan plan = BuildStartupPlan(in);

        Check(CountLaunches(plan, "firefox -P work") == 1, "profile 'work' still launches next to a plain firefox");
        Check(CountLaunches(plan, "firefox -P home") == 1, "profile 'home' still launches");
        Check(CountLaunches(plan, "firefox") == 1 && WorkspaceOf(plan, "firefox") == 1,
              "the argument-less XDG firefox was claimed by workspace1=firefox, leaving one plain launch");
        Check(plan.launches.size() == 3 && plan.skipped.size() == 1, "3 launches, 1 skipped");
    }

    std::printf("-- INTENTIONAL: extra instances beyond what a higher source declares still launch --\n");
    {
        // auto_start_programs wants 2 xterms; workspace1 places one of them.
        StartupPlanInput in;
        in.autoStartPrograms = {"xterm", "xterm"};
        in.workspacePrograms = {{"xterm"}};
        StartupPlan plan = BuildStartupPlan(in);

        Check(CountLaunches(plan, "xterm") == 2, "2 xterms total: the workspace line claims ONE of the two, not both");
        int onWorkspace = 0;
        for (const auto& e : plan.launches)
            onWorkspace += (e.workspace == 1);
        Check(onWorkspace == 1, "exactly one of them is the placed (workspace 1) instance");
    }

    std::printf("-- Determinism and ordering --\n");
    {
        StartupPlanInput in;
        in.autoStartPrograms = {"a", "b"};
        in.xdgCommands = {"c"};
        in.workspacePrograms = {{"d"}, {"e"}};

        StartupPlan first = BuildStartupPlan(in);
        StartupPlan second = BuildStartupPlan(in);

        std::string order;
        for (const auto& e : first.launches) order += e.command;
        Check(order == "abcde", "launch order is auto_start, XDG, then workspace lines (historical order kept)");

        bool same = first.launches.size() == second.launches.size();
        for (std::size_t i = 0; same && i < first.launches.size(); ++i)
            same = first.launches[i].command == second.launches[i].command && first.launches[i].workspace == second.launches[i].workspace;
        Check(same, "the same input always yields the same plan");
    }

    std::printf("-- Edge cases --\n");
    {
        StartupPlan empty = BuildStartupPlan(StartupPlanInput{});
        Check(empty.launches.empty() && empty.skipped.empty(), "no configuration -> nothing to launch");

        StartupPlanInput blank;
        blank.autoStartPrograms = {"", "   "};
        Check(BuildStartupPlan(blank).launches.empty(), "blank commands are ignored");

        StartupPlanInput only;
        only.autoStartPrograms = {"flameshot"};
        StartupPlan plan = BuildStartupPlan(only);
        Check(plan.launches.size() == 1 && plan.launches[0].workspace == 0, "an auto_start-only program is launched unplaced");
    }

    std::printf("\nAll %d checks passed.\n", g_pass);
    return 0;
}
