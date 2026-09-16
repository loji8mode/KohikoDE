#pragma once

#include <X11/Xlib.h>

#include <string>

namespace Kohiko
{

// Renders an .svg/.svgz file directly to a pair of size×size X11
// Pixmaps (a colour pixmap at `depth`, and a depth-1 mask pixmap
// suitable for XSetClipMask - the exact same contract
// UiIconCache::Get() already promises its own callers, previously
// fulfilled by Imlib2's imlib_render_pixmaps_for_whole_image_at_size()
// for every icon regardless of source format), using librsvg and
// Cairo directly - NOT Imlib2's own SVG loader plugin
// (/usr/lib/.../imlib2/loaders/svg.so on a Debian-family system, which
// is itself a thin bridge to the exact same librsvg/Cairo underneath).
//
// Why bypass a loader that (going through Imlib2) uses the same
// underlying libraries anyway: real testing (see
// docs/AUDIO_NETWORK_BLUETOOTH.md's "Icon loading" section) found
// Imlib2's own SVG-to-pixmap bridge unreliable under repeated use in
// one process - a heap corruption that reproduced deterministically
// in one investigation and could not be reproduced again in a later
// one despite substantial, instrumented effort (ASan/UBSan, glibc's
// MALLOC_CHECK_, hundreds of stress cycles), plus a separate,
// highly-reproducible pattern of X11 BadPixmap errors when rapidly
// cycling through many different SVG-sourced pixmaps - both isolated
// to Imlib2's own bridging code specifically, not to librsvg itself
// (librsvg's own rsvg-convert CLI, and this module's own direct
// Cairo-surface approach, show neither failure mode against the same
// files). Rather than depend on a workaround for a bridge with a
// demonstrated reliability problem, this renders through librsvg and
// Cairo directly, using exactly the ordinary, widely-used APIs the
// rest of the Linux desktop ecosystem already renders SVGs through
// (this is genuinely the standard way - GTK, and so most of the
// desktop icon themes this is reading, render their own SVG content
// through this exact same pair of libraries), and builds the X11
// Pixmaps this returns using plain, well-understood Xlib/Cairo-xlib
// calls this project already controls end to end.
//
// PNG/XPM/other raster icon formats are NOT affected by any of this -
// they never went through Imlib2's SVG loader in the first place, and
// still go through imlib_load_image() exactly as before; this is
// purely an alternative path for the one format that showed a real
// reliability problem via its previous route.
namespace SvgRenderer
{

// Returns false (and sets neither out-param) if the file can't be
// parsed as SVG, or if `size` is not positive. On success, the caller
// owns both returned Pixmaps (via XFreePixmap) exactly as it already
// did for Imlib2-rendered ones - see UiIconCache::Get()/~UiIconCache().
bool RenderToPixmaps(
    Display* display,
    Visual* visual,
    int depth,
    Drawable contextDrawable,
    const std::string& path,
    int size,
    Pixmap& outPixmap,
    Pixmap& outMask
);

// True for a path this module can handle (currently: a ".svg" or
// ".svgz" extension, case-insensitively) - UiIconCache::Get() checks
// this to decide whether to route a resolved icon path here instead
// of to Imlib2's imlib_load_image(). A simple extension check, not a
// content sniff: every icon theme this reads from names files this
// way already (see the freedesktop Icon Theme Specification's own
// file-naming rules), and every icon this project ships its own
// assets as does too.
bool CanHandle(
    const std::string& path
);

}

}
