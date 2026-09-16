#include "AppInstanceLock.h"
#include "PipeWireClient.h"
#include "TrayIconClient.h"
#include "UiIconCache.h"
#include "UiPopupMenu.h"

#include <X11/Xlib.h>

using namespace Kohiko;

namespace
{

const AudioNode* FindDefaultOutput(const PipeWireClient& pw)
{
    for (auto& node : pw.Nodes())
        if (!node.isSource && node.isDefault)
            return &node;

    return nullptr;
}

std::string IconForOutput(const AudioNode* node)
{
    if (!node || node->muted || node->volume <= 0.001f)
        return "audio-volume-muted";
    if (node->volume < 0.33f)
        return "audio-volume-low";
    if (node->volume < 0.66f)
        return "audio-volume-medium";
    return "audio-volume-high";
}

// Drawn instead whenever the installed icon theme doesn't actually
// have IconForOutput()'s name (UiIconCache::Get() returning false) -
// see TrayIconClient::DrawText()'s own comment for why this can never
// itself fail to render something. Block-height characters for the
// three "has volume" levels read as a simple level meter even at
// tray-icon size; deliberately its own small switch over the same
// states above rather than trying to derive one representation from
// the other, since a filename-shaped string and a one-glyph fallback
// don't share enough structure for that to simplify anything.
struct IconFallback { std::string glyph; unsigned long color; };

IconFallback FallbackForOutput(const TrayIconClient& tray, const AudioNode* node)
{
    if (!node || node->muted || node->volume <= 0.001f)
        return { "\u00d7", tray.Theme().muted }; // ×
    if (node->volume < 0.33f)
        return { "\u2582", tray.Theme().foreground }; // ▂
    if (node->volume < 0.66f)
        return { "\u2585", tray.Theme().foreground }; // ▅
    return { "\u2587", tray.Theme().foreground }; // ▇
}

}

int main()
{
    TrayIconClient tray;
    if (!tray.Create(22))
        return 1;

    PipeWireClient pipewire;
    pipewire.Connect();

    UiIconCache iconCache(tray.GetDisplay(), tray.GetVisual(), tray.GetColormap(),
        RootWindow(tray.GetDisplay(), tray.GetScreen()));

    pipewire.SetChangeHandler([&] { tray.RequestRedraw(); });

    if (pipewire.Available())
        tray.WatchFd(pipewire.Fd(), [&] { pipewire.Iterate(); });

    // Retries connecting to PipeWire periodically in case this tray
    // widget's own autostart entry runs before the audio stack has
    // finished starting - the same session-startup race
    // TrayIconClient's own dock-retry timer covers for the system
    // tray itself.
    tray.SetInterval(2000, [&]
    {
        if (!pipewire.Available() && pipewire.Connect())
            tray.WatchFd(pipewire.Fd(), [&] { pipewire.Iterate(); });
    });

    tray.SetDrawCallback([&](Display* display, Window window, GC gc, Visual*, Colormap, int size)
    {
        XSetForeground(display, gc, tray.Theme().background);
        XFillRectangle(display, window, gc, 0, 0, size, size);

        const AudioNode* node = FindDefaultOutput(pipewire);
        std::string iconName = IconForOutput(node);

        Pixmap pixmap = None, mask = None;
        if (iconCache.Get(iconName, size - 4, pixmap, mask))
        {
            int offset = (size - (size - 4)) / 2;
            if (mask != None)
            {
                XSetClipMask(display, gc, mask);
                XSetClipOrigin(display, gc, offset, offset);
            }
            XCopyArea(display, pixmap, window, gc, 0, 0, size - 4, size - 4, offset, offset);
            if (mask != None)
                XSetClipMask(display, gc, None);
        }
        else
        {
            // The installed icon theme doesn't have IconForOutput()'s
            // name (very common - see FallbackForOutput()'s own
            // comment) - draw a guaranteed-to-render substitute rather
            // than leaving this icon slot blank.
            IconFallback fallback = FallbackForOutput(tray, node);
            int textWidth = tray.GetFont().TextWidth(fallback.glyph);
            int baseline = (size + tray.GetFont().Ascent()) / 2;
            tray.DrawText((size - textWidth) / 2, baseline, fallback.glyph, fallback.color);
        }
    });

    tray.SetLeftClickHandler([]
    {
        AppInstanceLock::LaunchOrRaise("kohiko-audio", "kohiko-audio");
    });

    tray.SetScrollHandler([&](int delta)
    {
        const AudioNode* node = FindDefaultOutput(pipewire);
        if (node)
            pipewire.AdjustVolume(node->id, delta * 0.05f);
    });

    tray.SetRightClickHandler([&]
    {
        const AudioNode* node = FindDefaultOutput(pipewire);

        PopupMenu menu;

        if (node)
        {
            std::uint32_t id = node->id;
            bool muted = node->muted;
            menu.AddItem(muted ? "Unmute" : "Mute", [&pipewire, id, muted] { pipewire.SetMute(id, !muted); });
            menu.AddSeparator();

            for (auto& other : pipewire.Nodes())
            {
                if (other.isSource)
                    continue;

                std::uint32_t otherId = other.id;
                std::string label = (other.isDefault ? "\u2713 " : "   ") + other.description;
                menu.AddItem(label, [&pipewire, otherId] { pipewire.SetDefaultNode(otherId); });
            }

            menu.AddSeparator();
        }

        menu.AddItem("Open Sound Settings", [] { AppInstanceLock::LaunchOrRaise("kohiko-audio", "kohiko-audio"); });

        Window rootReturn, childReturn;
        int rootX, rootY, winX, winY;
        unsigned int mask;
        XQueryPointer(tray.GetDisplay(), tray.GetWindow(), &rootReturn, &childReturn, &rootX, &rootY, &winX, &winY, &mask);

        menu.Show(tray.GetDisplay(), tray.GetScreen(), tray.GetVisual(), tray.GetColormap(), tray.GetFont(), tray.Theme(), rootX, rootY);
    });

    tray.Run();
    return 0;
}
