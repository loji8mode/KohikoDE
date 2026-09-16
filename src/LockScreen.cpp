#include "LockScreen.h"

#include "Authenticator.h"
#include "Config.h"
#include "Monitor.h"
#include "MonitorManager.h"
#include "Utils.h"
#include "WallpaperManager.h"
#include "XConnection.h"

#include <Imlib2.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <pwd.h>
#include <unistd.h>

namespace Kohiko
{

namespace
{

// strftime() against the current local time - used for both
// lockscreen.clock_format and lockscreen.date_format, which really
// are just two independent strftime patterns rather than needing
// separate clock/calendar logic of their own. Returns "" if the
// formatted result wouldn't fit the buffer (an unreasonably long
// pattern) rather than a truncated, possibly-misleading string.
std::string FormatNow(
    const std::string& pattern)
{
    if (pattern.empty())
        return "";

    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);

    char buffer[128];
    std::size_t written = std::strftime(buffer, sizeof(buffer), pattern.c_str(), &local);

    return written > 0 ? std::string(buffer, written) : "";
}

}

LockScreen::LockScreen(
    XConnection& connection)
    :
    m_connection(connection)
{
}

LockScreen::~LockScreen()
{
    Display* display = m_connection.GetDisplay();

    if (!display)
        return;

    if (m_xic)
        XDestroyIC(m_xic);

    if (m_xim)
        XCloseIM(m_xim);

    if (m_xftDraw)
        XftDrawDestroy(m_xftDraw);

    m_font.Unload();

    for (Pixmap pixmap : m_backgroundPixmaps)
        if (pixmap)
            XFreePixmap(display, pixmap);

    if (m_logoPixmap)
        XFreePixmap(display, m_logoPixmap);

    if (m_logoMask)
        XFreePixmap(display, m_logoMask);

    if (m_gc)
        XFreeGC(display, m_gc);

    if (m_window)
        XDestroyWindow(display, m_window);
}

void LockScreen::SetLockStateChangedCallback(
    LockStateChangedCallback callback)
{
    m_lockStateChanged = std::move(callback);
}

void LockScreen::Configure(
    const Config& config)
{
    m_backgroundPixel = std::strtoul(config.GetString("lockscreen.background_color", "0x1e1e2e").c_str(), nullptr, 0);
    m_foregroundPixel = std::strtoul(config.GetString("lockscreen.foreground", "0xcdd6f4").c_str(), nullptr, 0);
    m_fieldPixel       = std::strtoul(config.GetString("lockscreen.field_color", "0x313244").c_str(), nullptr, 0);
    m_errorPixel       = std::strtoul(config.GetString("lockscreen.error_color", "0xf38ba8").c_str(), nullptr, 0);

    m_backgroundImagePath = config.GetString("lockscreen.background_image", "");
    m_backgroundMode = ImageRenderer::ParseMode(
        config.GetString("lockscreen.background_mode", "fill"), ImageScaleMode::Fill);
    m_useDesktopWallpaper = config.GetBool("lockscreen.use_desktop_wallpaper", false);
    m_logoPath            = config.GetString("lockscreen.logo", "");

    m_showClock    = config.GetBool("lockscreen.show_clock", true);
    m_showDate     = config.GetBool("lockscreen.show_date", true);
    m_showHostname = config.GetBool("lockscreen.show_hostname", false);
    m_showUsername = config.GetBool("lockscreen.show_username", true);
    m_clockFormat  = config.GetString("lockscreen.clock_format", "%H:%M");
    m_dateFormat   = config.GetString("lockscreen.date_format", "%A, %B %d");

    // Resolved once here rather than per-Redraw() - the local
    // hostname doesn't change while Kohiko is running.
    char hostnameBuffer[256];

    if (gethostname(hostnameBuffer, sizeof(hostnameBuffer)) == 0)
    {
        hostnameBuffer[sizeof(hostnameBuffer) - 1] = '\0';
        m_hostname = hostnameBuffer;
    }
    else
    {
        m_hostname.clear();
    }

    std::string fontPattern = config.GetString(
        "lockscreen.font",
        config.GetString("general.font", "monospace:pixelsize=14"));

    Display* display = m_connection.GetDisplay();
    int screen = m_connection.Screen();

    if (m_window == 0)
    {
        int width  = DisplayWidth(display, screen);
        int height = DisplayHeight(display, screen);

        XSetWindowAttributes attrs{};
        attrs.override_redirect = True; // this is *our* window - never redirect it back to ourselves as a MapRequest
        attrs.background_pixel = m_backgroundPixel;
        attrs.event_mask = ExposureMask | KeyPressMask | ButtonPressMask;

        m_window = XCreateWindow(
            display,
            m_connection.Root(),
            0, 0,
            static_cast<unsigned int>(width),
            static_cast<unsigned int>(height),
            0,
            DefaultDepth(display, screen),
            InputOutput,
            DefaultVisual(display, screen),
            CWOverrideRedirect | CWBackPixel | CWEventMask,
            &attrs
        );

        m_gc = XCreateGC(display, m_window, 0, nullptr);

        m_xftDraw = XftDrawCreate(
            display,
            m_window,
            DefaultVisual(display, screen),
            DefaultColormap(display, screen));

        // Same reasoning as Notepad's own XIM/XIC setup: without this,
        // a password containing anything outside plain ASCII wouldn't
        // compose correctly (dead keys, IME sequences, ...).
        m_xim = XOpenIM(display, nullptr, nullptr, nullptr);

        if (m_xim)
        {
            m_xic = XCreateIC(
                m_xim,
                XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                XNClientWindow, m_window,
                XNFocusWindow, m_window,
                nullptr);
        }
    }
    else
    {
        XSetWindowBackground(display, m_window, m_backgroundPixel);
    }

    m_font.Load(display, screen, fontPattern);

    if (m_logoPixmap)
    {
        XFreePixmap(display, m_logoPixmap);
        m_logoPixmap = 0;
    }

    if (m_logoMask)
    {
        XFreePixmap(display, m_logoMask);
        m_logoMask = 0;
    }

    if (!m_logoPath.empty())
    {
        imlib_context_set_display(display);
        imlib_context_set_visual(DefaultVisual(display, screen));
        imlib_context_set_colormap(DefaultColormap(display, screen));
        imlib_context_set_drawable(m_window);

        Imlib_Image image = imlib_load_image(m_logoPath.c_str());

        if (image)
        {
            imlib_context_set_image(image);

            imlib_render_pixmaps_for_whole_image_at_size(
                &m_logoPixmap, &m_logoMask,
                m_logoSize, m_logoSize);

            imlib_free_image();
        }
    }
}

