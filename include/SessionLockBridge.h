#pragma once

#include <functional>
#include <string>

// Kept opaque here, same reasoning as ScreenSaverInhibitor.h's own
// comment - only SessionLockBridge.cpp itself needs the real libdbus
// types.
struct DBusConnection;

namespace Kohiko
{

// Integrates Kohiko's own native LockScreen with the system's session-
// lock mechanism - systemd-logind's `org.freedesktop.login1.Session`
// interface - so `loginctl lock-session`, a suspend hook, or any other
// standard tool that asks *this* session to lock reaches Kohiko's real
// lock screen, and so anything checking whether the session is locked
// (a display manager's user switcher, `loginctl session-status`) sees
// the truth. This is deliberately a one-way bridge:
//
//   logind's `Lock` signal  -->  LockScreen::Lock()   (via the
//                                 callback set through
//                                 SetLockRequestedCallback())
//   LockScreen locking/unlocking  -->  logind's SetLockedHint()  (via
//                                 NotifyLocked(), which WindowManager
//                                 calls from LockScreen's own
//                                 LockStateChangedCallback - see
//                                 LockScreen.h)
//
// Deliberately does *not* subscribe to logind's `Unlock` signal:
// honoring an external "unlock this session" request would mean
// bypassing LockScreen's own PAM-backed Authenticator entirely - a
// second, weaker authentication path this bridge exists specifically
// to avoid creating. The only way this session ever actually unlocks
// is still the same as always: the correct password, checked by PAM,
// through LockScreen::HandleKeyPress(). This class only ever *reports*
// that state to logind after the fact; it never grants it.
//
// Same graceful-degradation shape as ScreenSaverInhibitor throughout:
// Available() simply stays false - no error, nothing logged - on any
// system without a session bus, without logind running, or where this
// process isn't a logind-managed session at all (e.g. a bare `startx`
// outside any login manager). Kohiko's lock screen still works exactly
// as it always has either way; this class only ever adds *external*
// integration on top, never anything the lock screen itself depends
// on to function.
class SessionLockBridge
{
public:

    SessionLockBridge();
    ~SessionLockBridge();

    SessionLockBridge(const SessionLockBridge&) = delete;
    SessionLockBridge& operator=(const SessionLockBridge&) = delete;

    // Connects to the session bus and resolves *this process's own*
    // logind session via Manager.GetSessionByPID(getpid()) - not
    // $XDG_SESSION_ID, so this still works correctly even in the rare
    // setup where that variable isn't set. See this class's own
    // header comment for every way Available() can end up false
    // afterward; none of them are logged as errors.
    void Initialize();

    bool Available() const;

    // For EventLoop's own select() set, exactly like
    // ScreenSaverInhibitor::Fd() - -1 whenever !Available().
    int Fd() const;

    // Processes every pending D-Bus message on this connection - call
    // whenever Fd() is readable. A no-op whenever !Available().
    void Dispatch();

    // WindowManager wires this in once, to LockScreen::Lock() with
    // its own current MonitorManager - the same "set a callback once,
    // the subsystem calls it later" shape already used for
    // MonitorManager::SetSessionWorkspaceLookup() and
    // AppDirWatcher/HandleAppDirChanged().
    using LockRequestedCallback = std::function<void()>;

    void SetLockRequestedCallback(
        LockRequestedCallback callback
    );

    // Tells logind whether this session is currently locked - see
    // this class's own header comment for exactly when/why
    // WindowManager calls this. A no-op whenever !Available(); never
    // blocks waiting for a reply (nothing here depends on logind
    // actually having received it - see the .cpp).
    void NotifyLocked(
        bool locked
    );

    // Public only because libdbus's C callback API needs a plain
    // function pointer, not a member function - see
    // ScreenSaverInhibitor::HandleMessage()'s own identical comment.
    void HandleMessage(DBusConnection* connection, void* message);

private:

    bool m_available = false;
    DBusConnection* m_connection = nullptr;
    std::string m_sessionPath;

    LockRequestedCallback m_lockRequested;

};

}
