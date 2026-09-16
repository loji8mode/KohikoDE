#include "XConnection.h"

#include "XAtoms.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <clocale>

namespace Kohiko
{

XConnection* XConnection::s_instance = nullptr;

XConnection::XConnection()
{
}

XConnection::~XConnection()
{
    Disconnect();
}

bool XConnection::Connect()
{
    // Required before any other Xlib call for the UTF-8 side of things
    // to work at all: Xutf8LookupString/Xutf8DrawString/XCreateFontSet
    // key off the current locale, and without this the process stays
    // in the "C" locale regardless of the user's actual LANG/LC_*
    // environment, which silently breaks multi-byte text input,
    // rendering, and font-set selection. setlocale(..., "") pulls the
    // locale from the environment; XSetLocaleModifiers("") does the
    // same for the X input-method modifiers, and both must happen
    // before XOpenDisplay().
    std::setlocale(LC_ALL, "");
    XSetLocaleModifiers("");

    m_display = XOpenDisplay(nullptr);

    if (!m_display)
    {
        m_lastError = "Cannot open display.";
        return false;
    }

    s_instance = this;

    m_screen = DefaultScreen(m_display);
    m_root = RootWindow(m_display, m_screen);

    return true;
}

bool XConnection::BecomeWindowManager()
{
    XSetErrorHandler(ErrorHandler);

    XSelectInput(
        m_display,
        m_root,
        SubstructureRedirectMask |
        SubstructureNotifyMask |
        StructureNotifyMask |
        PropertyChangeMask |
        EnterWindowMask |
        LeaveWindowMask |
        // Multi-monitor: lets WindowManager track the pointer's
        // current monitor even while it's gliding over bare desktop
        // between two monitors (a single root window spans every
        // monitor - RandR outputs are regions within it, not separate
        // X11 windows - so EnterNotify/LeaveNotify alone can never see
        // that particular crossing; see WindowManager::
        // HandlePointerMotion()). Harmless to select unconditionally:
        // MouseManager only acts on these events when no Super+drag is
        // in progress, and EventLoop already compresses a burst of
        // MotionNotify for the same window down to the latest one.
        PointerMotionMask
    );

    XSync(m_display, False);

    if (m_wmExists)
    {
        m_lastError = "Window manager already running.";
        return false;
    }

    return true;
}

void XConnection::Disconnect()
{
    if (!m_display)
        return;

    XCloseDisplay(m_display);
    m_display = nullptr;
}

Display* XConnection::GetDisplay() const
{
    return m_display;
}

::Window XConnection::Root() const
{
    return m_root;
}

Point XConnection::QueryPointer() const
{
    ::Window root = None;
    ::Window child = None;
    int rootX = 0;
    int rootY = 0;
    int winX = 0;
    int winY = 0;
    unsigned int mask = 0;

    // Failure here (the pointer isn't on this screen at all) is
    // vanishingly unlikely in a single-screen X11 session, and just
    // leaves the result at the origin - callers already treat "no
    // monitor claims this point" as a safe no-op (see
    // WindowManager::UpdateFocusedMonitorFromPointer()).
    XQueryPointer(
        m_display, m_root,
        &root, &child,
        &rootX, &rootY, &winX, &winY,
        &mask);

    return Point{rootX, rootY};
}

void XConnection::WarpPointer(int x, int y)
{
    // Root-relative, matching QueryPointer()'s own return coordinates
    // (root_x/root_y) - src_w=None means "don't confine to any
    // particular source window", the usual form for an absolute,
    // screen-wide warp.
    XWarpPointer(m_display, None, m_root, 0, 0, 0, 0, x, y);
}

int XConnection::Screen() const
{
    return m_screen;
}

std::string XConnection::DisplayName() const
{
    if (!m_display)
        return std::string();

    // XDisplayString() reports the name actually in effect (Connect()
    // passes nullptr to XOpenDisplay(), which resolves via $DISPLAY
    // itself) - reading it back here rather than re-reading getenv()
    // guarantees we report exactly the session this connection is on.
    return XDisplayString(m_display);
}

int XConnection::ConnectionFd() const
{
    return ConnectionNumber(m_display);
}

void XConnection::Flush()
{
    XFlush(m_display);
}

void XConnection::Sync()
{
    XSync(m_display, False);
}

void XConnection::MapWindow(::Window window)
{
    XMapWindow(m_display, window);
}

void XConnection::UnmapWindow(::Window window)
{
    XUnmapWindow(m_display, window);
}

void XConnection::DestroyWindow(::Window window)
{
    XDestroyWindow(m_display, window);
}

void XConnection::CloseWindow(::Window window, const XAtoms& atoms)
{
    Atom* protocols = nullptr;
    int count = 0;
    bool supportsDelete = false;

    if (XGetWMProtocols(m_display, window, &protocols, &count))
    {
        for (int i = 0; i < count; ++i)
        {
            if (protocols[i] == atoms.WM_DELETE_WINDOW)
            {
                supportsDelete = true;
                break;
            }
        }

        if (protocols)
            XFree(protocols);
    }

    if (supportsDelete)
    {
        XEvent event{};
        event.type = ClientMessage;
        event.xclient.window = window;
        event.xclient.message_type = atoms.WM_PROTOCOLS;
        event.xclient.format = 32;
        event.xclient.data.l[0] = static_cast<long>(atoms.WM_DELETE_WINDOW);
        event.xclient.data.l[1] = CurrentTime;

        XSendEvent(m_display, window, False, NoEventMask, &event);
    }
    else
    {
        XKillClient(m_display, window);
    }
}

void XConnection::MoveResizeWindow(::Window window, const Rect& rect)
{
    XMoveResizeWindow(
        m_display,
        window,
        rect.x,
        rect.y,
        static_cast<unsigned int>(rect.width > 0 ? rect.width : 1),
        static_cast<unsigned int>(rect.height > 0 ? rect.height : 1)
    );
}

void XConnection::MoveWindowTo(::Window window, int x, int y)
{
    XMoveWindow(m_display, window, x, y);
}

void XConnection::SetBorderWidth(::Window window, int width)
{
    XSetWindowBorderWidth(m_display, window, static_cast<unsigned int>(width));
}

void XConnection::SetBorderColor(::Window window, unsigned long pixel)
{
    XSetWindowBorder(m_display, window, pixel);
}

void XConnection::Raise(::Window window)
{
    XRaiseWindow(m_display, window);
}

void XConnection::Lower(::Window window)
{
    XLowerWindow(m_display, window);
}

void XConnection::SetInputFocus(::Window window)
{
    XSetInputFocus(m_display, window, RevertToPointerRoot, CurrentTime);
}

void XConnection::SelectInputFor(::Window window, long mask)
{
    XSelectInput(m_display, window, mask);
}

void XConnection::SendConfigureNotify(::Window window, const Rect& rect, int borderWidth)
{
    XConfigureEvent event{};

    event.type = ConfigureNotify;
    event.event = window;
    event.window = window;
    event.x = rect.x;
    event.y = rect.y;
    event.width = rect.width;
    event.height = rect.height;
    event.border_width = borderWidth;
    event.above = None;
    event.override_redirect = False;

    XSendEvent(m_display, window, False, StructureNotifyMask, reinterpret_cast<XEvent*>(&event));
}

void XConnection::ClearArea(::Window window)
{
    // width/height 0 means "to the edge of the window" from x,y - so
    // 0,0,0,0 is the whole window. exposures=True is what actually
    // asks the server to generate the Expose event(s); without it this
    // would just repaint the window's background and stop there.
    XClearArea(m_display, window, 0, 0, 0, 0, True);
}

void XConnection::ConfigureWindowRaw(::Window window, XWindowChanges changes, unsigned int valueMask)
{
    XConfigureWindow(m_display, window, valueMask, &changes);
}

// --- EWMH --------------------------------------------------------------------

::Window XConnection::InitializeEwmhSupport(const XAtoms& atoms, const std::string& wmName)
{
    // Plenty of EWMH-aware tools - flameshot's screenshot overlay
    // among them - check _NET_SUPPORTING_WM_CHECK before trusting
    // _NET_CLIENT_LIST/_NET_ACTIVE_WINDOW at all, falling back to
    // cruder XQueryTree-based window discovery otherwise. Per the
    // spec, that property must point at a real window which carries
    // the *same* property pointing back at itself - that round trip
    // is how a client tells a live compliant WM apart from a stale
    // property some previous, now-dead, WM left sitting on the root
    // window.
    ::Window checkWindow = XCreateSimpleWindow(m_display, m_root, -1, -1, 1, 1, 0, 0, 0);

    XChangeProperty(
        m_display, checkWindow, atoms.NET_SUPPORTING_WM_CHECK, XA_WINDOW, 32,
        PropModeReplace, reinterpret_cast<unsigned char*>(&checkWindow), 1);

    XChangeProperty(
        m_display, checkWindow, atoms.NET_WM_NAME, atoms.UTF8_STRING, 8,
        PropModeReplace, reinterpret_cast<const unsigned char*>(wmName.c_str()),
        static_cast<int>(wmName.size()));

    XChangeProperty(
        m_display, m_root, atoms.NET_SUPPORTING_WM_CHECK, XA_WINDOW, 32,
        PropModeReplace, reinterpret_cast<unsigned char*>(&checkWindow), 1);

    // Everything Kohiko actually implements - this is what a client
    // queries _NET_SUPPORTED for before relying on any one of them.
    Atom supported[] = {
        atoms.NET_SUPPORTED,
        atoms.NET_SUPPORTING_WM_CHECK,
        atoms.NET_ACTIVE_WINDOW,
        atoms.NET_CLIENT_LIST,
        atoms.NET_WM_NAME,
        atoms.NET_WM_STATE,
        atoms.NET_WM_STATE_FULLSCREEN,
        atoms.NET_CURRENT_DESKTOP,
        atoms.NET_WM_DESKTOP,
        atoms.NET_WM_WINDOW_TYPE,
        atoms.NET_WM_WINDOW_TYPE_NORMAL,
        atoms.NET_WM_WINDOW_TYPE_DIALOG,
        atoms.NET_WM_WINDOW_TYPE_UTILITY,
        atoms.NET_WM_WINDOW_TYPE_SPLASH,
        atoms.NET_WM_WINDOW_TYPE_TOOLBAR,
        atoms.NET_WM_WINDOW_TYPE_POPUP_MENU,
        atoms.NET_WM_WINDOW_TYPE_DROPDOWN_MENU,
        atoms.NET_WM_WINDOW_TYPE_MENU,
        atoms.NET_WM_PID,
    };

    XChangeProperty(
        m_display, m_root, atoms.NET_SUPPORTED, XA_ATOM, 32,
        PropModeReplace, reinterpret_cast<unsigned char*>(supported),
        static_cast<int>(sizeof(supported) / sizeof(supported[0])));

    // Starts empty/unfocused - Manage()/Unmanage()/Focus() keep both
    // properties in sync with reality from here on.
    SetClientList(atoms, {});
    SetActiveWindow(atoms, None);

    return checkWindow;
}

void XConnection::SetClientList(const XAtoms& atoms, const std::vector<::Window>& clients)
{
    // XChangeProperty accepts a null data pointer when nelements is 0
    // on every Xlib implementation this has ever run against, but
    // pointing it at a real (unused) local instead of relying on that
    // costs nothing and reads less like an accident.
    ::Window empty = 0;
    const ::Window* data = clients.empty() ? &empty : clients.data();

    XChangeProperty(
        m_display, m_root, atoms.NET_CLIENT_LIST, XA_WINDOW, 32,
        PropModeReplace,
        reinterpret_cast<const unsigned char*>(data),
        static_cast<int>(clients.size()));
}

void XConnection::SetActiveWindow(const XAtoms& atoms, ::Window window)
{
    XChangeProperty(
        m_display, m_root, atoms.NET_ACTIVE_WINDOW, XA_WINDOW, 32,
        PropModeReplace,
        reinterpret_cast<unsigned char*>(&window), 1);
}

bool XConnection::HasNetWmStateFullscreen(::Window window, const XAtoms& atoms)
{
    Atom actualType;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesLeft = 0;
    unsigned char* data = nullptr;

    bool fullscreen = false;

    if (XGetWindowProperty(
            m_display, window, atoms.NET_WM_STATE, 0, 16, False,
            XA_ATOM, &actualType, &actualFormat, &itemCount, &bytesLeft, &data
        ) == Success && data)
    {
        Atom* states = reinterpret_cast<Atom*>(data);

        for (unsigned long i = 0; i < itemCount; ++i)
        {
            if (states[i] == atoms.NET_WM_STATE_FULLSCREEN)
            {
                fullscreen = true;
                break;
            }
        }

        XFree(data);
    }

    return fullscreen;
}

void XConnection::SetNetWmState(const XAtoms& atoms, ::Window window, bool fullscreen)
{
    // Kohiko only ever tracks one EWMH state itself (fullscreen), so
    // this is a plain replace rather than a read-modify-write against
    // whatever else a client might have listed there - matching the
    // same "here's exactly what we implement" scope as the rest of
    // Kohiko's EWMH support (see InitializeEwmhSupport()).
    if (fullscreen)
    {
        Atom value = atoms.NET_WM_STATE_FULLSCREEN;

        XChangeProperty(
            m_display, window, atoms.NET_WM_STATE, XA_ATOM, 32,
            PropModeReplace, reinterpret_cast<unsigned char*>(&value), 1);
    }
    else
    {
        XDeleteProperty(m_display, window, atoms.NET_WM_STATE);
    }
}

bool XConnection::GetPreferredSize(::Window window, int& width, int& height)
{
    XSizeHints hints{};
    long suppliedMask = 0;

    if (!XGetWMNormalHints(m_display, window, &hints, &suppliedMask))
        return false;

    if ((hints.flags & (PSize | USSize)) && hints.width > 0 && hints.height > 0)
    {
        width = hints.width;
        height = hints.height;
        return true;
    }

    if ((hints.flags & PBaseSize) && hints.base_width > 0 && hints.base_height > 0)
    {
        width = hints.base_width;
        height = hints.base_height;
        return true;
    }

    return false;
}

bool XConnection::GetMinSize(::Window window, int& width, int& height)
{
    XSizeHints hints{};
    long suppliedMask = 0;

    if (!XGetWMNormalHints(m_display, window, &hints, &suppliedMask))
        return false;

    if ((hints.flags & PMinSize) && hints.min_width > 0 && hints.min_height > 0)
    {
        width = hints.min_width;
        height = hints.min_height;
        return true;
    }

    return false;
}

void XConnection::GrabKey(unsigned int keycode, unsigned int modifiers)
{
    XGrabKey(
        m_display,
        static_cast<int>(keycode),
        modifiers,
        m_root,
        True,
        GrabModeAsync,
        GrabModeAsync
    );
}

void XConnection::GrabButtonOnRoot(unsigned int button, unsigned int modifiers, long eventMask)
{
    XGrabButton(
        m_display,
        button,
        modifiers,
        m_root,
        True,
        static_cast<unsigned int>(eventMask),
        GrabModeAsync,
        GrabModeAsync,
        None,
        None
    );
}

void XConnection::UngrabAllKeys()
{
    XUngrabKey(m_display, AnyKey, AnyModifier, m_root);
}

void XConnection::UngrabAllButtonsOnRoot()
{
    XUngrabButton(m_display, AnyButton, AnyModifier, m_root);
}

void XConnection::GrabButtonOnWindow(::Window window, unsigned int button, unsigned int modifiers)
{
    XGrabButton(
        m_display,
        button,
        modifiers,
        window,
        False,
        ButtonPressMask,
        GrabModeSync,
        GrabModeAsync,
        None,
        None
    );
}

void XConnection::AllowReplayPointer()
{
    XAllowEvents(m_display, ReplayPointer, CurrentTime);
}

unsigned int XConnection::NumLockMask()
{
    if (m_numLockComputed)
        return m_numLockMask;

    m_numLockComputed = true;
    m_numLockMask = 0;

    XModifierKeymap* map = XGetModifierMapping(m_display);

    if (!map)
        return m_numLockMask;

    KeyCode numLockCode = XKeysymToKeycode(m_display, XK_Num_Lock);

    if (numLockCode != 0)
    {
        for (int mod = 0; mod < 8; ++mod)
        {
            for (int k = 0; k < map->max_keypermod; ++k)
            {
                KeyCode code = map->modifiermap[mod * map->max_keypermod + k];

                if (code == numLockCode)
                    m_numLockMask = (1u << mod);
            }
        }
    }

    XFreeModifiermap(map);

    return m_numLockMask;
}

std::vector<unsigned int> XConnection::LockVariants(unsigned int baseModifiers)
{
    unsigned int numLock = NumLockMask();

    return {
        baseModifiers,
        baseModifiers | LockMask,
        baseModifiers | numLock,
        baseModifiers | numLock | LockMask,
    };
}

KeyCode XConnection::KeysymToKeycode(KeySym keysym)
{
    return XKeysymToKeycode(m_display, keysym);
}

bool XConnection::GetWindowAttributes(::Window window, XWindowAttributes& out)
{
    return XGetWindowAttributes(m_display, window, &out) != 0;
}

std::string XConnection::GetWindowTitle(::Window window, const XAtoms& atoms)
{
    Atom actualType;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesLeft = 0;
    unsigned char* data = nullptr;

    if (XGetWindowProperty(
            m_display, window, atoms.NET_WM_NAME, 0, 1024, False,
            AnyPropertyType, &actualType, &actualFormat, &itemCount, &bytesLeft, &data
        ) == Success && data)
    {
        std::string title(reinterpret_cast<char*>(data), itemCount);
        XFree(data);

        if (!title.empty())
            return title;
    }

    char* legacyName = nullptr;

    if (XFetchName(m_display, window, &legacyName) && legacyName)
    {
        std::string title(legacyName);
        XFree(legacyName);
        return title;
    }

    return {};
}

void XConnection::GetWindowClass(::Window window, std::string& className, std::string& instanceName)
{
    XClassHint hint{};

    if (XGetClassHint(m_display, window, &hint))
    {
        if (hint.res_class)
        {
            className = hint.res_class;
            XFree(hint.res_class);
        }

        if (hint.res_name)
        {
            instanceName = hint.res_name;
            XFree(hint.res_name);
        }
    }
}

std::string XConnection::GetWindowRole(::Window window, const XAtoms& atoms)
{
    Atom actualType;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesLeft = 0;
    unsigned char* data = nullptr;

    std::string role;

    if (XGetWindowProperty(
            m_display, window, atoms.WM_WINDOW_ROLE, 0, 1024, False,
            AnyPropertyType, &actualType, &actualFormat, &itemCount, &bytesLeft, &data
        ) == Success && data)
    {
        role.assign(reinterpret_cast<char*>(data), itemCount);
        XFree(data);
    }

    return role;
}

long XConnection::GetWindowPid(::Window window, const XAtoms& atoms)
{
    Atom actualType;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesLeft = 0;
    unsigned char* data = nullptr;

    long pid = 0;

    if (XGetWindowProperty(
            m_display, window, atoms.NET_WM_PID, 0, 1, False,
            XA_CARDINAL, &actualType, &actualFormat, &itemCount, &bytesLeft, &data
        ) == Success && data)
    {
        if (itemCount > 0)
            pid = static_cast<long>(*reinterpret_cast<unsigned long*>(data));

        XFree(data);
    }

    return pid;
}

bool XConnection::GetTransientFor(::Window window, ::Window& owner)
{
    ::Window transient = 0;

    if (XGetTransientForHint(m_display, window, &transient) && transient != 0)
    {
        owner = transient;
        return true;
    }

    return false;
}

bool XConnection::IsFloatingWindowType(::Window window, const XAtoms& atoms)
{
    Atom actualType;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesLeft = 0;
    unsigned char* data = nullptr;

    bool isFloatingType = false;

    if (XGetWindowProperty(
            m_display, window, atoms.NET_WM_WINDOW_TYPE, 0, 16, False,
            XA_ATOM, &actualType, &actualFormat, &itemCount, &bytesLeft, &data
        ) == Success && data)
    {
        Atom* types = reinterpret_cast<Atom*>(data);

        // _NET_WM_WINDOW_TYPE can legally carry more than one atom
        // (most specific first, per the spec) - a window is one of
        // Kohiko's "always float" types if *any* entry names one,
        // regardless of position, the same tolerant approach the
        // single-type DIALOG check this replaced already took.
        for (unsigned long i = 0; i < itemCount; ++i)
        {
            if (types[i] == atoms.NET_WM_WINDOW_TYPE_DIALOG ||
                types[i] == atoms.NET_WM_WINDOW_TYPE_UTILITY ||
                types[i] == atoms.NET_WM_WINDOW_TYPE_SPLASH ||
                types[i] == atoms.NET_WM_WINDOW_TYPE_TOOLBAR ||
                types[i] == atoms.NET_WM_WINDOW_TYPE_POPUP_MENU ||
                types[i] == atoms.NET_WM_WINDOW_TYPE_DROPDOWN_MENU ||
                types[i] == atoms.NET_WM_WINDOW_TYPE_MENU)
            {
                isFloatingType = true;
                break;
            }
        }

        XFree(data);
    }

    return isFloatingType;
}

bool XConnection::IsDockWindowType(::Window window, const XAtoms& atoms)
{
    Atom actualType;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesLeft = 0;
    unsigned char* data = nullptr;

    bool isDock = false;

    if (XGetWindowProperty(
            m_display, window, atoms.NET_WM_WINDOW_TYPE, 0, 16, False,
            XA_ATOM, &actualType, &actualFormat, &itemCount, &bytesLeft, &data
        ) == Success && data)
    {
        Atom* types = reinterpret_cast<Atom*>(data);

        for (unsigned long i = 0; i < itemCount; ++i)
        {
            if (types[i] == atoms.NET_WM_WINDOW_TYPE_DOCK)
            {
                isDock = true;
                break;
            }
        }

        XFree(data);
    }

    return isDock;
}

bool XConnection::IsNotificationWindowType(::Window window, const XAtoms& atoms)
{
    Atom actualType;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesLeft = 0;
    unsigned char* data = nullptr;

    bool isNotification = false;

    if (XGetWindowProperty(
            m_display, window, atoms.NET_WM_WINDOW_TYPE, 0, 16, False,
            XA_ATOM, &actualType, &actualFormat, &itemCount, &bytesLeft, &data
        ) == Success && data)
    {
        Atom* types = reinterpret_cast<Atom*>(data);

        for (unsigned long i = 0; i < itemCount; ++i)
        {
            if (types[i] == atoms.NET_WM_WINDOW_TYPE_NOTIFICATION)
            {
                isNotification = true;
                break;
            }
        }

        XFree(data);
    }

    return isNotification;
}

bool XConnection::IsXEmbedWindow(::Window window, const XAtoms& atoms)
{
    Atom actualType;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesLeft = 0;
    unsigned char* data = nullptr;

    // AnyPropertyType, not atoms.XEMBED_INFO itself: the property's
    // required *type* per spec is also _XEMBED_INFO (a client is
    // supposed to set it as XChangeProperty(..., XEMBED_INFO,
    // XEMBED_INFO, 32, ...), matching what TrayIconClient::Create()
    // does), but the only thing that actually matters here is whether
    // some property under this *name* exists at all - being lenient
    // about the declared type costs nothing and means a client that
    // gets that one detail wrong still isn't accidentally tiled.
    bool present =
        XGetWindowProperty(
            m_display, window, atoms.XEMBED_INFO, 0, 2, False,
            AnyPropertyType, &actualType, &actualFormat, &itemCount, &bytesLeft, &data
        ) == Success && actualType != None;

    if (data)
        XFree(data);

    return present;
}

std::vector<::Window> XConnection::QueryChildren(::Window window)
{
    std::vector<::Window> children;

    ::Window root = 0;
    ::Window parent = 0;
    ::Window* list = nullptr;
    unsigned int count = 0;

    if (XQueryTree(m_display, window, &root, &parent, &list, &count) && list)
    {
        children.assign(list, list + count);
        XFree(list);
    }

    return children;
}

std::string XConnection::LastError() const
{
    return m_lastError;
}

int XConnection::ErrorHandler(Display*, XErrorEvent* event)
{
    if (!s_instance)
        return 0;

    if (event->error_code == BadAccess)
        s_instance->m_wmExists = true;

    return 0;
}

}
