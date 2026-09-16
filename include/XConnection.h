#pragma once

#include "Types.h"

#include <X11/Xlib.h>

#include <string>
#include <vector>

namespace Kohiko
{

class XAtoms;

// A thin wrapper around Xlib so the rest of Kohiko almost never touches
// X11 directly: MoveResize, Map, Unmap, Focus, Raise, Lower, Destroy,
// SendEvent, GrabKey, GrabButton, plus the handful of property reads
// (title/class/role/pid/transient-for/window-type) every window
// manager ends up needing.
class XConnection
{
public:

    XConnection();

    ~XConnection();

    bool Connect();

    bool BecomeWindowManager();

    void Disconnect();

    Display* GetDisplay() const;

    ::Window Root() const;

    // The pointer's current position in absolute (root) coordinates -
    // a plain XQueryPointer() wrapper. Exists for the handful of
    // moments nothing has generated a MotionNotify/EnterNotify to
    // report the pointer's location: right after startup, right
    // after a monitor topology change, and after several other
    // layout/workspace/session transitions that can otherwise leave
    // real focus stale relative to wherever the pointer is actually
    // resting - see WindowManager::HandleMonitorTopologyChanged(),
    // WindowManager::Initialize(), and WindowManager::
    // SyncFocusToPointer() and its own callers.
    Point QueryPointer() const;

    // Moves the pointer to an absolute (root-relative) position -
    // a plain XWarpPointer() wrapper. Kohiko otherwise never moves the
    // pointer on its own (every other focus change here follows the
    // pointer, not the other way around); the one deliberate exception
    // is an explicit monitor-focus switch (WindowManager::
    // FocusMonitorCommand()) under focus_follows_mouse - without
    // actually relocating the pointer too, the very next incidental
    // bit of mouse movement on whichever monitor the pointer physically
    // never left would immediately switch focus straight back, silently
    // undoing the switch that was just explicitly requested.
    void WarpPointer(int x, int y);

    int Screen() const;

    // The actual display string this connection resolved to (e.g.
    // ":1"), regardless of whether Connect() was given an explicit
    // name or fell back to $DISPLAY. Used to pin every process Kohiko
    // spawns to *this* X11 session instead of whatever DISPLAY value
    // (or none) happens to be sitting in the environment.
    std::string DisplayName() const;

    int ConnectionFd() const;

    void Flush();

    void Sync();

    // --- window lifecycle ---------------------------------------------

    void MapWindow(::Window window);

    void UnmapWindow(::Window window);

    // Forceful close.
    void DestroyWindow(::Window window);

    // Graceful close: WM_DELETE_WINDOW if the client advertises support
    // for it via WM_PROTOCOLS, otherwise XKillClient so an
    // unresponsive/legacy client can't block Close.
    void CloseWindow(::Window window, const XAtoms& atoms);

    void MoveResizeWindow(::Window window, const Rect& rect);

    // Pure reposition, no resize - used while a Swap drag is following
    // the cursor, where the window's size must stay exactly as picked
    // up until it lands on its new tile.
    void MoveWindowTo(::Window window, int x, int y);

    void SetBorderWidth(::Window window, int width);

    void SetBorderColor(::Window window, unsigned long pixel);

    void Raise(::Window window);

    void Lower(::Window window);

    void SetInputFocus(::Window window);

    void SelectInputFor(::Window window, long mask);

    // Needed whenever we *don't* honour a ConfigureRequest as asked
    // (tiled windows): most toolkits still expect an acknowledgement.
    void SendConfigureNotify(::Window window, const Rect& rect, int borderWidth);

    // Forces Expose event(s) covering a window's whole area without
    // changing its geometry at all - the server only generates Expose
    // on its own for a *real* geometry change, so a client that got
    // its ConfigureRequest silently denied (see HandleConfigureRequest)
    // and already repainted itself at the size it *asked* for gets no
    // other prompt to redraw the rest of the window it actually still
    // has.
    void ClearArea(::Window window);

