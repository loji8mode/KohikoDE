#pragma once

#include "Font.h"
#include "NotificationLayout.h"
#include "NotificationPopup.h"
#include "Types.h"
#include "UiTheme.h"

#include <X11/Xlib.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace Kohiko
{

// The reusable "Kohiko notification mechanism" this whole feature is
// actually about - not the audio-device text that happens to be its
// first real caller (see kohiko-audio-tray's own PostDeviceToast()).
// Nothing here is specific to audio, headphones, or any other single
// component: any future caller with a Display*/screen/root and a
// short piece of text can Post() one, exactly like WindowManager's own
// ShowPopupNotification() does for itself.
//
// Owns a small pool of NotificationPopup "cards" (see that class for
// why each one is override-redirect and carries _NET_WM_WINDOW_TYPE_
// NOTIFICATION), stacked bottom-right-up per monitor (see
// NotificationLayout.h for the actual placement math), each with its
// own independent expiry - and nothing else: no polling loop, no
// timer of its own, no thread. See Tick()'s own comment for why that
// matters.
class NotificationCenter
{
public:

    NotificationCenter();
    ~NotificationCenter();

    NotificationCenter(const NotificationCenter&) = delete;
    NotificationCenter& operator=(const NotificationCenter&) = delete;

    // Loads the one Font every popup this instance ever creates
    // shares (same reasoning as Bar/Launcher's own single m_font) and
    // records `theme`/`root` for later Post() calls. Safe to call
    // again later (e.g. on a config reload) - reloads the font and
    // updates the theme new popups will use; any already-showing
    // popup keeps whatever it was already drawn with until it expires,
    // exactly like Bar's own Configure() leaves whatever's currently
    // on screen alone until the next Redraw().
    bool Initialize(
        Display* display,
        int screen,
        ::Window root,
        const std::string& fontPattern,
        const UiTheme& theme
    );

    // Posts a new toast, bottom-right of `monitorGeometry`, stacked
    // above any of this NotificationCenter's own still-active popups
    // on that same monitor rather than overlapping them - newest
    // closest to the corner, older ones pushed up, matching the
    // "similar to Telegram's notification popups" ask - alive for
    // `lifetime` (NotificationLayout::kDefaultLifetime - 2.5s, per the
    // spec - is what every real caller uses; overridable purely so a
    // test doesn't have to sleep 2.5 real seconds to see one expire -
    // see that constant's own comment for where the 2.5s figure
    // itself is actually asserted on). If kMaxStacked notifications
    // are already active (on any monitor combined), the single oldest
    // one is destroyed first to make room - "handled cleanly", per the
    // spec, rather than letting the stack grow without bound if
    // something posts notifications faster than they expire.
    void Post(
        const std::string& text,
        const Rect& monitorGeometry,
        std::chrono::milliseconds lifetime = NotificationLayout::kDefaultLifetime
    );

    // Destroys whichever popups have passed their own ExpiresAt(),
    // then repositions whatever's left on each affected monitor so
    // there's never a gap where an expired one used to be - "disappear
    // cleanly without affecting the underlying window stack", per the
    // spec, extended to "without leaving a hole in its own stack"
    // too. Must be called periodically by whatever event loop/timer
    // the owning process already runs - WindowManager::Tick() (see
    // WindowManager::HasActiveNotification(), which briefly speeds
    // that up while anything here is still showing, the same way
    // HasActiveAnimation() already does for Swap-drop animations) or
    // TrayIconClient::SetInterval() for a standalone tray tool - see
    // those two real call sites for the "existing EventLoop/timer
    // infrastructure" the spec asks this be built on, rather than
    // this class quietly starting a second one of its own.
    void Tick();

    // Routes an Expose event to whichever active popup owns `window`,
    // if any - a no-op otherwise, so it's always safe to call for
    // every Expose the host process sees.
    void HandleExpose(::Window window);

    bool HasActive() const;

    // How many notifications are currently active, across every
    // monitor combined - HasActive() is just this being nonzero;
    // exposed separately since confirming an exact count (e.g. "three
    // posted in quick succession are all still showing", or "posting
    // past kMaxStacked evicts down to exactly kMaxStacked") is more
    // precise than a single boolean for tests/diagnostics.
    std::size_t ActiveCount() const;

    // For WindowManager::Manage()'s own defence-in-depth guard (the
    // same "never manage Kohiko's own popups by ID, not just by
    // override-redirect" reasoning already applied to the bar/
    // launcher/notepad/power menu/lock screen - see that function's
    // own comment) - true for any window this NotificationCenter
    // currently owns.
    bool OwnsWindow(::Window window) const;

private:

    void RestackMonitor(const Rect& monitorGeometry);

    Display* m_display = nullptr;
    int m_screen = 0;
    ::Window m_root = 0;

    Font m_font;
    UiTheme m_theme;

    struct Entry
    {
        std::unique_ptr<NotificationPopup> popup;

        // Which monitor this one is stacked against - by value, not a
        // Monitor* (this class only ever receives a Rect from Post(),
        // never a Monitor - see the class comment on having zero
        // WindowManager dependency), so restacking after one expires
        // groups by matching Rect rather than any monitor identity.
        Rect monitorGeometry;
    };

    std::vector<Entry> m_active;
};

}
