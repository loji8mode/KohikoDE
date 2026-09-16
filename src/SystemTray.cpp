#include "SystemTray.h"

#include "XConnection.h"

#include <X11/Xatom.h>

#include <algorithm>
#include <string>

namespace Kohiko
{

namespace
{
    // System Tray Protocol opcodes (sent as data.l[1] of a ClientMessage
    // whose message_type is _NET_SYSTEM_TRAY_OPCODE). Only DOCK is
    // handled - see the class comment in SystemTray.h.
    constexpr long kSystemTrayRequestDock = 0;

    // XEMBED opcode (sent as data.l[1] of a ClientMessage whose
    // message_type is _XEMBED).
    constexpr long kXEmbedEmbeddedNotify = 0;

    // The XEMBED protocol version Kohiko speaks - just enough of the
    // handshake for an icon to map itself and start drawing.
    constexpr long kXEmbedProtocolVersion = 0;
}

void SystemTray::Initialize(
    XConnection& connection,
    ::Window barWindow)
{
    m_connection = &connection;
    m_barWindow = barWindow;

    Display* display = connection.GetDisplay();
    int screen = connection.Screen();

    std::string selectionName =
        "_NET_SYSTEM_TRAY_S" + std::to_string(screen);

    m_selectionAtom   = XInternAtom(display, selectionName.c_str(), False);
    m_trayOpcodeAtom  = XInternAtom(display, "_NET_SYSTEM_TRAY_OPCODE", False);
    m_managerAtom     = XInternAtom(display, "MANAGER", False);
    m_orientationAtom = XInternAtom(display, "_NET_SYSTEM_TRAY_ORIENTATION", False);
    m_xembedAtom      = XInternAtom(display, "_XEMBED", False);
    m_xembedInfoAtom  = XInternAtom(display, "_XEMBED_INFO", False);

    // Launcher.cpp gets per-pixel icon transparency by asking Imlib2
    // for a 1-bit shape mask and clipping XCopyArea with it - that
    // works there because Launcher draws every icon pixmap itself.
    // Tray icons are the opposite: they're real top-level windows
    // belonging to other processes, reparented in via XEmbed, so
    // there's no pixmap of ours to mask. The equivalent trick for a
    // reparented window is ParentRelative: instead of owning a solid
    // fill (this used to be BlackPixel, which is why every tray icon
    // sat on a visible black square that didn't match the Bar's actual
    // - configurable, non-black - background colour), the container
    // asks X to paint whatever its parent (the Bar window) would have
    // painted there. Because that's a live reference to the parent's
    // background attribute rather than a snapshot, it keeps tracking
    // the Bar's background automatically even across a config reload
    // that changes bar.background.
    XSetWindowAttributes attrs{};
    attrs.background_pixmap = ParentRelative;
    attrs.event_mask = 0; // icons paint themselves - we don't need Expose here

    m_container = XCreateWindow(
        display,
        barWindow,
        0, 0,
        1, 1, // real size comes from Reposition()/ReflowIcons() once icons dock
        0,
        DefaultDepth(display, screen),
        InputOutput,
        DefaultVisual(display, screen),
        CWBackPixmap | CWEventMask,
        &attrs);

    XMapWindow(display, m_container);

    XSetSelectionOwner(display, m_selectionAtom, m_container, CurrentTime);

    // If another tray (a leftover Kohiko instance, a separate status
    // bar, ...) already owns this selection, back off entirely rather
    // than fight over it - two embedders racing for the same icons
    // would just bounce them back and forth.
    m_ownsSelection =
        (XGetSelectionOwner(display, m_selectionAtom) == m_container);

    if (!m_ownsSelection)
        return;

    long orientation = 0; // SYSTEM_TRAY_ORIENTATION_HORZ

    XChangeProperty(
        display,
        m_container,
        m_orientationAtom,
        XA_CARDINAL,
        32,
        PropModeReplace,
        reinterpret_cast<unsigned char*>(&orientation),
        1);

    // This is how tray-icon applications discover a tray exists at
    // all - broadcast the MANAGER selection announcement the System
    // Tray Protocol spec requires.
    XClientMessageEvent manager{};
    manager.type = ClientMessage;
    manager.window = connection.Root();
    manager.message_type = m_managerAtom;
    manager.format = 32;
    manager.data.l[0] = CurrentTime;
    manager.data.l[1] = static_cast<long>(m_selectionAtom);
    manager.data.l[2] = static_cast<long>(m_container);

    XSendEvent(
        display,
        connection.Root(),
        False,
        StructureNotifyMask,
        reinterpret_cast<XEvent*>(&manager));

    XFlush(display);
}

void SystemTray::Shutdown()
{
    if (!m_connection)
        return;

    Display* display = m_connection->GetDisplay();

    if (!display)
        return;

    if (m_ownsSelection)
        XSetSelectionOwner(display, m_selectionAtom, None, CurrentTime);

    if (m_container)
        XDestroyWindow(display, m_container);

    m_container = 0;
    m_icons.clear();
}

void SystemTray::Reposition(
    int barWidth,
    int barHeight)
{
    if (!m_ownsSelection || !m_connection || m_container == 0)
        return;

    if (barHeight != m_barHeight)
    {
        m_barHeight = barHeight;
        m_iconSize = std::max(12, std::min(barHeight - 6, 24));
        ReflowIcons();
    }

    int width = Width();
    int x = std::max(0, barWidth - width - m_margin);
    int y = std::max(0, (barHeight - m_iconSize) / 2);

    Display* display = m_connection->GetDisplay();

    XMoveResizeWindow(
        display,
        m_container,
        x,
        y,
        static_cast<unsigned int>(std::max(width, 1)),
        static_cast<unsigned int>(m_iconSize));
}

int SystemTray::Width() const
{
    if (m_icons.empty())
        return 0;

    return static_cast<int>(m_icons.size()) * m_iconSize +
           static_cast<int>(m_icons.size() - 1) * m_iconSpacing;
}

::Window SystemTray::HandleClientMessage(
    const XClientMessageEvent& event)
{
    if (!m_ownsSelection || event.window != m_container)
        return 0;

    if (event.message_type != m_trayOpcodeAtom || event.format != 32)
        return 0;

    if (event.data.l[1] == kSystemTrayRequestDock)
    {
        ::Window icon = static_cast<::Window>(event.data.l[2]);

        if (icon != 0 &&
            std::find(m_icons.begin(), m_icons.end(), icon) == m_icons.end())
        {
            DockIcon(icon);
            return icon;
        }
    }

    // SYSTEM_TRAY_BEGIN_MESSAGE / SYSTEM_TRAY_CANCEL_MESSAGE (balloon
    // popups) are intentionally left unhandled - see the class
    // comment in SystemTray.h.
    return 0;
}

void SystemTray::HandleWindowDestroyed(
    WindowID window)
{
    UndockIcon(window);
}

::Window SystemTray::ContainerWindow() const
{
    return m_container;
}

bool SystemTray::IsDocked(::Window icon) const
{
    return std::find(m_icons.begin(), m_icons.end(), icon) != m_icons.end();
}

void SystemTray::DockIcon(
    ::Window icon)
{
    Display* display = m_connection->GetDisplay();

    // So DestroyNotify (cleanup) and PropertyNotify (XEMBED_INFO
    // changes) for this specific icon reach WindowManager's normal
    // event dispatch - the same path every other window's events
    // already go through.
    XSelectInput(display, icon, StructureNotifyMask | PropertyChangeMask);

    // Best-effort only: an icon app that paints its own opaque surface
    // (most toolkits do) will just draw straight over this, and that's
    // fine - this only helps the simpler icons that leave X to fill in
    // whatever they don't explicitly paint, so those show the tray's
    // real background instead of a black/white square around the
    // glyph. A misbehaving client refusing this is a normal, harmless
    // async X error (caught by XConnection's global error handler),
    // not something that can take the rest of DockIcon() down with it.
    XSetWindowAttributes iconAttrs{};
    iconAttrs.background_pixmap = ParentRelative;
    XChangeWindowAttributes(display, icon, CWBackPixmap, &iconAttrs);

    XReparentWindow(display, icon, m_container, 0, 0);

    m_icons.push_back(icon);
    ReflowIcons();

    XMapWindow(display, icon);

    // Tell the icon it's been embedded, per the XEMBED handshake -
    // most toolkits (GTK's GtkStatusIcon/libappindicator backends
    // included) wait for this before they start drawing.
    XClientMessageEvent notify{};
    notify.type = ClientMessage;
    notify.window = icon;
    notify.message_type = m_xembedAtom;
    notify.format = 32;
    notify.data.l[0] = CurrentTime;
    notify.data.l[1] = kXEmbedEmbeddedNotify;
    notify.data.l[2] = 0;
    notify.data.l[3] = static_cast<long>(m_container);
    notify.data.l[4] = kXEmbedProtocolVersion;

    XSendEvent(display, icon, False, NoEventMask, reinterpret_cast<XEvent*>(&notify));
    XFlush(display);
}

void SystemTray::UndockIcon(
    ::Window icon)
{
    auto it = std::find(m_icons.begin(), m_icons.end(), icon);

    if (it == m_icons.end())
        return;

    m_icons.erase(it);
    ReflowIcons();
}

void SystemTray::ReflowIcons()
{
    if (!m_connection)
        return;

    Display* display = m_connection->GetDisplay();

    int x = 0;

    for (::Window icon : m_icons)
    {
        XMoveResizeWindow(
            display,
            icon,
            x,
            0,
            static_cast<unsigned int>(m_iconSize),
            static_cast<unsigned int>(m_iconSize));

        x += m_iconSize + m_iconSpacing;
    }
}

}
