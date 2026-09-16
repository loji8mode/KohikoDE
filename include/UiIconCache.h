#pragma once

#include "IconResolver.h"

#include <X11/Xlib.h>

#include <map>
#include <string>

namespace Kohiko
{

// Resolves an icon name to a file (via the existing IconResolver) and
// renders it to a cached, size-specific X Pixmap+mask pair - through
// SvgRenderer (librsvg/Cairo, directly) for .svg/.svgz files, and
// through Imlib2 (imlib_context_set_*/imlib_render_pixmaps_for_
// whole_image_at_size(), the same sequence Launcher.cpp already uses
// for its own results list - see that file's DrawIcon() for the
// original of this pattern) for everything else - pulled out here so
// kohiko-audio/network/bluetooth's device-list icons and the three
// tray widgets' status icons don't each reimplement it - the "icon
// handling" entry on the shared-infrastructure list. See
// SvgRenderer.h's own comment for why SVGs specifically get a
// different path than every other format here.
class UiIconCache
{
public:

    UiIconCache(Display* display, Visual* visual, Colormap colormap, Window contextDrawable);
    ~UiIconCache();

    UiIconCache(const UiIconCache&) = delete;
    UiIconCache& operator=(const UiIconCache&) = delete;

    // Looks up (rendering and caching on first use) `iconName` at
    // `size` pixels square. Returns false (leaving outPixmap/outMask
    // untouched) if no theme icon could be resolved at all - callers
    // should fall back to drawing nothing or a simple shape rather
    // than treat this as an error, same contract as IconResolver
    // itself. `outMask` is a 1-bit shape mask suitable for
    // XSetClipMask/XCopyArea-with-mask (None if the source image has
    // no alpha channel, i.e. is fully opaque).
    bool Get(const std::string& iconName, int size, Pixmap& outPixmap, Pixmap& outMask);

private:

    Display* m_display;
    Visual* m_visual;
    Colormap m_colormap;
    Window m_contextDrawable;
    int m_depth;

    IconResolver m_resolver;

    struct CachedIcon
    {
        Pixmap pixmap = None;
        Pixmap mask = None;
    };

    std::map<std::string, CachedIcon> m_cache; // key: iconName + ":" + size
};

}
