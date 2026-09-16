#include "UiIconCache.h"

#include <Imlib2.h>

namespace Kohiko
{

UiIconCache::UiIconCache(Display* display, Visual* visual, Colormap colormap, Window contextDrawable)
    : m_display(display)
    , m_visual(visual)
    , m_colormap(colormap)
    , m_contextDrawable(contextDrawable)
    , m_resolver()
{
    // Imlib2's own internal image cache (distinct from m_cache below,
    // which is this class's own pixmap-level cache and stays exactly
    // as it was) has a real, reproducible correctness bug when a
    // process loads and frees a run of small, unrelated images in
    // succession - which is exactly this class's own access pattern
    // (a tray widget cycling through several status icons over its
    // lifetime, or an audio/network/Bluetooth window's device list
    // rendering one icon per row). Confirmed directly: on this
    // project's own Imlib2/librsvg build, loading 13 real status
    // icons back-to-back through the exact sequence Get() below uses
    // corrupts the heap partway through - but the same file loads
    // fine in isolation, and independently in plain rsvg-convert, so
    // the SVGs themselves and this class's own load/render/free
    // sequence are not at fault. Disabling Imlib2's cache (0 bytes -
    // it becomes a pure passthrough) removed the crash across all 13
    // icons in the same repeated-load sequence that reproduced it;
    // this class keeps its own m_cache precisely so that trade is
    // free - nothing here relies on Imlib2 re-serving an image it's
    // already decoded once. This is a global Imlib2 setting (shared
    // by every Imlib2 call in this process, not just this instance's
    // own), which is exactly what's needed here: every tray/device-
    // list binary using this class constructs exactly one of it, in
    // main(), before decoding any icon.
    imlib_set_cache_size(0);
}

UiIconCache::~UiIconCache()
{
    for (auto& [key, icon] : m_cache)
    {
        if (icon.pixmap != None) XFreePixmap(m_display, icon.pixmap);
        if (icon.mask != None) XFreePixmap(m_display, icon.mask);
    }
}

bool UiIconCache::Get(const std::string& iconName, int size, Pixmap& outPixmap, Pixmap& outMask)
{
    std::string key = iconName + ":" + std::to_string(size);

    auto it = m_cache.find(key);
    if (it != m_cache.end())
    {
        outPixmap = it->second.pixmap;
        outMask = it->second.mask;
        return outPixmap != None;
    }

    std::string path = m_resolver.Resolve(iconName);
    if (path.empty())
    {
        m_cache[key] = {}; // negative-cache: don't re-hit the resolver/filesystem for this name+size again
        return false;
    }

    imlib_context_set_display(m_display);
    imlib_context_set_visual(m_visual);
    imlib_context_set_colormap(m_colormap);
    imlib_context_set_drawable(m_contextDrawable);

    Imlib_Image image = imlib_load_image(path.c_str());
    if (!image)
    {
        m_cache[key] = {};
        return false;
    }

    imlib_context_set_image(image);

    Pixmap pixmap = None, mask = None;
    imlib_render_pixmaps_for_whole_image_at_size(&pixmap, &mask, size, size);
    imlib_free_image();

    m_cache[key] = { pixmap, mask };
    outPixmap = pixmap;
    outMask = mask;
    return pixmap != None;
}

}
