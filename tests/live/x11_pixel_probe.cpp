// Tiny X11 client that reads REAL pixels back from the X server - what is
// actually visible on screen after stacking, clipping and painting - rather
// than trusting any window manager's internal bookkeeping. Used by the live
// tests to answer "is window A actually visible there, or is something on
// top of it?" and "did the taskbar's highlighted workspace really change?".
//
//   x11_pixel_probe pixel X Y            -> prints the RRGGBB at screen (X,Y)
//   x11_pixel_probe checksum X Y W H     -> prints a checksum of that screen rect
//   x11_pixel_probe count RRGGBB X Y W H -> number of pixels in the rect that
//                                           are exactly RRGGBB
//
// Reads from the ROOT window, which on an uncomposited X server is the
// composite of every mapped window as actually stacked.

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

static XImage* Grab(Display* d, int x, int y, int w, int h)
{
    return XGetImage(d, DefaultRootWindow(d), x, y, w, h, AllPlanes, ZPixmap);
}

int main(int argc, char** argv)
{
    Display* d = XOpenDisplay(nullptr);

    if (!d || argc < 2)
    {
        std::fprintf(stderr, "usage: x11_pixel_probe pixel X Y | checksum X Y W H | count RRGGBB X Y W H\n");
        return 2;
    }

    const char* mode = argv[1];

    if (!std::strcmp(mode, "pixel") && argc == 4)
    {
        XImage* image = Grab(d, std::atoi(argv[2]), std::atoi(argv[3]), 1, 1);
        if (!image) return 1;
        std::printf("%06lx\n", XGetPixel(image, 0, 0) & 0xFFFFFFUL);
        XDestroyImage(image);
        return 0;
    }

    if (!std::strcmp(mode, "checksum") && argc == 6)
    {
        int w = std::atoi(argv[4]), h = std::atoi(argv[5]);
        XImage* image = Grab(d, std::atoi(argv[2]), std::atoi(argv[3]), w, h);
        if (!image) return 1;
        unsigned long hash = 1469598103934665603UL; // FNV-1a
        for (int yy = 0; yy < h; ++yy)
            for (int xx = 0; xx < w; ++xx)
                hash = (hash ^ (XGetPixel(image, xx, yy) & 0xFFFFFFUL)) * 1099511628211UL;
        std::printf("%016lx\n", hash);
        XDestroyImage(image);
        return 0;
    }

    if (!std::strcmp(mode, "count") && argc == 7)
    {
        unsigned long want = std::strtoul(argv[2], nullptr, 16);
        int w = std::atoi(argv[5]), h = std::atoi(argv[6]);
        XImage* image = Grab(d, std::atoi(argv[3]), std::atoi(argv[4]), w, h);
        if (!image) return 1;
        long n = 0;
        for (int yy = 0; yy < h; ++yy)
            for (int xx = 0; xx < w; ++xx)
                n += (XGetPixel(image, xx, yy) & 0xFFFFFFUL) == want;
        std::printf("%ld\n", n);
        XDestroyImage(image);
        return 0;
    }

    std::fprintf(stderr, "bad arguments\n");
    return 2;
}
