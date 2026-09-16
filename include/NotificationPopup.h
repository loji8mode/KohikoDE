#pragma once

#include "Font.h"
#include "Types.h"
#include "UiTheme.h"

#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>

#include <chrono>
#include <string>

namespace Kohiko
{

// A single native, transient "toast" card - Kohiko's own equivalent of
// a freedesktop desktop-notification popup (see NotificationCenter.h
// for the "why native" reasoning and the multi-popup stacking/expiry
// policy built on top of this one class).
//
// Deliberately depends on nothing but plain Xlib plus the same
// Font/UiTheme machinery kohiko-audio/network/bluetooth's own shared
// toolkit already uses (see UiPopupMenu.h, built on the exact same
// Display*/screen/Visual*/Colormap/Font&/UiTheme& parameter shape this
// follows) - no XConnection, no WindowManager. That is what makes
// this genuinely reusable rather than WindowManager-only: the same
// class can be created either by WindowManager itself (an internal
// alert, posted through NotificationCenter - see WindowManager::
// ShowPopupNotification()) or by a standalone process like
// kohiko-audio-tray (an actual device connect/disconnect toast, see
// that tool's own PostDeviceToast()), which has neither of those and
// only ever talks to X11 directly.
//
// Deliberately override-redirect (see Create()) - the *only* thing
// that actually has to be true for "never tiled, never in the
// taskbar, never focused": an override-redirect window bypasses a
// window manager's SubstructureRedirect entirely at the X protocol
// level, so it never even generates the MapRequest a window manager
// would otherwise have to *choose* not to act on - the same guarantee
// Bar/Launcher/Notepad/PowerMenu/LockScreen already rely on for
// exactly the same reason (see WindowManager::Manage()'s own comment
// on why checking their window IDs there too is still "explicit
// insurance", not what actually keeps them out of the tiling layout).
// _NET_WM_WINDOW_TYPE_NOTIFICATION is set too, purely for spec-
// correctness/any pager or other tool that inspects window types -
// not because Kohiko's own WindowManager needs it to leave this
// window alone (it already does, per the above); see
// XConnection::IsNotificationWindowType() for the belt-and-suspenders
// case that check covers instead: some *other*, non-Kohiko
// notification window that isn't override-redirect.
class NotificationPopup
{
public:

    NotificationPopup();
    ~NotificationPopup();

    NotificationPopup(const NotificationPopup&) = delete;
    NotificationPopup& operator=(const NotificationPopup&) = delete;

    // Creates the (still unmapped, minimally-sized) window this popup
    // owns. `font` must already be loaded - NotificationCenter loads
    // one font and shares it across every popup it owns, rather than
    // each one loading its own (same reasoning as Bar's single
    // m_font). Safe to call only once per instance; construct a fresh
    // NotificationPopup for each new toast instead of trying to reuse
    // one (see NotificationCenter::Post(), which does exactly that -
    // a handful of short-lived windows created and destroyed as
    // toasts come and go is exactly the "lightweight... no
    // unnecessary... redraw loops" the spec asks for, not a
    // persistent pool that would need its own visibility/reuse state
    // machine for what is, in practice, a rare event).
    bool Create(
        Display* display,
        int screen,
        ::Window root,
        Font& font,
        const UiTheme& theme
    );

    // Sizes (auto, to `text` - see NotificationLayout::ComputeWidth/
    // ComputeHeight), positions at `topLeft` (already fully computed -
    // see NotificationLayout::ComputeRect for the bottom-right/
    // stacking math this deliberately doesn't do itself, so this
    // class never needs to know which monitor it's on or how many
    // siblings it has), shows `text`, and maps + raises the window -
    // so a freshly-posted toast always starts above whatever's
    // currently on screen, without ever calling anything that would
    // request input focus (see the class comment's "never focused").
    //
    // `expiresAt` is purely bookkeeping - NotificationCenter reads it
    // back via ExpiresAt() to decide when to destroy this popup; Show()
    // itself never starts a timer of its own (see the class comment's
    // "no XConnection/WindowManager", and NotificationCenter.h's own
    // comment, for why: whatever process owns this popup already runs
    // its own event loop/timer, and this class stays a passive, poll-
    // driven shape instead of quietly spinning up a second one).
    void Show(
        const std::string& text,
        const Point& topLeft,
        std::chrono::steady_clock::time_point expiresAt
    );

    // Repositions an already-shown popup without touching its text,
    // size, or expiry - used by NotificationCenter::RestackMonitor()
    // when an older sibling on the same monitor expires and everything
    // still active needs to slide down to close the gap it leaves
    // behind, per the spec's "place them without overlap". A no-op if
    // Show() was never called.
    void MoveTo(
        const Point& topLeft
    );

    // Unmaps (does not destroy - the destructor does that, once,
    // exactly like every other X11 window this codebase owns) this
    // popup's window. Safe to call on an already-hidden or never-shown
    // popup.
    void Hide();

    // Repaints exactly what Show() last set - used both by Show()
    // itself and by HandleExpose() (an Expose event means some region
    // of the window was just uncovered and X11 makes no guarantee
    // what's left there, the same reasoning Bar/PowerMenu's own
    // HandleExpose() already documents).
    void Redraw();

    void HandleExpose();

    ::Window WindowId() const;

    int Width() const;
    int Height() const;

    std::chrono::steady_clock::time_point ExpiresAt() const;

private:

    Display* m_display = nullptr;
    int m_screen = 0;

    ::Window m_window = 0;
    GC m_gc = nullptr;
    XftDraw* m_xftDraw = nullptr;

    // Not owned - shared across every popup a single NotificationCenter
    // creates (see Create()'s own comment).
    Font* m_font = nullptr;

    UiTheme m_theme;

    std::string m_text;
    Rect m_geometry;
    std::chrono::steady_clock::time_point m_expiresAt{};
};

}
