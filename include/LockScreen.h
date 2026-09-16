#pragma once

#include "Font.h"
#include "ImageRenderer.h"
#include "Types.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace Kohiko
{

class XConnection;
class Config;
class MonitorManager;
class WallpaperManager;

// Kohiko's own lock screen - no i3lock/betterlockscreen dependency,
// authenticated via PAM (see Authenticator.h). Full-screen password
// field, hidden input, Escape clears what's typed, Enter submits.
//
// Deliberately a single override-redirect window sized to the whole X
// *screen* rather than one per monitor: under XRandR (the only
// multi-monitor backend this project supports - see MonitorManager),
// the X screen itself already spans the bounding box of every
// connected monitor, so one window covers all of them at once with no
// per-monitor keyboard-focus juggling needed. The password
// prompt/field is simply drawn once per monitor region within that
// single canvas - see Redraw().
//
// While locked, this holds an *active* XGrabKeyboard/XGrabPointer, not
// just window stacking + real X input focus the way Launcher/Notepad
// do - stacking alone wouldn't stop Kohiko's own global hotkeys (bound
// as passive grabs on the root window) from still firing underneath.
// See Lock()/Unlock().
class LockScreen
{
public:

    explicit LockScreen(
        XConnection& connection
    );

    ~LockScreen();

    // Reads every lockscreen.* setting - safe to call again later
    // (a config reload), same as Bar/Launcher/Notepad/PowerMenu's own
    // Configure().
    void Configure(
        const Config& config
    );

    // Locks immediately - UNLESS the current user's account has no
    // password configured at all (see Authenticator's own header
    // comment for exactly how that's detected), in which case this
    // returns without ever mapping anything, per the spec.
    //
    // `wallpapers` is consulted only when lockscreen.background_image
    // is empty and lockscreen.use_desktop_wallpaper is turned on -
    // see Redraw()'s own comment for exactly how the two combine.
    void Lock(
        const MonitorManager& monitors,
        const WallpaperManager& wallpapers
    );

    // Only ever called internally, once Authenticate() actually
    // succeeds (see HandleKeyPress()) - there is deliberately no other
    // way to reach this from the outside.
    void Unlock();

    bool IsLocked() const;

    ::Window WindowId() const;

    void HandleExpose();

    // Escape clears whatever's typed so far (never unlocks - see this
    // class's own header comment on why that's the *only* thing
    // Escape does here, unlike every other modal in this codebase).
    // Enter attempts authentication: on success, unlocks; on failure,
    // clears the field and shows a brief error instead.
    void HandleKeyPress(
        const XKeyEvent& event
    );

    // Re-covers every monitor after a hotplug/topology change while
    // still locked - WindowManager's own HandleMonitorTopologyChanged()
    // calls this the same way it already rebuilds bars. A no-op if
    // not currently locked. Same `wallpapers` reasoning as Lock()
    // above.
    void Reposition(
        const MonitorManager& monitors,
        const WallpaperManager& wallpapers
    );

    // Clears the brief post-failed-attempt error message once its
    // time is up - called from WindowManager::Tick(), the same place
    // Bar's own notification and Launcher's caret blink already get
    // driven from.
    void Tick();

    // Fires exactly once per genuine m_locked transition - true right
    // after Lock() actually engages (never for the "account has no
    // password, unlocked immediately" early-return case - nothing
    // ever became locked there), false right after Unlock(). Fires
    // regardless of *what* triggered the transition (a keybind,
    // kohikoctl, the automatic suspend/startup config triggers, or an
    // external session-lock request), since this is the one place
    // every one of those paths already funnels through. Set once, by
    // WindowManager, to relay into SessionLockBridge's
    // SetLockedHint() - see that class's own header comment for why
    // this needs to be a callback here rather than WindowManager
    // wrapping every individual call site itself (Unlock() in
    // particular is only ever called from inside HandleKeyPress()).
    using LockStateChangedCallback = std::function<void(bool locked)>;

    void SetLockStateChangedCallback(
        LockStateChangedCallback callback
    );

private:

    void Redraw();

    // (Re)renders m_backgroundPixmaps/m_monitorGeometries for every
    // monitor in `monitors`, freeing whatever was there before -
    // shared by Lock() and Reposition(), which previously each had
    // their own copy of this exact loop. Resolves each monitor's
    // effective image/mode itself (own lockscreen.background_image/
    // lockscreen.background_mode if set, else `wallpapers`' current
    // resolution for that monitor if lockscreen.use_desktop_wallpaper
    // is on, else neither - Redraw() already falls back to
    // lockscreen.background_color whenever a pixmap here is 0) via
    // the shared ImageRenderer, rather than a lock-screen-specific
    // stretch-only loader.
    void RenderBackgroundPixmaps(
        const MonitorManager& monitors,
        const WallpaperManager& wallpapers
    );

private:

    XConnection& m_connection;

    ::Window m_window = 0;
    GC m_gc = nullptr;
    Font m_font;
    XftDraw* m_xftDraw = nullptr;

    XIM m_xim = nullptr;
    XIC m_xic = nullptr;

    bool m_locked = false;
    std::string m_username;
    std::string m_typed;

    // See Tick()/HandleKeyPress().
    bool m_showError = false;
    std::chrono::steady_clock::time_point m_errorUntil;

    // One entry per currently-locked-onto monitor, refreshed by
    // Lock()/Reposition() - Redraw()/HandleExpose() only ever read
    // this, so neither needs a MonitorManager reference threaded
    // through them.
    std::vector<Rect> m_monitorGeometries;

    // The background image, if configured, rendered once per monitor
    // at that monitor's own size - the same Lock()/Reposition() calls
    // that refresh m_monitorGeometries above refresh this too (via
    // RenderBackgroundPixmaps()) - a fresh render per monitor rather
    // than one shared pixmap, since each monitor can be a different
    // size (and, with lockscreen.use_desktop_wallpaper on, can even
    // resolve to a completely different image).
    std::vector<Pixmap> m_backgroundPixmaps;

    // The optional logo, loaded once at a fixed size and reused
    // as-is on every monitor (unlike the background, its native size
    // is exactly what should be drawn everywhere, not stretched to
    // fill each monitor).
    Pixmap m_logoPixmap = 0;
    Pixmap m_logoMask = 0;
    int m_logoSize = 96;

    std::string m_backgroundImagePath;
    ImageScaleMode m_backgroundMode = ImageScaleMode::Fill;
    bool m_useDesktopWallpaper = false;
    std::string m_logoPath;

    unsigned long m_backgroundPixel = 0;
    unsigned long m_foregroundPixel = 0;
    unsigned long m_fieldPixel = 0;
    unsigned long m_errorPixel = 0;

    // See Configure() - lockscreen.show_clock/show_date/show_hostname/
    // show_username and lockscreen.clock_format/date_format. Hostname
    // is resolved once, in Configure() (it never changes at runtime),
    // rather than on every Redraw().
    bool m_showClock = true;
    bool m_showDate = true;
    bool m_showHostname = false;
    bool m_showUsername = true;
    std::string m_clockFormat;
    std::string m_dateFormat;
    std::string m_hostname;

    LockStateChangedCallback m_lockStateChanged;

};

}
