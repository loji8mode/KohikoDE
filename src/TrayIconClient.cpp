#include "TrayIconClient.h"

#include <X11/Xatom.h>

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

    XSetWindowAttributes attrs{};
    attrs.background_pixel = m_theme.background;
    attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask;

    m_window = XCreateWindow(
        m_display, RootWindow(m_display, m_screen),
        0, 0, m_iconSize, m_iconSize, 0,
        DefaultDepth(m_display, m_screen), InputOutput, m_visual,
        CWBackPixel | CWEventMask, &attrs
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

void TrayIconClient::Redraw()
{
    if (m_drawCallback)
        m_drawCallback(m_display, m_window, m_gc, m_visual, m_colormap, m_iconSize);

    m_dirty = false;
}

void TrayIconClient::HandleEvent(XEvent& event)
{
    switch (event.type)
    {
        case Expose:
            if (event.xexpose.count == 0)
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
    if (!m_gc)
        m_gc = XCreateGC(m_display, m_window, 0, nullptr);

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

        int timeoutMs = m_docked ? 250 : 500;
        auto now = std::chrono::steady_clock::now();
        for (auto& timer : m_timers)
        {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(timer.next - now).count();
            timeoutMs = std::min<int>(timeoutMs, std::max<int>(0, static_cast<int>(remaining)));
        }

        poll(pfds.data(), pfds.size(), timeoutMs);

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
