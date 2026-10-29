// AutostartPlan: which startup programs actually get launched when
// workspace<N>=, auto_start_programs and XDG autostart overlap.
//
// Regression for the real conflict: the same program named by two
// mechanisms was launched by each of them independently (three
// unrelated loops in RunAutostart()), i.e. started twice. And the
// opposite guard, equally important: listing a program more than once
// WITHIN one mechanism is an explicit request for several instances and
// must NOT be collapsed (there is deliberately no global
// "deduplicate by executable name").

#include "AutostartPlan.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace Kohiko;

static int g_pass = 0;
static int g_fail = 0;

static void Check(bool condition, const char* message)
{
    if (condition) { ++g_pass; std::printf("  PASS: %s\n", message); }
    else           { ++g_fail; std::printf("  FAIL: %s\n", message); }
}

static int CountLaunches(const AutostartPlan& plan, const std::string& program)
{
    int n = 0;
    for (const AutostartItem& item : plan.launch)
        if (ProgramIdentity(item.command) == ProgramIdentity(program))
            ++n;
    return n;
}

static const AutostartItem* FindLaunch(const AutostartPlan& plan, const std::string& program)
{
    for (const AutostartItem& item : plan.launch)
        if (ProgramIdentity(item.command) == ProgramIdentity(program))
            return &item;
    return nullptr;
}

using Source = AutostartItem::Source;

