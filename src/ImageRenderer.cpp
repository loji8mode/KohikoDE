#include "ImageRenderer.h"

#include "Utils.h"
#include "XConnection.h"

#include <Imlib2.h>

#include <algorithm>

namespace Kohiko
{

namespace
{

struct RenderRect
{
    int srcX, srcY, srcWidth, srcHeight;
    int destX, destY, destWidth, destHeight;
};

// The one piece of real logic in this whole file: how much of the
// source image to read from, and where/how large to draw it, so that
// a single imlib_render_image_part_on_drawable_at_size() call (which
// crops from (srcX,srcY,srcWidth,srcHeight) and scales into
// (destX,destY,destWidth,destHeight) in one step) implements whichever
// ImageScaleMode was asked for - see ImageScaleMode's own comment for
// what each mode actually means; this only computes the rectangles
// that produce it.
RenderRect ComputeRenderRect(
    int imageWidth, int imageHeight,
    int targetWidth, int targetHeight,
    ImageScaleMode mode)
{
    double imageAspect = static_cast<double>(imageWidth) / imageHeight;
    double targetAspect = static_cast<double>(targetWidth) / targetHeight;

    switch (mode)
    {
        case ImageScaleMode::Stretch:
            return { 0, 0, imageWidth, imageHeight, 0, 0, targetWidth, targetHeight };

        case ImageScaleMode::Fill:
        {
            int srcWidth, srcHeight, srcX, srcY;

            if (imageAspect > targetAspect)
            {
                // Image is relatively wider than the target - crop
                // its left/right edges, keeping the full height.
                srcHeight = imageHeight;
                srcWidth = static_cast<int>(imageHeight * targetAspect);
                srcX = (imageWidth - srcWidth) / 2;
                srcY = 0;
            }
            else
            {
                // Image is relatively taller/narrower - crop its
                // top/bottom edges, keeping the full width.
                srcWidth = imageWidth;
                srcHeight = static_cast<int>(imageWidth / targetAspect);
                srcX = 0;
                srcY = (imageHeight - srcHeight) / 2;
            }

            return { srcX, srcY, srcWidth, srcHeight, 0, 0, targetWidth, targetHeight };
        }

        case ImageScaleMode::Fit:
        {
            int destWidth, destHeight, destX, destY;

            if (imageAspect > targetAspect)
            {
                // Image is relatively wider - constrained by the
                // target's width; letterbox bars top and bottom.
                destWidth = targetWidth;
                destHeight = static_cast<int>(targetWidth / imageAspect);
                destX = 0;
                destY = (targetHeight - destHeight) / 2;
            }
            else
            {
                // Constrained by the target's height; pillarbox bars
                // left and right.
                destHeight = targetHeight;
                destWidth = static_cast<int>(targetHeight * imageAspect);
                destX = (targetWidth - destWidth) / 2;
                destY = 0;
            }

            return { 0, 0, imageWidth, imageHeight, destX, destY, destWidth, destHeight };
        }

        case ImageScaleMode::Center:
        default:
        {
            int srcWidth = std::min(imageWidth, targetWidth);
            int srcHeight = std::min(imageHeight, targetHeight);
            int srcX = (imageWidth - srcWidth) / 2;
            int srcY = (imageHeight - srcHeight) / 2;

            // No scaling at all - dest is exactly the (possibly
            // cropped) source size, centered in the target.
            int destX = (targetWidth - srcWidth) / 2;
            int destY = (targetHeight - srcHeight) / 2;

            return { srcX, srcY, srcWidth, srcHeight, destX, destY, srcWidth, srcHeight };
        }
    }
}

}

ImageScaleMode ImageRenderer::ParseMode(
    const std::string& text,
    ImageScaleMode fallback)
{
    std::string lower = Utils::Lower(text);

    if (lower == "fill")
        return ImageScaleMode::Fill;
    if (lower == "fit")
        return ImageScaleMode::Fit;
    if (lower == "center")
        return ImageScaleMode::Center;
    if (lower == "stretch")
        return ImageScaleMode::Stretch;

    return fallback;
}

Pixmap ImageRenderer::Render(
    XConnection& connection,
    ::Window drawable,
    const std::string& path,
    int width,
    int height,
    ImageScaleMode mode,
    unsigned long backgroundColor)
{
    if (path.empty() || width <= 0 || height <= 0)
        return 0;

    Display* display = connection.GetDisplay();

    if (!display)
        return 0;

    int screen = connection.Screen();

    Pixmap result = XCreatePixmap(
        display, drawable,
        static_cast<unsigned int>(width), static_cast<unsigned int>(height),
        DefaultDepth(display, screen));

    // Fill the whole target first - cheap, and means Fit's letterbox
    // bars and Center's padding (when the image is smaller than the
    // target) are always correctly colored without needing mode-
    // specific fill logic. Fill/Stretch always cover every pixel of
    // the target themselves afterward, so this is simply never
    // visible for those two modes - not worth special-casing around.
    GC gc = XCreateGC(display, result, 0, nullptr);
    XSetForeground(display, gc, backgroundColor);
    XFillRectangle(display, result, gc, 0, 0,
        static_cast<unsigned int>(width), static_cast<unsigned int>(height));
    XFreeGC(display, gc);

    imlib_context_set_display(display);
    imlib_context_set_visual(DefaultVisual(display, screen));
    imlib_context_set_colormap(DefaultColormap(display, screen));
    imlib_context_set_drawable(result);

    Imlib_Image image = imlib_load_image(path.c_str());

    if (!image)
    {
        // Whatever the caller asked for couldn't be loaded - the
        // solid backgroundColor fill above is left as-is; the caller
        // decides whether that's an acceptable fallback or whether to
        // try something else (see WallpaperManager/LockScreen).
        return result;
    }

    imlib_context_set_image(image);

    int imageWidth = imlib_image_get_width();
    int imageHeight = imlib_image_get_height();

    if (imageWidth > 0 && imageHeight > 0)
    {
        RenderRect rect = ComputeRenderRect(imageWidth, imageHeight, width, height, mode);

        imlib_render_image_part_on_drawable_at_size(
            rect.srcX, rect.srcY, rect.srcWidth, rect.srcHeight,
            rect.destX, rect.destY, rect.destWidth, rect.destHeight);
    }

    imlib_free_image();

    return result;
}

}
