#include "SvgRenderer.h"

#include <cairo-xlib.h>
#include <cairo.h>
#include <librsvg/rsvg.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Kohiko
{
namespace SvgRenderer
{

namespace
{

// Renders `handle` into `size`×`size` of whatever `cr` draws to,
// scaled to exactly fill that square. Real-world freedesktop status/
// app icons are overwhelmingly authored square (matching their own
// viewBox), so a direct fill - rather than computing an aspect-
// preserving letterboxed fit - matches what's actually shipped
// closely enough that the difference isn't visible in practice, and
// keeps this simple; a severely non-square source SVG would come out
// stretched rather than letterboxed, which no icon this project has
// actually loaded triggers.
bool RenderInto(RsvgHandle* handle, cairo_t* cr, int size)
{
    RsvgRectangle viewport {0.0, 0.0, static_cast<double>(size), static_cast<double>(size)};
    GError* error = nullptr;

    gboolean ok = rsvg_handle_render_document(handle, cr, &viewport, &error);

    if (!ok)
    {
        if (error)
            g_error_free(error);

        return false;
    }

    return true;
}

}

bool CanHandle(const std::string& path)
{
    auto hasSuffix = [&path](const char* suffix)
    {
        std::size_t len = std::strlen(suffix);

        if (path.size() < len)
            return false;

        return std::equal(
            path.end() - static_cast<long>(len), path.end(), suffix,
            [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; }
        );
    };

    return hasSuffix(".svg") || hasSuffix(".svgz");
}

bool RenderToPixmaps(
    Display* display,
    Visual* visual,
    int depth,
    Drawable contextDrawable,
    const std::string& path,
    int size,
    Pixmap& outPixmap,
    Pixmap& outMask)
{
    if (size <= 0)
        return false;

    GError* error = nullptr;
    RsvgHandle* handle = rsvg_handle_new_from_file(path.c_str(), &error);

    if (!handle)
    {
        if (error)
            g_error_free(error);

        return false;
    }

    // --- Colour: render straight onto a Cairo surface backed by a
    // real X11 Pixmap (cairo-xlib), so Cairo itself handles every bit
    // of visual/depth-specific pixel format conversion - nothing here
    // has to know or assume anything about byte order or channel
    // layout. "Transparent" regions are left as whatever
    // XCreatePixmap happened to initialize them to; that's fine, and
    // matches how Imlib2's own equivalent function already behaved,
    // because outMask (below) is what actually determines on-screen
    // visibility via XSetClipMask - a caller drawing this pixmap
    // without applying that mask was already relying on behaviour
    // neither this nor the previous implementation promised.
    Pixmap colorPixmap = XCreatePixmap(display, contextDrawable,
        static_cast<unsigned int>(size), static_cast<unsigned int>(size),
        static_cast<unsigned int>(depth));

    cairo_surface_t* xlibSurface = cairo_xlib_surface_create(
        display, colorPixmap, visual, size, size);
    cairo_t* xlibContext = cairo_create(xlibSurface);

    bool colorOk = RenderInto(handle, xlibContext, size);

    cairo_surface_flush(xlibSurface);
    cairo_destroy(xlibContext);
    cairo_surface_destroy(xlibSurface);

    if (!colorOk)
    {
        XFreePixmap(display, colorPixmap);
        g_object_unref(handle);
        return false;
    }

    // --- Mask: render the same document a second time, into a plain
    // in-memory ARGB32 surface this time (not X11-backed - nothing
    // about a clip mask needs a round trip through the X server to
    // build), purely to read back each pixel's alpha byte. Rendering
    // twice - once for colour, once for alpha - is simpler and safer
    // than trying to extract both from a single XImage of unknown/
    // visual-dependent channel layout, at the cost of one extra,
    // still-fast vector rerender; SVG rasterization of a ~20px icon
    // is not where any real time is going to go here.
    cairo_surface_t* argbSurface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t* argbContext = cairo_create(argbSurface);

    bool maskOk = RenderInto(handle, argbContext, size);

    cairo_surface_flush(argbSurface);
    cairo_destroy(argbContext);

    g_object_unref(handle);

    if (!maskOk)
    {
        cairo_surface_destroy(argbSurface);
        XFreePixmap(display, colorPixmap);
        return false;
    }

    // CAIRO_FORMAT_ARGB32 is always a 32-bit-per-pixel, premultiplied,
    // native-endian word with alpha in the high byte - true regardless
    // of host byte order, since it's defined in terms of the *value*
    // of the 32-bit word, not its in-memory byte sequence (see Cairo's
    // own cairo_format_t documentation). Reading it back as
    // uint32_t*, on the host's own native endianness, is therefore
    // always correct - no byte-order assumption of our own involved.
    int stride = cairo_image_surface_get_stride(argbSurface);
    const unsigned char* pixels = cairo_image_surface_get_data(argbSurface);

    // XCreateBitmapFromData's own data format: XBM-style, one bit per
    // pixel, LSB of each byte is the leftmost pixel, each row padded
    // up to a whole byte - not related to, and not required to match,
    // the host's own bit/byte order (this is a fixed convention of
    // that specific Xlib helper, not a hardware-dependent one).
    int bytesPerRow = (size + 7) / 8;
    std::vector<unsigned char> bitmapData(static_cast<std::size_t>(bytesPerRow) * static_cast<std::size_t>(size), 0);

    constexpr unsigned char kAlphaThreshold = 128; // half-opaque or more counts as "shown" - a plain cutoff, matching a 1-bit mask's own binary nature (no partial transparency to preserve)

    for (int y = 0; y < size; ++y)
    {
        const std::uint32_t* row = reinterpret_cast<const std::uint32_t*>(pixels + static_cast<std::ptrdiff_t>(y) * stride);

        for (int x = 0; x < size; ++x)
        {
            unsigned char alpha = static_cast<unsigned char>(row[x] >> 24);

            if (alpha >= kAlphaThreshold)
            {
                std::size_t byteIndex = static_cast<std::size_t>(y) * static_cast<std::size_t>(bytesPerRow) + static_cast<std::size_t>(x / 8);
                bitmapData[byteIndex] |= static_cast<unsigned char>(1u << (x % 8));
            }
        }
    }

    cairo_surface_destroy(argbSurface);

    Pixmap maskPixmap = XCreateBitmapFromData(
        display, contextDrawable,
        reinterpret_cast<const char*>(bitmapData.data()),
        static_cast<unsigned int>(size), static_cast<unsigned int>(size));

    if (maskPixmap == None)
    {
        XFreePixmap(display, colorPixmap);
        return false;
    }

    outPixmap = colorPixmap;
    outMask = maskPixmap;

    return true;
}

}
}
