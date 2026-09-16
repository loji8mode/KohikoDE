// Standalone correctness test for RecoveryMode - no X11 needed.
// Build & run: see the "test-recoverymode" target in the Makefile.

#include "RecoveryMode.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

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

}

int main()
{
    std::filesystem::path tempDir =
        std::filesystem::temp_directory_path() / "kohiko-test-recoverymode";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directories(tempDir);
    setenv("XDG_DATA_HOME", tempDir.string().c_str(), 1);

    std::printf("-- A fresh install's very first launch is never treated as a crash --\n");
    {
        bool previousFailed = RecoveryMode::NoteStartupAttempt();
        Check(!previousFailed, "no marker exists yet -> not a recovery scenario");
    }

    std::printf("\n-- A clean run (reaches MarkStartupSucceeded) leaves no trace --\n");
    {
        RecoveryMode::NoteStartupAttempt();
        RecoveryMode::MarkStartupSucceeded();

        bool previousFailed = RecoveryMode::NoteStartupAttempt();
        Check(!previousFailed, "the marker was cleared by the successful run before this one");
        RecoveryMode::MarkStartupSucceeded(); // clean up after this check too
    }

    std::printf("\n-- A crash between the two calls is detected next launch --\n");
    {
        RecoveryMode::NoteStartupAttempt();
        // Simulate a crash: MarkStartupSucceeded() is never called.

        bool previousFailed = RecoveryMode::NoteStartupAttempt();
        Check(previousFailed,
              "the marker from the crashed attempt was still there -> recovery mode");
    }

    std::printf("\n-- Recovering successfully clears the marker for good --\n");
    {
        // Continuing from the previous block: a marker is currently
        // set (written by that block's second NoteStartupAttempt()).
        RecoveryMode::MarkStartupSucceeded();

        bool previousFailed = RecoveryMode::NoteStartupAttempt();
        Check(!previousFailed, "the recovery run's own success cleared the marker");
        RecoveryMode::MarkStartupSucceeded();
    }

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    std::filesystem::remove_all(tempDir);
    return 0;
}