void LockScreen::RenderBackgroundPixmaps(
    const MonitorManager& monitors,
    const WallpaperManager& wallpapers)
{
    Display* display = m_connection.GetDisplay();

    m_monitorGeometries.clear();

    for (Pixmap pixmap : m_backgroundPixmaps)
        if (pixmap)
            XFreePixmap(display, pixmap);

    m_backgroundPixmaps.clear();

    for (const auto& monitor : monitors.All())
    {
        const Rect& geometry = monitor->Geometry();
        m_monitorGeometries.push_back(geometry);

        std::string path = m_backgroundImagePath;
        ImageScaleMode mode = m_backgroundMode;

        if (path.empty() && m_useDesktopWallpaper)
        {
            WallpaperManager::Resolved resolved = wallpapers.ResolveFor(*monitor);
            path = resolved.path;
            mode = resolved.mode;
        }

        // A 0 pixmap here is a normal, valid outcome, not a failure -
        // Redraw() already falls back to lockscreen.background_color
        // for it (see its own check), same as when ImageRenderer
        // itself couldn't load `path` at all.
        m_backgroundPixmaps.push_back(
            path.empty()
                ? 0
                : ImageRenderer::Render(
                    m_connection, m_window, path,
                    geometry.width, geometry.height, mode, m_backgroundPixel));
    }
}

void LockScreen::Lock(
    const MonitorManager& monitors,
    const WallpaperManager& wallpapers)
{
    if (m_locked || m_window == 0)
        return;

    struct passwd* pw = getpwuid(getuid());
    m_username = pw ? pw->pw_name : std::string();

    // Per the spec: an account with no password configured at all
    // unlocks immediately - which, using PAM for the check the exact
    // same way a real attempt would, just means trying an empty
    // password right now, before ever mapping anything. See
    // Authenticator's own header comment.
    if (Authenticator::Authenticate(m_username, ""))
        return;

    Display* display = m_connection.GetDisplay();
    int screen = m_connection.Screen();

    RenderBackgroundPixmaps(monitors, wallpapers);

    Rect full{0, 0, DisplayWidth(display, screen), DisplayHeight(display, screen)};
    m_connection.MoveResizeWindow(m_window, full);
    m_connection.MapWindow(m_window);
    m_connection.Raise(m_window);

    // XGrabKeyboard/XGrabPointer, not just stacking + real X input
    // focus the way Launcher/Notepad get away with - see this class's
    // own header comment for why a *cooperative* focus model isn't
    // enough for a lock screen specifically: any client that calls
    // XSetInputFocus on itself (some do, bypassing the WM entirely)
    // could otherwise steal keystrokes - including the password
    // itself - right out from under it. An active grab makes Kohiko
    // the sole arbiter of where input goes, full stop, regardless of
    // what any other client does.
    //
    // GrabNotViewable can happen transiently for a window that was
    // *just* mapped (the map request may not have reached the server
    // yet) - a short bounded retry is the standard fix every other
    // X11 locker uses for this exact race, not a sign of anything
    // actually wrong.
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        if (XGrabKeyboard(display, m_window, False, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess)
            break;

        usleep(20000);
    }

    for (int attempt = 0; attempt < 50; ++attempt)
    {
        if (XGrabPointer(
                display, m_window, False,
                ButtonPressMask | ButtonReleaseMask,
                GrabModeAsync, GrabModeAsync,
                m_window, None, CurrentTime) == GrabSuccess)
            break;

        usleep(20000);
    }

    m_connection.SetInputFocus(m_window);

    Utils::SecureErase(m_typed);
    m_showError = false;
    m_locked = true;

    Redraw();

    if (m_lockStateChanged)
        m_lockStateChanged(true);
}

