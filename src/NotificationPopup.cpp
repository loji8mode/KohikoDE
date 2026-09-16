#include "NotificationPopup.h"

#include "NotificationLayout.h"

#include <X11/Xatom.h>

namespace Kohiko
{

NotificationPopup::NotificationPopup()
{
}

NotificationPopup::~NotificationPopup()
{
    if (!m_display)
        return;

    if (m_xftDraw)
        XftDrawDestroy(m_xftDraw);

    if (m_gc)
        XFreeGC(m_display, m_gc);

    if (m_window)
        XDestroyWindow(m_display, m_window);
}

bool NotificationPopup::Create(
    Display* display,
    int screen,
    ::Window root,
    Font& font,
    const UiTheme& theme)
{
    if (!display || !font.IsLoaded())
        return false;

    m_display = display;
    m_screen = screen;
    m_font = &font;
    m_theme = theme;

    // Real geometry gets set on every Show() (it depends on the
    // text/stack position) - this initial size just needs to be
    // non-zero for XCreateWindow, same convention PowerMenu::Configure()
    // already uses for its own placeholder size.
    m_geometry.width = NotificationLayout::kMinWidth;
    m_geometry.height = font.Height() + NotificationLayout::kVerticalPadding * 2;

    XSetWindowAttributes attrs{};
    attrs.override_redirect = True; // this is *our* window - never redirect it back to ourselves as a MapRequest, and never let WindowManager tile/manage it (see the class comment)
    attrs.background_pixel = theme.surface;
    attrs.event_mask = ExposureMask; // deliberately no button/key/pointer mask - a notification is never clicked or focused

    m_window = XCreateWindow(
        display,
        root,
        -1, -1,
        static_cast<unsigned int>(m_geometry.width),
        static_cast<unsigned int>(m_geometry.height),
        1,
        DefaultDepth(display, screen),
        InputOutput,
        DefaultVisual(display, screen),
        CWOverrideRedirect | CWBackPixel | CWEventMask,
        &attrs
    );

    if (!m_window)
        return false;

    // Spec-correctness for any pager/tool that inspects window types
    // (see the class comment) - not required for Kohiko's own
    // WindowManager to leave this window alone (override-redirect
    // above already guarantees that), so failure to set this property
    // (a barely-possible XChangeProperty failure) still leaves a
    // perfectly functional, just less strictly spec-labelled, popup -
    // not worth failing Create() over.
    Atom windowTypeAtom = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
    Atom notificationTypeAtom = XInternAtom(display, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);

    XChangeProperty(
        display, m_window, windowTypeAtom, XA_ATOM, 32,
        PropModeReplace, reinterpret_cast<unsigned char*>(&notificationTypeAtom), 1
    );

    m_gc = XCreateGC(display, m_window, 0, nullptr);

    m_xftDraw = XftDrawCreate(
        display,
        m_window,
        DefaultVisual(display, screen),
        DefaultColormap(display, screen));

    return true;
}

void NotificationPopup::Show(
    const std::string& text,
    const Point& topLeft,
    std::chrono::steady_clock::time_point expiresAt)
{
    if (m_window == 0 || !m_font)
        return;

    m_text = text;
    m_expiresAt = expiresAt;

    int textWidth = m_font->TextWidth(text);

    m_geometry.width = NotificationLayout::ComputeWidth(textWidth);
    m_geometry.height = NotificationLayout::ComputeHeight(m_font->Height());
    m_geometry.x = topLeft.x;
    m_geometry.y = topLeft.y;

    XMoveResizeWindow(
        m_display, m_window,
        m_geometry.x, m_geometry.y,
        static_cast<unsigned int>(m_geometry.width),
        static_cast<unsigned int>(m_geometry.height));

    XMapWindow(m_display, m_window);

    // Raised on every Show() (not just the first) so a freshly-posted
    // toast always starts above whatever's currently on screen, per
    // the spec's "appear above normal windows" - see the class
    // comment for why override-redirect placement in the stacking
    // order otherwise isn't something this class has to fight for.
    XRaiseWindow(m_display, m_window);

    Redraw();
}

void NotificationPopup::MoveTo(
    const Point& topLeft)
{
    if (m_window == 0)
        return;

    m_geometry.x = topLeft.x;
    m_geometry.y = topLeft.y;

    XMoveWindow(m_display, m_window, m_geometry.x, m_geometry.y);
}

void NotificationPopup::Hide()
{
    if (m_window == 0)
        return;

    XUnmapWindow(m_display, m_window);
}

void NotificationPopup::Redraw()
{
    if (m_window == 0 || !m_xftDraw || !m_font)
        return;

    XSetForeground(m_display, m_gc, m_theme.surface);
    XFillRectangle(m_display, m_window, m_gc, 0, 0,
        static_cast<unsigned int>(m_geometry.width),
        static_cast<unsigned int>(m_geometry.height));

    XSetForeground(m_display, m_gc, m_theme.border);
    XDrawRectangle(m_display, m_window, m_gc, 0, 0,
        static_cast<unsigned int>(m_geometry.width) - 1,
        static_cast<unsigned int>(m_geometry.height) - 1);

    TextColor textColor(
        m_display,
        DefaultVisual(m_display, m_screen),
        DefaultColormap(m_display, m_screen),
        m_theme.foreground);

    int baseline = (m_geometry.height + m_font->Ascent()) / 2;

    m_font->DrawString(
        m_xftDraw,
        NotificationLayout::kHorizontalPadding,
        baseline,
        m_text,
        textColor.Get());

    XFlush(m_display);
}

void NotificationPopup::HandleExpose()
{
    Redraw();
}

::Window NotificationPopup::WindowId() const
{
    return m_window;
}

int NotificationPopup::Width() const
{
    return m_geometry.width;
}

int NotificationPopup::Height() const
{
    return m_geometry.height;
}

std::chrono::steady_clock::time_point NotificationPopup::ExpiresAt() const
{
    return m_expiresAt;
}

}
