#pragma once

namespace Kohiko
{

// Detects a Kohiko that crashed before finishing startup last time,
// and tells Application::Run() to fall back to built-in defaults
// instead of retrying whatever configuration most likely caused it -
// "a bad configuration should never make the WM unusable".
//
// The mechanism is a single small marker file
// (Xdg::DataDir()/"startup-in-progress", touched only by this class):
//
//   - NoteStartupAttempt(), called once at the very top of
//     Application::Run() (before anything that could plausibly
//     crash, including loading the user's config at all), checks
//     whether the marker is already there from a previous run - if
//     so, that run never reached MarkStartupSucceeded(), so this
//     function returns true ("use safe defaults this time"). Either
//     way, it then (re)creates the marker for *this* attempt.
//   - MarkStartupSucceeded(), called once Kohiko actually reaches a
//     stable running state (Initialize() returned and the event loop
//     is about to start), removes the marker.
//
// If Kohiko crashes anywhere between those two calls, there's no
// cleanup code to run and none is needed: the marker is simply left
// behind, exactly recording the failure for the *next* launch's
// NoteStartupAttempt() to see. When NoteStartupAttempt() returns true,
// Application::Run() skips loading/migrating the user's config file
// entirely, so every Config getter falls back to its own hardcoded
// default (see Config.h) - no separate "safe mode" needs to exist
// anywhere else for this to work.
//
// Deliberately Logger-only for the user-visible half of this ("notify
// the user"), rather than routing through NotificationClient/D-Bus:
// this path exists specifically to keep Kohiko usable when something
// is badly wrong, so it can't itself depend on a notification daemon -
// which might be down for the very same reason, or might not have
// started yet this early - to be effective. A desktop notification
// once the bar/tray exist would be a reasonable future addition on
// top of this, not a substitute for it.
class RecoveryMode
{
public:

    // Returns true if the *previous* Kohiko process left the marker
    // set (i.e. crashed, or was killed, before calling
    // MarkStartupSucceeded()) - see this class's own comment. Always
    // (re)writes the marker for the current attempt before returning,
    // regardless of the result.
    static bool NoteStartupAttempt();

    static void MarkStartupSucceeded();

};

}