    // The one place a raw XWindowChanges/value-mask pair is
    // unavoidable: honouring a ConfigureRequest exactly as a floating
    // or not-yet-managed client asked for it.
    void ConfigureWindowRaw(::Window window, XWindowChanges changes, unsigned int valueMask);

    // --- EWMH ---------------------------------------------------------------

    // Creates the invisible check window, points root and the check
    // window's _NET_SUPPORTING_WM_CHECK at each other, and publishes
    // _NET_SUPPORTED - see XConnection.cpp for why tools like flameshot
    // care about this. Returns the check window (Kohiko doesn't need
    // to do anything else with it, but keeps it around for the
    // lifetime of the connection rather than leaking an anonymous X
    // window). Called once, from WindowManager::Initialize().
    ::Window InitializeEwmhSupport(const XAtoms& atoms, const std::string& wmName);

    // Kept in sync with WindowRepository by Manage()/Unmanage() every
    // time a window starts or stops being managed.
    void SetClientList(const XAtoms& atoms, const std::vector<::Window>& clients);

    // Kept in sync by Focus()/FocusNextAvailable() - `window` is None
    // when nothing is focused (e.g. the last client on a workspace
    // just closed).
    void SetActiveWindow(const XAtoms& atoms, ::Window window);

    // True if `window` already carries _NET_WM_STATE_FULLSCREEN in its
    // _NET_WM_STATE property. Some toolkits set this directly (via
    // XChangeProperty) before ever mapping the window, rather than
    // sending a ClientMessage - the ClientMessage form of this request
    // is only meaningful for an already-mapped window, per the EWMH
    // spec, so a pre-map fullscreen request has to be read back this
    // way instead. WindowManager::Manage() checks this once, at map
    // time; WindowManager::HandleClientMessage() handles the
    // after-map ClientMessage form.
    bool HasNetWmStateFullscreen(::Window window, const XAtoms& atoms);

    // Keeps the window's own _NET_WM_STATE property honest after
    // Kohiko changes its fullscreen state (whether that came from the
    // client's own request, `windowrule=fullscreen`, or Super+F) - a
    // handful of apps re-check this after asking, and every one of
    // them is entitled to see it reflect reality either way.
    void SetNetWmState(const XAtoms& atoms, ::Window window, bool fullscreen);

    // The size a client itself would prefer, read from WM_NORMAL_HINTS
    // (PSize/USize, falling back to PBaseSize) - what a floating
    // window should actually be sized to instead of an arbitrary
    // fraction of the screen. Returns false (leaving width/height
    // untouched) if the client never specified a preferred size at
    // all, which callers should treat as "fall back to the window's
    // current/default size instead".
    bool GetPreferredSize(::Window window, int& width, int& height);

    // The client's own declared *minimum* usable size, read from
    // WM_NORMAL_HINTS (PMinSize) - what it's telling any resizing
    // agent (interactive resize, or a tiling WM handing it a slot) is
    // the smallest it can render itself into without clipping content.
    // Returns false (leaving width/height untouched) if the client
    // never declared one.
    bool GetMinSize(::Window window, int& width, int& height);

    // --- grabs -----------------------------------------------------------

    // Global grabs on the root window (keybinds, Super+drag).
    void GrabKey(unsigned int keycode, unsigned int modifiers);

    void GrabButtonOnRoot(unsigned int button, unsigned int modifiers, long eventMask);

    void UngrabAllKeys();

    void UngrabAllButtonsOnRoot();

    // Synchronous grab on one client window (click-to-focus). The
    // caller must call AllowReplayPointer() after updating focus so
    // the click still reaches the client normally.
    void GrabButtonOnWindow(::Window window, unsigned int button, unsigned int modifiers);

    void AllowReplayPointer();

    // NumLock's modifier bit, detected once and cached. XGrabKey/
    // XGrabButton match an *exact* modifier mask, so every bind needs
    // to be grabbed once per NumLock/CapsLock combination or it will
    // silently stop firing whenever NumLock happens to be on.
    unsigned int NumLockMask();

    std::vector<unsigned int> LockVariants(unsigned int baseModifiers);

