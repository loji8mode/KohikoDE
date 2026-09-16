// Regression test for WallpaperManager's parsing/resolution logic -
// no X11 needed (Monitor/Workspace are both deliberately X11-free -
// see their own header comments; ApplyToRoot()'s actual rendering
// isn't exercised here since it needs a live X connection).
//
// Build & run: see the "test-wallpapermanager" target in the Makefile.

#include "WallpaperManager.h"

#include "Config.h"
#include "Monitor.h"
#include "Workspace.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace Kohiko;

namespace
{

int g_pass = 0;

void Check(bool condition, const char* what)
{
    if (condition)
    {
        ++g_pass;
        std::printf("  PASS: %s\n", what);
    }
    else
    {
        std::printf("  FAIL: %s\n", what);
        std::exit(1);
    }
}

Config LoadConfigFromText(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream file(path, std::ios::trunc);
    file << text;
    file.close();

    Config config;
    config.Load(path.string());
    return config;
}

// A Monitor showing `workspaceId` on a workspace object owned by the
// caller (kept alive via the returned unique_ptr) - Monitor only ever
// stores a raw Workspace* (MonitorManager/WorkspaceManager own the
// real objects in production - see Monitor.h's own comment), so the
// test has to keep the Workspace itself alive exactly the same way.
std::unique_ptr<Workspace> MakeMonitor(Monitor& monitor, const std::string& name, int workspaceId)
{
    monitor.SetName(name);
    auto workspace = std::make_unique<Workspace>(workspaceId);
    monitor.SetWorkspace(workspace.get());
    return workspace;
}

}

