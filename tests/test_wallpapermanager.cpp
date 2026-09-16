// Regression test for WallpaperManager's parsing/resolution logic -
// no X11 needed (Monitor/Workspace are both deliberately X11-free -
// see their own header comments; ApplyToRoot()'s actual rendering
// isn't exercised here since it needs a live X connection) - plus a
// real-inotify regression suite for RefreshWatches()/Poll() (see
// "-- Idle-CPU regression --" below), which needs no X11 either since
// RefreshWatches() takes a plain path list rather than a
// MonitorManager specifically so this is possible - exercises the
// same real Linux inotify AppDirWatcher's own test does, against
// throwaway temp directories.
//
// Build & run: see the "test-wallpapermanager" target in the Makefile.

#include "WallpaperManager.h"

#include "Config.h"
#include "Monitor.h"
#include "Workspace.h"

#include <sys/select.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

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

// Waits up to `timeoutMs` for `fd` to become readable - same helper
// (and same reasoning: local-filesystem inotify events are
// effectively immediate, but this avoids flakiness under load) as
// tests/test_appdirwatcher.cpp's own WaitReadable().
bool WaitReadable(int fd, int timeoutMs)
{
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(fd, &readSet);

    timeval timeout{};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;

    return select(fd + 1, &readSet, nullptr, nullptr, &timeout) > 0;
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

    std::printf("\n-- Idle-CPU regression (0.20.5): RefreshWatches()/Poll() around inotify_rm_watch()'s own IN_IGNORED --\n");
    std::printf("   (see CHANGELOG.md's 0.20.5 entry for the full root-cause writeup)\n");
    {
        std::filesystem::path dirA = tempDir / "wallpapers-a";
        std::filesystem::path dirB = tempDir / "wallpapers-b";
        std::filesystem::create_directories(dirA);
        std::filesystem::create_directories(dirB);

        std::filesystem::path fileA = dirA / "violet-peak.png";
        std::filesystem::path fileB = dirB / "nebula.png";

        { std::ofstream(fileA) << "not a real png - content doesn't matter for watch bookkeeping"; }
        { std::ofstream(fileB) << "not a real png - content doesn't matter for watch bookkeeping"; }

        WallpaperManager wallpapers;

        std::printf("\n  -- Installing the initial watch --\n");
        wallpapers.RefreshWatches({ fileA.string() });
        Check(wallpapers.Available(), "inotify initialized by the first RefreshWatches() call");
        Check(wallpapers.Fd() >= 0, "Fd() returns a valid descriptor");
        Check(!WaitReadable(wallpapers.Fd(), 200), "installing the watch itself generates no event");
        Check(!wallpapers.Poll(), "...and Poll() confirms nothing is pending");

        std::printf("\n  -- THE ACTUAL FIX: repeatedly re-processing the *same* resolved path --\n");
        std::printf("     (exactly what ApplyToRoot() -> RefreshWatches() did on every single\n");
        std::printf("     Poll()-triggered re-render before this fix, even with zero real\n");
        std::printf("     filesystem activity - this is the idle-desktop 94%%-CPU loop itself)\n");
        {
            bool anyStrayEvent = false;

            for (int i = 0; i < 50; ++i)
            {
                // Nothing about the resolved wallpaper changed - same
                // single path, same directory - so this must be a
                // total no-op against an already-correct watch set.
                // Before the fix, RefreshWatches() unconditionally
                // tore down and rebuilt the watch every single time
                // this was called, and each teardown
                // (inotify_rm_watch()) generated a fresh, readable
                // IN_IGNORED event on this exact fd - which is
                // precisely the "unrelated/empty event" this
                // regression test is about: it carries no actual
                // wallpaper-file change, yet the pre-fix Poll() (which
                // didn't look at the event mask at all) would have
                // reported it as one anyway, and the watch descriptor
                // returned by inotify_add_watch() would keep
                // incrementing every iteration - exactly the "watch ID
                // increments every cycle" from the live strace.
                wallpapers.RefreshWatches({ fileA.string() });

                if (WaitReadable(wallpapers.Fd(), 20))
                    anyStrayEvent = true;
            }

            Check(!anyStrayEvent,
                "50 repeated RefreshWatches() calls with an unchanged path never make the fd readable "
                "- i.e. the watch is never torn down/recreated when nothing actually changed");
            Check(!wallpapers.Poll(),
                "...and Poll() confirms there is still nothing queued after all 50 calls");
        }

        std::printf("\n  -- A *real* directory-set change still legitimately swaps the watch --\n");
        {
            // This is the one case that's still SUPPOSED to touch the
            // underlying inotify watch: the resolved wallpaper moved
            // to a different directory entirely (e.g. wallpaper.default
            // edited to point elsewhere, or a workspace switched onto
            // a monitor with a different wallpaper.workspace= rule).
            wallpapers.RefreshWatches({ fileB.string() });

            Check(WaitReadable(wallpapers.Fd(), 200),
                "swapping to a genuinely different directory *does* generate one IN_IGNORED (dirA's removal) - as expected");

            std::printf("\n  -- ...but Poll() correctly does not mistake that removal for a content change --\n");
            Check(!wallpapers.Poll(),
                "Poll() reads the IN_IGNORED from the real watch swap above and correctly reports no change "
                "(IN_IGNORED carries no file-content information)");

            std::printf("\n  -- ...and the swap itself doesn't cascade into repeated re-processing --\n");
            int cascadeIterations = 0;

            // The exact shape of WindowManager::HandleWallpaperFileChanged():
            // "if Poll() says something changed, call ApplyToRoot()
            // again (which calls RefreshWatches() again)". Capped well
            // above any legitimate iteration count as a safety valve -
            // against the pre-fix code this loop does not terminate on
            // its own at all.
            while (wallpapers.Poll() && cascadeIterations <= 200)
            {
                ++cascadeIterations;
                wallpapers.RefreshWatches({ fileB.string() });
            }

            Check(cascadeIterations == 0,
                "the one real watch swap above triggers zero follow-up RefreshWatches() calls "
                "- no infinite/runaway watch-recreation loop");
        }

        std::printf("\n  -- Live-reload still works: an actual wallpaper file replace is still caught --\n");
        {
            Check(!WaitReadable(wallpapers.Fd(), 200), "quiescent again after the checks above");

            // The realistic "save" pattern this class's own comments
            // describe: write a new file, rename() it over the
            // original - replaces the inode entirely rather than
            // editing it in place.
            std::filesystem::path replacement = dirB / "nebula.png.tmp";
            { std::ofstream(replacement) << "a new wallpaper's worth of bytes"; }
            std::filesystem::rename(replacement, fileB);

            Check(WaitReadable(wallpapers.Fd(), 2000),
                "the fd becomes readable after a real wallpaper file replace");
            Check(wallpapers.Poll(),
                "...and Poll() reports it as a real change (this is live-reload actually firing)");

            // ApplyToRoot() would now re-render and call this again -
            // confirms doing so doesn't itself manufacture more events
            // (same idempotency as above, now exercised right after a
            // genuine reload rather than from a freshly-installed watch).
            wallpapers.RefreshWatches({ fileB.string() });
            Check(!WaitReadable(wallpapers.Fd(), 200),
                "...and re-running RefreshWatches() right after a real reload is still a clean no-op");
        }

        std::printf("\n  -- Editing the watched file in place (no rename) is still caught too --\n");
        {
            { std::ofstream file(fileB, std::ios::app); file << "\nmore bytes appended in place"; }

            Check(WaitReadable(wallpapers.Fd(), 2000),
                "the fd becomes readable after an in-place edit");
            Check(wallpapers.Poll(), "...and Poll() reports it as a real change");
        }
    }

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    std::filesystem::remove_all(tempDir);
    return 0;
}
