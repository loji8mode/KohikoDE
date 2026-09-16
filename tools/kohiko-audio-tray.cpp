#include "AppInstanceLock.h"
#include "NotificationCenter.h"
#include "PipeWireClient.h"
#include "TrayIconClient.h"
#include "UiIconCache.h"
#include "UiPopupMenu.h"

#include <X11/Xlib.h>

#include <map>

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

// id -> what the toast should call it - node.description when
// PipeWire actually reports one (the common case), falling back to
// node.name otherwise, same "description if we have one" convention
// the right-click menu's own device list above already follows.
std::map<std::uint32_t, std::string> NodeLabels(const PipeWireClient& pw)
{
    std::map<std::uint32_t, std::string> labels;

    for (auto& node : pw.Nodes())
        labels[node.id] = node.description.empty() ? node.name : node.description;

    return labels;
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

    // Native toast notifications for real device connect/disconnect
    // events - see NotificationCenter.h for the reusable mechanism
    // itself, and docs/ARCHITECTURE.md's "Native notifications"
    // section for why this exists instead of relying on an external
    // freedesktop notification daemon Kohiko doesn't ship (see
    // NotificationClient.h's own comment on that). Shares this
    // process's own X connection and tray theme - the exact same
    // NotificationPopup/NotificationCenter classes WindowManager
    // itself uses for ShowPopupNotification(), which is what makes
    // this "a reusable Kohiko notification mechanism... used by other
    // components" rather than something built specifically for audio.
    NotificationCenter notifications;
    notifications.Initialize(
        tray.GetDisplay(), tray.GetScreen(),
        RootWindow(tray.GetDisplay(), tray.GetScreen()),
        "monospace:pixelsize=14",
        tray.Theme());

    // A 2.5-second-lifetime toast needs to be caught reasonably
    // promptly, not up to ~2 seconds late - reuses TrayIconClient's
    // own existing timer mechanism (the same one the PipeWire
    // reconnect-retry below already uses) rather than a new polling
    // loop, at a tighter cadence than that one specifically because
    // this one actually needs it (see EventLoop.cpp's own comment on
    // kohiko's equivalent choice for the exact same reason).
    tray.SetInterval(100, [&] { notifications.Tick(); });

    // Routes Expose events for the notification popups' own windows -
    // which share this same Display connection, see NotificationPopup's
    // own comment on why - to the right place; see
    // TrayIconClient::SetEventHandler()'s own comment for why this
    // hook exists at all.
    tray.SetEventHandler([&](XEvent& event)
    {
        if (event.type == Expose)
            notifications.HandleExpose(event.xexpose.window);
    });

    // This tray widget has no MonitorManager/XRandr awareness of its
    // own (see docs/ARCHITECTURE.md's "remaining limitations" note) -
    // positions against the whole root window instead of a specific
    // monitor, which is exactly right on any single-monitor session
    // and on the common multi-monitor case where XRandr monitors form
    // one seamless virtual screen, but will land at the bottom-right
    // of the *combined* virtual desktop rather than a specific
    // physical monitor on a more unusual multi-screen layout.
    auto rootGeometry = [&]
    {
        return Rect{0, 0,
            DisplayWidth(tray.GetDisplay(), tray.GetScreen()),
            DisplayHeight(tray.GetDisplay(), tray.GetScreen())};
    };

    // Seeded from whatever's already plugged in immediately after
    // Connect() *and* re-seeded, without posting anything, on the
    // change handler's own first invocation below - covering both
    // "Connect() itself already populated Nodes() synchronously" and
    // "the initial registry sync only arrives asynchronously, via
    // that first callback" without needing to know which one this
    // particular PipeWire/WirePlumber setup actually does. Either way,
    // only a genuine *later* connect/disconnect ever posts a toast -
    // startup itself never does.
    std::map<std::uint32_t, std::string> knownNodes = NodeLabels(pipewire);
    bool knownNodesSeeded = false;

    pipewire.SetChangeHandler([&]
    {
        tray.RequestRedraw();

        // SetChangeHandler() fires for volume changes and default-
        // device switches too, not just a device actually being
        // plugged in or unplugged (see PipeWireClient::SetChangeHandler()'s
        // own comment) - diffing Nodes() by id against the last known
        // snapshot is what isolates "a device was actually connected/
        // disconnected" from every other kind of update, so this only
        // ever posts a toast for the specific event the spec asks for.
        std::map<std::uint32_t, std::string> currentNodes = NodeLabels(pipewire);

        if (!knownNodesSeeded)
        {
            // See knownNodes' own comment above - this first callback
            // might just be reporting the async initial registry
            // sync, not a real event.
            knownNodes = std::move(currentNodes);
            knownNodesSeeded = true;
            return;
        }

        for (auto& [id, label] : currentNodes)
            if (knownNodes.find(id) == knownNodes.end())
                notifications.Post(label + " connected", rootGeometry());

        for (auto& [id, label] : knownNodes)
            if (currentNodes.find(id) == currentNodes.end())
                notifications.Post(label + " disconnected", rootGeometry());

        knownNodes = std::move(currentNodes);
    });

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