void LockScreen::Unlock()
{
    if (!m_locked)
        return;

    Display* display = m_connection.GetDisplay();

    XUngrabKeyboard(display, CurrentTime);
    XUngrabPointer(display, CurrentTime);

    m_connection.UnmapWindow(m_window);

    // Wiped, not just cleared - see Utils::SecureErase's own comment
    // for exactly what that does and doesn't guarantee. This is the
    // "password memory is wiped immediately after authentication"
    // requirement; the failed-attempt and Escape paths in
    // HandleKeyPress() below do the same.
    Utils::SecureErase(m_typed);
    m_showError = false;
    m_locked = false;

    if (m_lockStateChanged)
        m_lockStateChanged(false);
}

bool LockScreen::IsLocked() const
{
    return m_locked;
}

::Window LockScreen::WindowId() const
{
    return m_window;
}

void LockScreen::HandleExpose()
{
    Redraw();
}

void LockScreen::Reposition(
    const MonitorManager& monitors,
    const WallpaperManager& wallpapers)
{
    if (!m_locked || m_window == 0)
        return;

    Display* display = m_connection.GetDisplay();
    int screen = m_connection.Screen();

    RenderBackgroundPixmaps(monitors, wallpapers);

    Rect full{0, 0, DisplayWidth(display, screen), DisplayHeight(display, screen)};
    m_connection.MoveResizeWindow(m_window, full);

    Redraw();
}

void LockScreen::HandleKeyPress(
    const XKeyEvent& event)
{
    if (!m_locked)
        return;

    char buffer[32];
    KeySym keysym = NoSymbol;
    XKeyEvent mutableEvent = event; // Xutf8LookupString wants a non-const pointer
    Status status;
    int len;

    if (m_xic)
        len = Xutf8LookupString(m_xic, &mutableEvent, buffer, sizeof(buffer) - 1, &keysym, &status);
    else
        len = XLookupString(&mutableEvent, buffer, sizeof(buffer) - 1, &keysym, nullptr);

    if (len < 0)
        len = 0;

    switch (keysym)
    {
        case XK_Escape:
            Utils::SecureErase(m_typed);
            m_showError = false;
            break;

        case XK_BackSpace:

            if (!m_typed.empty())
            {
                std::size_t prev = Utils::Utf8PrevBoundary(m_typed, m_typed.size());
                m_typed.erase(prev);
            }

            break;

        case XK_Return:
        case XK_KP_Enter:

            if (Authenticator::Authenticate(m_username, m_typed))
            {
                Unlock();
                return;
            }

            Utils::SecureErase(m_typed);
            m_showError = true;
            m_errorUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(1800);
            break;

        default:

            buffer[len] = '\0';

            // Filters out control characters (Tab, arrow keys mapping
            // to escape sequences under plain XLookupString without an
            // XIC, ...) - a real typed character never starts below
            // 0x20 in UTF-8.
            if (len > 0 && static_cast<unsigned char>(buffer[0]) >= 0x20)
                m_typed.append(buffer, static_cast<std::size_t>(len));

            break;
    }

    Redraw();
}

void LockScreen::Tick()
{
    bool needsRedraw = false;

    if (m_showError && std::chrono::steady_clock::now() >= m_errorUntil)
    {
        m_showError = false;
        needsRedraw = true;
    }

    // Same reasoning as Bar::Redraw() being called every Tick() to
    // keep its own clock current - Tick() runs roughly once a second
    // while idle (see EventLoop's select() timeout), which is exactly
    // the resolution lockscreen.clock_format needs.
    if (m_locked && (m_showClock || m_showDate))
        needsRedraw = true;

    if (needsRedraw)
        Redraw();
}

