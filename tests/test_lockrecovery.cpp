// Standalone correctness test for LockRecovery - no X11 needed.
// Build & run: see the "test" target in the Makefile.
//
// Regression coverage for the 0.20.4 fix: a Kohiko that crashed (or
// was killed) while the screen was locked used to restart into a
// completely unlocked desktop, confirmed by direct reproduction
// (lock, then `kill -9` the running process, then watch
// kohiko-session's automatic restart come up fully exposed) before
// this class existed at all. See LockRecovery.h.

#include "LockRecovery.h"

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
        std::filesystem::temp_directory_path() / "kohiko-test-lockrecovery";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directories(tempDir);
    setenv("XDG_DATA_HOME", tempDir.string().c_str(), 1);

    std::printf("-- A fresh install/session that was never locked isn't a recovery scenario --\n");
    {
        Check(!LockRecovery::WasLockedAtLastExit(), "no marker exists yet -> nothing to recover");
    }

    std::printf("\n-- Locking, then unlocking normally, leaves no trace for the next startup --\n");
    {
        LockRecovery::NoteLocked();
        LockRecovery::NoteUnlocked();

        Check(!LockRecovery::WasLockedAtLastExit(),
              "the ordinary lock-then-unlock cycle cleared the marker - the overwhelmingly "
              "common case must never spuriously trigger recovery");
    }

    std::printf("\n-- THE regression guard: locked, then never unlocked (a crash), is detected "
                 "the next time the marker is checked --\n");
    {
        LockRecovery::NoteLocked();
        // Simulate a crash: NoteUnlocked() is never called - exactly
        // what `kill -9`-ing a locked Kohiko does live.

        Check(LockRecovery::WasLockedAtLastExit(),
              "the marker from the never-unlocked session is still there -> recover by "
              "locking again, instead of silently resuming exposed");
    }

    std::printf("\n-- Recovering (locking again) and then genuinely unlocking clears the "
                 "marker for good, same as any other lock/unlock cycle --\n");
    {
        // Continuing from the previous block: a marker is currently
        // set. This is what Initialize() does in response -
        // Lock() runs, which (via the lock-state-changed callback)
        // calls NoteLocked() again - harmless, the marker was already
        // there.
        LockRecovery::NoteLocked();
        LockRecovery::NoteUnlocked();

        Check(!LockRecovery::WasLockedAtLastExit(),
              "a real unlock after recovering clears the marker just like any other unlock");
    }

    std::printf("\n-- NoteUnlocked() is always safe to call even with no marker present "
                 "(WindowManager::Shutdown() calls it unconditionally, locked or not) --\n");
    {
        LockRecovery::NoteUnlocked();
        LockRecovery::NoteUnlocked();

        Check(!LockRecovery::WasLockedAtLastExit(), "still nothing there, no error either way");
    }

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    std::filesystem::remove_all(tempDir);
    return 0;
}
