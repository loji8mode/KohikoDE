// Standalone correctness test for SessionStore's on-disk format - no
// X11 display needed (Save() itself needs a live MonitorManager/
// WorkspaceManager and so isn't exercised directly here; this instead
// writes session files by hand, in exactly the format Save() produces,
// and checks that Load() parses them correctly), and no real
// XDG_DATA_HOME either: this redirects XDG_DATA_HOME to a throwaway
// temp directory before touching SessionStore, same as
// test_placementhabits.cpp does for PlacementHabitStore.
//
// Build & run: see the "test-sessionstore" target in the Makefile.

#include "SessionStore.h"

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

std::filesystem::path SessionFilePath(const std::filesystem::path& xdgDataHome)
{
    return xdgDataHome / "kohiko" / "session";
}

// Matches SessionStore.cpp's kFormatVersion exactly - see that file's
// comment for why a version header is required at all.
const char* const kFormatVersion = "KOHIKO_SESSION_V2";

void WriteRawSessionFile(
    const std::filesystem::path& xdgDataHome,
    const std::string& body)
{
    std::filesystem::create_directories(xdgDataHome / "kohiko");
    std::ofstream file(SessionFilePath(xdgDataHome), std::ios::trunc);
    file << body;
}

}

int main()
{
    // Isolate this run's persisted state from any real user data.
    std::filesystem::path tempDir =
        std::filesystem::temp_directory_path() / "kohiko-test-sessionstore";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directories(tempDir);
    setenv("XDG_DATA_HOME", tempDir.string().c_str(), 1);

    std::printf("-- No session file yet --\n");
    {
        SessionStore store;
        Check(store.Find(12345) == nullptr, "no record for any window id");
        Check(store.LastWorkspaceForMonitor("eDP-1") == 0,
              "no saved workspace for any monitor name");
    }

    std::printf("\n-- A file with no MONITORS section at all still loads window records --\n");
    {
        // This is exactly what a pre-monitor-section (but still
        // KOHIKO_SESSION_V2) Save() would have written - the format
        // this feature needed to stay compatible with.
        std::string body = std::string(kFormatVersion) + "\n" +
            "555 3 0 0 0 0 0 0 - 0 -\n";
        WriteRawSessionFile(tempDir, body);

        SessionStore store;
        const SessionWindowState* state = store.Find(555);
        Check(state != nullptr, "window record still parses with no trailing section");
        Check(state && state->workspace == 3, "...and its workspace field is correct");
        Check(store.LastWorkspaceForMonitor("eDP-1") == 0,
              "no MONITORS section -> no monitor has a saved workspace");
    }

    std::printf("\n-- MONITORS section alone (no window records) --\n");
    {
        std::string body = std::string(kFormatVersion) + "\n" +
            "MONITORS 2\n" +
            "HDMI-1 3\n" +
            "eDP-1 1\n";
        WriteRawSessionFile(tempDir, body);

        SessionStore store;
        Check(store.LastWorkspaceForMonitor("HDMI-1") == 3, "HDMI-1's saved workspace is 3");
        Check(store.LastWorkspaceForMonitor("eDP-1") == 1, "eDP-1's saved workspace is 1");
        Check(store.LastWorkspaceForMonitor("DP-2") == 0,
              "a monitor name with no record returns 0");
        Check(store.Find(1) == nullptr, "no window records were written -> none parse");
    }

    std::printf("\n-- Window records AND a MONITORS section together --\n");
    {
        std::string body = std::string(kFormatVersion) + "\n" +
            "10 1 0 0 0 0 0 0 eDP-1 0 -\n"
            "20 2 1 0 100 100 640 480 eDP-1 0 -\n" +
            "MONITORS 1\n" +
            "eDP-1 2\n";
        WriteRawSessionFile(tempDir, body);

        SessionStore store;
        const SessionWindowState* a = store.Find(10);
        const SessionWindowState* b = store.Find(20);
        Check(a != nullptr && a->workspace == 1, "first window record parses correctly");
        Check(b != nullptr && b->floating, "second window record (floating) parses correctly");
        Check(store.LastWorkspaceForMonitor("eDP-1") == 2,
              "the trailing MONITORS section still parses after real window records");
    }

    std::printf("\n-- An empty monitor name never matches anything --\n");
    {
        std::string body = std::string(kFormatVersion) + "\n" +
            "MONITORS 1\n" +
            "eDP-1 2\n";
        WriteRawSessionFile(tempDir, body);

        SessionStore store;
        Check(store.LastWorkspaceForMonitor("") == 0,
              "an empty monitor name is never looked up, even if somehow present");
    }

    std::printf("\n-- A pre-version file (no header at all) is rejected entirely --\n");
    {
        std::string body = "555 3 0 0 0 0 0 0 - 0 -\n";
        WriteRawSessionFile(tempDir, body);

        SessionStore store;
        Check(store.Find(555) == nullptr,
              "a file with no recognised version header loads nothing at all");
        Check(store.LastWorkspaceForMonitor("eDP-1") == 0,
              "...including no monitor section");
    }

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    std::filesystem::remove_all(tempDir);
    return 0;
}
