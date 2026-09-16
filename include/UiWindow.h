#pragma once

#include "Font.h"
#include "Types.h"
#include "UiIconCache.h"
#include "UiTheme.h"
#include "UiWidget.h"

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Kohiko
{

// The application shell every one of kohiko-audio/network/bluetooth
// is built on: owns the X11 top-level window, a double-buffered
// backing Pixmap+XftDraw (so every redraw is a single XCopyArea, no
// flicker), the shared Font/Theme/IconCache, and - the part none of
// Kohiko's existing single-purpose windows needed before now - a
// poll()-based run loop that can wait on this process's own backend
// connections (a DBusClient's Fd(), PipeWireClient's Fd(),
// AppInstanceLock's Fd()) at the same time as X11 events, plus
// lightweight repeating timers (level-meter/spinner animation).
class UiWindow
{
public:

    UiWindow();
    ~UiWindow();

    UiWindow(const UiWindow&) = delete;
    UiWindow& operator=(const UiWindow&) = delete;

    bool Create(const std::string& title, const std::string& wmClass, int width, int height);

    // Ownership of the whole widget tree - Redraw() draws `root` and
    // every descendant; input events are hit-tested against it.
    void SetRoot(std::unique_ptr<Widget> root);
    Widget* Root() const { return m_root.get(); }

    using FdCallback = std::function<void()>;

    // Adds `fd` to the set this window's Run() polls alongside its
    // own X11 connection; `callback` runs whenever it's readable.
    // Every backend client (DBusClient, PipeWireClient,
    // AppInstanceLock) exposes exactly the Fd()/Dispatch() shape this
    // is meant for - e.g. `window.WatchFd(bus.Fd(), [&]{ bus.Dispatch(); });`
    void WatchFd(int fd, FdCallback callback);

    using TimerCallback = std::function<void()>;

    // Calls `callback` roughly every `intervalMs`, for as long as
    // this window runs - what level meters and the Bluetooth/Wi-Fi
    // "scanning..." spinner animation use, since neither has an fd of
    // its own to wait on.
    void SetInterval(int intervalMs, TimerCallback callback);

    void RequestRedraw() { m_dirty = true; }

    // True while a widget is captured for a drag (see m_pressedWidget)
    // - a widget tree rebuild triggered by an async backend change
    // notification (a device's volume changing, a new Wi-Fi network
    // appearing, ...) must be deferred while this is true, since
    // replacing the tree out from under an in-progress drag would
    // leave this window's own captured pointer dangling. See e.g.
    // AudioWindow's throttled-rebuild timer for the pattern this is
    // meant to support.
    bool IsInteracting() const { return m_pressedWidget != nullptr; }

    void Run();
    void Quit() { m_running = false; }

    // Brings this window to the front and gives it input focus -
    // what a tray widget's "raise the existing app instead of
    // spawning a new one" request (see AppInstanceLock) resolves to.
    void RaiseAndFocus();

    // --- drawing helpers, used by Widget::Draw() overrides ---------

    void FillRect(const Rect& rect, std::uint32_t colorHex);
    void FillRoundedRect(const Rect& rect, std::uint32_t colorHex, int radius);
    void DrawBorder(const Rect& rect, std::uint32_t colorHex, int thickness = 1);
    void DrawText(int x, int baseline, const std::string& utf8Text, std::uint32_t colorHex);
    void DrawTextClipped(const Rect& rect, const std::string& utf8Text, std::uint32_t colorHex, Label::Align align);
    void DrawIcon(const Rect& rect, const std::string& iconName);
    void SetClip(const Rect& rect);
    void ClearClip();

    int TextWidth(const std::string& utf8Text) const { return m_font.TextWidth(utf8Text); }
    int TextHeight() const { return m_font.Height(); }
    int TextAscent() const { return m_font.Ascent(); }
    Font& GetFont() { return m_font; }

    Display* GetDisplay() const { return m_display; }
    ::Window GetWindow() const { return m_window; }
    int Width() const { return m_width; }
    int Height() const { return m_height; }
    const UiTheme& Theme() const { return m_theme; }

private:

    void Redraw();
    void HandleEvent(XEvent& event);
    void ResizeBacking(int width, int height);
    Widget* HitTest(Widget* widget, Point p, bool& hitSomething);

    struct FdWatch { int fd; FdCallback callback; };
    struct Timer { std::chrono::milliseconds interval; std::chrono::steady_clock::time_point next; TimerCallback callback; };

    Display* m_display = nullptr;
    int m_screen = 0;
    ::Window m_window = 0;
    GC m_gc = nullptr;
    Pixmap m_backing = 0;
    XftDraw* m_xftDraw = nullptr;
    Visual* m_visual = nullptr;
    Colormap m_colormap = 0;

    Font m_font;
    UiTheme m_theme;
    std::unique_ptr<UiIconCache> m_iconCache;

    std::unique_ptr<Widget> m_root;
    Widget* m_pressedWidget = nullptr;

    std::vector<FdWatch> m_fdWatches;
    std::vector<Timer> m_timers;

    Atom m_wmDeleteAtom = 0;
    Atom m_wmProtocolsAtom = 0;

    int m_width = 0;
    int m_height = 0;
    bool m_dirty = true;
    bool m_running = false;
};

}
