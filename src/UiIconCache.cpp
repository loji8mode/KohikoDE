#include "UiIconCache.h"

#include "SvgRenderer.h"

#include <Imlib2.h>

namespace Kohiko
{

UiIconCache::UiIconCache(Display* display, Visual* visual, Colormap colormap, Window contextDrawable)
    : m_display(display)
    , m_visual(visual)
    , m_colormap(colormap)
    , m_contextDrawable(contextDrawable)
    , m_depth(24)
    , m_resolver()
{
    // contextDrawable's actual depth, queried once (it's fixed for
    // the lifetime of the window this was constructed with) - needed
    // by SvgRenderer::RenderToPixmaps() to create a colour Pixmap
    // that actually matches it; m_depth's 24 above is just a
    // reasonable default if this somehow fails, not a real fallback
    // path (XGetGeometry failing here would mean contextDrawable
    // itself is already invalid, at which point nothing downstream
    // is going to work regardless).
    Window rootReturn;
    int xReturn, yReturn;
    unsigned int widthReturn, heightReturn, borderWidthReturn, depthReturn;

    if (XGetGeometry(m_display, m_contextDrawable, &rootReturn, &xReturn, &yReturn,
            &widthReturn, &heightReturn, &borderWidthReturn, &depthReturn))
    {
        m_depth = static_cast<int>(depthReturn);
    }
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

    Pixmap pixmap = None, mask = None;

    if (SvgRenderer::CanHandle(path))
    {
        // See SvgRenderer.h's own comment for why .svg/.svgz is
        // rendered through librsvg/Cairo directly rather than through
        // Imlib2's own SVG loader plugin below.
        SvgRenderer::RenderToPixmaps(
            m_display, m_visual, m_depth, m_contextDrawable, path, size, pixmap, mask);
    }
    else
    {
        imlib_context_set_display(m_display);
        imlib_context_set_visual(m_visual);
        imlib_context_set_colormap(m_colormap);
        imlib_context_set_drawable(m_contextDrawable);

        Imlib_Image image = imlib_load_image(path.c_str());

        if (image)
        {
            imlib_context_set_image(image);
            imlib_render_pixmaps_for_whole_image_at_size(&pixmap, &mask, size, size);
            imlib_free_image();
        }
    }

    m_cache[key] = { pixmap, mask };
    outPixmap = pixmap;
    outMask = mask;
    return pixmap != None;
}

}
