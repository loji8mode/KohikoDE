#include "UiPopupMenu.h"

#include <chrono>
#include <cstdlib>
#include <functional>

#include <X11/Xatom.h>
#include <X11/keysym.h>

#include <algorithm>

namespace Kohiko
{

namespace
{

// Predicate for XIfEvent() below: matches only events for the popup's
// own window. `arg` is a pointer to the Window to match against -
// Xlib's C callback signature has no way to capture context otherwise.
Bool IsEventForWindow(Display*, XEvent* event, XPointer arg)
{
    Window target = *reinterpret_cast<Window*>(arg);
    return event->xany.window == target;
}

}

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

    // Where the user was pointing when the menu was requested, before
    // the clamp below moves the menu. See the release handler's guard.
    const int openX = x;
    const int openY = y;
    const auto openedAt = std::chrono::steady_clock::now();

    // Keep the whole menu on the screen. The tray sits at the right end
    // of the bar and the pointer is on it when the menu opens, so
    // placing the menu's top-left corner exactly at the pointer pushed
    // its right part - and the end of every label - off the screen.
    {
        const int screenWidth = DisplayWidth(display, screen);
        const int screenHeight = DisplayHeight(display, screen);

        x = std::max(0, std::min(x, screenWidth - width));
        y = std::max(0, std::min(y, screenHeight - height));
    }

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
    int pointerGrab = XGrabPointer(display, popup, False,
        ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
        GrabModeAsync, GrabModeAsync, None, None, CurrentTime);

    // Without the pointer grab there is no way to learn that the user
    // clicked outside (those clicks go to whatever is underneath), so
    // the loop below could never end: a popup that cannot be dismissed
    // and a process stuck inside it. Another client holding a grab at
    // that instant (a window manager mid-drag, another popup) is the
    // realistic cause; showing nothing is the safe answer.
    if (pointerGrab != GrabSuccess)
    {
        XDestroyWindow(display, popup);
        XFlush(display);
        return;
    }

    // The keyboard grab only adds Escape-to-dismiss; the menu works
    // (and can be dismissed with the pointer) without it.
    XGrabKeyboard(display, popup, False, GrabModeAsync, GrabModeAsync, CurrentTime);

    GC gc = XCreateGC(display, popup, 0, nullptr);
    XftDraw* xftDraw = XftDrawCreate(display, popup, visual, colormap);

    int hoveredIndex = -1;
    bool dismissed = false;

    // The chosen item's action. It is NOT run inside this loop: the
    // loop holds an active pointer grab and keyboard grab and has the
    // menu on screen, so an action that takes a while (it used to be a
    // synchronous BlueZ/NetworkManager call of several seconds) froze
    // the whole desktop's input - not just this tray - with the menu
    // still showing. It runs after everything below has been torn down.
    std::function<void()> chosen;

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

        // XIfEvent(), not XNextEvent(): this loop shares its
        // connection with whatever else is going on - most
        // importantly, the TrayIconClient that opened this popup in
        // the first place, whose own window can perfectly well need
        // an Expose delivered (typically once this menu closes and
        // uncovers it again) while this loop still owns the only
        // thing reading this connection. XNextEvent() would hand back
        // literally the next event for *any* window on the
        // connection - already caught misattributing exactly that
        // Expose to this popup's own redraw() and silently losing the
        // real one. The predicate above restricts this to events for
        // `popup` specifically; anything else is left exactly where
        // it is in Xlib's own queue, in order, for whoever reads this
        // connection next (this loop's own next iteration, or
        // TrayIconClient::Run() once this function returns) to see -
        // nothing is consumed, reordered, or dropped.
        XIfEvent(display, &event, IsEventForWindow, reinterpret_cast<XPointer>(&popup));

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

                // The menu opens with its corner exactly under the pointer,
                // so a second quick click on the same tray icon - a
                // double click, or an impatient repeat - lands on the
                // FIRST menu item and used to run it ("Mute", "Turn
                // Bluetooth off", ...) though the user never aimed at it.
                // A click that arrives almost at once and where the menu
                // was opened is that repeat, not a choice: ignore it and
                // leave the menu as it is. Anything deliberate - waiting
                // a moment, or moving the pointer onto an item first -
                // is unaffected.
                auto sinceOpen = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - openedAt);

                if (sinceOpen.count() < 350 &&
                    std::abs(event.xbutton.x_root - openX) <= 4 &&
                    std::abs(event.xbutton.y_root - openY) <= 4)
                    break;

                if (!m_items[idx].separator && m_items[idx].enabled && m_items[idx].onClick)
                    chosen = m_items[idx].onClick;

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

    // Only now - grabs released, menu gone from the screen, the flush
    // above sent - run what the user picked. Whatever it does (even a
    // slow call) the desktop is already usable again.
    if (chosen)
        chosen();
}

}
