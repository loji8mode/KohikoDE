#pragma once

#include <X11/Xlib.h>

namespace Kohiko
{

class XConnection;

// Every X atom Kohiko needs, interned once at startup.
class XAtoms
{
public:

    explicit XAtoms(
        XConnection& connection
    );

    void Initialize();

    Atom WM_PROTOCOLS;
    Atom WM_DELETE_WINDOW;

    // EWMH root-window identification, published by
    // XConnection::InitializeEwmhSupport() so tools that check for a
    // compliant window manager (flameshot's screenshot overlay among
    // them) can trust NET_CLIENT_LIST/NET_ACTIVE_WINDOW instead of
    // falling back to cruder window discovery.
    Atom NET_SUPPORTED;
    Atom NET_SUPPORTING_WM_CHECK;
    Atom NET_CLIENT_LIST;

    Atom NET_ACTIVE_WINDOW;
    Atom NET_WM_STATE;
    Atom NET_WM_STATE_FULLSCREEN;
    Atom NET_CURRENT_DESKTOP;
    Atom NET_WM_DESKTOP;
    Atom NET_WM_NAME;
    Atom NET_WM_PID;

    // _NET_WM_WINDOW_TYPE and every type value Kohiko treats as
    // "never tile this, always float it attached to whatever it's
    // transient for" - see XConnection::IsFloatingWindowType(). NORMAL
    // is interned too even though it's never checked against directly:
    // its absence (a window with no recognised type at all) is what
    // EWMH says to treat as NORMAL anyway, so WindowManager::Manage()
    // never needs to compare against it - it's here purely so
    // `kohikoctl`/debugging output and any future rule matching has it
    // available without a second round of XInternAtom calls.
    Atom NET_WM_WINDOW_TYPE;
    Atom NET_WM_WINDOW_TYPE_NORMAL;
    Atom NET_WM_WINDOW_TYPE_DIALOG;
    Atom NET_WM_WINDOW_TYPE_UTILITY;
    Atom NET_WM_WINDOW_TYPE_SPLASH;
    Atom NET_WM_WINDOW_TYPE_TOOLBAR;
    Atom NET_WM_WINDOW_TYPE_POPUP_MENU;
    Atom NET_WM_WINDOW_TYPE_DROPDOWN_MENU;
    Atom NET_WM_WINDOW_TYPE_MENU;
    Atom NET_WM_WINDOW_TYPE_DOCK;

    // A transient toast/status popup - see XConnection::
    // IsNotificationWindowType() and NotificationPopup::Create() (the
    // only thing in this codebase that currently sets this on windows
    // it creates itself). Interned unconditionally, the same as every
    // other type atom above, not gated on whether NotificationCenter
    // happens to be in use this session.
    Atom NET_WM_WINDOW_TYPE_NOTIFICATION;

    // _XEMBED_INFO - required by the freedesktop XEmbed spec to be set
    // on any window that wants to be embedded into someone else's
    // window rather than managed as a normal top-level one, *before*
    // that window is ever mapped. The System Tray Protocol's icon
    // windows are the only XEmbed clients Kohiko currently creates
    // itself (see TrayIconClient::Create()), but this is a check on
    // the property, not on any specific application - see
    // XConnection::IsXEmbedWindow()'s own comment.
    Atom XEMBED_INFO;

    Atom WM_WINDOW_ROLE;

    Atom UTF8_STRING;

private:

    XConnection& m_connection;

};

}
