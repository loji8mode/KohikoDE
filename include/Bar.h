#pragma once

#include "Font.h"
#include "Types.h"

#include <X11/Xlib.h>

#include <chrono>
#include <string>

namespace Kohiko
{

class XConnection;
class Config;
class SystemTray;

// A minimal always-on-top status bar: workspace list, active window
// title, a scratchpad indicator, and a clock. Drawn with plain Xlib
// shapes and Xft text (see Font.h) - no GTK/Qt, no fixed layout
// toolkit - matching the spec's "own bar, no toolkit" goal while still
// rendering every script a system font actually has installed for,
// not just the handful of charsets classic X11 core fonts cover.
class Bar
{
public:

    explicit Bar(
        XConnection& connection
    );

    ~Bar();

    void Configure(
        const Config& config,
        const Rect& monitorGeometry
    );

    void Show();

    void Hide();

    bool IsVisible() const;

    ::Window WindowId() const;

    void SetWorkspaces(
        int count,
        int current
    );

    // Records the focused window's title - kept (WindowManager still
    // calls this on every focus/title change, via UpdateAllBars())
    // purely so that plumbing doesn't need touching too, but Redraw()
    // no longer draws m_title anywhere: showing it in the bar read as
    // unwanted "what's under the mouse" info rather than the deliberate
    // status-bar feature it actually was, so the on-screen display was
    // removed. A harmless no-op call as far as anything visible goes -
    // easy to wire back up to Redraw() again (see m_title's own
    // comment) if that's ever wanted back.
    void SetTitle(
        const std::string& title
    );

    // Replaces the title area with `text` for `durationMs` (default a
    // few seconds), then reverts to showing the real title again on
    // its own - Redraw() checks the expiry itself, so nothing needs to
    // explicitly clear it. Used for things the user needs to notice
    // right now but that aren't worth a persistent indicator (a
    // rejected workspace-switch request - see WindowManager::
    // SwitchWorkspaceOnMonitor()). Calling this again before the
    // previous one expired just replaces it/resets the timer.
    void ShowNotification(
        const std::string& text,
        int durationMs = 2500
    );

    void SetScratchpadActive(
        bool active
    );

    // Lights up a "[N]" indicator - the doc asks for a small icon
    // ("for example, 📝") to show whether the Notepad exists/is open,
    // but this uses the same bracketed-letter style as the scratchpad
    // indicator instead, since not every font covers emoji glyphs
    // either and a missing one would just draw a tofu box.
    void SetNotepadActive(
        bool active
    );

    // Wires up the system tray (see SystemTray.h) so Redraw() can
    // reposition its container flush against the right edge and leave
    // room for it when placing the clock. Kohiko has exactly one tray
    // for the lifetime of the process, so this is meant to be called
    // once, right after both Bar and SystemTray are constructed.
    void AttachSystemTray(
        SystemTray* tray
    );

    // Where the "[Power]" button was last drawn, in this bar's own
    // window coordinates - WindowManager hit-tests a ButtonPress
    // against this to decide whether to open PowerMenu. Empty (all
    // zeros) before the first Redraw(), which Rect::Contains() treats
    // correctly as "nothing there yet" since a real click position is
    // never negative.
    const Rect& PowerButtonRect() const;

    // This bar's own on-screen position/size - WindowManager needs it
    // to translate PowerButtonRect() (bar-window-relative) into the
    // root-relative coordinates PowerMenu::Open() expects.
    const Rect& Geometry() const;

    // Redraws everything, including the clock, into the off-screen
    // backing Pixmap (see m_backing) and composites it onto the real
    // window in one XCopyArea - cheap enough to call on every relevant
    // WindowManager event plus once a second, and unlike drawing
    // straight onto the window (this bar's original approach), the
    // window itself never shows a briefly-blank intermediate frame.
    // That "once a second" call is what keeps the clock live even when
    // nothing else about the bar has changed - which is by far the
    // most common reason this gets called at all, so a call where
    // every other part of the bar's state is provably identical to
    // what's already on screen skips repainting (and X-flushing) the
    // rest of the bar entirely and only touches the clock's own
    // rectangle. See m_hasDrawnOnce's own comment for what "provably
    // identical" is checked against and why it's safe.
    void Redraw();

    int Height() const;

private:

    void DrawText(
        int x,
        int baseline,
        const std::string& text,
        unsigned long color
    );

    // (Re)creates m_backing/m_xftDraw sized to `width` x `height` -
    // called once from Configure() when the window is first created,
    // and again any time its width actually changes afterward (bar
    // height is fixed from config, but width follows monitor
    // geometry, which a hotplug/resolution change can and does alter).
    // Same "destroy the old pixmap/XftDraw, create fresh ones sized to
    // match" shape as UiWindow::ResizeBacking(), which this mirrors.
    void ResizeBacking(
        int width,
        int height
    );

private:

    XConnection& m_connection;

    ::Window m_window = 0;
    GC m_gc = nullptr;
    Font m_font;
    XftDraw* m_xftDraw = nullptr;

    // Off-screen buffer everything in Redraw() actually draws onto -
    // see Redraw()'s own comment for why (eliminates the
    // clear-then-redraw flicker a plain-Xlib bar would otherwise show
    // on every single tick of its own clock).
    Pixmap m_backing = 0;

    // Width m_backing was last sized to - compared against
    // m_geometry.width in Configure() to decide whether the backing
    // pixmap needs recreating (see ResizeBacking()'s comment). Bar
    // height never changes at runtime, so unlike UiWindow this only
    // needs to track width.
    int m_backingWidth = 0;

    Rect m_geometry;
    int m_height = 26;

    bool m_visible = false;

    int m_workspaceCount = 10;
    int m_currentWorkspace = 1;
    std::string m_title;
    bool m_scratchpadActive = false;
    bool m_notepadActive = false;

    // See ShowNotification() - empty/default-constructed time_point
    // when there's nothing to show; Redraw() clears m_notificationText
    // itself the first time it notices `now >= m_notificationExpiry`.
    std::string m_notificationText;
    std::chrono::steady_clock::time_point m_notificationExpiry;

    SystemTray* m_tray = nullptr;

    Rect m_powerButtonRect;

    unsigned long m_backgroundPixel = 0;
    unsigned long m_foregroundPixel = 0;
    unsigned long m_activePixel = 0;

    // Everything Redraw() actually painted onto m_backing/m_window the
    // last time it did a full repaint - compared against the current
    // state on every call so a tick where nothing but the clock's
    // digits changed (the overwhelming majority of ticks, while idle)
    // can repaint just the clock's own rectangle instead of the whole
    // bar. False/default until the first full redraw, and deliberately
    // reset to false any time something could make the window's actual
    // on-screen pixels stop matching this snapshot without going
    // through a full redraw first - m_geometry changing size
    // (Configure()) or the window having been unmapped and remapped
    // (Show(), which - unlike m_backing, which is untouched by this -
    // X11 doesn't guarantee preserves prior window content for). See
    // Redraw()'s own comment for the full reasoning.
    bool m_hasDrawnOnce = false;
    int m_lastDrawnWorkspaceCount = 0;
    int m_lastDrawnCurrentWorkspace = 0;
    bool m_lastDrawnScratchpadActive = false;
    bool m_lastDrawnNotepadActive = false;
    std::string m_lastDrawnNotificationText;
    int m_lastDrawnTrayWidth = 0;
    std::string m_lastDrawnClockText;
    int m_lastDrawnClockWidth = 0;

};

}
