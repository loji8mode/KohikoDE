// Standalone correctness test for the two building blocks behind XDG
// autostart execution (see WindowManager::RunXdgAutostartEntries() and
// its own comment on why this exists): ShouldAutostart(), which
// decides whether a parsed .desktop entry gets launched, and
// Xdg::AutostartDirs(), which decides where to look for one in the
// first place. Both are pure filesystem/string logic - no X11, and no
// real WindowManager instance (which would need a live X connection
// to construct at all) - so this can't exercise the dedup-by-desktop-
// ID loop in RunXdgAutostartEntries() itself; that part is covered by
// an actual Xephyr session instead (see the audio/network/Bluetooth
// tray section of docs/AUDIO_NETWORK_BLUETOOTH.md).
//
// Redirects XDG_CONFIG_HOME/XDG_CONFIG_DIRS/PATH to throwaway temp
// locations throughout, same idea as test_placementhabits redirecting
// XDG_DATA_HOME: never touches a real user's actual autostart entries
// or real system PATH.
//
// Build & run: see the "test-desktopentry" target in the Makefile, or
// `ctest` via CMake.

#include "DesktopEntry.h"
#include "Xdg.h"

#include <sys/stat.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

// A DesktopEntry with every field ShouldAutostart() would accept -
// each test below starts from this and changes exactly one thing, so
// a failure clearly implicates that one field's handling.
DesktopEntry ValidEntry()
{
    DesktopEntry entry;
    entry.type = "Application";
    entry.exec = "/bin/true";
    entry.hidden = false;
    entry.noDisplay = true; // Kohiko's own tray entries all set this - see the NoDisplay check below
    entry.autostartEnabled = true;
    return entry;
}

std::filesystem::path WriteExecutable(
    const std::filesystem::path& path)
{
    std::ofstream file(path);
    file << "#!/bin/sh\nexit 0\n";
    file.close();
    ::chmod(path.c_str(), 0755);
    return path;
}

}

