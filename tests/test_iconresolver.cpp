// Standalone correctness test for IconResolver - specifically the
// "-symbolic" fallback (see IconResolver.h's own comment on Resolve()
// for why this exists): real status-icon names like
// "network-wireless-signal-excellent" are standard freedesktop icon-
// naming-spec content, but most actively-maintained themes (Adwaita
// included) only actually ship the "-symbolic" variant of them -
// confirmed against a real Xephyr/Xvfb Kohiko session where all three
// tray widgets docked successfully but rendered as blank squares
// under a real, installed Adwaita theme until this fallback was
// added (see the audio/network/Bluetooth tray section of
// docs/AUDIO_NETWORK_BLUETOOTH.md).
//
// Pure filesystem/string logic, no X11 needed - builds a real
// throwaway icon theme directory under a redirected
// XDG_DATA_HOME/HOME, same idea as test_placementhabits redirecting
// XDG_DATA_HOME and test_desktopentry redirecting XDG_CONFIG_HOME.
//
// Build & run: see the "test-iconresolver" target in the Makefile, or
// `ctest` via CMake.

#include "IconResolver.h"

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

void WriteFile(
    const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << "not a real icon, just needs to exist\n";
}

}

int main()
{
    std::filesystem::path tempDir =
        std::filesystem::temp_directory_path() / "kohiko-test-iconresolver";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directories(tempDir);

    // A minimal but real theme: an index.theme (Resolve() won't look
    // at a theme directory at all without one) with one 24x24
    // directory, holding one bare-named icon and one -symbolic-only
    // one - exactly the two situations under test. Placed under
    // $XDG_DATA_HOME/icons/TestTheme, matching Xdg::IconThemeDirs()'s
    // own search list (see Xdg.cpp) and how a real icon theme package
    // actually lays out on disk under ~/.local/share/icons/<name>.
    std::filesystem::path xdgDataHome = tempDir / "xdg-data-home";
    std::filesystem::path themeBase = xdgDataHome / "icons" / "TestTheme";

    std::filesystem::create_directories(themeBase / "24x24" / "status");
    {
        std::ofstream index(themeBase / "index.theme");
        index <<
            "[Icon Theme]\n"
            "Name=TestTheme\n"
            "Directories=24x24/status\n"
            "\n"
            "[24x24/status]\n"
            "Size=24\n"
            "Type=Fixed\n";
    }

    WriteFile(themeBase / "24x24" / "status" / "bluetooth-active.svg");
    WriteFile(themeBase / "24x24" / "status" / "network-wireless-signal-excellent-symbolic.svg");

    setenv("HOME", tempDir.string().c_str(), 1);
    setenv("XDG_DATA_HOME", xdgDataHome.string().c_str(), 1);

    std::printf("IconResolver::Resolve() tests (theme override = \"TestTheme\"):\n");

    std::printf("-- An icon that exists under its exact bare name --\n");
    {
        IconResolver resolver("TestTheme");
        std::string result = resolver.Resolve("bluetooth-active");
        Check(!result.empty(), "bluetooth-active resolves");
        Check(result.find("bluetooth-active.svg") != std::string::npos,
              "...to the exact bare-named file, not some -symbolic guess");
    }

    std::printf("\n-- An icon that only exists as its \"-symbolic\" variant --\n");
    {
        IconResolver resolver("TestTheme");
        std::string result = resolver.Resolve("network-wireless-signal-excellent");
        Check(!result.empty(),
              "network-wireless-signal-excellent resolves via the -symbolic fallback "
              "(this is the exact case that rendered as a blank tray icon before the fix)");
        Check(result.find("network-wireless-signal-excellent-symbolic.svg") != std::string::npos,
              "...to the -symbolic file specifically");
    }

    std::printf("\n-- Requesting the -symbolic name directly still works (no double-suffixing) --\n");
    {
        IconResolver resolver("TestTheme");
        std::string result = resolver.Resolve("network-wireless-signal-excellent-symbolic");
        Check(!result.empty(), "the -symbolic name itself still resolves");
        Check(result.find("-symbolic-symbolic") == std::string::npos,
              "...and was never searched as \"...-symbolic-symbolic\"");
    }

    std::printf("\n-- An icon absent under both the bare and -symbolic name --\n");
    {
        IconResolver resolver("TestTheme");
        std::string result = resolver.Resolve("totally-nonexistent-icon-name");
        Check(result.empty(), "resolves to nothing, rather than some incorrect match");
    }

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    std::filesystem::remove_all(tempDir);
    return 0;
}
