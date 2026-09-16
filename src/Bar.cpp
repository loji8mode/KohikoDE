#include "Bar.h"

#include "Config.h"
#include "SystemTray.h"
#include "XConnection.h"

#include <cstdlib>
#include <cstring>
#include <ctime>

namespace Kohiko
{

Bar::Bar(
    XConnection& connection)
    :
    m_connection(connection)
{
}

Bar::~Bar()
{
    Display* display = m_connection.GetDisplay();

    if (!display)
        return;

    if (m_xftDraw)
        XftDrawDestroy(m_xftDraw);

    if (m_backing)
        XFreePixmap(display, m_backing);

    m_font.Unload();

    if (m_gc)
        XFreeGC(display, m_gc);

    if (m_window)
        XDestroyWindow(display, m_window);
}

void Bar::ResizeBacking(
    int width,
    int height)
{
    Display* display = m_connection.GetDisplay();
    int screen = m_connection.Screen();

    if (m_xftDraw) { XftDrawDestroy(m_xftDraw); m_xftDraw = nullptr; }
    if (m_backing) { XFreePixmap(display, m_backing); m_backing = 0; }

    m_backing = XCreatePixmap(
        display, m_window,
        static_cast<unsigned int>(width > 0 ? width : 1),
        static_cast<unsigned int>(height),
        DefaultDepth(display, screen));

    m_xftDraw = XftDrawCreate(
        display,
        m_backing,
        DefaultVisual(display, screen),
        DefaultColormap(display, screen));
}

void Bar::Configure(
    const Config& config,
    const Rect& monitorGeometry)
{
    m_height = config.GetInt("general.bar_height", 26);

    m_geometry = monitorGeometry;
    m_geometry.height = m_height;

    Display* display = m_connection.GetDisplay();
    int screen = m_connection.Screen();

    if (m_window == 0)
    {
        XSetWindowAttributes attrs{};
        attrs.override_redirect = True; // this is *our* window - never redirect it back to ourselves as a MapRequest
        attrs.background_pixel = BlackPixel(display, screen);
        attrs.event_mask = ExposureMask | ButtonPressMask;

        m_window = XCreateWindow(
            display,
            m_connection.Root(),
            m_geometry.x, m_geometry.y,
            static_cast<unsigned int>(m_geometry.width > 0 ? m_geometry.width : 1),
            static_cast<unsigned int>(m_height),
            0,
            DefaultDepth(display, screen),
            InputOutput,
            DefaultVisual(display, screen),
            CWOverrideRedirect | CWBackPixel | CWEventMask,
            &attrs
        );

        m_gc = XCreateGC(display, m_window, 0, nullptr);

        m_font.Load(
            display,
            screen,
            config.GetString("general.font", "monospace:pixelsize=14"));

        ResizeBacking(m_geometry.width, m_height);
    }
    else
    {
        XMoveResizeWindow(
            display,
            m_window,
            m_geometry.x, m_geometry.y,
            static_cast<unsigned int>(m_geometry.width > 0 ? m_geometry.width : 1),
            static_cast<unsigned int>(m_height)
        );

        // Bar height comes from config and never changes at runtime,
        // but width follows monitor geometry - a resolution change or
        // hotplug can and does alter it, and the backing pixmap has to
        // stay exactly window-sized (a stale, too-small one would just
        // silently clip everything drawn past its old width).
        if (m_geometry.width != m_backingWidth)
            ResizeBacking(m_geometry.width, m_height);
    }

    m_backingWidth = m_geometry.width;

    m_backgroundPixel = std::strtoul(config.GetString("bar.background", "0x1e1e2e").c_str(), nullptr, 0);
    m_foregroundPixel = std::strtoul(config.GetString("bar.foreground", "0xcdd6f4").c_str(), nullptr, 0);
    m_activePixel     = std::strtoul(config.GetString("bar.active",     "0x89b4fa").c_str(), nullptr, 0);

    XSetWindowBackground(display, m_window, m_backgroundPixel);

    // The geometry and/or colors above may just have changed (a
    // resolution change/hotplug, or a config reload) - whatever
    // Redraw() last painted is no longer guaranteed to still be
    // correct on screen, so its clock-only fast path isn't safe to
    // take until a full repaint has happened at least once more. See
    // m_hasDrawnOnce's own comment.
    m_hasDrawnOnce = false;
}

void Bar::Show()
{
    if (m_window == 0)
        return;

    m_visible = true;

    m_connection.MapWindow(m_window);
    m_connection.Raise(m_window);

    // Unlike m_backing (an off-screen pixmap, untouched by
    // unmapping), X11 doesn't guarantee a plain window's prior pixels
    // survive an unmap/remap cycle - so the next Redraw() can't trust
    // that the window still shows what m_lastDrawn* claims it does,
    // and needs to do a real full repaint at least once before the
    // clock-only fast path is safe to take again. See m_hasDrawnOnce's
    // own comment.
    m_hasDrawnOnce = false;
}

void Bar::Hide()
{
    if (m_window == 0)
        return;

    m_visible = false;

    m_connection.UnmapWindow(m_window);
}

bool Bar::IsVisible() const
{
    return m_visible;
}

::Window Bar::WindowId() const
{
    return m_window;
}

void Bar::SetWorkspaces(
    int count,
    int current)
{
    m_workspaceCount = count;
    m_currentWorkspace = current;
}

void Bar::SetTitle(
    const std::string& title)
{
    m_title = title;
}

void Bar::ShowNotification(
    const std::string& text,
    int durationMs)
{
    m_notificationText = text;
    m_notificationExpiry = std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs);
}

