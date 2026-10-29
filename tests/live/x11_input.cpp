// Real (XTEST) input injection for the live tests - not part of Kohiko.
//
// The live tests used to have no way to press a key or click: the
// sandbox has no xdotool. XTEST FakeInput is what xdotool/xte use under
// the hood, and unlike XSendEvent it goes through the X server's normal
// input path - so passive key grabs (Kohiko's global shortcuts) and
// pointer grabs fire exactly as they do for a physical keyboard/mouse.
//
// libXtst's headers are not needed: the request is built with Xlib's own
// internals from the protocol header (xtestproto.h), which ships with
// x11proto.
//
// Usage:
//   x11_input key  <keysym> [mod ...]     press+release, e.g. key 3 super
//   x11_input type <text>                 press+release each character
//   x11_input click <x> <y> [button]      move, press, release (default 1)
//   x11_input move  <x> <y>
//   x11_input keydown|keyup <keysym>      hold/release one key (e.g. Super_L
//                                         around a drag - Kohiko's mouse
//                                         bindings need Super held)
//   x11_input press|release <button>      hold/release a pointer button
//   x11_input burst <keysym>...           press+release each key with NO
//                                         pause between them and a single
//                                         flush at the end: every event is
//                                         already queued in the X server
//                                         before Kohiko reads the first -
//                                         the "rapid successive input"
//                                         case (Enter immediately followed
//                                         by page/query/selection keys)
//   x11_input close <window-id>           ask a window to close the way a
//                                         window manager's [x] does: a
//                                         WM_DELETE_WINDOW ClientMessage
//   x11_input movewin <window-id> <x> <y> move a top-level window (what dragging
//                                         its title bar does; there is no window
//                                         manager in some tests)
//   x11_input wait                        just XSync (settle)
//   x11_input drag  <x1> <y1> <x2> <y2> [button]   press at 1, move, release at 2
// Modifiers: shift ctrl alt super
// Exit status 0 on success, 2 on a usage/lookup error.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xlibint.h>
#include <X11/keysym.h>
#include <X11/extensions/xtestconst.h>
#include <X11/extensions/xtestproto.h>

// Xlibint.h defines min()/max() as macros; nothing below needs them.
#undef min
#undef max

static Display* g_dpy = nullptr;
static int g_xtestOpcode = 0;

static void FakeInput(int type, int detail, int x, int y)
{
    Display* dpy = g_dpy; // the Xlibint request macros expect a variable named `dpy`
    LockDisplay(dpy);
    xXTestFakeInputReq* req;
    GetReq(XTestFakeInput, req);
    req->reqType = static_cast<CARD8>(g_xtestOpcode);
    req->xtReqType = X_XTestFakeInput;
    req->type = static_cast<CARD8>(type);
    req->detail = static_cast<CARD8>(detail);
    req->time = 0;
    req->root = None;
    req->rootX = static_cast<INT16>(x);
    req->rootY = static_cast<INT16>(y);
    req->deviceid = 0;
    UnlockDisplay(dpy);
    SyncHandle();
}

static void Settle() { XSync(g_dpy, False); usleep(25 * 1000); }

static void KeyEvent(KeyCode code, bool down)
{
    FakeInput(down ? KeyPress : KeyRelease, code, 0, 0);
}

static KeyCode CodeFor(KeySym sym)
{
    return XKeysymToKeycode(g_dpy, sym);
}

static bool ModSym(const std::string& m, KeySym& out)
{
    if (m == "shift") out = XK_Shift_L;
    else if (m == "ctrl") out = XK_Control_L;
    else if (m == "alt") out = XK_Alt_L;
    else if (m == "super") out = XK_Super_L;
    else return false;
    return true;
}

