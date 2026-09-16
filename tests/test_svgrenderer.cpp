// Regression/safety test for SvgRenderer - the direct librsvg/Cairo
// rendering path .svg/.svgz icons go through instead of Imlib2's own
// SVG loader plugin (see SvgRenderer.h's own comment for the full
// "why": a demonstrated reliability problem in Imlib2's SVG-to-pixmap
// bridge specifically, isolated away from librsvg itself, from this
// module's own approach, and from the SVG files involved).
//
// CanHandle() is pure string logic and always runs. RenderToPixmaps()
// needs a real X11 connection (it creates real Pixmaps) - same
// "gracefully skip without a real, even if headless/Xvfb, $DISPLAY"
// deal as test_monitormanager/test_windowclassification, so this is
// kept out of `make test` the same way (see "test-svgrenderer" in the
// Makefile).
//
// The stress section is the actual safety regression test: it
// reproduces, as closely as a portable/self-contained test
// reasonably can, the exact adversarial pattern that reliably drove
// Imlib2's own SVG loader into producing X11 BadPixmap errors during
// this investigation - many different SVG files, loaded, rendered,
// and freed in sequence within one process, some of them repeatedly -
// and asserts zero X protocol errors result. Uses small, synthetic,
// self-contained SVGs generated into a temp directory rather than
// depending on any particular icon theme being installed, matching
// how test_iconresolver/test_desktopentry avoid depending on real
// system state too.

#include "SvgRenderer.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace Kohiko;

namespace
{

int g_pass = 0;

void Check(bool condition, const char* what)
{
    if (condition)
    {
        ++g_pass;
        std::printf("  PASS: %s\n", what);
    }
    else
    {
        std::printf("  FAIL: %s\n", what);
        std::exit(1);
    }
}

int g_xErrorCount = 0;

int CountingErrorHandler(Display*, XErrorEvent*)
{
    ++g_xErrorCount;
    return 0;
}

// A handful of distinct, deliberately-varied small SVGs (different
// element types, different viewBoxes, different fill rules) - not
// trying to reproduce any specific real icon theme's files, just
// giving the stress test below several genuinely different documents
// to cycle through, the same way a tray widget cycles through several
// different real status icons over its lifetime.
void WriteTestSvgs(const std::filesystem::path& dir, std::vector<std::filesystem::path>& outPaths)
{
    std::filesystem::create_directories(dir);

    const char* kDocs[] = {
        R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><circle cx="12" cy="12" r="8" fill="black"/></svg>)",
        R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><rect x="4" y="4" width="16" height="16" fill="black" fill-opacity="0.7"/></svg>)",
        R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><path d="M4 20 L12 4 L20 20 Z" fill="black"/></svg>)",
        R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><ellipse cx="12" cy="12" rx="10" ry="6" fill="black"/><circle cx="12" cy="12" r="3" fill="white"/></svg>)",
        R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><polygon points="12,2 22,20 2,20" fill="black" fill-opacity="0.5"/></svg>)",
        R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><line x1="2" y1="2" x2="22" y2="22" stroke="black" stroke-width="3"/><line x1="22" y1="2" x2="2" y2="22" stroke="black" stroke-width="3"/></svg>)",
    };

    int index = 0;

    for (const char* doc : kDocs)
    {
        std::filesystem::path path = dir / ("icon" + std::to_string(index++) + ".svg");
        std::ofstream(path) << doc;
        outPaths.push_back(path);
    }
}

}