    KeyCode KeysymToKeycode(KeySym keysym);

    // --- queries -----------------------------------------------------------

    bool GetWindowAttributes(::Window window, XWindowAttributes& out);

    std::string GetWindowTitle(::Window window, const XAtoms& atoms);

    void GetWindowClass(::Window window, std::string& className, std::string& instanceName);

    std::string GetWindowRole(::Window window, const XAtoms& atoms);

    long GetWindowPid(::Window window, const XAtoms& atoms);

    bool GetTransientFor(::Window window, ::Window& owner);

    // True if _NET_WM_WINDOW_TYPE declares this window as one of the
    // types that should never enter the BSP tiling algorithm and
    // should instead always open floating: DIALOG, UTILITY, SPLASH,
    // TOOLBAR, POPUP_MENU, DROPDOWN_MENU, or MENU. A window with no
    // _NET_WM_WINDOW_TYPE at all (most plain top-level application
    // windows) - or one that only carries NORMAL - correctly returns
    // false here, exactly as EWMH says a type-less top-level window
    // with no WM_TRANSIENT_FOR should be treated as NORMAL and left to
    // the tiling algorithm; WindowManager::Manage() combines this with
    // GetTransientFor() (a window can lack any of these types and
    // still need to float purely because it's transient-for another
    // window) rather than either check alone deciding it.
    bool IsFloatingWindowType(::Window window, const XAtoms& atoms);

    // True if _NET_WM_WINDOW_TYPE declares this window a DOCK - a
    // taskbar/panel/status-bar-style window. Kohiko is its own bar
    // (see Bar.h) and has no docking/strut logic of its own, so the
    // only thing this is used for is telling Manage()'s window-
    // adoption path (see WindowManager::AdoptExistingWindows()) not
    // to swallow some *other* already-running panel into the tiling
    // layout - the same reason that path also skips override-redirect
    // windows.
    bool IsDockWindowType(::Window window, const XAtoms& atoms);

    // True if this window carries a _XEMBED_INFO property - the
    // freedesktop XEmbed spec requires any client that wants to be
    // embedded into someone else's window (rather than managed as a
    // normal top-level one) to set this *before* ever mapping that
    // window. Kohiko's own TrayIconClient (see its Create()) is the
    // only thing that currently does this, for the System Tray
    // Protocol's icon windows specifically, but this checks the
    // property itself, not any particular application - any future
    // XEmbed-based client gets the same treatment automatically.
    // WindowManager::Manage() checks this before anything else about
    // window *type* or *class*, and for good reason: unlike
    // IsDockWindowType() above, which only has to stop Manage() from
    // permanently adopting a window, this has to win a real race.
    // TrayIconClient maps its window immediately after requesting a
    // dock, without waiting for the tray to actually reparent it
    // first (see that class's own comment on TryDock()) - so by the
    // time SystemTray reparents the window away, a WindowManager that
    // didn't check this already would have fully managed it: a BSP
    // slot, a taskbar entry, focus eligibility, all before the
    // reparent it was always going to lose ever happens. This is
    // checked purely by property, not by process name or binary path,
    // so it applies the same way to any window that sets it, present
    // or future.
    bool IsXEmbedWindow(::Window window, const XAtoms& atoms);

    // Direct children of the root window, in bottom-to-top stacking
    // order (as XQueryTree returns them) - used once, at startup, by
    // WindowManager::AdoptExistingWindows() to find windows mapped by
    // a previous window manager. Nothing else needs a live view of
    // the whole window tree; every other window discovery in this
    // codebase happens window-by-window, driven off events.
    std::vector<::Window> QueryChildren(::Window window);

    std::string LastError() const;

private:

    static int ErrorHandler(
        Display*,
        XErrorEvent*
    );

private:

    Display* m_display = nullptr;

    ::Window m_root = 0;

    int m_screen = 0;

    bool m_wmExists = false;

    std::string m_lastError;

    unsigned int m_numLockMask = 0;
    bool m_numLockComputed = false;

    static XConnection* s_instance;

};

}