int main()
{
    std::printf("ProgramIdentity():\n");
    Check(ProgramIdentity("firefox") == "firefox", "plain name");
    Check(ProgramIdentity("/usr/bin/Firefox") == "firefox", "path and case are ignored");
    Check(ProgramIdentity("telegram-desktop -- %u") == "telegram-desktop", "arguments are ignored");
    Check(ProgramIdentity("env FOO=1 BAR=2 /opt/app/bin/MyApp --flag") == "myapp", "leading env and VAR=value words are skipped");
    Check(ProgramIdentity("FOO=1 myapp") == "myapp", "bare VAR=value prefix is skipped");
    Check(ProgramIdentity("") == "", "empty command has no identity");
    Check(ProgramIdentity("   ") == "", "blank command has no identity");

    std::printf("\nNo overlap: everything launches exactly as before, in the original order:\n");
    {
        AutostartInputs in;
        in.programs = { "flameshot", "nm-applet" };
        in.xdgEntries = { { "kohiko-audio-tray", "kohiko-audio-tray" } };
        in.workspacePrograms = { { 1, "zen-browser" }, { 2, "discord" }, { 2, "telegram-desktop" } };

        AutostartPlan plan = PlanAutostart(in);
        Check(plan.skipped.empty(), "nothing is skipped");
        Check(plan.launch.size() == 6, "all six programs launch");
        Check(plan.launch[0].command == "flameshot" && plan.launch[1].command == "nm-applet" &&
              plan.launch[2].command == "kohiko-audio-tray" &&
              plan.launch[3].command == "zen-browser" && plan.launch[5].command == "telegram-desktop",
              "order: auto_start_programs, then XDG, then workspace lines (as it always was)");
        Check(plan.launch[3].source == Source::WorkspaceLine && plan.launch[3].workspace == 1 &&
              plan.launch[5].workspace == 2, "workspace lines keep their target workspace");
    }

    std::printf("\nThe known conflict: workspace<N>= AND auto_start_programs name the same program:\n");
    {
        AutostartInputs in;
        in.programs = { "firefox", "flameshot" };
        in.workspacePrograms = { { 3, "firefox" } };

        AutostartPlan plan = PlanAutostart(in);
        Check(CountLaunches(plan, "firefox") == 1, "firefox is launched ONCE, not twice");
        const AutostartItem* ff = FindLaunch(plan, "firefox");
        Check(ff && ff->source == Source::WorkspaceLine && ff->workspace == 3,
              "...by the workspace line (the more explicit mechanism: it also says where the window goes)");
        Check(CountLaunches(plan, "flameshot") == 1, "an unrelated program is unaffected");
        Check(plan.skipped.size() == 1 && plan.skipped[0].item.source == Source::ProgramList &&
              plan.skipped[0].reason.find("workspace") != std::string::npos,
              "the dropped duplicate is reported with the reason (it is logged)");
    }

    std::printf("\nThe known conflict: an app's own XDG autostart entry AND auto_start_programs:\n");
    {
        AutostartInputs in;
        in.programs = { "flameshot" };
        in.xdgEntries = { { "org.flameshot.Flameshot", "flameshot" }, { "other", "otherapp" } };

        AutostartPlan plan = PlanAutostart(in);
        Check(CountLaunches(plan, "flameshot") == 1, "flameshot is launched once");
        Check(FindLaunch(plan, "flameshot")->source == Source::ProgramList,
              "...by auto_start_programs (what the user wrote beats what a package installed)");
        Check(CountLaunches(plan, "otherapp") == 1, "an XDG entry nothing else mentions still launches");
    }

    std::printf("\nXDG entry vs a workspace line:\n");
    {
        AutostartInputs in;
        in.xdgEntries = { { "org.telegram.desktop", "/usr/bin/telegram-desktop -autostart" } };
        in.workspacePrograms = { { 2, "telegram-desktop" } };

        AutostartPlan plan = PlanAutostart(in);
        Check(CountLaunches(plan, "telegram-desktop") == 1, "launched once even though the Exec line has a path and arguments");
        Check(FindLaunch(plan, "telegram-desktop")->source == Source::WorkspaceLine, "...by the workspace line");
    }

    std::printf("\nAll three name it:\n");
    {
        AutostartInputs in;
        in.programs = { "app" };
        in.xdgEntries = { { "app", "app" } };
        in.workspacePrograms = { { 4, "app" } };
        AutostartPlan plan = PlanAutostart(in);
        Check(CountLaunches(plan, "app") == 1 && FindLaunch(plan, "app")->workspace == 4 && plan.skipped.size() == 2,
              "launched once, by the workspace line; the other two are skipped and reported");
    }

    std::printf("\nIntentional multiple instances are preserved (no global dedup):\n");
    {
        AutostartInputs in;
        in.programs = { "xterm", "xterm", "xterm" };
        AutostartPlan plan = PlanAutostart(in);
        Check(CountLaunches(plan, "xterm") == 3 && plan.skipped.empty(),
              "the same program listed 3x in auto_start_programs launches 3 times");

        AutostartInputs ws;
        ws.workspacePrograms = { { 1, "kitty" }, { 2, "kitty" }, { 2, "kitty" } };
        plan = PlanAutostart(ws);
        Check(CountLaunches(plan, "kitty") == 3, "the same program on several workspaces (and twice on one) launches each time");
        Check(plan.launch[0].workspace == 1 && plan.launch[1].workspace == 2 && plan.launch[2].workspace == 2,
              "...each with its own workspace");

        AutostartInputs xdg;
        xdg.xdgEntries = { { "a", "myapp" }, { "b", "myapp" } };
        plan = PlanAutostart(xdg);
        Check(CountLaunches(plan, "myapp") == 2, "two XDG entries with the same Exec both launch (they're different entries)");
    }

    std::printf("\nOnly the SAME program is dropped:\n");
    {
        AutostartInputs in;
        in.programs = { "firefox-esr" };
        in.workspacePrograms = { { 1, "firefox" } };
        AutostartPlan plan = PlanAutostart(in);
        Check(CountLaunches(plan, "firefox") == 1 && CountLaunches(plan, "firefox-esr") == 1 && plan.skipped.empty(),
              "firefox and firefox-esr are different programs - both launch (match is on the whole name, never a prefix)");
    }

    std::printf("\nEmpty / degenerate input:\n");
    {
        AutostartPlan plan = PlanAutostart(AutostartInputs{});
        Check(plan.launch.empty() && plan.skipped.empty(), "nothing configured, nothing launched");

        AutostartInputs in;
        in.programs = { "" };
        in.workspacePrograms = { { 1, "" } };
        plan = PlanAutostart(in);
        Check(plan.skipped.empty(), "empty commands (no identity) are never treated as duplicates of each other");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
