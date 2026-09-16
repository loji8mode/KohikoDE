#include "XAtoms.h"

#include "XConnection.h"

namespace Kohiko
{

XAtoms::XAtoms(
    XConnection& connection)
    :
    m_connection(connection)
{
}

void XAtoms::Initialize()
{
    Display* display = m_connection.GetDisplay();

    WM_PROTOCOLS            = XInternAtom(display, "WM_PROTOCOLS", False);
    WM_DELETE_WINDOW         = XInternAtom(display, "WM_DELETE_WINDOW", False);

    NET_SUPPORTED            = XInternAtom(display, "_NET_SUPPORTED", False);
    NET_SUPPORTING_WM_CHECK  = XInternAtom(display, "_NET_SUPPORTING_WM_CHECK", False);
    NET_CLIENT_LIST          = XInternAtom(display, "_NET_CLIENT_LIST", False);

    NET_ACTIVE_WINDOW        = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    NET_WM_STATE             = XInternAtom(display, "_NET_WM_STATE", False);
    NET_WM_STATE_FULLSCREEN  = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    NET_CURRENT_DESKTOP      = XInternAtom(display, "_NET_CURRENT_DESKTOP", False);
    NET_WM_DESKTOP           = XInternAtom(display, "_NET_WM_DESKTOP", False);
    NET_WM_NAME              = XInternAtom(display, "_NET_WM_NAME", False);
    NET_WM_PID               = XInternAtom(display, "_NET_WM_PID", False);

    NET_WM_WINDOW_TYPE            = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
    NET_WM_WINDOW_TYPE_NORMAL     = XInternAtom(display, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    NET_WM_WINDOW_TYPE_DIALOG     = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    NET_WM_WINDOW_TYPE_UTILITY    = XInternAtom(display, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    NET_WM_WINDOW_TYPE_SPLASH     = XInternAtom(display, "_NET_WM_WINDOW_TYPE_SPLASH", False);
    NET_WM_WINDOW_TYPE_TOOLBAR    = XInternAtom(display, "_NET_WM_WINDOW_TYPE_TOOLBAR", False);
    NET_WM_WINDOW_TYPE_POPUP_MENU = XInternAtom(display, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
    NET_WM_WINDOW_TYPE_DROPDOWN_MENU = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", False);
    NET_WM_WINDOW_TYPE_MENU       = XInternAtom(display, "_NET_WM_WINDOW_TYPE_MENU", False);
    NET_WM_WINDOW_TYPE_DOCK       = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DOCK", False);

    XEMBED_INFO              = XInternAtom(display, "_XEMBED_INFO", False);

    WM_WINDOW_ROLE           = XInternAtom(display, "WM_WINDOW_ROLE", False);

    UTF8_STRING              = XInternAtom(display, "UTF8_STRING", False);
}

}
