#include "UiWindow.h"
#include "Config.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cstdlib>
#include <poll.h>
#include <unistd.h>

namespace Kohiko
{

UiWindow::UiWindow() = default;

UiWindow::~UiWindow()
{
    m_iconCache.reset(); // must be destroyed before the Display it references

    if (m_xftDraw) XftDrawDestroy(m_xftDraw);
    if (m_backing) XFreePixmap(m_display, m_backing);
    if (m_gc) XFreeGC(m_display, m_gc);
    if (m_window) XDestroyWindow(m_display, m_window);
    if (m_display) XCloseDisplay(m_display);
}

bool UiWindow::Create(const std::string& title, const std::string& wmClass, int width, int height)
{
    m_display = XOpenDisplay(nullptr);
    if (!m_display)
        return false;

    m_screen = DefaultScreen(m_display);
    m_visual = DefaultVisual(m_display, m_screen);
    m_colormap = DefaultColormap(m_display, m_screen);

    m_width = width;
    m_height = height;

    m_window = XCreateSimpleWindow(
        m_display, RootWindow(m_display, m_screen),
        0, 0, width, height, 0,
        BlackPixel(m_display, m_screen),
        m_theme.background
    );

    XSelectInput(m_display, m_window,
        ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
        KeyPressMask | StructureNotifyMask);

    XStoreName(m_display, m_window, title.c_str());

    XClassHint* classHint = XAllocClassHint();
    if (classHint)
    {
        classHint->res_name = const_cast<char*>(wmClass.c_str());
        classHint->res_class = const_cast<char*>(wmClass.c_str());
        XSetClassHint(m_display, m_window, classHint);
        XFree(classHint);
    }

    m_wmProtocolsAtom = XInternAtom(m_display, "WM_PROTOCOLS", False);
    m_wmDeleteAtom = XInternAtom(m_display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(m_display, m_window, &m_wmDeleteAtom, 1);

    XSizeHints sizeHints{};
    sizeHints.flags = PMinSize | PSize;
    // A real floor for usable tiled/half-screen layouts, not the
    // initial preferred size - now that RebuildChrome()/RebuildPage()
    // actually respond to SetResizeHandler(), there's no reason to
    // prevent shrinking below whatever size an app happened to open
    // at.
    sizeHints.min_width = std::min(width, 420);
    sizeHints.min_height = std::min(height, 360);
    sizeHints.width = width;
    sizeHints.height = height;
    XSetWMNormalHints(m_display, m_window, &sizeHints);

    m_gc = XCreateGC(m_display, m_window, 0, nullptr);

    ResizeBacking(width, height);

    const char* home = std::getenv("HOME");
    std::string configPath = (home ? std::string(home) : std::string(".")) + "/.config/kohiko/kohiko.conf";
    Config config;
    config.Load(configPath); // fine if this doesn't exist yet - GetString()'s fallback covers it

    std::string fontPattern = config.GetString("general.font", "monospace:pixelsize=14");
    m_font.Load(m_display, m_screen, fontPattern);

    m_iconCache = std::make_unique<UiIconCache>(m_display, m_visual, m_colormap, m_window);

    XMapWindow(m_display, m_window);
    return true;
}

void UiWindow::ResizeBacking(int width, int height)
{
    if (m_xftDraw) { XftDrawDestroy(m_xftDraw); m_xftDraw = nullptr; }
    if (m_backing) { XFreePixmap(m_display, m_backing); m_backing = 0; }

    m_backing = XCreatePixmap(m_display, m_window, width, height, DefaultDepth(m_display, m_screen));
    m_xftDraw = XftDrawCreate(m_display, m_backing, m_visual, m_colormap);

    m_width = width;
    m_height = height;
}

void UiWindow::SetRoot(std::unique_ptr<Widget> root)
{
    ClearFocus();
    m_pressedWidget = nullptr;

    // See ScrollView::SetContent()'s comment for why this can't just
    // be `m_root = std::move(root);` - RebuildChrome()-style callers
    // can end up here from inside a widget callback that's part of
    // the very tree m_root points at.
    m_retiredRoot.reset();
    m_retiredRoot = std::move(m_root);

    m_root = std::move(root);
    m_dirty = true;
}

void UiWindow::SetFocus(Widget* widget)
{
    if (widget == m_focusedWidget)
        return;

    if (m_focusedWidget)
        m_focusedWidget->OnBlur();

    m_focusedWidget = (widget && widget->WantsFocus()) ? widget : nullptr;

    if (m_focusedWidget)
        m_focusedWidget->OnFocus();

    m_dirty = true;
}

void UiWindow::ClearFocus()
{
    if (m_focusedWidget)
    {
        m_focusedWidget->OnBlur();
        m_focusedWidget = nullptr;
    }
}

void UiWindow::WatchFd(int fd, FdCallback callback)
{
    if (fd >= 0)
        m_fdWatches.push_back({ fd, std::move(callback) });
}

void UiWindow::SetInterval(int intervalMs, TimerCallback callback)
{
    m_timers.push_back({
        std::chrono::milliseconds(intervalMs),
        std::chrono::steady_clock::now() + std::chrono::milliseconds(intervalMs),
        std::move(callback)
    });
}

void UiWindow::RaiseAndFocus()
{
    XMapRaised(m_display, m_window);
    XSetInputFocus(m_display, m_window, RevertToParent, CurrentTime);
    XFlush(m_display);
}

// --- drawing -----------------------------------------------------------------

void UiWindow::FillRect(const Rect& rect, std::uint32_t colorHex)
{
    if (rect.width <= 0 || rect.height <= 0)
        return;

    XSetForeground(m_display, m_gc, colorHex);
    XFillRectangle(m_display, m_backing, m_gc, rect.x, rect.y, rect.width, rect.height);
}

void UiWindow::FillRoundedRect(const Rect& rect, std::uint32_t colorHex, int radius)
{
    if (rect.width <= 0 || rect.height <= 0)
        return;

    // A cheap "rounded" rect - a plain rect with its four corners
    // covered by small filled arcs - rather than a real path-based
    // fill: perfectly fine at the corner radii these controls use
    // (4-10px) and keeps this toolkit off any Xrender/cairo
    // dependency the rest of Kohiko doesn't otherwise need either.
    radius = std::max(0, std::min({ radius, rect.width / 2, rect.height / 2 }));

    XSetForeground(m_display, m_gc, colorHex);

    if (radius <= 0)
    {
        XFillRectangle(m_display, m_backing, m_gc, rect.x, rect.y, rect.width, rect.height);
        return;
    }

    XFillRectangle(m_display, m_backing, m_gc, rect.x + radius, rect.y, rect.width - 2 * radius, rect.height);
    XFillRectangle(m_display, m_backing, m_gc, rect.x, rect.y + radius, radius, rect.height - 2 * radius);
    XFillRectangle(m_display, m_backing, m_gc, rect.x + rect.width - radius, rect.y + radius, radius, rect.height - 2 * radius);

    int d = radius * 2;
    XFillArc(m_display, m_backing, m_gc, rect.x, rect.y, d, d, 90 * 64, 90 * 64);
    XFillArc(m_display, m_backing, m_gc, rect.Right() - d, rect.y, d, d, 0 * 64, 90 * 64);
    XFillArc(m_display, m_backing, m_gc, rect.x, rect.Bottom() - d, d, d, 180 * 64, 90 * 64);
    XFillArc(m_display, m_backing, m_gc, rect.Right() - d, rect.Bottom() - d, d, d, 270 * 64, 90 * 64);
}

void UiWindow::DrawBorder(const Rect& rect, std::uint32_t colorHex, int thickness)
{
    if (rect.width <= 0 || rect.height <= 0)
        return;

    XSetForeground(m_display, m_gc, colorHex);
    XSetLineAttributes(m_display, m_gc, thickness, LineSolid, CapButt, JoinMiter);
    XDrawRectangle(m_display, m_backing, m_gc, rect.x, rect.y, rect.width - 1, rect.height - 1);
}

void UiWindow::DrawText(int x, int baseline, const std::string& utf8Text, std::uint32_t colorHex)
{
    TextColor color(m_display, m_visual, m_colormap, colorHex);
    m_font.DrawString(m_xftDraw, x, baseline, utf8Text, color.Get());
}

void UiWindow::DrawTextClipped(const Rect& rect, const std::string& utf8Text, std::uint32_t colorHex, Label::Align align)
{
    SetClip(rect);

    int textWidth = m_font.TextWidth(utf8Text);
    int baseline = rect.y + (rect.height + m_font.Ascent()) / 2 - m_font.Height() / 4;

    int x = rect.x;
    if (align == Label::Align::Center)
        x = rect.x + (rect.width - textWidth) / 2;
    else if (align == Label::Align::Right)
        x = rect.Right() - textWidth;

    DrawText(x, baseline, utf8Text, colorHex);
    ClearClip();
}

void UiWindow::DrawIcon(const Rect& rect, const std::string& iconName)
{
    int size = std::min(rect.width, rect.height);
    if (size <= 0)
        return;

    Pixmap pixmap = None, mask = None;
    if (!m_iconCache->Get(iconName, size, pixmap, mask))
        return;

    int x = rect.x + (rect.width - size) / 2;
    int y = rect.y + (rect.height - size) / 2;

    if (mask != None)
    {
        XSetClipMask(m_display, m_gc, mask);
        XSetClipOrigin(m_display, m_gc, x, y);
    }

    XCopyArea(m_display, pixmap, m_backing, m_gc, 0, 0, size, size, x, y);

    if (mask != None)
        XSetClipMask(m_display, m_gc, None);
}

void UiWindow::SetClip(const Rect& rect)
{
    XRectangle xr{ static_cast<short>(rect.x), static_cast<short>(rect.y),
                   static_cast<unsigned short>(std::max(0, rect.width)),
                   static_cast<unsigned short>(std::max(0, rect.height)) };
    XSetClipRectangles(m_display, m_gc, 0, 0, &xr, 1, Unsorted);
    XftDrawSetClipRectangles(m_xftDraw, 0, 0, &xr, 1);
}

void UiWindow::ClearClip()
{
    XSetClipMask(m_display, m_gc, None);
    XftDrawSetClip(m_xftDraw, None);
}

// --- input dispatch ------------------------------------------------------------

Widget* UiWindow::HitTest(Widget* widget, Point p, bool& hitSomething)
{
    if (!widget || !widget->visible)
        return nullptr;

    // A clipping container (see ScrollView) only ever exposes its
    // children to hit-testing when the point is within its own
    // visible bounds - otherwise a child scrolled outside that area
    // could still be "hit" purely because its stale absolute
    // coordinates happen to overlap some unrelated widget elsewhere
    // in the window.
    if (widget->ClipsHitTesting() && !widget->bounds.Contains(p))
        return nullptr;

    // Later (topmost) children are hit-tested first.
    auto& children = widget->Children();
    for (auto it = children.rbegin(); it != children.rend(); ++it)
    {
        Widget* hit = HitTest(it->get(), p, hitSomething);
        if (hit)
            return hit;
    }

    if (widget->bounds.Contains(p))
    {
        hitSomething = true;
        if (widget->WantsInput())
            return widget;
    }

    return nullptr;
}

Widget* UiWindow::HitTestForScroll(Widget* widget, Point p)
{
    if (!widget || !widget->visible)
        return nullptr;

    // Same clip early-out as HitTest(): a point outside a clipping
    // container's own bounds can't hit anything inside it, however
    // deep that child's stale absolute coordinates might otherwise
    // place it.
    if (widget->ClipsHitTesting() && !widget->bounds.Contains(p))
        return nullptr;

    // Descend first so a ScrollView nested inside another scrollable
    // area (not a real case today, but keeps this consistent with
    // HitTest()'s "most specific match wins" rule) still resolves to
    // the innermost one.
    auto& children = widget->Children();
    for (auto it = children.rbegin(); it != children.rend(); ++it)
    {
        Widget* hit = HitTestForScroll(it->get(), p);
        if (hit)
            return hit;
    }

    if (widget->WantsScroll() && widget->bounds.Contains(p))
        return widget;

    return nullptr;
}

void UiWindow::HandleEvent(XEvent& event)
{
    switch (event.type)
    {
        case Expose:
            if (event.xexpose.count == 0)
                m_dirty = true;
            break;

        case ConfigureNotify:
            if (event.xconfigure.width != m_width || event.xconfigure.height != m_height)
            {
                ResizeBacking(event.xconfigure.width, event.xconfigure.height);
                if (m_resizeHandler)
                    m_resizeHandler(m_width, m_height);
                m_dirty = true;
            }
            break;

        case ButtonPress:
        {
            Point p{ event.xbutton.x, event.xbutton.y };

            if (event.xbutton.button == Button4 || event.xbutton.button == Button5)
            {
                Widget* target = HitTestForScroll(m_root.get(), p);
                if (target)
                    target->OnScroll(p, event.xbutton.button == Button4 ? 1 : -1);
                m_dirty = true;
                break;
            }

            bool hit = false;
            m_pressedWidget = HitTest(m_root.get(), p, hit);

            // A click moves keyboard focus: to the pressed widget if
            // it accepts focus (see TextField), otherwise away from
            // whatever had it before - the same "click outside a text
            // field to leave it" behaviour as any other text input.
            SetFocus(m_pressedWidget);

            if (m_pressedWidget)
                m_pressedWidget->OnPress(p);
            m_dirty = true;
            break;
        }

        case KeyPress:
        {
            if (!m_focusedWidget)
                break;

            char buffer[32];
            KeySym keysym = NoSymbol;
            int len = XLookupString(&event.xkey, buffer, sizeof(buffer) - 1, &keysym, nullptr);

            switch (keysym)
            {
                case XK_BackSpace: m_focusedWidget->OnKeyInput(KeyAction::Backspace); break;
                case XK_Delete:    m_focusedWidget->OnKeyInput(KeyAction::Delete); break;
                case XK_Return:
                case XK_KP_Enter:  m_focusedWidget->OnKeyInput(KeyAction::Enter); break;
                case XK_Escape:    m_focusedWidget->OnKeyInput(KeyAction::Escape); break;
                case XK_Left:      m_focusedWidget->OnKeyInput(KeyAction::Left); break;
                case XK_Right:     m_focusedWidget->OnKeyInput(KeyAction::Right); break;
                case XK_Home:      m_focusedWidget->OnKeyInput(KeyAction::Home); break;
                case XK_End:       m_focusedWidget->OnKeyInput(KeyAction::End); break;

                default:
                    // Anything XLookupString resolved to a printable
                    // byte sequence (letters, digits, punctuation,
                    // space, ...) - control characters below 0x20
                    // (other than the named keys above) are ignored
                    // rather than inserted as literal bytes.
                    if (len > 0 && static_cast<unsigned char>(buffer[0]) >= 0x20)
                        m_focusedWidget->OnKeyInput(KeyAction::Printable, std::string(buffer, len));
                    break;
            }

            m_dirty = true;
            break;
        }

        case ButtonRelease:
        {
            Point p{ event.xbutton.x, event.xbutton.y };
            if (m_pressedWidget)
            {
                m_pressedWidget->OnRelease(p);
                m_pressedWidget = nullptr;
            }
            m_dirty = true;
            break;
        }

        case MotionNotify:
        {
            Point p{ event.xmotion.x, event.xmotion.y };
            if (m_pressedWidget)
            {
                m_pressedWidget->OnMotion(p, true);
                m_dirty = true;
            }
            break;
        }

        case ClientMessage:
            if (event.xclient.message_type == m_wmProtocolsAtom &&
                static_cast<Atom>(event.xclient.data.l[0]) == m_wmDeleteAtom)
            {
                m_running = false;
            }
            break;

        default:
            break;
    }
}

void UiWindow::Redraw()
{
    FillRect({ 0, 0, m_width, m_height }, m_theme.background);

    if (m_root)
    {
        m_root->bounds = { 0, 0, m_width, m_height };
        m_root->Draw(*this);
    }

    XCopyArea(m_display, m_backing, m_window, m_gc, 0, 0, m_width, m_height, 0, 0);
    XFlush(m_display);

    m_dirty = false;
}

void UiWindow::Run()
{
    m_running = true;
    int x11Fd = ConnectionNumber(m_display);

    while (m_running)
    {
        if (m_dirty)
            Redraw();

        std::vector<pollfd> pfds;
        pfds.push_back({ x11Fd, POLLIN, 0 });
        for (auto& watch : m_fdWatches)
            pfds.push_back({ watch.fd, POLLIN, 0 });

        // No unconditional polling floor: block indefinitely
        // (timeout -1) when nothing is actually pending, rather than
        // waking up several times a second forever regardless of
        // whether any timer is even registered - most windows built
        // on UiWindow never call SetInterval() at all. Whichever
        // timer (if any) is due soonest still wakes this up exactly
        // on time, so level-meter/spinner animation stays just as
        // smooth as before wherever one is actually running.
        bool haveDeadline = false;
        int timeoutMs = 0;

        auto now = std::chrono::steady_clock::now();

        for (auto& timer : m_timers)
        {
            int remaining = std::max<int>(0,
                static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(timer.next - now).count()));

            timeoutMs = haveDeadline ? std::min(timeoutMs, remaining) : remaining;
            haveDeadline = true;
        }

        poll(pfds.data(), pfds.size(), haveDeadline ? timeoutMs : -1);

        if (pfds[0].revents & POLLIN)
        {
            while (XPending(m_display))
            {
                XEvent event;
                XNextEvent(m_display, &event);
                HandleEvent(event);
            }
        }

        for (std::size_t i = 0; i < m_fdWatches.size(); ++i)
        {
            if (pfds[i + 1].revents & POLLIN)
                m_fdWatches[i].callback();
        }

        now = std::chrono::steady_clock::now();
        for (auto& timer : m_timers)
        {
            if (now >= timer.next)
            {
                timer.callback();
                timer.next = now + timer.interval;
            }
        }
    }
}

}
