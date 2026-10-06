#include "TrayIconClient.h"

#include <X11/Xatom.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <poll.h>

namespace Kohiko
{

namespace
{

constexpr long kXembedMapped = 1;
constexpr long kSystemTrayRequestDock = 0;

}

TrayIconClient::TrayIconClient() = default;

TrayIconClient::~TrayIconClient()
{
    if (m_xftDraw) XftDrawDestroy(m_xftDraw);

    // Explicit, ahead of XCloseDisplay() below: m_font's own
    // destructor runs automatically after this function body
    // finishes, and Font::Unload() frees its Xft resources through
    // the same Display* this class owns and is about to close - m_font
    // has no way to know that's about to happen. Doing it here first
    // means that automatic call finds m_font already unloaded (a
    // harmless no-op - see Font::Unload()'s own guard) instead of
    // calling XftFontClose() on an already-closed display. This was
    // always latent rather than ever actually hit in production - the
    // real tray tools (kohiko-audio-tray and friends) only ever exit
    // by being killed, never by a clean return from main() that would
    // run this destructor - but a test that constructs and cleanly
    // destroys a TrayIconClient hits it immediately.
    m_font.Unload();

    if (m_gc) XFreeGC(m_display, m_gc);
    if (m_window) XDestroyWindow(m_display, m_window);
    if (m_display) XCloseDisplay(m_display);
}

bool TrayIconClient::Create(int iconSize)
{
    m_iconSize = iconSize;

    m_display = XOpenDisplay(nullptr);
    if (!m_display)
        return false;

    m_screen = DefaultScreen(m_display);
    m_visual = DefaultVisual(m_display, m_screen);
    m_colormap = DefaultColormap(m_display, m_screen);

    // Background: ParentRelative, not a solid fill of any colour this
    // class itself owns - m_theme (UiTheme::Default()'s built-in
    // values, see UiTheme.h's own comment on why those are
    // deliberately not read from Kohiko's config) is a reasonable
    // guess for a standalone control-panel window, but this window
    // isn't one - once docked, it's reparented flush against Kohiko's
    // real Bar (see SystemTray::DockIcon()), whose own background is
    // whatever `bar.background` is actually configured to. A solid
    // guess here would only coincidentally ever match that, which is
    // exactly what used to make every one of Kohiko's own tray icons
    // sit on a visibly wrong-coloured square on any desktop whose
    // configured bar colour wasn't the same as this struct's built-in
    // default. ParentRelative instead asks X to paint whatever this
    // window's own parent would have painted there - a live reference
    // that resolves correctly whether this window is (briefly, before
    // TryDock() succeeds) still a direct child of the root window, or
    // (after docking) a child of SystemTray's own container, which is
    // itself ParentRelative all the way up to the Bar window's real
    // background_pixel - and keeps tracking it automatically even
    // across a config reload that changes bar.background, with no
    // extra work here. See Redraw()'s own comment for the other half
    // of this: painting the background attribute alone isn't enough,
    // since this window is also unconditionally redrawn from
    // RequestRedraw() with no real Expose behind it at all.
    XSetWindowAttributes attrs{};
    attrs.background_pixmap = ParentRelative;
    attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask;

    m_window = XCreateWindow(
        m_display, RootWindow(m_display, m_screen),
        0, 0, m_iconSize, m_iconSize, 0,
        DefaultDepth(m_display, m_screen), InputOutput, m_visual,
        CWBackPixmap | CWEventMask, &attrs
    );

    // Every window docked by a system tray host is required to carry
    // this property (freedesktop System Tray spec, section
    // "_XEMBED_INFO Property") - version 0, and the mapped flag set
    // up front (Kohiko's own tray host - see SystemTray.cpp - only
    // actually shows a docked window once this flag is set).
    Atom xembedInfoAtom = XInternAtom(m_display, "_XEMBED_INFO", False);
    long xembedInfo[2] = { 0, kXembedMapped };
    XChangeProperty(m_display, m_window, xembedInfoAtom, xembedInfoAtom, 32,
        PropModeReplace, reinterpret_cast<unsigned char*>(xembedInfo), 2);

    m_font.Load(m_display, m_screen, "monospace:pixelsize=14");

    m_xftDraw = XftDrawCreate(m_display, m_window, m_visual, m_colormap);

    m_nextDockAttempt = std::chrono::steady_clock::now();
    return true;
}

bool TrayIconClient::TryDock()
{
    std::string atomName = "_NET_SYSTEM_TRAY_S" + std::to_string(m_screen);
    Atom trayAtom = XInternAtom(m_display, atomName.c_str(), False);
    Window owner = XGetSelectionOwner(m_display, trayAtom);

    if (owner == None)
        return false;

    Atom opcodeAtom = XInternAtom(m_display, "_NET_SYSTEM_TRAY_OPCODE", False);

    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = owner;
    event.xclient.message_type = opcodeAtom;
    event.xclient.format = 32;
    event.xclient.data.l[0] = CurrentTime;
    event.xclient.data.l[1] = kSystemTrayRequestDock;
    event.xclient.data.l[2] = static_cast<long>(m_window);
    event.xclient.data.l[3] = 0;
    event.xclient.data.l[4] = 0;

    XSendEvent(m_display, owner, False, NoEventMask, &event);
    XMapWindow(m_display, m_window);
    XFlush(m_display);

    return true;
}

void TrayIconClient::SetDrawCallback(DrawCallback callback) { m_drawCallback = std::move(callback); }

void TrayIconClient::DrawText(int x, int baseline, const std::string& text, unsigned long rgbColor)
{
    if (m_window == 0 || text.empty() || !m_xftDraw)
        return;

    TextColor textColor(m_display, m_visual, m_colormap, rgbColor);

    m_font.DrawString(m_xftDraw, x, baseline, text, textColor.Get());
}

void TrayIconClient::SetLeftClickHandler(ClickHandler handler) { m_leftClick = std::move(handler); }
void TrayIconClient::SetRightClickHandler(ClickHandler handler) { m_rightClick = std::move(handler); }
void TrayIconClient::SetScrollHandler(ScrollHandler handler) { m_scroll = std::move(handler); }

void TrayIconClient::WatchFd(int fd, FdCallback callback)
{
    if (fd >= 0)
        m_fdWatches.push_back({ fd, std::move(callback) });
}

void TrayIconClient::SetInterval(int intervalMs, TimerCallback callback)
{
    m_timers.push_back({
        std::chrono::milliseconds(intervalMs),
        std::chrono::steady_clock::now() + std::chrono::milliseconds(intervalMs),
        std::move(callback)
    });
}

void TrayIconClient::SetEventHandler(EventHandler handler)
{
    m_extraEventHandler = std::move(handler);
}

void TrayIconClient::Redraw()
{
    // Lazily created here rather than only in Run() - Redraw() being
    // public (see its own header comment) means it can now run before
    // Run() ever does, e.g. from a test that wants one synchronous
    // redraw without an event loop.
    if (!m_gc)
        m_gc = XCreateGC(m_display, m_window, 0, nullptr);

    // Repaints the whole window with its current background
    // attribute first - ParentRelative, per Create() - so every
    // redraw starts from the tray host's real, live background rather
    // than whatever was left over from the previous frame. Needed
    // even though the X server *also* auto-paints newly-exposed
    // regions from that same attribute on its own: this Redraw() runs
    // just as often from RequestRedraw() alone (a mute/signal/
    // connection-state change, with no real Expose behind it -
    // XCopyArea drawing a smaller or differently-shaped icon than
    // last time would otherwise leave old pixels showing at the
    // edges) as it does from an actual Expose.
    XClearWindow(m_display, m_window);

    if (m_drawCallback)
        m_drawCallback(m_display, m_window, m_gc, m_visual, m_colormap, m_iconSize);

    m_dirty = false;
}

void TrayIconClient::HandleEvent(XEvent& event)
{
    switch (event.type)
    {
        case Expose:
            if (event.xexpose.count == 0 && event.xexpose.window == m_window)
                m_dirty = true;
            break;

        case ButtonPress:
            break; // acted on at ButtonRelease, matching UiWindow's own click convention

        case ButtonRelease:
        {
            switch (event.xbutton.button)
            {
                case Button1: if (m_leftClick) m_leftClick(); break;
                case Button3: if (m_rightClick) m_rightClick(); break;
                case Button4: if (m_scroll) m_scroll(1); break;
                case Button5: if (m_scroll) m_scroll(-1); break;
                default: break;
            }
            break;
        }

        default:
            break;
    }
}

void TrayIconClient::Run()
{
    m_running = true;
    int x11Fd = ConnectionNumber(m_display);

    while (m_running)
    {
        if (!m_docked)
        {
            auto now = std::chrono::steady_clock::now();
            if (now >= m_nextDockAttempt)
            {
                m_docked = TryDock();
                m_nextDockAttempt = now + std::chrono::seconds(3);
            }
        }

        if (m_dirty)
            Redraw();

        std::vector<pollfd> pfds;
        pfds.push_back({ x11Fd, POLLIN, 0 });
        for (auto& watch : m_fdWatches)
            pfds.push_back({ watch.fd, POLLIN, 0 });

        // No unconditional polling floor: with nothing pending, this
        // blocks in poll() indefinitely (timeout -1) rather than
        // waking up several times a second forever for no reason -
        // every real deadline this loop needs to notice (the next
        // registered timer, and the dock-retry attempt while not yet
        // docked) is folded in explicitly below instead. m_dirty
        // doesn't need a deadline of its own: RequestRedraw() is only
        // ever called from inside one of the callbacks below, which
        // only run after poll() has already woken this same iteration
        // for an unrelated reason (an fd becoming readable, or one of
        // those same timers) - so the very next loop iteration's
        // `if (m_dirty) Redraw()` at the top always sees it with no
        // extra wakeup needed to notice it sooner.
        bool haveDeadline = false;
        int timeoutMs = 0;

        auto now = std::chrono::steady_clock::now();

        auto considerDeadline = [&](std::chrono::steady_clock::time_point when)
        {
            int remaining = std::max<int>(0,
                static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(when - now).count()));

            timeoutMs = haveDeadline ? std::min(timeoutMs, remaining) : remaining;
            haveDeadline = true;
        };

        if (!m_docked)
            considerDeadline(m_nextDockAttempt);

        for (auto& timer : m_timers)
            considerDeadline(timer.next);

        poll(pfds.data(), pfds.size(), haveDeadline ? timeoutMs : -1);

        if (pfds[0].revents & POLLIN)
        {
            while (XPending(m_display))
            {
                XEvent event;
                XNextEvent(m_display, &event);
                HandleEvent(event);

                if (m_extraEventHandler)
                    m_extraEventHandler(event);
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
