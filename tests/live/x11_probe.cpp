// Read-only X11 probe for the live tests - not part of Kohiko.
//
// What a window-manager test needs to *see*, independent of the WM's own
// bookkeeping (kohikoctl reports what Kohiko believes; this reports what
// the X server actually has):
//
//   x11_probe tree            top-level windows under the root, BOTTOM TO
//                             TOP (XQueryTree's order IS the stacking
//                             order): "id=0x.. state=mapped|unmapped
//                             or=0|1 x= y= w= h= bw= name=..."
//   x11_probe pixel X Y       RGB of the on-screen pixel at X,Y (XGetImage
//                             on the root: what is really visible there,
//                             i.e. the topmost mapped window's pixels)
//   x11_probe watch-pixel X Y RRGGBB MS
//                             sample the on-screen pixel every ~1 ms for
//                             up to MS ms; print every colour CHANGE with
//                             its time in ms since the command started
//                             ("t=12.4 1e1e2e"), then "END <final colour>".
//                             For measuring how long something stays
//                             blank/stale, not just whether it recovers.
//   x11_probe grab-test       from THIS (other) client, try to grab the
//                             pointer: prints "free" or "HELD". Another
//                             client's active grab (a popup menu that has
//                             not been torn down) makes it HELD - a grab
//                             attempt from the grab-holder itself would
//                             succeed and prove nothing.
//   x11_probe focus           input focus window id (0x0 = none/root)
//   x11_probe property ID NAME  prints the raw value of a property
//                             --report-events for exposure accounting)
//
// Plain Xlib, no extensions.

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <ctime>
#include <unistd.h>

static void PrintTree(Display* dpy)
{
    Window root = DefaultRootWindow(dpy);
    Window rootRet, parent, *children = nullptr;
    unsigned int count = 0;

    if (!XQueryTree(dpy, root, &rootRet, &parent, &children, &count))
        return;

    for (unsigned int i = 0; i < count; ++i)
    {
        XWindowAttributes a{};
        XGetWindowAttributes(dpy, children[i], &a);

        char* name = nullptr;
        XFetchName(dpy, children[i], &name);

        std::printf("id=0x%lx state=%s or=%d x=%d y=%d w=%d h=%d bw=%d name=%s\n",
            children[i],
            a.map_state == IsUnmapped ? "unmapped" : "mapped",
            a.override_redirect ? 1 : 0,
            a.x, a.y, a.width, a.height, a.border_width,
            name ? name : "");

        if (name) XFree(name);
    }

    if (children) XFree(children);
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: x11_probe tree|pixel|focus|property ...\n"); return 2; }

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) { std::fprintf(stderr, "cannot open display\n"); return 2; }

    std::string cmd = argv[1];
    int rc = 0;

    if (cmd == "tree")
    {
        PrintTree(dpy);
    }
    else if (cmd == "pixel" && argc >= 4)
    {
        XImage* img = XGetImage(dpy, DefaultRootWindow(dpy), std::atoi(argv[2]), std::atoi(argv[3]), 1, 1, AllPlanes, ZPixmap);
        if (!img) { std::fprintf(stderr, "XGetImage failed\n"); rc = 2; }
        else
        {
            unsigned long p = XGetPixel(img, 0, 0);
            std::printf("%02lx%02lx%02lx\n", (p >> 16) & 0xff, (p >> 8) & 0xff, p & 0xff);
            XDestroyImage(img);
        }
    }
    else if (cmd == "watch-pixel" && argc >= 6)
    {
        int px = std::atoi(argv[2]), py = std::atoi(argv[3]);
        int budgetMs = std::atoi(argv[5]);
        timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
        unsigned long last = 0xffffffffUL;
        for (;;)
        {
            timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            double ms = (now.tv_sec - t0.tv_sec) * 1000.0 + (now.tv_nsec - t0.tv_nsec) / 1e6;
            XImage* img = XGetImage(dpy, DefaultRootWindow(dpy), px, py, 1, 1, AllPlanes, ZPixmap);
            if (img)
            {
                unsigned long p = XGetPixel(img, 0, 0) & 0xffffff;
                XDestroyImage(img);
                if (p != last) { std::printf("t=%.1f abs=%.1f %06lx\n", ms, now.tv_sec * 1000.0 + now.tv_nsec / 1e6, p); last = p; }
            }
            if (ms > budgetMs) break;
            usleep(1000);
        }
        std::printf("END %06lx\n", last);
    }
    else if (cmd == "grab-test")
    {
        int r = XGrabPointer(dpy, DefaultRootWindow(dpy), False, ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
        std::printf("%s\n", r == GrabSuccess ? "free" : "HELD");
        if (r == GrabSuccess) XUngrabPointer(dpy, CurrentTime);
    }
    else if (cmd == "focus")
    {
        Window w; int revert;
        XGetInputFocus(dpy, &w, &revert);
        std::printf("0x%lx\n", w);
    }
    else if (cmd == "property" && argc >= 4)
    {
        Window w = std::strtoul(argv[2], nullptr, 0);
        Atom prop = XInternAtom(dpy, argv[3], False);
        Atom type; int fmt; unsigned long n, after; unsigned char* data = nullptr;
        if (XGetWindowProperty(dpy, w, prop, 0, 1024, False, AnyPropertyType, &type, &fmt, &n, &after, &data) == Success && data)
        {
            if (fmt == 8) std::printf("%s\n", reinterpret_cast<char*>(data));
            else if (fmt == 32) for (unsigned long i = 0; i < n; ++i) std::printf("%lu%s", reinterpret_cast<unsigned long*>(data)[i], i + 1 < n ? " " : "\n");
            XFree(data);
        }
    }
    else { std::fprintf(stderr, "bad arguments\n"); rc = 2; }

    XCloseDisplay(dpy);
    return rc;
}
