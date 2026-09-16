#pragma once

#include <functional>
#include <string>

namespace Kohiko
{

// Backs two related needs shared by every one of the three new
// control-panel apps and their tray widgets:
//
//  1. "Don't open a second kohiko-audio window if one's already
//     open" - the app itself calls AcquireOrListen() on startup and
//     simply exits if it comes back false.
//  2. "Left-clicking the tray icon should raise the existing window,
//     not spawn a new process" - the tray widget calls
//     RequestRaise() first and only falls back to launching the app
//     itself if that reports nobody's listening.
//
// Implemented as a single UNIX domain socket per app under
// $XDG_RUNTIME_DIR (falling back to /tmp if that's unset), named
// "kohiko-<app-id>.sock" - whichever process can bind it is "the"
// instance; anyone else can only connect to it. A stale socket file
// left behind by a crashed process is harmless: connecting to it
// fails immediately (nothing is listening), so the next
// AcquireOrListen() unlinks and rebinds it normally.
class AppInstanceLock
{
public:

    AppInstanceLock();
    ~AppInstanceLock();

    AppInstanceLock(const AppInstanceLock&) = delete;
    AppInstanceLock& operator=(const AppInstanceLock&) = delete;

    // Call once at startup. Returns true (and starts listening for
    // raise requests) if no other instance of `appId` currently holds
    // the lock; returns false if one already does, in which case this
    // process should just exit - it's the caller's responsibility to
    // not do any further app initialization once this returns false.
    bool AcquireOrListen(const std::string& appId);

    // The fd to add to this process's own poll() loop once
    // AcquireOrListen() has returned true - -1 otherwise.
    int Fd() const;

    // Accepts and processes any pending raise request(s), invoking
    // the handler set via SetRaiseHandler() for each one. Call
    // whenever Fd() is readable.
    void Dispatch();

    using RaiseHandler = std::function<void()>;
    void SetRaiseHandler(RaiseHandler handler);

    // Standalone helper for the tray-widget side: attempts to connect
    // to `appId`'s socket and ask the running instance (if any) to
    // raise its window. Returns true if a running instance was found
    // and notified, false if nobody's listening (the tray widget
    // should then launch the app itself instead).
    static bool RequestRaise(const std::string& appId);

    // What every tray widget's "open the full app" click handler
    // calls: RequestRaise() first, falling back to spawning
    // `executableName` as a detached child process only if nobody
    // answered. Kept here (rather than duplicated three times across
    // kohiko-audio-tray/kohiko-network-tray/kohiko-bluetooth-tray)
    // since it's the same two-step dance for all three.
    static void LaunchOrRaise(const std::string& appId, const std::string& executableName);

private:

    int m_listenFd = -1;
    std::string m_socketPath;
    RaiseHandler m_handler;
};

}
