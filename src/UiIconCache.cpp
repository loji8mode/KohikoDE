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