int main()
{
    std::filesystem::path tempDir =
        std::filesystem::temp_directory_path() / "kohiko-test-wallpapermanager";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directories(tempDir);

    std::filesystem::path configPath = tempDir / "kohiko.conf";

    std::printf("-- Nothing configured at all - resolves to empty (background color only) --\n");
    {
        Config config = LoadConfigFromText(configPath, "# empty\n");
        WallpaperManager wallpapers;
        wallpapers.Configure(config);

        Monitor monitor(1);
        auto ws = MakeMonitor(monitor, "eDP-1", 1);

        auto resolved = wallpapers.ResolveFor(monitor);
        Check(resolved.path.empty(), "no wallpaper.default, no rules -> empty path");
        Check(wallpapers.BackgroundColor() == 0x000000, "default background color is black");
    }

    std::printf("\n-- wallpaper.default applies when nothing more specific matches --\n");
    {
        Config config = LoadConfigFromText(configPath,
            "wallpaper.default=/wallpapers/nebula.png\n"
            "wallpaper.mode=fit\n");
        WallpaperManager wallpapers;
        wallpapers.Configure(config);

        Monitor monitor(1);
        auto ws = MakeMonitor(monitor, "eDP-1", 1);

        auto resolved = wallpapers.ResolveFor(monitor);
        Check(resolved.path == "/wallpapers/nebula.png", "resolves to wallpaper.default's path");
        Check(resolved.mode == ImageScaleMode::Fit, "resolves to wallpaper.mode's mode");
    }

    std::printf("\n-- A monitor-specific rule overrides wallpaper.default --\n");
    {
        Config config = LoadConfigFromText(configPath,
            "wallpaper.default=/wallpapers/nebula.png\n"
            "wallpaper.monitor=eDP-1,path=/wallpapers/skyline.png,mode=center\n");
        WallpaperManager wallpapers;
        wallpapers.Configure(config);

        Monitor matching(1);
        auto ws1 = MakeMonitor(matching, "eDP-1", 1);
        auto matchResolved = wallpapers.ResolveFor(matching);
        Check(matchResolved.path == "/wallpapers/skyline.png", "the matching monitor gets its own rule's path");
        Check(matchResolved.mode == ImageScaleMode::Center, "...and its own rule's mode");

        Monitor other(2);
        auto ws2 = MakeMonitor(other, "HDMI-1", 1);
        auto otherResolved = wallpapers.ResolveFor(other);
        Check(otherResolved.path == "/wallpapers/nebula.png",
              "a different monitor with no rule of its own falls back to wallpaper.default");
    }

    std::printf("\n-- A workspace-specific rule overrides a monitor-specific rule --\n");
    {
        Config config = LoadConfigFromText(configPath,
            "wallpaper.default=/wallpapers/nebula.png\n"
            "wallpaper.monitor=eDP-1,path=/wallpapers/skyline.png\n"
            "wallpaper.workspace=3,path=/wallpapers/violet-peak.png,mode=stretch\n");
        WallpaperManager wallpapers;
        wallpapers.Configure(config);

        Monitor monitor(1);
        auto wsOther = MakeMonitor(monitor, "eDP-1", 1); // workspace 1 - no workspace rule for this one
        auto resolvedWs1 = wallpapers.ResolveFor(monitor);
        Check(resolvedWs1.path == "/wallpapers/skyline.png",
              "on workspace 1 (no workspace rule), the monitor rule still applies");

        monitor.SetWorkspace(nullptr);
        Workspace ws3(3);
        monitor.SetWorkspace(&ws3);
        auto resolvedWs3 = wallpapers.ResolveFor(monitor);
        Check(resolvedWs3.path == "/wallpapers/violet-peak.png",
              "switched to workspace 3 - the workspace rule now wins over the monitor rule");
        Check(resolvedWs3.mode == ImageScaleMode::Stretch, "...with its own mode");
    }

    std::printf("\n-- Every scale mode keyword parses correctly --\n");
    {
        Config config = LoadConfigFromText(configPath,
            "wallpaper.monitor=A,path=/x.png,mode=fill\n"
            "wallpaper.monitor=B,path=/x.png,mode=fit\n"
            "wallpaper.monitor=C,path=/x.png,mode=center\n"
            "wallpaper.monitor=D,path=/x.png,mode=stretch\n");
        WallpaperManager wallpapers;
        wallpapers.Configure(config);

        Monitor a(1); auto wsa = MakeMonitor(a, "A", 1);
        Monitor b(2); auto wsb = MakeMonitor(b, "B", 1);
        Monitor c(3); auto wsc = MakeMonitor(c, "C", 1);
        Monitor d(4); auto wsd = MakeMonitor(d, "D", 1);

        Check(wallpapers.ResolveFor(a).mode == ImageScaleMode::Fill, "mode=fill parses correctly");
        Check(wallpapers.ResolveFor(b).mode == ImageScaleMode::Fit, "mode=fit parses correctly");
        Check(wallpapers.ResolveFor(c).mode == ImageScaleMode::Center, "mode=center parses correctly");
        Check(wallpapers.ResolveFor(d).mode == ImageScaleMode::Stretch, "mode=stretch parses correctly");
    }

    std::printf("\n-- A rule with no mode= falls back to wallpaper.mode --\n");
    {
        Config config = LoadConfigFromText(configPath,
            "wallpaper.mode=center\n"
            "wallpaper.monitor=eDP-1,path=/x.png\n");
        WallpaperManager wallpapers;
        wallpapers.Configure(config);

        Monitor monitor(1);
        auto ws = MakeMonitor(monitor, "eDP-1", 1);
        Check(wallpapers.ResolveFor(monitor).mode == ImageScaleMode::Center,
              "no mode= on the rule itself -> falls back to the global wallpaper.mode");
    }

    std::printf("\n-- A malformed rule (no path=) is silently skipped, not fatal --\n");
    {
        Config config = LoadConfigFromText(configPath,
            "wallpaper.default=/wallpapers/nebula.png\n"
            "wallpaper.monitor=eDP-1,mode=fill\n"); // no path= at all
        WallpaperManager wallpapers;
        wallpapers.Configure(config);

        Monitor monitor(1);
        auto ws = MakeMonitor(monitor, "eDP-1", 1);
        Check(wallpapers.ResolveFor(monitor).path == "/wallpapers/nebula.png",
              "a rule missing path= is ignored entirely - falls through to wallpaper.default");
    }

    std::printf("\n-- A non-numeric wallpaper.workspace= id is silently skipped --\n");
    {
        Config config = LoadConfigFromText(configPath,
            "wallpaper.default=/wallpapers/nebula.png\n"
            "wallpaper.workspace=notanumber,path=/x.png\n");
        WallpaperManager wallpapers;
        wallpapers.Configure(config);

        Monitor monitor(1);
        auto ws = MakeMonitor(monitor, "eDP-1", 1);
        Check(wallpapers.ResolveFor(monitor).path == "/wallpapers/nebula.png",
              "a malformed workspace id doesn't crash and doesn't match anything");
    }

    std::printf("\n-- A later rule for the same monitor overrides an earlier one --\n");
    {
        Config config = LoadConfigFromText(configPath,
            "wallpaper.monitor=eDP-1,path=/first.png\n"
            "wallpaper.monitor=eDP-1,path=/second.png\n");
        WallpaperManager wallpapers;
        wallpapers.Configure(config);

        Monitor monitor(1);
        auto ws = MakeMonitor(monitor, "eDP-1", 1);
        Check(wallpapers.ResolveFor(monitor).path == "/second.png",
              "the later line for the same output name wins");
    }

    std::printf("\n-- wallpaper.background_color parses as a hex color --\n");
    {
        Config config = LoadConfigFromText(configPath, "wallpaper.background_color=0x1e1e2e\n");
        WallpaperManager wallpapers;
        wallpapers.Configure(config);
        Check(wallpapers.BackgroundColor() == 0x1e1e2eUL, "background_color parses correctly");
    }

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    std::filesystem::remove_all(tempDir);
    return 0;
}