int main()
{
    std::filesystem::path tempDir =
        std::filesystem::temp_directory_path() / "kohiko-test-desktopentry";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directories(tempDir);

    std::printf("ShouldAutostart() tests:\n");
    std::printf("-- A minimal valid entry, exactly like Kohiko's own tray .desktop files --\n");
    {
        Check(ShouldAutostart(ValidEntry()),
              "Type=Application, Exec set, not Hidden, autostart enabled: accepted");
    }

    std::printf("\n-- NoDisplay=true does NOT disable autostart (unlike ShouldDisplay()) --\n");
    {
        DesktopEntry entry = ValidEntry();
        entry.noDisplay = true;
        Check(ShouldAutostart(entry),
              "an entry with NoDisplay=true (no menu presence) still autostarts - "
              "this is the exact case desktop/kohiko-*-tray.desktop relies on");
    }

    std::printf("\n-- Hidden=true disables autostart --\n");
    {
        DesktopEntry entry = ValidEntry();
        entry.hidden = true;
        Check(!ShouldAutostart(entry), "Hidden=true is rejected");
    }

    std::printf("\n-- X-GNOME-Autostart-enabled=false disables autostart --\n");
    {
        DesktopEntry entry = ValidEntry();
        entry.autostartEnabled = false;
        Check(!ShouldAutostart(entry), "autostartEnabled=false is rejected");
    }

    std::printf("\n-- Empty Exec= disables autostart --\n");
    {
        DesktopEntry entry = ValidEntry();
        entry.exec = "";
        Check(!ShouldAutostart(entry), "an entry with nothing to launch is rejected");
    }

    std::printf("\n-- Type=Link (or anything but Application) disables autostart --\n");
    {
        DesktopEntry entry = ValidEntry();
        entry.type = "Link";
        Check(!ShouldAutostart(entry), "Type=Link is rejected");
    }

    std::printf("\n-- TryExec= resolution --\n");
    {
        std::filesystem::path realBin = WriteExecutable(tempDir / "real-tray-helper");

        DesktopEntry absoluteOk = ValidEntry();
        absoluteOk.tryExec = realBin.string();
        Check(ShouldAutostart(absoluteOk), "TryExec= as an absolute path to a real executable is accepted");

        DesktopEntry absoluteMissing = ValidEntry();
        absoluteMissing.tryExec = (tempDir / "no-such-binary").string();
        Check(!ShouldAutostart(absoluteMissing), "TryExec= as an absolute path to a missing file is rejected");

        setenv("PATH", tempDir.string().c_str(), 1);

        DesktopEntry onPath = ValidEntry();
        onPath.tryExec = "real-tray-helper";
        Check(ShouldAutostart(onPath), "TryExec= as a bare name found on $PATH is accepted");

        DesktopEntry notOnPath = ValidEntry();
        notOnPath.tryExec = "definitely-not-a-real-binary";
        Check(!ShouldAutostart(notOnPath), "TryExec= as a bare name absent from $PATH is rejected");

        DesktopEntry noTryExec = ValidEntry();
        Check(noTryExec.tryExec.empty() && ShouldAutostart(noTryExec),
              "no TryExec= at all (the common case) is accepted unconditionally");
    }

    std::printf("\n-- OnlyShowIn=/NotShowIn= against $XDG_CURRENT_DESKTOP --\n");
    {
        unsetenv("XDG_CURRENT_DESKTOP");

        DesktopEntry onlyGnome = ValidEntry();
        onlyGnome.onlyShowIn = {"GNOME"};
        Check(ShouldAutostart(onlyGnome),
              "OnlyShowIn=GNOME with $XDG_CURRENT_DESKTOP unset is accepted "
              "(can't tell what desktop this is - see PassesShowIn()'s own comment)");

        setenv("XDG_CURRENT_DESKTOP", "Kohiko", 1);
        Check(!ShouldAutostart(onlyGnome),
              "the same OnlyShowIn=GNOME entry is rejected once XDG_CURRENT_DESKTOP=Kohiko");

        DesktopEntry onlyKohiko = ValidEntry();
        onlyKohiko.onlyShowIn = {"Kohiko"};
        Check(ShouldAutostart(onlyKohiko), "OnlyShowIn=Kohiko is accepted under XDG_CURRENT_DESKTOP=Kohiko");

        DesktopEntry notKohiko = ValidEntry();
        notKohiko.notShowIn = {"Kohiko"};
        Check(!ShouldAutostart(notKohiko), "NotShowIn=Kohiko is rejected under XDG_CURRENT_DESKTOP=Kohiko");

        unsetenv("XDG_CURRENT_DESKTOP");
    }

    std::printf("\nParseDesktopFile() tests (X-GNOME-Autostart-enabled specifically):\n");
    std::printf("-- Key absent defaults to enabled --\n");
    {
        std::filesystem::path file = tempDir / "no-key.desktop";
        std::ofstream(file) <<
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=No Key\n"
            "Exec=/bin/true\n";

        auto entry = ParseDesktopFile(file, 0);
        Check(entry.has_value(), "the file parses");
        Check(entry && entry->autostartEnabled, "autostartEnabled defaults to true when the key is absent");
    }

    std::printf("\n-- Key explicitly false is honoured --\n");
    {
        std::filesystem::path file = tempDir / "disabled.desktop";
        std::ofstream(file) <<
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Disabled\n"
            "Exec=/bin/true\n"
            "X-GNOME-Autostart-enabled=false\n";

        auto entry = ParseDesktopFile(file, 0);
        Check(entry.has_value(), "the file parses");
        Check(entry && !entry->autostartEnabled, "autostartEnabled is false when the key says so");
        Check(entry && !ShouldAutostart(*entry), "...and ShouldAutostart() rejects it end-to-end");
    }

    std::printf("\n-- A real Kohiko tray .desktop file, verbatim, is accepted end-to-end --\n");
    {
        std::filesystem::path file = tempDir / "kohiko-audio-tray.desktop";
        std::ofstream(file) <<
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Kohiko Audio Tray\n"
            "Comment=Audio tray widget for Kohiko (volume/mute indicator and quick controls)\n"
            "Exec=kohiko-audio-tray\n"
            "Icon=kohiko-audio\n"
            "NoDisplay=true\n"
            "X-GNOME-Autostart-enabled=true\n"
            "Terminal=false\n";

        auto entry = ParseDesktopFile(file, 0);
        Check(entry.has_value(), "the file parses");
        Check(entry && ShouldAutostart(*entry),
              "the exact contents of desktop/kohiko-audio-tray.desktop pass ShouldAutostart()");
    }

    std::printf("\nXdg::AutostartDirs() tests:\n");
    std::printf("-- Priority order: XDG_CONFIG_HOME first, then each XDG_CONFIG_DIRS entry --\n");
    {
        setenv("XDG_CONFIG_HOME", (tempDir / "user-config").string().c_str(), 1);
        setenv("XDG_CONFIG_DIRS", ((tempDir / "sys-a").string() + ":" + (tempDir / "sys-b").string()).c_str(), 1);

        std::vector<std::filesystem::path> dirs = Xdg::AutostartDirs();

        Check(dirs.size() == 3, "one directory per XDG_CONFIG_HOME entry plus each XDG_CONFIG_DIRS entry");
        Check(!dirs.empty() && dirs[0] == tempDir / "user-config" / "autostart",
              "the user's own autostart directory (XDG_CONFIG_HOME) is first");
        Check(dirs.size() > 1 && dirs[1] == tempDir / "sys-a" / "autostart",
              "the first XDG_CONFIG_DIRS entry is second");
        Check(dirs.size() > 2 && dirs[2] == tempDir / "sys-b" / "autostart",
              "the second XDG_CONFIG_DIRS entry is third");
    }

    std::printf("\n-- Default XDG_CONFIG_DIRS falls back to /etc/xdg per spec --\n");
    {
        unsetenv("XDG_CONFIG_DIRS");

        std::vector<std::filesystem::path> dirs = Xdg::AutostartDirs();

        bool foundEtcXdg = false;
        for (const auto& dir : dirs)
        {
            if (dir == std::filesystem::path("/etc/xdg/autostart"))
                foundEtcXdg = true;
        }

        Check(foundEtcXdg, "/etc/xdg/autostart is included when $XDG_CONFIG_DIRS is unset - "
                            "this is where `make install`/`cmake --install` actually puts "
                            "desktop/kohiko-*-tray.desktop");
    }

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    std::filesystem::remove_all(tempDir);
    return 0;
}