void LockScreen::Redraw()
{
    if (!m_locked || m_window == 0 || !m_xftDraw)
        return;

    Display* display = m_connection.GetDisplay();
    Visual* visual = DefaultVisual(display, m_connection.Screen());
    Colormap colormap = DefaultColormap(display, m_connection.Screen());

    TextColor foreground(display, visual, colormap, m_foregroundPixel);
    TextColor error(display, visual, colormap, m_errorPixel);

    for (std::size_t i = 0; i < m_monitorGeometries.size(); ++i)
    {
        const Rect& geo = m_monitorGeometries[i];

        if (i < m_backgroundPixmaps.size() && m_backgroundPixmaps[i] != 0)
        {
            XCopyArea(
                display, m_backgroundPixmaps[i], m_window, m_gc,
                0, 0,
                static_cast<unsigned int>(geo.width), static_cast<unsigned int>(geo.height),
                geo.x, geo.y);
        }
        else
        {
            XSetForeground(display, m_gc, m_backgroundPixel);
            XFillRectangle(
                display, m_window, m_gc,
                geo.x, geo.y,
                static_cast<unsigned int>(geo.width), static_cast<unsigned int>(geo.height));
        }

        int centerX = geo.CenterX();
        int centerY = geo.CenterY();

        // Clock/date, if enabled - drawn well above the logo/field so
        // neither ever overlaps regardless of which combination of
        // logo/clock/date is turned on. Reuses the same font as
        // everything else on this screen rather than loading a second,
        // larger one - see this class's header comment on why that's
        // an acceptable simplification for a deliberately plain lock
        // screen rather than a full theming system.
        int clockY = centerY - 200;
        int dateY = clockY + m_font.Height() + 6;

        if (m_showClock)
        {
            std::string clockText = FormatNow(m_clockFormat);
            int clockWidth = m_font.TextWidth(clockText);
            m_font.DrawString(m_xftDraw, centerX - clockWidth / 2, clockY, clockText, foreground.Get());
        }

        if (m_showDate)
        {
            std::string dateText = FormatNow(m_dateFormat);
            int dateWidth = m_font.TextWidth(dateText);
            m_font.DrawString(m_xftDraw, centerX - dateWidth / 2, dateY, dateText, foreground.Get());
        }

        if (m_logoPixmap)
        {
            int logoX = centerX - m_logoSize / 2;
            int logoY = centerY - 110;

            if (m_logoMask)
            {
                XSetClipMask(display, m_gc, m_logoMask);
                XSetClipOrigin(display, m_gc, logoX, logoY);
            }

            XCopyArea(
                display, m_logoPixmap, m_window, m_gc,
                0, 0,
                static_cast<unsigned int>(m_logoSize), static_cast<unsigned int>(m_logoSize),
                logoX, logoY);

            if (m_logoMask)
                XSetClipMask(display, m_gc, None);
        }

        int fieldWidth = 260;
        int fieldHeight = 40;
        int fieldX = centerX - fieldWidth / 2;
        int fieldY = centerY - fieldHeight / 2;

        XSetForeground(display, m_gc, m_fieldPixel);
        XFillRectangle(
            display, m_window, m_gc,
            fieldX, fieldY,
            static_cast<unsigned int>(fieldWidth), static_cast<unsigned int>(fieldHeight));

        XSetForeground(display, m_gc, m_showError ? m_errorPixel : m_foregroundPixel);
        XDrawRectangle(
            display, m_window, m_gc,
            fieldX, fieldY,
            static_cast<unsigned int>(fieldWidth) - 1, static_cast<unsigned int>(fieldHeight) - 1);

        // Masked input: a fixed placeholder character rather than the
        // real text, and a plain ASCII one specifically (not a nicer
        // bullet glyph) so it's guaranteed to render on every font -
        // same reasoning as the bar's "[S]"/"[N]" indicators.
        std::string dots(std::min(m_typed.size(), static_cast<std::size_t>(24)), '*');
        int fieldBaseline = fieldY + fieldHeight / 2 + m_font.Ascent() / 2;
        m_font.DrawString(m_xftDraw, fieldX + 14, fieldBaseline, dots, foreground.Get());

        std::string caption;

        if (m_showError)
        {
            caption = "Wrong password";
        }
        else
        {
            // Username and hostname are independent toggles - either,
            // both ("user@host"), or neither ("Locked", the same
            // fallback as before either existed) all fall out of this
            // one bit of string-building rather than needing four
            // separate cases spelled out.
            std::string identity;

            if (m_showUsername && !m_username.empty())
                identity = m_username;

            if (m_showHostname && !m_hostname.empty())
                identity = identity.empty() ? m_hostname : identity + "@" + m_hostname;

            caption = identity.empty() ? "Locked" : identity;
        }

        int captionWidth = m_font.TextWidth(caption);

        m_font.DrawString(
            m_xftDraw,
            centerX - captionWidth / 2,
            fieldY - 16,
            caption,
            m_showError ? error.Get() : foreground.Get());
    }

    XFlush(display);
}

}
