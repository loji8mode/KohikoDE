#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

// Kept opaque here (not #include <dbus/dbus.h>) so this header stays
// cheap for every other translation unit that just needs to hold/pass
// a ScreenSaverInhibitor - only ScreenSaverInhibitor.cpp itself needs
// the real libdbus types.
struct DBusConnection;

namespace Kohiko
{

// Implements the two standard D-Bus interfaces browsers (Firefox,
// Chromium - any tab playing a YouTube video, say), video players
// (VLC, mpv), and presentation software already call into, on every
// major desktop, specifically to keep the screen from sleeping while
// they're actively playing media - regardless of whether their window
// happens to be fullscreen:
//
//   org.freedesktop.ScreenSaver       Inhibit(appname, reason) -> cookie
//                                      UnInhibit(cookie)
//   org.freedesktop.PowerManagement   Inhibit(appname, reason) -> cookie
//                                      UnInhibit(cookie)          (legacy
//                                      name; still what Chromium tries
//                                      first on Linux)
//
// Both have the identical signature and meaning, so both are served
// by the same Inhibit/UnInhibit handling here - see the .cpp.
//
// A full desktop environment (GNOME, KDE, ...) already runs its own
// implementation of these, so Initialize() backs off entirely
// (Available() stays false) if either bus name is already owned
// rather than fighting over it - Kohiko only becomes the provider on
// a bare X session that doesn't have one of its own already.
//
// This is the PRIMARY mechanism behind display-sleep inhibition (see
// WindowManager::CheckSleepInhibition()); the "focused window is
// fullscreen" X11-only heuristic there is strictly a fallback, for
// whatever content doesn't (or, running under an older browser/
// player version, can't) make this D-Bus call itself.
//
// Deliberately does not touch DPMS/screensaver state directly - it
// only tracks whether anything currently holds an inhibit (see
// AnyActiveInhibit()); WindowManager decides what to actually do
// about that.
class ScreenSaverInhibitor
{
public:

    ScreenSaverInhibitor();
    ~ScreenSaverInhibitor();

    ScreenSaverInhibitor(const ScreenSaverInhibitor&) = delete;
    ScreenSaverInhibitor& operator=(const ScreenSaverInhibitor&) = delete;

    // Connects to the session bus and requests both well-known names
    // described above. Safe to call even where none of this is
    // available (no session bus running - common for a bare `startx`
    // session with no systemd --user/dbus-launch, or built without
    // D-Bus support at all) - Available() simply reports false and
    // every other method becomes a harmless no-op rather than an
    // error either way.
    void Initialize();

    bool Available() const;

    // True while at least one application currently holds an
    // Inhibit() cookie it hasn't UnInhibit()'d yet, or that hasn't
    // been dropped automatically because the application holding it
    // quit/crashed - see the .cpp's NameOwnerChanged handling, which
    // exists specifically so a crashed browser can never permanently
    // inhibit sleep.
    bool AnyActiveInhibit() const;

    // The fd to add to the main event loop's own select() set (see
    // EventLoop.cpp, alongside the X11 connection and IPC socket) -
    // -1 whenever !Available(). Becomes readable whenever there's an
    // incoming D-Bus message to process.
    int Fd() const;

    // Processes every pending D-Bus message (Inhibit/UnInhibit calls,
    // Introspect, NameOwnerChanged signals) - call whenever Fd() is
    // readable. A no-op whenever !Available().
    void Dispatch();

    // Public only because libdbus's C callback API
    // (dbus_connection_add_filter()) needs a plain function pointer
    // to call into this, not a member function - Dispatch() is the
    // only genuinely public entry point; this is an implementation
    // detail of how it's wired up, not something any other caller
    // should ever need to reach for directly.
    void HandleMessage(DBusConnection* connection, void* message);

private:

    bool RequestName(const char* busName);

    bool m_available = false;
    DBusConnection* m_connection = nullptr;
    std::uint32_t m_nextCookie = 1;

    // cookie -> the D-Bus unique name (e.g. ":1.42") of whichever
    // connection called Inhibit() - lets HandleMessage() drop every
    // cookie a client was holding the moment that connection
    // disappears from the bus (NameOwnerChanged with an empty new
    // owner), without the client ever having to call UnInhibit()
    // itself.
    std::unordered_map<std::uint32_t, std::string> m_inhibitors;
};

}