static int PressKey(KeySym sym, const std::vector<std::string>& mods)
{
    std::vector<KeyCode> modCodes;
    for (const auto& m : mods)
    {
        KeySym ms;
        if (!ModSym(m, ms)) { fprintf(stderr, "unknown modifier %s\n", m.c_str()); return 2; }
        KeyCode c = CodeFor(ms);
        if (!c) { fprintf(stderr, "no keycode for modifier %s\n", m.c_str()); return 2; }
        modCodes.push_back(c);
    }
    KeyCode code = CodeFor(sym);
    if (!code) { fprintf(stderr, "no keycode for keysym\n"); return 2; }

    for (KeyCode c : modCodes) KeyEvent(c, true);
    KeyEvent(code, true);
    KeyEvent(code, false);
    for (auto it = modCodes.rbegin(); it != modCodes.rend(); ++it) KeyEvent(*it, false);
    Settle();
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: x11_input key|type|click|move|drag ...\n"); return 2; }

    g_dpy = XOpenDisplay(nullptr);
    if (!g_dpy) { fprintf(stderr, "cannot open display\n"); return 2; }

    int ev, err;
    if (!XQueryExtension(g_dpy, "XTEST", &g_xtestOpcode, &ev, &err))
    {
        fprintf(stderr, "XTEST extension not available\n");
        return 2;
    }

    std::string cmd = argv[1];
    int rc = 0;

    if (cmd == "key" && argc >= 3)
    {
        KeySym sym = XStringToKeysym(argv[2]);
        if (sym == NoSymbol) { fprintf(stderr, "bad keysym %s\n", argv[2]); return 2; }
        std::vector<std::string> mods;
        for (int i = 3; i < argc; ++i) mods.push_back(argv[i]);
        rc = PressKey(sym, mods);
    }
    else if (cmd == "type" && argc >= 3)
    {
        for (const char* p = argv[2]; *p; ++p)
        {
            char buf[2] = { *p, 0 };
            KeySym sym = XStringToKeysym(*p == ' ' ? "space" : buf);
            if (sym == NoSymbol) { fprintf(stderr, "cannot type '%c'\n", *p); return 2; }
            std::vector<std::string> mods;
            if (*p >= 'A' && *p <= 'Z') mods.push_back("shift");
            rc = PressKey(sym, mods);
            if (rc) break;
        }
    }
    else if ((cmd == "keydown" || cmd == "keyup") && argc >= 3)
    {
        KeySym sym = XStringToKeysym(argv[2]);
        KeyCode code = sym == NoSymbol ? 0 : CodeFor(sym);
        if (!code) { fprintf(stderr, "bad keysym %s\n", argv[2]); return 2; }
        KeyEvent(code, cmd == "keydown");
        Settle();
    }
    else if ((cmd == "press" || cmd == "release") && argc >= 3)
    {
        FakeInput(cmd == "press" ? ButtonPress : ButtonRelease, atoi(argv[2]), 0, 0);
        Settle();
    }
    else if (cmd == "burst" && argc >= 3)
    {
        for (int i = 2; i < argc; ++i)
        {
            KeySym sym = XStringToKeysym(argv[i]);
            KeyCode code = sym == NoSymbol ? 0 : CodeFor(sym);
            if (!code) { fprintf(stderr, "bad keysym %s\n", argv[i]); return 2; }
            KeyEvent(code, true);
            KeyEvent(code, false);
        }
        Settle();
    }
    else if (cmd == "close" && argc >= 3)
    {
        Window target = strtoul(argv[2], nullptr, 0);
        XEvent ev{};
        ev.xclient.type = ClientMessage;
        ev.xclient.window = target;
        ev.xclient.message_type = XInternAtom(g_dpy, "WM_PROTOCOLS", False);
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = static_cast<long>(XInternAtom(g_dpy, "WM_DELETE_WINDOW", False));
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(g_dpy, target, False, NoEventMask, &ev);
        Settle();
    }
    else if (cmd == "movewin" && argc >= 5)
    {
        XMoveWindow(g_dpy, strtoul(argv[2], nullptr, 0), atoi(argv[3]), atoi(argv[4]));
        Settle();
    }
    else if (cmd == "wait")
    {
        Settle();
    }
    else if (cmd == "move" && argc >= 4)
    {
        FakeInput(MotionNotify, 0, atoi(argv[2]), atoi(argv[3]));
        Settle();
    }
    else if (cmd == "click" && argc >= 4)
    {
        int button = argc >= 5 ? atoi(argv[4]) : 1;
        FakeInput(MotionNotify, 0, atoi(argv[2]), atoi(argv[3]));
        Settle();
        FakeInput(ButtonPress, button, 0, 0);
        Settle();
        FakeInput(ButtonRelease, button, 0, 0);
        Settle();
    }
    else if (cmd == "drag" && argc >= 6)
    {
        int button = argc >= 7 ? atoi(argv[6]) : 1;
        FakeInput(MotionNotify, 0, atoi(argv[2]), atoi(argv[3]));
        Settle();
        FakeInput(ButtonPress, button, 0, 0);
        Settle();
        FakeInput(MotionNotify, 0, atoi(argv[4]), atoi(argv[5]));
        Settle();
        FakeInput(ButtonRelease, button, 0, 0);
        Settle();
    }
    else
    {
        fprintf(stderr, "bad arguments\n");
        rc = 2;
    }

    XCloseDisplay(g_dpy);
    return rc;
}
