#pragma once

#include "Font.h"
#include "Types.h"
#include "UiIconCache.h"
#include "UiTheme.h"
#include "UiWidget.h"

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include <chrono>
#include <cstdint>
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

    // Drops keyboard focus, if any (calling the focused widget's own
    // OnBlur() first). Callers about to destroy the current widget
    // tree - SetRoot() itself, or an app's own
    // ScrollView::SetContent() call when only rebuilding a page's
    // content - must call this *before* doing so: a TextField that
    // still held focus (see Widget::WantsFocus()) would otherwise
    // leave m_focusedWidget pointing at freed memory the next time a
    // KeyPress arrives. SetRoot() does this itself; content-only
    // rebuilds (AudioWindow/NetworkWindow/BluetoothWindow's
    // RebuildPage()) call this explicitly right before SetContent().
    // Grants keyboard focus to `widget` (a no-op if it doesn't accept
    // focus - see Widget::WantsFocus()), blurring whatever had it
    // before. Used the same way a click would, but from app code: see
    // NetworkWindow's search field, which rebuilds its own containing
    // page on every keystroke (to re-filter the network list) and
    // needs to hand focus straight back to the new TextField instance
    // that rebuild just created, rather than dropping focus entirely
    // the way ClearFocus() would.
    void SetFocus(Widget* widget);

    void ClearFocus();

    using FdCallback = std::function<void()>;

    // Adds `fd` to the set this window's Run() polls alongside its
    // own X11 connection; `callback` runs whenever it's readable.
    // Every backend client (DBusClient, PipeWireClient,
    // AppInstanceLock) exposes exactly the Fd()/Dispatch() shape this
    // is meant for - e.g. `window.WatchFd(bus.Fd(), [&]{ bus.Dispatch(); });`
    void WatchFd(int fd, FdCallback callback);

    using TimerCallback = std::function<void()>;

    // Called after the window's actual pixel size changes (not on
    // every ConfigureNotify - only when width/height genuinely
    // differ from before). This is what makes a tiled half-screen
    // window, a maximized window, and an ultra-wide monitor all
    // usable with the *same* app: every one of kohiko-audio/network/
    // bluetooth's RebuildChrome()/RebuildPage() methods recomputes
    // column counts, card widths, and how much inline detail to show
    // per row from the window's current size, and this is the hook
    // that tells them to do it again after a resize rather than
    // leaving whatever was laid out at startup stretched or clipped.
    using ResizeHandler = std::function<void(int width, int height)>;
    void SetResizeHandler(ResizeHandler handler) { m_resizeHandler = std::move(handler); }

    // Calls `callback` roughly every `intervalMs`, for as long as
    // this window runs - what level meters and the Bluetooth/Wi-Fi
    // "scanning..." spinner animation use, since neither has an fd of
    // its own to wait on.
    void SetInterval(int intervalMs, TimerCallback callback);

    void RequestRedraw() { m_dirty = true; }

    // True while a widget is captured for a drag, or while something
    // holds keyboard focus (see m_pressedWidget/m_focusedWidget) - a
    // widget tree rebuild triggered by an async backend change
    // notification (a device's volume changing, a new Wi-Fi network
    // appearing, ...) must be deferred while either is true, since
    // replacing the tree out from under an in-progress drag or
    // focused text entry would be just as disruptive either way (and,
    // for focus specifically, could otherwise dangle m_focusedWidget
    // outright - see ClearFocus()'s comment). See e.g. AudioWindow's
    // throttled-rebuild timer for the pattern this is meant to
    // support.
    bool IsInteracting() const { return m_pressedWidget != nullptr || m_focusedWidget != nullptr; }

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

    // The *previous* root, kept alive one extra generation rather
    // than destroyed immediately - see SetRoot()'s comment, and
    // ScrollView::SetContent()'s longer version of the same reasoning.
    std::unique_ptr<Widget> m_retiredRoot;

    Widget* m_pressedWidget = nullptr;

    // The widget currently receiving KeyPress events, if any - see
    // Widget::WantsFocus()/OnFocus()/OnBlur()/OnKeyInput(). Assigned
    // in HandleEvent()'s ButtonPress case and cleared the same way
    // (a click that hits nothing focusable blurs whatever had focus,
    // same as a normal text field losing focus on an outside click).
    Widget* m_focusedWidget = nullptr;

    std::vector<FdWatch> m_fdWatches;
    std::vector<Timer> m_timers;
    ResizeHandler m_resizeHandler;

    Atom m_wmDeleteAtom = 0;
    Atom m_wmProtocolsAtom = 0;

    int m_width = 0;
    int m_height = 0;
    bool m_dirty = true;
    bool m_running = false;
};

}
