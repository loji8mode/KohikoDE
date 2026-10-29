#include "EventDispatcher.h"

#include "KeyboardManager.h"
#include "MouseManager.h"
#include "WindowManager.h"

namespace Kohiko
{

EventDispatcher::EventDispatcher(
    WindowManager& windowManager,
    KeyboardManager& keyboard,
    MouseManager& mouse)
    :
    m_windowManager(windowManager),
    m_keyboard(keyboard),
    m_mouse(mouse)
{
}

void EventDispatcher::Dispatch(
    const XEvent& event)
{
    // XRandR's event numbers (screen/output/CRTC change notifications)
    // are only known at runtime - XRRQueryExtension's event base, set
    // up in MonitorManager::Initialize() - not fixed constants like
    // every case below, so they can't be a `case` label in the switch
    // itself; check for them first instead. IsMonitorEvent() is always
    // false (and this a guaranteed no-op) when XRandR wasn't available
    // at build time.
    if (m_windowManager.IsMonitorEvent(event))
    {
        m_windowManager.HandleMonitorEvent(event);
        return;
    }

    switch (event.type)
    {
        case MapRequest:
            m_windowManager.HandleMapRequest(event.xmaprequest);
            break;

        case ConfigureRequest:
            m_windowManager.HandleConfigureRequest(event.xconfigurerequest);
            break;

        case UnmapNotify:
            m_windowManager.HandleUnmapNotify(event.xunmap);
            break;

        case DestroyNotify:
            m_windowManager.HandleDestroyNotify(event.xdestroywindow);
            break;

        case EnterNotify:
            m_windowManager.HandleEnterNotify(event.xcrossing);
            break;

        case PropertyNotify:
            m_windowManager.HandlePropertyNotify(event.xproperty);
            break;

        // Every managed window has FocusChangeMask selected (see
        // WindowManager::Manage()), so this fires no matter *how* a
        // window ends up focused - through our own SetInputFocus calls
        // as much as through a client calling XSetInputFocus on itself
        // directly, which nothing in the X protocol stops a client
        // from doing regardless of what the window manager wants. This
        // is what lets WindowManager notice and immediately hand focus
        // back to the Launcher/Notepad when that happens (see
        // WindowManager::HandleFocusIn()) instead of the modal quietly
        // going deaf to the keyboard.
        case FocusIn:
            m_windowManager.HandleFocusIn(event.xfocus);
            break;

        case Expose:
            m_windowManager.HandleExpose(event.xexpose);
            break;

        case ClientMessage:
            m_windowManager.HandleClientMessage(event.xclient);
            break;

        case KeyPress:

            // Key routing, in order of authority:
            //
            //  1. A locked screen owns the keyboard outright - not
            //     even a configured shortcut may run behind it.
            //  2. A key combination the user BOUND is a global
            //     shortcut, wherever input focus is - including while
            //     the Launcher/Notepad is open. These arrive here
            //     because Kohiko XGrabKey()s every configured
            //     combination on the root window, so the X server
            //     delivers them to us instead of to the focused
            //     window; they are never "typed into" anything.
            //     (Before this, the modal took first refusal on every
            //     key, so Super+3 in the Launcher inserted a "3"
            //     instead of switching workspace.) Asking the
            //     existing keybinding table - rather than listing
            //     special keys in the modal - is what keeps this
            //     exact: a plain "3" is not a binding, so it still
            //     reaches the Launcher as text.
            //  3. Everything else is input for whichever modal holds
            //     focus (a plain conditional here, never an
            //     XGrabKeyboard - see WindowManager::Manage()'s
            //     Super+Q hardening notes).
            if (m_windowManager.IsScreenLocked())
            {
                m_windowManager.HandleModalKeyPress(event.xkey);
                break;
            }

            if (m_keyboard.HandleKeyPress(event.xkey))
                break;

            m_windowManager.HandleModalKeyPress(event.xkey);
            break;

        case ButtonPress:

    if (event.xbutton.window ==
        m_windowManager.LauncherWindowId())
    {
        m_windowManager.HandleLauncherButtonPress(
            event.xbutton);
    }
    else if (m_windowManager.HandleBarOrPowerMenuButtonPress(
        event.xbutton))
    {
        // Handled - either the bar's power button, or a click
        // landing inside the already-open PowerMenu itself.
    }
    else if (event.xbutton.state & Mod4Mask)
    {
        m_mouse.HandlePress(event.xbutton);
    }
    else
    {
        m_windowManager.HandleButtonPressOnClient(
            event.xbutton);
    }

    break;

        case ButtonRelease:
            m_mouse.HandleRelease(event.xbutton);
            break;

        case MotionNotify:
            m_mouse.HandleMotion(event.xmotion);
            break;

        default:
            break;
    }
}

}
