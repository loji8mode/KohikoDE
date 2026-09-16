#pragma once

#include "Types.h"

#include <X11/Xlib.h>

#include <vector>

namespace Kohiko
{

class XConnection;

// A minimal implementation of the freedesktop System Tray Protocol
// (the mechanism behind e.g. NetworkManager's/Bluetooth's/volume
// applets' tray icons - the same protocol trayer/stalonetray/polybar's
// tray module implement). Kohiko becomes the tray by taking ownership
// of the `_NET_SYSTEM_TRAY_S<screen>` selection on a small child
// window of the Bar; tray-icon applications notice that ownership,
// ask to be docked via a ClientMessage, and get reparented into that
// window, laid out left-to-right.
//
// This deliberately implements only what's needed for icons to show
// up and stay clickable: SYSTEM_TRAY_REQUEST_DOCK and the
// XEMBED_EMBEDDED_NOTIFY handshake. The optional balloon-message
// opcodes (SYSTEM_TRAY_BEGIN_MESSAGE/CANCEL_MESSAGE) are part of the
// same spec but are for tooltip-style popups, not icon docking, so
// they're not needed here and are left unhandled (the spec allows an
// embedder to ignore opcodes it doesn't support).
class SystemTray
{
public:

    // `barWindow` becomes the tray container's parent, so it moves,
    // shows and hides together with the bar for free. Safe to call
    // only once - Kohiko never needs more than one tray.
    void Initialize(
        XConnection& connection,
        ::Window barWindow
    );

    void Shutdown();

    // Called by Bar::Redraw() every time it redraws: repositions the
    // container flush against the right edge of a `barWidth` x
    // `barHeight` bar, and resizes docked icons if the bar's height
    // changed (e.g. after a config reload).
    void Reposition(
        int barWidth,
        int barHeight
    );

    // Current pixel width of the docked-icons area (0 with no icons
    // docked), so Bar knows how much space to leave for it when it
    // positions the clock.
    int Width() const;

    // Routes SYSTEM_TRAY_REQUEST_DOCK messages sent to the tray
    // window. Safe to call with any ClientMessage - messages not
    // addressed to the tray window, or with an opcode this class
    // doesn't implement, are ignored (returning 0/None).
    //
    // Returns the window ID that was just docked, or 0/None if this
    // particular message didn't result in one - WindowManager needs
    // this (rather than a plain bool) to know *which* window might
    // need Unmanage()'d if it had already been tiled as an ordinary
    // window before this dock request arrived (see
    // WindowManager::HandleClientMessage() and CHANGELOG.md's 0.20.4
    // entry on the XEmbed dock-timeout fallback this pairs with) -
    // deliberately not this class's own concern, since SystemTray has
    // no reason to know what a "tile" even is.
    ::Window HandleClientMessage(
        const XClientMessageEvent& event
    );

    // Un-docks `window` if it was a docked tray icon (e.g. its
    // process exited). A no-op for any other window.
    void HandleWindowDestroyed(
        WindowID window
    );

    ::Window ContainerWindow() const;

    // Whether `icon` is currently a docked tray icon - added
    // alongside WindowManager's XEmbed dock-timeout fallback (see
    // CHANGELOG.md's 0.20.4 entry): it needs to tell "this window was
    // claimed by the tray in the meantime" apart from "the dock
    // request never came" before deciding whether to fall back to
    // managing a window normally.
    bool IsDocked(
        ::Window icon
    ) const;

private:

    void DockIcon(
        ::Window icon
    );

    void UndockIcon(
        ::Window icon
    );

    void ReflowIcons();

private:

    XConnection* m_connection = nullptr;

    ::Window m_container = 0;
    ::Window m_barWindow = 0;

    // _NET_SYSTEM_TRAY_S<screen> - who owns this selection *is* who
    // the tray is, per spec.
    Atom m_selectionAtom = 0;
    Atom m_trayOpcodeAtom = 0;
    Atom m_managerAtom = 0;
    Atom m_orientationAtom = 0;
    Atom m_xembedAtom = 0;
    Atom m_xembedInfoAtom = 0;

    bool m_ownsSelection = false;

    std::vector<::Window> m_icons;

    int m_iconSize = 20;
    int m_iconSpacing = 4;
    int m_margin = 6;
    int m_barHeight = 26;

};

}
