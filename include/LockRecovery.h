#pragma once

namespace Kohiko
{

// Detects a Kohiko that crashed (or was killed) while the screen was
// locked, and tells Initialize() to lock again immediately on this
// restart - closing a real, confirmed-live exposure: kohiko-session
// (see its own header) restarts a crashed Kohiko automatically, but
// the restarted process has no memory of its own of whatever state
// the previous one was in. Without this, a crash while locked - rare,
// but not impossible - resumes into a completely unlocked desktop,
// with nothing on screen to say a crash happened at all. Confirmed by
// direct reproduction: locking, then `kill -9`-ing the running Kohiko
// process to simulate a crash, left the session kohiko-session
// restarted fully exposed - workspaces, bar, everything - with no
// lock screen anywhere.
//
// The mechanism is deliberately the same shape as RecoveryMode's own
// startup-crash marker (see that class's own header comment): a
// single small marker file (Xdg::DataDir()/"session-locked", touched
// only by this class):
//
//   - NoteLocked(), called from LockScreen's own lock-state-changed
//     callback whenever it locks, for any reason (keybind, kohikoctl,
//     an idle timeout, `loginctl lock-session`, or this same class
//     re-locking after a crash) - creates the marker.
//   - NoteUnlocked(), called from that same callback whenever it
//     unlocks, removes it - covering the overwhelmingly common case
//     (locked, then unlocked normally) cleanly, leaving nothing behind
//     for the next startup to misread. Also called directly from
//     WindowManager::Shutdown(), unconditionally, so a deliberate
//     logout-while-still-locked doesn't cause the *next*, fresh login
//     to come up pre-locked for no reason - this class is purely about
//     surviving a crash, not about remembering lock state across an
//     intentional session end.
//   - WasLockedAtLastExit(), called once, early in Initialize() -
//     returns true if the marker is still there, meaning the previous
//     process ended (crashed, was killed, or the machine itself lost
//     power) without ever reaching NoteUnlocked(). Purely a read - the
//     caller is expected to actually lock in response
//     (m_lockScreen.Lock(...)), which runs NoteLocked() as a normal
//     side effect of that and recreates the marker for the new,
//     now-locked process; this function doesn't touch the file itself.
class LockRecovery
{
public:

    static bool WasLockedAtLastExit();

    static void NoteLocked();

    static void NoteUnlocked();

};

}
