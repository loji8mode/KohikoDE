#pragma once

#include "Font.h"
#include "UiTheme.h"

#include <X11/Xlib.h>

#include <chrono>
#include <functional>
#include <vector>

namespace Kohiko
{

// Docks a small icon window into Kohiko's own system tray (see
// include/SystemTray.h, which implements the *host* side of this same
// freedesktop System Tray protocol inside the bar) - the "tray widget
// framework" piece of the shared infrastructure, since all three new
// tray widgets (audio/network/bluetooth) are otherwise identical: a
// small window, an icon that needs redrawing when state changes, and
// left/right-click/scroll-wheel handling. Kept as its own class
// rather than reusing UiWindow - despite the very similar fd/timer/
// event-loop shape - because a tray icon's window lifecycle (never
// decorated, docked into and reparented by someone else's window,
// starts undocked and must retry until Kohiko's bar exists) is
// different enough from a normal top-level app window that sharing a
// base class would mean more special-casing than the two classes
// otherwise have in common.
class TrayIconClient
{
public:

    TrayIconClient();
    ~TrayIconClient();

    TrayIconClient(const TrayIconClient&) = delete;
    TrayIconClient& operator=(const TrayIconClient&) = delete;

    bool Create(int iconSize = 22);

    // Called on every redraw (after any RequestRedraw()) with the
    // window this widget owns, already sized iconSize x iconSize -
    // draw the current mute/signal/connection-state icon directly
    // with Xlib/Imlib2 calls using the given GC/Visual/Colormap.
    using DrawCallback = std::function<void(Display* display, Window window, GC gc, Visual* visual, Colormap colormap, int size)>;
    void SetDrawCallback(DrawCallback callback);

    using ClickHandler = std::function<void()>;
    void SetLeftClickHandler(ClickHandler handler);
    void SetRightClickHandler(ClickHandler handler);

    using ScrollHandler = std::function<void(int delta)>; // +1 = wheel up, -1 = wheel down
    void SetScrollHandler(ScrollHandler handler);

    using FdCallback = std::function<void()>;
    void WatchFd(int fd, FdCallback callback);

    using TimerCallback = std::function<void()>;
    void SetInterval(int intervalMs, TimerCallback callback);

    void RequestRedraw() { m_dirty = true; }

    void Run();
    void Quit() { m_running = false; }

    Display* GetDisplay() const { return m_display; }
    int GetScreen() const { return m_screen; }
    Window GetWindow() const { return m_window; }
    Visual* GetVisual() const { return m_visual; }
    Colormap GetColormap() const { return m_colormap; }
    Font& GetFont() { return m_font; }
    const UiTheme& Theme() const { return m_theme; }

private:

    bool TryDock();
    void HandleEvent(XEvent& event);
    void Redraw();

    struct FdWatch { int fd; FdCallback callback; };
    struct Timer { std::chrono::milliseconds interval; std::chrono::steady_clock::time_point next; TimerCallback callback; };

    Display* m_display = nullptr;
    int m_screen = 0;
    Window m_window = 0;
    GC m_gc = nullptr;
    Visual* m_visual = nullptr;
    Colormap m_colormap = 0;
    int m_iconSize = 22;
    bool m_docked = false;

    Font m_font;
    UiTheme m_theme;

    DrawCallback m_drawCallback;
    ClickHandler m_leftClick;
    ClickHandler m_rightClick;
    ScrollHandler m_scroll;

    std::vector<FdWatch> m_fdWatches;
    std::vector<Timer> m_timers;

    // A dock attempt is also retried on this same timer until it
    // succeeds - covers the ordinary session-startup race where a
    // tray widget's autostart entry runs before Kohiko's own bar has
    // created its tray window yet.
    std::chrono::steady_clock::time_point m_nextDockAttempt;

    bool m_running = false;
    bool m_dirty = true;
};

}