int main()
{
    std::printf("SvgRenderer::CanHandle() tests (pure logic, no display needed):\n");
    Check(SvgRenderer::CanHandle("/foo/bar.svg"), "a .svg path is handled");
    Check(SvgRenderer::CanHandle("/foo/bar.svgz"), "a .svgz path is handled");
    Check(SvgRenderer::CanHandle("/foo/bar.SVG"), "extension matching is case-insensitive (.SVG)");
    Check(SvgRenderer::CanHandle("/foo/bar.SvGz"), "extension matching is case-insensitive (.SvGz)");
    Check(!SvgRenderer::CanHandle("/foo/bar.png"), "a .png path is not handled");
    Check(!SvgRenderer::CanHandle("/foo/bar"), "a path with no extension at all is not handled");
    Check(!SvgRenderer::CanHandle(""), "an empty path is not handled");

    const char* displayEnv = std::getenv("DISPLAY");

    if (!displayEnv || displayEnv[0] == '\0')
    {
        std::printf("\nNo $DISPLAY set - skipping RenderToPixmaps() checks "
                     "(needs a real, even if headless/Xvfb, X server).\n");
        std::printf("\n%d CHECKS PASSED (CanHandle() only).\n", g_pass);
        return 0;
    }

    XSetErrorHandler(CountingErrorHandler); // see this test's own header comment for why this is scoped to the test, not testing something SvgRenderer itself needs to guard against

    Display* display = XOpenDisplay(displayEnv);

    if (!display)
    {
        std::printf("\nCould not open display \"%s\" - skipping.\n", displayEnv);
        std::printf("\n%d CHECKS PASSED (CanHandle() only).\n", g_pass);
        return 0;
    }

    int screen = DefaultScreen(display);
    Window win = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 24, 24, 0, 0, 0);
    XMapWindow(display, win);
    XSync(display, False);

    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "kohiko-test-svgrenderer";
    std::filesystem::remove_all(tempDir);

    std::vector<std::filesystem::path> svgPaths;
    WriteTestSvgs(tempDir, svgPaths);

    std::printf("\nSvgRenderer::RenderToPixmaps() tests (real X connection: %s):\n", displayEnv);

    std::printf("-- A single real render produces a usable pixmap and a non-degenerate mask --\n");
    {
        Pixmap pm = None, mask = None;
        bool ok = SvgRenderer::RenderToPixmaps(display, DefaultVisual(display, screen),
            DefaultDepth(display, screen), win, svgPaths[0].string(), 18, pm, mask);

        Check(ok, "rendering a valid SVG succeeds");
        Check(pm != None, "a real colour Pixmap is returned");
        Check(mask != None, "a real mask Pixmap is returned");

        if (ok)
        {
            XImage* maskImage = XGetImage(display, mask, 0, 0, 18, 18, 1, XYPixmap);
            int setBits = 0;

            for (int y = 0; y < 18; ++y)
                for (int x = 0; x < 18; ++x)
                    if (XGetPixel(maskImage, x, y))
                        ++setBits;

            XDestroyImage(maskImage);

            // Neither fully transparent (a circle centred in its own
            // viewBox has to cover *some* of an 18x18 render) nor
            // fully opaque (a filled circle, not a filled square, has
            // to leave *some* corner pixels out) - a loose sanity
            // check that this rendered an actual shape rather than,
            // say, an uninitialized or all-one/all-zero buffer.
            Check(setBits > 0 && setBits < 18 * 18,
                "the mask is a real shape (not degenerate all-transparent or all-opaque)");

            XFreePixmap(display, pm);
            XFreePixmap(display, mask);
        }
    }

    std::printf("\n-- A nonexistent file fails cleanly, no X errors --\n");
    {
        Pixmap pm = None, mask = None;
        int errorsBefore = g_xErrorCount;
        bool ok = SvgRenderer::RenderToPixmaps(display, DefaultVisual(display, screen),
            DefaultDepth(display, screen), win, "/nonexistent/path/does-not-exist.svg", 18, pm, mask);
        Check(!ok, "a nonexistent file is rejected rather than crashing or fabricating a result");
        Check(g_xErrorCount == errorsBefore, "...without generating any X protocol errors along the way");
    }

    std::printf("\n-- Stress: many different SVGs, loaded/rendered/freed repeatedly in one process --\n");
    {
        // The actual safety regression check: this exact shape of
        // workload - many *different* SVG-sourced pixmap pairs,
        // created and destroyed in sequence within a single process,
        // some documents revisited multiple times at different sizes
        // - is what reproducibly broke Imlib2's own SVG loader during
        // this investigation (see SvgRenderer.h's comment). Zero
        // tolerance here: even one X error would mean this path can
        // still leave a tray/audio/network/Bluetooth process exposed
        // to exactly the failure mode this fix exists to close off.
        int sizes[] = {14, 16, 18, 20, 22, 24, 32};
        int totalRenders = 0;

        for (int round = 0; round < 30; ++round)
        {
            for (std::size_t i = 0; i < svgPaths.size(); ++i)
            {
                int size = sizes[(round * 7 + static_cast<int>(i)) % 7];
                Pixmap pm = None, mask = None;

                if (SvgRenderer::RenderToPixmaps(display, DefaultVisual(display, screen),
                        DefaultDepth(display, screen), win, svgPaths[i].string(), size, pm, mask))
                {
                    if (pm != None) XFreePixmap(display, pm);
                    if (mask != None) XFreePixmap(display, mask);
                }

                ++totalRenders;
            }
        }

        XSync(display, False);

        std::printf("  %d render/free cycles across %zu distinct SVGs, varying sizes\n", totalRenders, svgPaths.size());
        Check(g_xErrorCount == 0, "zero X protocol errors across the entire stress sequence");
    }

    std::filesystem::remove_all(tempDir);
    XCloseDisplay(display);

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    return 0;
}
