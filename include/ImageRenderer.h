#pragma once

#include <X11/Xlib.h>

#include <string>

namespace Kohiko
{

class XConnection;

// How an image's own aspect ratio is reconciled with a target
// rectangle that may not share it - the same four conventional modes
// every wallpaper tool offers:
enum class ImageScaleMode
{
    // Scales to exactly cover the target, cropping whatever overflows
    // (the image's own aspect ratio is preserved; nothing is
    // distorted, but part of the image may not be visible).
    Fill,

    // Scales to fit entirely within the target, preserving aspect
    // ratio - `backgroundColor` (see Render()) fills the letterbox/
    // pillarbox bars left over on whichever axis doesn't exactly
    // match.
    Fit,

    // No scaling at all - shown at the image's own native size,
    // centered; cropped if larger than the target, `backgroundColor`
    // padding on whichever side(s) it's smaller.
    Center,

    // Scales to exactly the target size on both axes independently -
    // simple, but distorts the image unless its aspect ratio already
    // happens to match the target's.
    Stretch,
};

// Loads an image file via Imlib2 and renders it into a *new* Pixmap
// of exactly the requested size, reconciling the image's own
// dimensions with that size per `mode` above - shared by LockScreen
// (its own background image) and WallpaperManager (the desktop
// wallpaper), so there is exactly one place this rendering logic
// exists rather than two independent copies. LockScreen's own
// original background-image loader only ever stretched (no other
// mode was configurable); extracting this gives it the same four real
// modes WallpaperManager has, at no extra cost.
class ImageRenderer
{
public:

    // Parses a config value ("fill"/"fit"/"center"/"stretch",
    // case-insensitive) into an ImageScaleMode - shared by
    // WallpaperManager and LockScreen so the exact same keywords mean
    // the same thing in both wallpaper.mode/wallpaper.monitor=.../
    // wallpaper.workspace=... and lockscreen.background_mode. Returns
    // `fallback` for anything unrecognised, rather than treating it as
    // an error - same future-proofing philosophy as MonitorRule's own
    // "unknown key ignored" handling.
    static ImageScaleMode ParseMode(
        const std::string& text,
        ImageScaleMode fallback
    );

    // `drawable` only matters for setting up Imlib2's rendering
    // context correctly (matching the target X screen's visual/
    // colormap) - the returned Pixmap is a brand new one, sized
    // exactly `width` x `height`, not a modification of `drawable`
    // itself. Typically the window/root the result will actually be
    // displayed on.
    //
    // Returns 0 (not a valid Pixmap - the caller decides what to fall
    // back to) if `path` can't be loaded at all (missing file,
    // unreadable, unsupported format) or if `width`/`height` <= 0.
    static Pixmap Render(
        XConnection& connection,
        ::Window drawable,
        const std::string& path,
        int width,
        int height,
        ImageScaleMode mode,
        unsigned long backgroundColor = 0x000000
    );

};

}
