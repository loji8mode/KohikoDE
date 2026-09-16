#include "UiPopupMenu.h"

#include <X11/Xatom.h>
#include <X11/keysym.h>

#include <algorithm>

namespace Kohiko
{

void PopupMenu::AddItem(const std::string& label, std::function<void()> onClick, bool enabled)
{
    m_items.push_back({ label, std::move(onClick), enabled, false });
}

void PopupMenu::AddSeparator()
{
    m_items.push_back({ "", nullptr, false, true });
}

void PopupMenu::Show(
    Display* display, int screen, Visual* visual, Colormap colormap,
    Font& font, const UiTheme& theme, int x, int y)
{
    if (m_items.empty())
        return;

    const int rowHeight = 30;
    const int separatorHeight = 9;
    const int paddingX = 14;
    const int minWidth = 170;

    int width = minWidth;
    int height = 4; // top inset

    std::vector<int> itemTops(m_items.size());
    std::vector<int> itemHeights(m_items.size());

    for (std::size_t i = 0; i < m_items.size(); ++i)
    {
        itemTops[i] = height;
        itemHeights[i] = m_items[i].separator ? separatorHeight : rowHeight;
        height += itemHeights[i];

        if (!m_items[i].separator)
            width = std::max(width, font.TextWidth(m_items[i].label) + paddingX * 2);
    }

    height += 4; // bottom inset

    XSetWindowAttributes attrs{};
    attrs.override_redirect = True;
    attrs.background_pixel = theme.surface;
    attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask;

    Window root = RootWindow(display, screen);
    Window popup = XCreateWindow(
        display, root, x, y, width, height, 0,
        DefaultDepth(display, screen), InputOutput, visual,
        CWOverrideRedirect | CWBackPixel | CWEventMask, &attrs
    );

    XMapRaised(display, popup);
    XFlush(display);

    // owner_events=False funnels every button/motion event to `popup`
    // for the duration of the grab, coordinates already translated
    // relative to it, regardless of what's actually under the pointer
    // - the simplest reliable way to detect "clicked outside the
    // menu" (event x/y outside [0,width)x[0,height)) without relying
    // on any particular window manager's focus-follows/click-to-focus
    // behaviour.
    XGrabPointer(display, popup, False,
        ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
        GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
    XGrabKeyboard(display, popup, False, GrabModeAsync, GrabModeAsync, CurrentTime);

    GC gc = XCreateGC(display, popup, 0, nullptr);
    XftDraw* xftDraw = XftDrawCreate(display, popup, visual, colormap);

    int hoveredIndex = -1;
    bool dismissed = false;

    auto redraw = [&]()
    {
        XSetForeground(display, gc, theme.surface);
        XFillRectangle(display, popup, gc, 0, 0, width, height);
        XSetForeground(display, gc, theme.border);
        XDrawRectangle(display, popup, gc, 0, 0, width - 1, height - 1);

        for (std::size_t i = 0; i < m_items.size(); ++i)
        {
            if (m_items[i].separator)
            {
                XSetForeground(display, gc, theme.border);
                XFillRectangle(display, popup, gc, 6, itemTops[i] + separatorHeight / 2, width - 12, 1);
                continue;
            }

            if (static_cast<int>(i) == hoveredIndex && m_items[i].enabled)
            {
                XSetForeground(display, gc, theme.surfaceHover);
                XFillRectangle(display, popup, gc, 4, itemTops[i], width - 8, itemHeights[i]);
            }

            TextColor color(display, visual, colormap,
                m_items[i].enabled ? theme.foreground : theme.muted);

            int baseline = itemTops[i] + (itemHeights[i] + font.Ascent()) / 2 - font.Height() / 4;
            font.DrawString(xftDraw, paddingX, baseline, m_items[i].label, color.Get());
        }

        XFlush(display);
    };

    auto indexAt = [&](int px, int py) -> int
    {
        if (px < 0 || px >= width || py < 0 || py >= height)
            return -1;

        for (std::size_t i = 0; i < m_items.size(); ++i)
            if (py >= itemTops[i] && py < itemTops[i] + itemHeights[i])
                return static_cast<int>(i);

        return -1;
    };

    redraw();

    while (!dismissed)
    {
        XEvent event;
        XNextEvent(display, &event);

        switch (event.type)
        {
            case Expose:
                redraw();
                break;

            case MotionNotify:
            {
                int idx = indexAt(event.xmotion.x, event.xmotion.y);
                if (idx != hoveredIndex)
                {
                    hoveredIndex = idx;
                    redraw();
                }
                break;
            }

            case ButtonRelease:
            {
                int idx = indexAt(event.xbutton.x, event.xbutton.y);
                if (idx < 0)
                {
                    dismissed = true; // clicked outside the menu entirely
                    break;
                }

                if (!m_items[idx].separator && m_items[idx].enabled && m_items[idx].onClick)
                    m_items[idx].onClick();

                dismissed = true;
                break;
            }

            case KeyPress:
            {
                KeySym keysym = XLookupKeysym(&event.xkey, 0);
                if (keysym == XK_Escape)
                    dismissed = true;
                break;
            }

            default:
                break;
        }
    }

    XUngrabKeyboard(display, CurrentTime);
    XUngrabPointer(display, CurrentTime);
    XftDrawDestroy(xftDraw);
    XFreeGC(display, gc);
    XDestroyWindow(display, popup);
    XFlush(display);
}

}