void Bar::SetScratchpadActive(
    bool active)
{
    m_scratchpadActive = active;
}

void Bar::SetNotepadActive(
    bool active)
{
    m_notepadActive = active;
}

void Bar::AttachSystemTray(
    SystemTray* tray)
{
    m_tray = tray;
}

int Bar::Height() const
{
    return m_height;
}

const Rect& Bar::PowerButtonRect() const
{
    return m_powerButtonRect;
}

const Rect& Bar::Geometry() const
{
    return m_geometry;
}

void Bar::Redraw(bool forceFullRepaint)
{
    if (!m_visible || m_window == 0 || !m_backing)
        return;

    Display* display = m_connection.GetDisplay();

    if (!m_notificationText.empty() && std::chrono::steady_clock::now() >= m_notificationExpiry)
        m_notificationText.clear();

    // A plain arithmetic computation (see SystemTray::Width()), not an
    // X11 round trip - cheap enough to call here purely to compare
    // against m_lastDrawnTrayWidth, even though the full-redraw path
    // below computes it again itself (unchanged) as part of actually
    // repositioning the tray container.
    int trayWidth = 0;

    if (m_tray)
    {
        trayWidth = m_tray->Width();

        if (trayWidth > 0)
            trayWidth += 12;
    }

    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);

    char clockText[16];
    std::strftime(clockText, sizeof(clockText), "%H:%M:%S", &local);

    int clockLen = static_cast<int>(std::strlen(clockText));
    int clockWidth = m_font.TextWidth(clockText);

    if (clockWidth == 0)
        clockWidth = clockLen * 8;

    // True when every part of the bar besides the clock text itself is
    // provably identical to what's already on screen - "provably"
    // because it's a direct comparison against exactly what the last
    // full redraw actually painted (m_lastDrawn*), not an assumption.
    // trayWidth matching means the clock's own x position hasn't
    // moved either (nothing else this bar draws depends on the
    // system's wall-clock time), so the fast path below never has to
    // guess at where the clock's rectangle is - it's exactly where the
    // last full redraw put it.
    bool onlyClockCouldDiffer =
        !forceFullRepaint &&
        m_hasDrawnOnce &&
        m_workspaceCount == m_lastDrawnWorkspaceCount &&
        m_currentWorkspace == m_lastDrawnCurrentWorkspace &&
        m_scratchpadActive == m_lastDrawnScratchpadActive &&
        m_notepadActive == m_lastDrawnNotepadActive &&
        m_notificationText == m_lastDrawnNotificationText &&
        trayWidth == m_lastDrawnTrayWidth &&
        clockWidth == m_lastDrawnClockWidth;

    if (onlyClockCouldDiffer && clockText == m_lastDrawnClockText)
        return; // truly nothing changed at all - not even worth touching the X server

    if (onlyClockCouldDiffer)
    {
        int clockX = m_geometry.width - clockWidth - 12 - trayWidth;
        int baseline = m_height / 2 + 5;

        XSetForeground(display, m_gc, m_backgroundPixel);
        XFillRectangle(
            display, m_backing, m_gc,
            clockX, 0,
            static_cast<unsigned int>(clockWidth > 0 ? clockWidth : 1),
            static_cast<unsigned int>(m_height));

        DrawText(clockX, baseline, clockText, m_foregroundPixel);

        XCopyArea(
            display, m_backing, m_window, m_gc,
            clockX, 0,
            static_cast<unsigned int>(clockWidth > 0 ? clockWidth : 1),
            static_cast<unsigned int>(m_height),
            clockX, 0);

        XFlush(display);

        m_lastDrawnClockText = clockText;
        return;
    }

    // --- Full redraw - identical to how this always worked, for any
    // call where something besides just the clock's digits changed
    // (including the very first call). ---

    // Everything below draws onto m_backing (via m_xftDraw, which
    // Configure()/ResizeBacking() point at it, not m_window) - the
    // window itself is only ever touched once, by the single
    // XCopyArea at the very end. XClearWindow only works on a real
    // window, not a pixmap, so the equivalent here is an explicit
    // fill - see this function's own header comment on why this
    // (rather than clearing/drawing the window directly, this bar's
    // original approach) is what actually eliminates the flicker.
    XSetForeground(display, m_gc, m_backgroundPixel);
    XFillRectangle(
        display, m_backing, m_gc,
        0, 0,
        static_cast<unsigned int>(m_geometry.width > 0 ? m_geometry.width : 1),
        static_cast<unsigned int>(m_height));

    int baseline = m_height / 2 + 5;
    int x = 10;

    for (int i = 1; i <= m_workspaceCount; ++i)
    {
        std::string label = std::to_string(i);
        unsigned long color = (i == m_currentWorkspace) ? m_activePixel : m_foregroundPixel;

        DrawText(x, baseline, label, color);
        x += 22;
    }

    if (m_scratchpadActive)
    {
        DrawText(x, baseline, "[S]", m_activePixel);
        x += 34;
    }

    if (m_notepadActive)
    {
        DrawText(x, baseline, "[N]", m_activePixel);
        x += 34;
    }

    if (!m_notificationText.empty())
        DrawText(x + 16, baseline, m_notificationText, m_activePixel);

    if (m_tray)
    {
        m_tray->Reposition(m_geometry.width, m_height);
        trayWidth = m_tray->Width();

        if (trayWidth > 0)
            trayWidth += 12;
    }

    // The power button - see PowerButtonRect()/WindowManager's own
    // ButtonPress routing for what opens PowerMenu. Plain bracketed
    // text rather than a glyph/icon, same reasoning as the "[S]"/"[N]"
    // indicators above: not every installed font covers a power
    // symbol, and a missing one would just draw a tofu box.
    static const std::string kPowerLabel = "[Power]";
    int powerWidth = m_font.TextWidth(kPowerLabel);

    if (powerWidth == 0)
        powerWidth = static_cast<int>(kPowerLabel.size()) * 8;

    int powerX = m_geometry.width - clockWidth - 12 - trayWidth - powerWidth - 16;

    DrawText(powerX, baseline, kPowerLabel, m_foregroundPixel);

    m_powerButtonRect = Rect{powerX - 6, 0, powerWidth + 12, m_height};

    DrawText(m_geometry.width - clockWidth - 12 - trayWidth, baseline, clockText, m_foregroundPixel);

    // The one and only touch of the real window this whole function
    // makes - see this function's own header comment. Composites the
    // fully-finished frame in a single operation, so there is no
    // intermediate state (a blank window, or a half-drawn one) for
    // the X server to ever actually display.
    XCopyArea(
        display, m_backing, m_window, m_gc,
        0, 0,
        static_cast<unsigned int>(m_geometry.width > 0 ? m_geometry.width : 1),
        static_cast<unsigned int>(m_height),
        0, 0);

    XFlush(display);

    m_hasDrawnOnce = true;
    m_lastDrawnWorkspaceCount = m_workspaceCount;
    m_lastDrawnCurrentWorkspace = m_currentWorkspace;
    m_lastDrawnScratchpadActive = m_scratchpadActive;
    m_lastDrawnNotepadActive = m_notepadActive;
    m_lastDrawnNotificationText = m_notificationText;
    m_lastDrawnTrayWidth = trayWidth;
    m_lastDrawnClockText = clockText;
    m_lastDrawnClockWidth = clockWidth;
}

void Bar::DrawText(
    int x,
    int baseline,
    const std::string& text,
    unsigned long color)
{
    if (m_window == 0 || text.empty() || !m_xftDraw)
        return;

    Display* display = m_connection.GetDisplay();

    TextColor textColor(
        display,
        DefaultVisual(display, m_connection.Screen()),
        DefaultColormap(display, m_connection.Screen()),
        color);

    m_font.DrawString(m_xftDraw, x, baseline, text, textColor.Get());
}

}
