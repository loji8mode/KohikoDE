#include "WindowManager.h"

#include "Command.h"
#include "Config.h"
#include "DesktopEntry.h"
#include "IpcPath.h"
#include "Json.h"
#include "Logger.h"
#include "ManagedWindow.h"
#include "MonitorRule.h"
#include "Process.h"
#include "Utils.h"
#include "XConnection.h"
#include "Xdg.h"

#include <X11/Xatom.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <sstream>

namespace Kohiko
{

WindowManager::WindowManager(
    XConnection& connection,
    Config& config,
    std::string configPath)
    :
    m_connection(connection),
    m_config(config),
    m_configPath(std::move(configPath)),
    m_atoms(connection),
    m_cursor(connection),
    m_repository(),
    m_workspaces(config.GetInt("workspace.count", 10)),
    m_monitors(connection, m_workspaces),
    m_layout(),
    m_scratchpad(),
    m_keyboard(connection, *this),
    m_mouse(connection, *this),
    m_dispatcher(*this, m_keyboard, m_mouse),
    m_ipc(),
    m_launcher(connection),
    m_notepad(connection),
    m_powerMenu(connection),
    m_lockScreen(connection),
    m_animator()
{
}

void WindowManager::Initialize()
{
    m_atoms.Initialize();
    m_cursor.Initialize();

    // m_session already loaded its file in its own constructor (well
    // before this point), so this is safe to wire up before the first
    // Detect() call below - which is exactly when it's first needed,
    // for that call's own "new output, no rule matched it" fallback.
    // See SetSessionWorkspaceLookup()'s comment.
    m_monitors.SetSessionWorkspaceLookup(
        [this](const std::string& monitorName)
        {
            return m_session.LastWorkspaceForMonitor(monitorName);
        });

    // Loads `monitor=` rules, turns on XRandR hotplug reporting (when
    // available), and runs the first Detect() - every Monitor exists
    // and has an ActiveWorkspace() assigned by the time this returns.
    m_monitors.Initialize(m_config);

    // Gives WindowManager one last look at a Monitor object right
    // before Detect() actually destroys it (a physical unplug, or a
    // resolution change XRandR reports as a full output replacement) -
    // see HandleMonitorTopologyChanged()/RelocateOrphanedFloatingWindows()
    // for what "relocate affected windows safely" means here.
    m_monitors.SetBeforeMonitorRemovedCallback(
        [this](Monitor&)
        {
            // Nothing needs the about-to-be-destroyed Monitor object
            // itself - Workspace objects (and therefore every window
            // on them, tiled or floating) live in WorkspaceManager
            // independently of any Monitor and are completely
            // unaffected by one disappearing; they simply stop being
            // displayed anywhere until some monitor switches back to
            // them, exactly like backgrounding a workspace normally
            // works. The one thing that *does* need fixing up -
            // floating/fullscreen windows whose absolute geometry no
            // longer overlaps any surviving monitor - is handled
            // uniformly afterwards, once the final surviving monitor
            // set is known, by HandleMonitorTopologyChanged() (called
            // right after every Detect() that changed anything -
            // see HandleMonitorEvent()) rather than per-monitor here.
        });

    // Creates one Bar per connected monitor and shows them; the system
    // tray pointer gets attached to whichever one is Primary()'s right
    // away too (safe even before m_tray.Initialize() below actually
    // creates its window - AttachSystemTray() just stores the pointer,
    // nothing dereferences it until the first real Redraw()).
    RebuildBars();

    // Advertise EWMH support - _NET_SUPPORTED/_NET_SUPPORTING_WM_CHECK
    // let tools that check for a compliant window manager (flameshot's
    // screenshot overlay among them) trust _NET_CLIENT_LIST and
    // _NET_ACTIVE_WINDOW, which Manage()/Unmanage()/Focus() below keep
    // current from here on.
    m_ewmhCheckWindow = m_connection.InitializeEwmhSupport(m_atoms, "kohiko");

    if (Bar* primaryBar = BarFor(m_monitors.Primary()))
        m_tray.Initialize(m_connection, primaryBar->WindowId());

    m_launcher.Configure(m_config, m_monitors.Primary().Geometry());
    m_notepad.Configure(m_config, m_monitors.Primary().Geometry());
    m_powerMenu.Configure(m_config);
    m_lockScreen.Configure(m_config);
    m_idleWatcher.Initialize(m_connection.GetDisplay());
    m_sleepInhibitor.Initialize();
    m_appDirWatcher.Initialize();
    m_sessionLockBridge.Initialize();

    // Every monitor already exists with its geometry/starting
    // workspace assigned by this point (m_monitors.Initialize() ran
    // earlier in this same function), so the very first render
    // already has everything ResolveFor() needs - no separate
    // "initial paint" special case beyond just calling this once,
    // here, the same way every later re-render (workspace switch,
    // topology change, config reload - see those call sites) does.
    m_wallpaperManager.Configure(m_config);
    m_wallpaperManager.ApplyToRoot(m_connection, m_monitors);

    // Relays LockScreen's own state to logind's SetLockedHint() -
    // fires for every trigger (keybind, kohikoctl, suspend/startup
    // config, or an external session-lock request below), since they
    // all funnel through LockScreen::Lock()/Unlock() already. See
    // SessionLockBridge.h.
    m_lockScreen.SetLockStateChangedCallback(
        [this](bool locked)
        {
            m_sessionLockBridge.NotifyLocked(locked);
        });

    // The other direction: `loginctl lock-session`/an equivalent
    // external request reaches Kohiko's own real lock screen, exactly
    // like every other way of locking it - never a second/duplicate
    // lock screen, never a separate authentication path. See
    // SessionLockBridge.h.
    m_sessionLockBridge.SetLockRequestedCallback(
        [this]()
        {
            m_lockScreen.Lock(m_monitors, m_wallpaperManager);
        });

    // Suspend integration without needing a DBus/logind sleep-signal
    // dependency: Kohiko itself is what spawns the suspend command
    // (see PowerMenu), so it locks the screen *before* issuing it
    // rather than trying to detect the resume afterward. Since
    // suspend just freezes the whole machine - X server, this
    // process, and every active grab included - and resumes it
    // exactly as it was, "lock right before suspending" and "lock
    // screen is already there the instant it wakes back up" end up
    // being the same thing, with no separate wake-detection code
    // needed at all. Whether this actually locks is governed by
    // lockscreen.after - see LockScreenAutoOnSuspend()'s own comment.
    m_powerMenu.SetSuspendCallback(
        [this]()
        {
            if (LockScreenAutoOnSuspend())
                m_lockScreen.Lock(m_monitors, m_wallpaperManager);
        });

    // lockscreen.after=always additionally locks once right here, at
    // startup - covering a freshly started or just-restarted session
    // the same way "suspend"/"always" already cover a resume. A no-op
    // for an account with no password configured, same as every other
    // call to Lock() - see its own comment.
    if (LockScreenAutoOnStartup())
        m_lockScreen.Lock(m_monitors, m_wallpaperManager);

    m_windowRules = LoadWindowRules(m_config);

    m_keyboard.Configure(m_config);
    m_mouse.Configure(m_config);

     m_fileManager =
    m_config.GetString(
        "launcher.file_manager",
        "pcmanfm");

    m_ipc.Start(
        IpcSocketPath(),
        [this](const std::string& request)
        {
            return HandleIpcCommand(request);
        });

    // Keyboard layouts are applied via setxkbmap (real XKB groups),
    // not anything Kohiko's own KeyboardManager tracks - that's what
    // makes typing in any listed language work in every window, not
    // just ones Kohiko manages, and it's why Super+H/J/K/L etc. never
    // needed special-casing for non-English layouts in the first
    // place: KeyboardManager grabs by physical keycode (see its class
    // comment), and a keycode means the same physical key no matter
    // which XKB group is currently active.
    ApplyKeyboardLayouts();

    // Adopt whatever a previous window manager (or no window manager
    // at all) left mapped, before autostart adds anything new - see
    // AdoptExistingWindows()'s own comment for why this needs
    // m_windowRules/monitors/bars already set up, which by this point
    // in Initialize() they are.
    AdoptExistingWindows();

    // Autostart programs launch exactly once here, after the bar/tray/
    // launcher above are already up, so anything they immediately try
    // to dock (a tray icon, a notification) has somewhere to land.
    RunAutostart();

    RefreshMonitorWorkAreas();
    Arrange();

    // Nothing has generated a MotionNotify/EnterNotify yet at this
    // point, so m_monitors.Focused() is still whatever Detect() fell
    // back to (Primary()) - which is wrong whenever the pointer
    // already happens to be sitting over a *different* monitor when
    // Kohiko starts. An empty monitor especially has nothing on it to
    // move over or enter, so without this, that monitor would stay
    // "unfocused" - unable to receive a keybind-launched window, etc,
    // even though it's exactly where the pointer already is - until
    // the pointer physically moves. Querying it directly here instead
    // of waiting for an event fixes that for the actual, common case
    // of "start Kohiko while the pointer is already on a monitor
    // that's still empty."
    UpdateFocusedMonitorFromPointer(m_connection.QueryPointer());
}

void WindowManager::AdoptExistingWindows()
{
    // Direct children of the root, bottom-to-top - exactly what a
    // previous window manager (or a bare X session with none at all)
    // left mapped. Manage() itself already re-checks override-redirect,
    // dock-type, and Kohiko's own bar/launcher/notepad windows - the
    // one thing specific to *this* path is map_state: a normal
    // MapRequest never fires for a window that isn't trying to become
    // visible, but a blind scan of root's children can turn up
    // withdrawn/iconified ones too, and those get nothing from being
    // pulled into the tiling layout right now.
    for (::Window child : m_connection.QueryChildren(m_connection.Root()))
    {
        XWindowAttributes attrs{};

        if (!m_connection.GetWindowAttributes(child, attrs))
            continue;

        if (attrs.override_redirect)
            continue;

        // A window sitting on a workspace that wasn't the active one
        // when Kohiko last exited is unmapped at the X level too -
        // that's Kohiko's own doing (see Arrange()), not the client
        // withdrawing - so a saved Session Restore record for it
        // means it should be adopted right alongside every genuinely
        // viewable window. Only a window with *neither* a saved
        // record nor a viewable map_state gets skipped here, which is
        // exactly "avoid managing... [things not meant to be managed]"
        // for the plain (no previous Kohiko session at all) adoption
        // case this otherwise is.
        bool hasSessionRecord = m_session.Find(child) != nullptr;

        if (attrs.map_state != IsViewable && !hasSessionRecord)
            continue;

        Manage(child);
    }
}

void WindowManager::Shutdown()
{
    m_session.Save(m_repository.All(), m_monitors, m_workspaces);

    m_ipc.Stop();
    m_tray.Shutdown();

    for (auto& [monitor, bar] : m_bars)
        bar->Hide();
}

bool WindowManager::IsRunning() const
{
    return m_running;
}

EventDispatcher& WindowManager::Dispatcher()
{
    return m_dispatcher;
}

IPCServer& WindowManager::Ipc()
{
    return m_ipc;
}

ScreenSaverInhibitor& WindowManager::SleepInhibitor()
{
    return m_sleepInhibitor;
}

AppDirWatcher& WindowManager::AppWatcher()
{
    return m_appDirWatcher;
}

void WindowManager::HandleAppDirChanged()
{
    if (m_appDirWatcher.Poll())
        m_launcher.ReloadDesktopEntries();
}

SessionLockBridge& WindowManager::LockBridge()
{
    return m_sessionLockBridge;
}

WallpaperManager& WindowManager::Wallpaper()
{
    return m_wallpaperManager;
}

void WindowManager::HandleWallpaperFileChanged()
{
    if (m_wallpaperManager.Poll())
        m_wallpaperManager.ApplyToRoot(m_connection, m_monitors);
}

void WindowManager::Tick()
{
    if (m_animator.Active())
        m_animator.Step(m_connection);

    if (m_launcher.IsOpen())
        m_launcher.Blink();

    if (m_notepad.IsOpen())
        m_notepad.Blink();

    m_lockScreen.Tick();
    CheckIdleTimeoutLock();
    CheckSleepInhibition();

    // Also what clears a Bar::ShowNotification() once it expires - see
    // Bar::Redraw()'s own expiry check.
    for (auto& [monitor, bar] : m_bars)
        bar->Redraw();
}

void WindowManager::CheckIdleTimeoutLock()
{
    // Same full opt-out lockscreen.after=never already gives the
    // manual lock command - see LockScreenManualAllowed()'s own
    // comment. Already-locked is its own quick exit rather than an
    // error: this runs roughly once a second the entire time Kohiko
    // is up (see Tick()), so "still idle, still locked" is by far the
    // most common case once idle-timeout locking has actually fired.
    if (!m_idleWatcher.Available() || m_lockScreen.IsLocked() || !LockScreenManualAllowed())
        return;

    int minutes = m_config.GetInt("lockscreen.idle_timeout_minutes", 0);

    if (minutes <= 0)
        return;

    if (m_idleWatcher.IdleTime(m_connection.GetDisplay()) >= std::chrono::minutes(minutes))
        m_lockScreen.Lock(m_monitors, m_wallpaperManager);
}

bool WindowManager::IsAnyVisibleWindowFullscreen() const
{
    for (ManagedWindow* window : m_repository.All())
        if (window->IsFullscreen() && IsWorkspaceVisible(window->Workspace()))
            return true;

    return false;
}

void WindowManager::CheckSleepInhibition()
{
    if (!m_config.GetBool("general.inhibit_sleep_during_playback", true))
        return;

    bool shouldInhibit = m_sleepInhibitor.AnyActiveInhibit() || IsAnyVisibleWindowFullscreen();

    if (!shouldInhibit)
        return;

    // Simply keeps telling the X server "activity just happened" -
    // this is exactly what actual keyboard/mouse input would do, so
    // it resets the exact same idle counter DPMS's own power-down
    // timers are driven by, without ever touching DPMS's
    // configuration (enabled/disabled, or its timeouts) at all. The
    // instant shouldInhibit above goes back to false - the video
    // stops, the fullscreen window closes, every Inhibit() cookie is
    // released - this function simply stops being called, and
    // whatever idle time has genuinely elapsed since resumes counting
    // completely normally toward the display's real timeout.
    XResetScreenSaver(m_connection.GetDisplay());
}

bool WindowManager::HasActiveAnimation() const
{
    return m_animator.Active();
}

// --- X11 event handlers -----------------------------------------------------

void WindowManager::HandleMapRequest(const XMapRequestEvent& event)
{
    Manage(event.window);
}

void WindowManager::HandleConfigureRequest(const XConfigureRequestEvent& event)
{
    ManagedWindow* window = m_repository.Get(event.window);

    if (!window || window->IsFloating())
    {
        // Start from whatever this window's geometry actually is right
        // now (falling back to the request itself if it isn't managed
        // yet) and only overwrite the fields this particular request
        // actually asked to change - a client resizing without moving,
        // say, shouldn't have its untouched x/y treated as "0,0" just
        // because CWX/CWY weren't set this time.
        Rect requested =
            window
                ? window->Geometry()
                : Rect{event.x, event.y, static_cast<int>(event.width), static_cast<int>(event.height)};

        if (event.value_mask & CWX)      requested.x      = event.x;
        if (event.value_mask & CWY)      requested.y      = event.y;
        if (event.value_mask & CWWidth)  requested.width  = event.width;
        if (event.value_mask & CWHeight) requested.height = event.height;

        // Geometry Rule: a floating client's own ConfigureRequest is
        // honoured - that's what makes it "floating" - but never at
        // the cost of letting it place itself off-screen. Clamped
        // right here, before it ever reaches X11, exactly like every
        // other geometry-then-apply site in this file - against its
        // own monitor, not always Primary(), so a floating window on
        // a secondary monitor can't be resized/moved out past *that*
        // monitor's edge just because Primary() happens to be bigger.
        Monitor* owner = window ? MonitorShowing(window->Workspace()) : nullptr;
        Monitor& monitorRef = owner ? *owner : FocusedMonitor();
        const Rect& monitor = monitorRef.Geometry();
        int borderWidth = window ? window->BorderWidth() : static_cast<int>(event.border_width);
        Rect clamped = requested.ClampedTo(monitor, borderWidth);

        XWindowChanges changes{};
        changes.x = clamped.x;
        changes.y = clamped.y;
        changes.width = clamped.width;
        changes.height = clamped.height;
        changes.border_width = event.border_width;
        changes.sibling = event.above;
        changes.stack_mode = event.detail;

        m_connection.ConfigureWindowRaw(event.window, changes, static_cast<unsigned int>(event.value_mask));

        if (window)
        {
            window->SetGeometry(clamped);
            window->SetFloatingGeometry(clamped);
        }

        return;
    }

    // Tiled: we own the geometry - acknowledge with what it actually
    // is so the client doesn't sit waiting for a reply.
    const Rect& actual = window->Geometry();
    m_connection.SendConfigureNotify(event.window, actual, window->BorderWidth());

    // A client that just asked for something *other* than what it's
    // actually getting - TLauncher re-asserting its own preferred size
    // mid-session is the reliable reproducer, e.g. right after its
    // "restart to finish updating" flow swaps in a smaller splash/
    // loading layout and tries to shrink to fit it - may well have
    // already speculatively repainted itself at the size it asked for,
    // before this refusal ever reaches it. Nothing about the window's
    // real geometry changed here, so the server has no reason to
    // generate an Expose on its own - without one, whatever the client
    // already drew over its old content just sits there, orphaned in a
    // corner of the real (unchanged, still fully tiled) window, which
    // is exactly the "opens crooked" look. Only forcing this when the
    // request actually asked for something different keeps it free for
    // a well-behaved client that only ever confirms the size it
    // already has.
    bool askedForSomethingElse =
        ((event.value_mask & CWX)      && event.x      != actual.x) ||
        ((event.value_mask & CWY)      && event.y      != actual.y) ||
        ((event.value_mask & CWWidth)  && event.width  != actual.width) ||
        ((event.value_mask & CWHeight) && event.height != actual.height);

    if (!askedForSomethingElse)
        return;

    m_connection.ClearArea(event.window);

    // Window Misbehavior Detection: a ConfigureRequest conflict on its
    // own isn't proof of anything - a client resizing itself once as
    // part of normal startup is completely ordinary. What actually
    // indicates a client fighting its tiled geometry is this
    // happening *repeatedly* in the same tile (repeated resize
    // attempts / a ConfigureRequest loop), which is exactly what
    // TilingMisbehaviorCount() accumulates - reset back to zero every
    // time this window is freshly tiled (TryTile()) or the fallback
    // below actually fires, so it only ever measures how the window
    // is behaving in its *current* placement.
    if (!window->IsTiled())
        return; // fullscreen/scratchpad ConfigureRequest handling is unrelated to tiling fit

    window->RegisterTilingMisbehavior();

    int threshold = m_config.GetInt("general.tiling_misbehavior_threshold", 3);

    if (threshold > 0 && window->TilingMisbehaviorCount() >= threshold)
        ApplyTilingMisbehaviorFallback(window);
}

void WindowManager::HandleUnmapNotify(const XUnmapEvent& event)
{
    ManagedWindow* window = m_repository.Get(event.window);

    if (!window)
        return;

    if (window->ConsumeIgnoredUnmap())
        return;

    Unmanage(event.window);
}

void WindowManager::HandleDestroyNotify(const XDestroyWindowEvent& event)
{
    if (m_repository.Contains(event.window))
        Unmanage(event.window);

    m_tray.HandleWindowDestroyed(event.window);
}

void WindowManager::HandleEnterNotify(const XCrossingEvent& event)
{
    if (event.detail == NotifyInferior)
        return;

    bool pointerActuallyMoved =
        (event.x_root != m_lastPointerX || event.y_root != m_lastPointerY);

    m_lastPointerX = event.x_root;
    m_lastPointerY = event.y_root;

    // Rearranging the tiled layout can put a different window under an
    // unmoved cursor, which X11 *also* reports as EnterNotify - as can
    // starting/ending a pointer grab (e.g. MouseManager's Super+drag).
    // Only treat this as "the user moved the mouse here" if it's a
    // normal crossing and the coordinates actually changed.
    if (event.mode != NotifyNormal || !pointerActuallyMoved)
        return;

    // Multi-monitor: the pointer crossing into *any* window (even one
    // on the same monitor) is as good a signal as the ambient
    // MotionNotify tracking in HandlePointerMotion() is for bare
    // desktop - update here too so a crossing between two monitors is
    // noticed the instant it happens, not on the next bit of ambient
    // motion over background.
    UpdateFocusedMonitorFromPointer(Point(event.x_root, event.y_root));

    // The Launcher/Notepad hold input focus for typing - a background
    // window happening to be under the pointer must not steal it back
    // (that's the exact "hotkeys/typing quietly stop working" failure
    // mode the Super+Q hardening elsewhere in this file is about).
    if (m_launcher.IsOpen() || m_notepad.IsOpen())
        return;

    if (!m_config.GetBool("general.focus_follows_mouse", true))
        return;

    ManagedWindow* window = m_repository.Get(event.window);

    if (window && !window->Focused())
        Focus(event.window);
}

void WindowManager::HandlePointerMotion(const XMotionEvent& event)
{
    // Purely the "gliding over bare desktop between two monitors"
    // case - see the class comment and XConnection::
    // BecomeWindowManager(). Crossing into an actual window is already
    // covered by HandleEnterNotify() above; this just fills the one
    // gap that leaves (a single root window spans every monitor, so
    // there's no window-boundary crossing to catch mid-glide).
    UpdateFocusedMonitorFromPointer(Point(event.x_root, event.y_root));
}

void WindowManager::UpdateFocusedMonitorFromPointer(const Point& pointer)
{
    // Same reasoning as HandleEnterNotify()'s own guard: the Launcher/
    // Notepad hold real X input focus for typing, and FocusMonitor()
    // below would otherwise yank it away the moment the pointer merely
    // drifted over another monitor - freezing which monitor is
    // "focused" while either is open (it picks back up again the
    // instant it closes) is what keeps that from happening.
    if (m_launcher.IsOpen() || m_notepad.IsOpen())
        return;

    Monitor* under = m_monitors.Containing(pointer);

    // No monitor claims this exact point (can happen transiently
    // mid-hotplug, or right at a pixel just past every monitor's
    // edge) - nothing sane to do, leave focus exactly where it was.
    if (!under || under == m_monitors.Focused())
        return;

    FocusMonitor(*under);
}

void WindowManager::HandleFocusIn(const XFocusChangeEvent& event)
{
    if (!m_launcher.IsOpen() && !m_notepad.IsOpen())
        return;

    ::Window modalWindow =
        m_launcher.IsOpen() ? m_launcher.WindowId() : m_notepad.WindowId();

    if (event.window == modalWindow)
        return;

    // Some apps (a handful of Electron/GTK ones, mostly) call
    // XSetInputFocus on their own window right after mapping, entirely
    // independent of whatever the window manager just did - there's
    // nothing in the X protocol that stops a client from doing this.
    // Without this, the Launcher/Notepad - which never uses an active
    // keyboard grab, see Launcher's class comment - would silently
    // stop receiving keystrokes the moment that happens, with no
    // visible sign anything had changed and no way to even close it
    // from the keyboard. Just take focus straight back.
    m_connection.SetInputFocus(modalWindow);
}

void WindowManager::HandlePropertyNotify(const XPropertyEvent& event)
{
    if (event.atom != m_atoms.NET_WM_NAME && event.atom != XA_WM_NAME)
        return;

    ManagedWindow* window = m_repository.Get(event.window);

    if (!window)
        return;

    window->SetTitle(m_connection.GetWindowTitle(event.window, m_atoms));

    if (window->Focused())
        UpdateAllBars();
}

void WindowManager::HandleButtonPressOnClient(const XButtonEvent& event)
{
    if (m_lockScreen.IsLocked())
        return;

    // Clicking a background window while the Launcher/Notepad is open
    // reads as "I'm done with this" - dismiss it first (as a cancel,
    // not a run) rather than leaving it visually open but no longer
    // able to receive the typing it needs focus for.
    if (m_launcher.IsOpen())
        CloseLauncher(false);

    if (m_notepad.IsOpen())
        CloseNotepad();

    if (m_powerMenu.IsOpen())
        ClosePowerMenu();

    if (m_repository.Contains(event.window))
        Focus(event.window);

    m_connection.AllowReplayPointer();
}

void WindowManager::HandleExpose(const XExposeEvent& event)
{
    if (event.count != 0)
        return;

    if (Bar* bar = BarWindowMatching(event.window))
        bar->Redraw();
    else if (event.window == m_launcher.WindowId())
        m_launcher.HandleExpose();
    else if (event.window == m_notepad.WindowId())
        m_notepad.HandleExpose();
    else if (event.window == m_powerMenu.WindowId())
        m_powerMenu.HandleExpose();
    else if (event.window == m_lockScreen.WindowId())
        m_lockScreen.HandleExpose();
}

void WindowManager::HandleClientMessage(const XClientMessageEvent& event)
{
    m_tray.HandleClientMessage(event);

    // The standard EWMH way an already-mapped client asks the WM for a
    // state change - "please make me fullscreen" (flameshot's overlay,
    // a video player's own fullscreen button/shortcut, a chat client's
    // media viewer, ...) chief among the ones Kohiko understands.
    // Message-format details straight from the spec: data.l[0] is the
    // action (0 remove / 1 add / 2 toggle - the EWMH
    // _NET_WM_STATE_REMOVE/ADD/TOGGLE constants, which aren't
    // otherwise exposed as X11 atoms/macros), and data.l[1]/data.l[2]
    // are up to two state atoms being changed at once; Kohiko only
    // actually implements one state, so it's enough to check both
    // slots for it and ignore anything else it doesn't recognise.
    if (event.message_type != m_atoms.NET_WM_STATE)
        return;

    ManagedWindow* window = m_repository.Get(event.window);

    if (!window)
        return;

    bool concernsFullscreen =
        static_cast<Atom>(event.data.l[1]) == m_atoms.NET_WM_STATE_FULLSCREEN ||
        static_cast<Atom>(event.data.l[2]) == m_atoms.NET_WM_STATE_FULLSCREEN;

    if (!concernsFullscreen)
        return;

    WindowRuleEffect rules =
        ResolveWindowRules(window->ClassName(), window->InstanceName(), window->Title());

    if (rules.denyFullscreen)
        return; // this class is configured to never be allowed real fullscreen

    static constexpr long kNetWmStateRemove = 0;
    static constexpr long kNetWmStateAdd    = 1;
    static constexpr long kNetWmStateToggle = 2;

    bool shouldBeFullscreen = window->IsFullscreen();

    switch (event.data.l[0])
    {
        case kNetWmStateAdd:    shouldBeFullscreen = true;                    break;
        case kNetWmStateRemove: shouldBeFullscreen = false;                   break;
        case kNetWmStateToggle: shouldBeFullscreen = !window->IsFullscreen(); break;
        default: return;
    }

    if (shouldBeFullscreen == window->IsFullscreen())
        return;

    if (shouldBeFullscreen)
    {
        window->SetPreviousState(window->State());
        window->SetState(WindowState::Fullscreen);
    }
    else
    {
        window->SetState(window->PreviousState());
    }

    m_connection.SetNetWmState(m_atoms, window->Id(), shouldBeFullscreen);

    Arrange();
}

bool WindowManager::IsMonitorEvent(const XEvent& event) const
{
    return m_monitors.IsRandrEvent(event);
}

void WindowManager::HandleMonitorEvent(const XEvent& event)
{
    // Re-enumerates outputs and reconciles the monitor list - see
    // MonitorManager::Detect(). Always followed by
    // HandleMonitorTopologyChanged() below, whether or not the
    // monitor *set* actually changed shape: a plain resolution/
    // rotation change on an already-known output still needs work
    // areas recomputed and everything re-Arrange()d against the new
    // geometry, even though no monitor was actually added or removed.
    m_monitors.HandleXEvent(event);

    Logger::Info(
        "monitor layout changed (" +
        std::to_string(m_monitors.All().size()) +
        " connected) - re-arranging");

    HandleMonitorTopologyChanged();
}

bool WindowManager::HandleModalKeyPress(const XKeyEvent& event)
{
    // Checked before anything else: while locked, nothing else in
    // this chain - launcher, notepad, power menu, global hotkeys via
    // KeyboardManager - should be reachable at all. In practice the
    // active XGrabKeyboard LockScreen::Lock() takes already routes
    // every KeyPress event's window to the lock screen regardless
    // (see its own header comment), but this check is what actually
    // stops Kohiko's own hotkey matching from running on it, not the
    // grab itself.
    if (m_lockScreen.IsLocked())
    {
        m_lockScreen.HandleKeyPress(event);
        return true;
    }

    if (m_launcher.IsOpen())
    {
        switch (m_launcher.HandleKeyPress(event))
        {
            case LauncherResult::Confirmed: CloseLauncher(true);  break;
            case LauncherResult::Cancelled: CloseLauncher(false); break;
            case LauncherResult::Editing:                         break;
        }

        return true;
    }

    if (m_notepad.IsOpen())
    {
        if (m_notepad.HandleKeyPress(event) == NotepadResult::Closed)
            CloseNotepad();

        return true;
    }

    if (m_powerMenu.IsOpen())
    {
        if (m_powerMenu.HandleKeyPress(event))
            ClosePowerMenu();

        return true;
    }

    return false;
}

bool WindowManager::IsManaged(WindowID id) const
{
    return m_repository.Contains(id);
}

// --- lockscreen.after ---------------------------------------------------------

bool WindowManager::LockScreenManualAllowed() const
{
    return Utils::Lower(m_config.GetString("lockscreen.after", "suspend")) != "never";
}

bool WindowManager::LockScreenAutoOnSuspend() const
{
    std::string mode = Utils::Lower(m_config.GetString("lockscreen.after", "suspend"));
    return mode == "suspend" || mode == "always";
}

bool WindowManager::LockScreenAutoOnStartup() const
{
    return Utils::Lower(m_config.GetString("lockscreen.after", "suspend")) == "always";
}

// --- Command execution -------------------------------------------------------

void WindowManager::Execute(const Command& command)
{
    switch (command.type)
    {
        case CommandType::Exec:
        {
            std::string resolved = m_config.GetString("exec." + command.stringArg);
            Process::Spawn(resolved.empty() ? command.stringArg : resolved, m_connection.DisplayName());
            break;
        }

        case CommandType::Close:            CloseFocused();                        break;
        case CommandType::ToggleFloating:   ToggleFloating();                      break;
        case CommandType::ToggleFullscreen: ToggleFullscreen();                    break;
        case CommandType::ScratchpadToggle: ToggleScratchpadForFocused();          break;
        case CommandType::Workspace:        SwitchWorkspace(command.intArg);       break;
        case CommandType::MoveToWorkspace:  MoveFocusedToWorkspace(command.intArg);break;
        case CommandType::FocusDirection:   FocusDirection(command.directionArg);  break;
        case CommandType::MoveDirection:    MoveFocusedDirection(command.directionArg); break;
        case CommandType::FocusCycle:       FocusCycle(command.intArg);            break;
        case CommandType::Rotate:           RotateFocused();                       break;
        case CommandType::Flip:             FlipFocused();                         break;
        case CommandType::FocusMonitor:     FocusMonitorCommand(command.stringArg);      break;
        case CommandType::MoveToMonitor:    MoveFocusedToMonitorCommand(command.stringArg); break;
        case CommandType::Reload:           ReloadConfig();                        break;
        case CommandType::Quit:             m_running = false;                     break;
        case CommandType::LauncherToggle:   ToggleLauncher();                      break;
        case CommandType::NotepadToggle:    ToggleNotepad();                       break;
        case CommandType::Lock:

            if (LockScreenManualAllowed())
                m_lockScreen.Lock(m_monitors, m_wallpaperManager);

            break;

        case CommandType::LauncherReload:
            m_launcher.ReloadDesktopEntries();
            m_launcher.HandleExpose();
            break;

        case CommandType::NoCommand:
        default:
            break;
    }
}

// --- MouseManager-facing API -------------------------------------------------

ManagedWindow* WindowManager::WindowAt(const Point& point)
{
    Monitor* monitor = m_monitors.Containing(point);

    if (!monitor)
        monitor = &FocusedMonitor();

    Workspace* workspace = monitor->ActiveWorkspace();

    if (!workspace)
        return nullptr;

    return workspace->Tree().HitTest(point);
}

ManagedWindow* WindowManager::FloatingWindowAt(const Point& point) const
{
    ManagedWindow* focusedMatch = nullptr;
    ManagedWindow* lastMatch = nullptr;

    // Scans every monitor, not just the focused one - the point being
    // dragged from could be over any of them, and there's no reason to
    // assume it's the focused monitor specifically (the mouse just put
    // the cursor there; UpdateFocusedMonitorFromPointer() will have
    // already made that monitor the focused one anyway by the time a
    // press event follows the crossing, but there's no need to depend
    // on that ordering here).
    for (const auto& monitor : m_monitors.All())
    {
        Workspace* workspace = monitor->ActiveWorkspace();

        if (!workspace)
            continue;

        for (ManagedWindow* window : m_repository.Visible(workspace->Id()))
        {
            if (!window->IsFloating())
                continue;

            if (!window->Geometry().Contains(point))
                continue;

            lastMatch = window;

            if (window->Focused())
                focusedMatch = window;
        }
    }

    return focusedMatch ? focusedMatch : lastMatch;
}

void WindowManager::SwapWindows(ManagedWindow* first, ManagedWindow* second)
{
    if (!first || !second)
        return;

    m_workspaces.Get(first->Workspace()).Tree().Swap(first, second);

    Arrange();

    // Adaptive Placement (see PlacementHabitStore's header comment):
    // both windows just changed sides by hand (a Super+LMB drag, or
    // Super+Shift+h/j/k/l - SwapWindows() is the sole chokepoint for
    // both), so both get a vote for wherever they actually landed,
    // using the fresh geometry Arrange() just wrote. Deliberately not
    // called from ApplyAdaptivePlacementNudge()'s own tree.Swap()
    // calls, which go straight to BSPTree rather than through here -
    // an automatic nudge isn't a user decision to learn from.
    RecordAdaptivePositionHabit(first);
    RecordAdaptivePositionHabit(second);
}

void WindowManager::ResizeWindow(ManagedWindow* window, int dx, int dy)
{
    if (!window)
        return;

    m_workspaces.Get(window->Workspace()).Tree().Resize(window, dx, dy);

    Arrange();
}

void WindowManager::SetResizingCursor(bool resizing)
{
    m_cursor.SetResizing(resizing);
}

void WindowManager::BeginSwapDrag(ManagedWindow* window, const Point& cursor)
{
    if (!window || !window->IsTiled())
        return;

    m_activeDragWindow = window;
    m_dragHoverTarget = nullptr;

    const Rect& geometry = window->Geometry();
    m_dragOffsetX = cursor.x - geometry.x;
    m_dragOffsetY = cursor.y - geometry.y;
    m_dragCurrentRect = geometry;

    m_connection.Raise(window->Id());
    RaiseModalWindows();
    m_cursor.SetDragging(true);
}

void WindowManager::UpdateSwapDrag(ManagedWindow* window, const Point& cursor)
{
    if (!window || window != m_activeDragWindow)
        return;

    // The window "breaks off and moves with the cursor" - only its
    // position changes, and only on screen; nothing else in the tree
    // is touched until the button is released.
    m_dragCurrentRect.x = cursor.x - m_dragOffsetX;
    m_dragCurrentRect.y = cursor.y - m_dragOffsetY;

    m_connection.MoveWindowTo(window->Id(), m_dragCurrentRect.x, m_dragCurrentRect.y);

    // Highlight whichever tiled window the cursor is currently over,
    // so - per the doc's "safety feedback loop" - it's clear exactly
    // what will be swapped with before that's committed by releasing
    // the button.
    ManagedWindow* hovered = m_workspaces.Get(window->Workspace()).Tree().HitTest(cursor);

    if (hovered == window)
        hovered = nullptr;

    if (hovered == m_dragHoverTarget)
        return;

    if (m_dragHoverTarget)
        RefreshBorderColor(m_dragHoverTarget);

    m_dragHoverTarget = hovered;

    if (m_dragHoverTarget && m_dragHoverTarget->BorderWidth() > 0)
        m_connection.SetBorderColor(m_dragHoverTarget->Id(), ParseColor("general.border_color_active", "0x89b4fa"));
}

void WindowManager::EndSwapDrag(ManagedWindow* window, const Point& cursor)
{
    if (!window || window != m_activeDragWindow)
        return;

    m_cursor.SetDragging(false);

    if (m_dragHoverTarget)
        RefreshBorderColor(m_dragHoverTarget);

    ManagedWindow* target = m_workspaces.Get(window->Workspace()).Tree().HitTest(cursor);

    if (target == window)
        target = nullptr;

    Rect fromRect = m_dragCurrentRect;

    // Clear the drag state *before* SwapWindows()/Arrange() run below,
    // so Arrange() is free to reposition this window like any other
    // tiled one again.
    m_activeDragWindow = nullptr;
    m_dragHoverTarget = nullptr;

    constexpr int kSwapAnimationMs = 160;

    if (target)
    {
        Rect targetFromRect = target->Geometry();

        // Arrange() (called inside SwapWindows()) immediately snaps
        // both windows to their new final rects - the two Animator
        // calls below then replay that transition visually, from
        // wherever each window actually was, instead of it happening
        // as an instant jump.
        SwapWindows(window, target);

        m_animator.Start(window, fromRect, window->Geometry(), kSwapAnimationMs);
        m_animator.Start(target, targetFromRect, target->Geometry(), kSwapAnimationMs);
    }
    else
    {
        // No valid drop target - snap back to exactly where it was.
        // The tree assignment was never touched during the drag, so
        // window->Geometry() still holds the original tiled rect.
        m_animator.Start(window, fromRect, window->Geometry(), kSwapAnimationMs);
    }
}

void WindowManager::BeginFloatingDrag(ManagedWindow* window, const Point& cursor)
{
    if (!window || !window->IsFloating())
        return;

    m_activeDragWindow = window;

    const Rect& geometry = window->Geometry();
    m_dragOffsetX = cursor.x - geometry.x;
    m_dragOffsetY = cursor.y - geometry.y;
    m_dragCurrentRect = geometry;

    m_connection.Raise(window->Id());
    RaiseModalWindows();
    m_cursor.SetDragging(true);
}

void WindowManager::UpdateFloatingDrag(ManagedWindow* window, const Point& cursor)
{
    if (!window || window != m_activeDragWindow)
        return;

    // Follows the cursor 1:1, same as a Swap drag - just never swaps
    // anything, since there's no tile to trade places with.
    m_dragCurrentRect.x = cursor.x - m_dragOffsetX;
    m_dragCurrentRect.y = cursor.y - m_dragOffsetY;

    m_connection.MoveWindowTo(window->Id(), m_dragCurrentRect.x, m_dragCurrentRect.y);
    window->SetGeometry(m_dragCurrentRect);

    // Requirement 3: live monitor crossing. Whichever monitor the
    // window's center currently sits over becomes its home immediately,
    // not just once the drag ends - "drag across the boundary, it
    // belongs to the new monitor" - purely bookkeeping (workspace
    // membership) here, deliberately leaving its on-screen position
    // exactly where the cursor put it; EndFloatingDrag() is what
    // finally clamps/re-homes the geometry itself once the button is
    // released.
    Point center(m_dragCurrentRect.CenterX(), m_dragCurrentRect.CenterY());
    Monitor* under = m_monitors.Containing(center);

    if (!under || !under->ActiveWorkspace() || under->Id() == window->Monitor())
        return;

    int newWorkspaceId = under->ActiveWorkspace()->Id();

    if (window->Workspace() != newWorkspaceId)
        window->SetWorkspace(newWorkspaceId);

    window->SetMonitor(under->Id());

    // Keeps everything else consistent (bar workspace indicators,
    // `kohikoctl clients`/`monitors`, any tiled windows on the
    // source/destination workspaces reflowing to account for one
    // gaining/losing a floating window - which doesn't actually change
    // their layout, since floating windows never participate in the
    // BSP tree, but keeps Visible()'s bookkeeping honest either way)
    // without moving this window's own geometry - it's exempted from
    // ArrangeMonitor()'s positioning while m_activeDragWindow, same as
    // during a Swap drag.
    Arrange();
}

void WindowManager::EndFloatingDrag(ManagedWindow* window, const Point& cursor)
{
    if (!window || window != m_activeDragWindow)
        return;

    m_cursor.SetDragging(false);

    m_dragCurrentRect.x = cursor.x - m_dragOffsetX;
    m_dragCurrentRect.y = cursor.y - m_dragOffsetY;

    Point center(m_dragCurrentRect.CenterX(), m_dragCurrentRect.CenterY());
    Monitor* landedOn = m_monitors.Containing(center);
    Monitor& finalMonitor = landedOn ? *landedOn : FocusedMonitor();

    // Final clamp against whichever monitor it's actually landing on -
    // releasing right at (or past) an edge can't leave it partially or
    // fully off every monitor.
    Rect finalRect = m_dragCurrentRect.ClampedTo(finalMonitor.WorkArea(), window->BorderWidth());

    window->SetGeometry(finalRect);
    window->SetFloatingGeometry(finalRect);
    window->SetMonitor(finalMonitor.Id());

    if (finalMonitor.ActiveWorkspace())
        window->SetWorkspace(finalMonitor.ActiveWorkspace()->Id());

    m_activeDragWindow = nullptr;

    Arrange();
}

void WindowManager::BeginFloatingResize(ManagedWindow* window, const Point& cursor)
{
    if (!window || !window->IsFloating())
        return;

    m_activeResizeWindow = window;
    m_resizeStartRect = window->Geometry();
    m_resizeStartCursor = cursor;

    // Whichever half of the window (horizontally and vertically) the
    // cursor grabbed decides which edge(s) move for the rest of this
    // drag - grabbing the left half resizes from the left, the right
    // half from the right, and a corner does both axes at once, the
    // same convention as dragging a border on any other floating WM.
    Point center(m_resizeStartRect.CenterX(), m_resizeStartRect.CenterY());
    m_resizeFromLeft = cursor.x < center.x;
    m_resizeFromTop  = cursor.y < center.y;

    m_connection.Raise(window->Id());
    RaiseModalWindows();
}

void WindowManager::UpdateFloatingResize(ManagedWindow* window, const Point& cursor)
{
    if (!window || window != m_activeResizeWindow)
        return;

    Monitor* shownOn = MonitorShowing(window->Workspace());
    Monitor& monitor = shownOn ? *shownOn : FocusedMonitor();

    Rect rect = ResizedFloatingRect(window, cursor, monitor);

    m_connection.MoveResizeWindow(window->Id(), rect);
    window->SetGeometry(rect);
}

void WindowManager::EndFloatingResize(ManagedWindow* window, const Point& cursor)
{
    if (!window || window != m_activeResizeWindow)
        return;

    Monitor* shownOn = MonitorShowing(window->Workspace());
    Monitor& monitor = shownOn ? *shownOn : FocusedMonitor();

    Rect rect = ResizedFloatingRect(window, cursor, monitor);

    m_connection.MoveResizeWindow(window->Id(), rect);
    window->SetGeometry(rect);
    window->SetFloatingGeometry(rect);

    m_activeResizeWindow = nullptr;

    Arrange();
}

// --- IPCServer-facing API ---------------------------------------------------

std::string WindowManager::HandleIpcCommand(const std::string& request)
{
    std::istringstream stream(request);

    std::string verb;
    stream >> verb;
    verb = Utils::Lower(verb);

    if (verb == "dispatch")
    {
        std::string rest;
        std::getline(stream, rest);
        Execute(Command::Parse(Utils::Trim(rest)));
        return "ok";
    }

    if (verb == "clients")
        return DumpClientsJson();

    if (verb == "monitors")
        return DumpMonitorsJson();

    if (verb == "activewindow")
        return DumpActiveWindowJson();

    if (verb == "tree")
    {
        int id = 0;
        stream >> id;

        if (id < 1 || id > m_workspaces.Count())
            id = FocusedWorkspaceId();

        return m_workspaces.Get(id).Tree().Serialize();
    }

    if (verb == "reload")
    {
        ReloadConfig();
        return "ok";
    }

    if (verb == "reloadlauncher")
    {
        m_launcher.ReloadDesktopEntries();
        m_launcher.HandleExpose();
        return "ok";
    }

    if (verb == "quit")
    {
        m_running = false;
        return "ok";
    }

    return "error: unknown command";
}

// --- internals ----------------------------------------------------------------

void WindowManager::Manage(WindowID id)
{
    if (m_repository.Contains(id))
        return;

    // Never manage Kohiko's own utility windows. They're all created
    // override_redirect (so X never even offers them to us as a
    // MapRequest in the first place), but checking their IDs here too
    // is cheap, explicit insurance that Super+Q - or anything else
    // that walks m_repository - can *never* end up targeting the bar,
    // the launcher, or the notepad, no matter what changes elsewhere.
    // That guarantee is what "the panel should never close this
    // combination" means in practice: CloseFocused() only ever acts on
    // m_repository.Focused(), and this is what keeps that set free of
    // Kohiko's own popups by construction, not by convention.
    if (BarWindowMatching(id) || id == m_launcher.WindowId() || id == m_notepad.WindowId() || id == m_powerMenu.WindowId() || id == m_lockScreen.WindowId())
        return;

    XWindowAttributes attrs{};

    if (m_connection.GetWindowAttributes(id, attrs) && attrs.override_redirect)
        return;

    // Some other panel/taskbar (not Kohiko's own Bar) declaring
    // _NET_WM_WINDOW_TYPE_DOCK - most set override-redirect too and
    // never reach here at all, but that's a convention, not something
    // EWMH requires, so this is the one check that reliably catches
    // the rest. Matters most for AdoptExistingWindows() at startup
    // (a leftover panel from before Kohiko started is exactly the
    // "docks/panels" case that path has to leave alone), but applies
    // here rather than only there so a dock mapped *after* startup is
    // never swallowed into the tiling layout either.
    if (m_connection.IsDockWindowType(id, m_atoms))
        return;

    // See XConnection::IsXEmbedWindow()'s own comment for why this
    // has to be checked here, this early, rather than cleaned up
    // after the fact once SystemTray reparents the window away: by
    // then it would already have a BSP slot, a taskbar entry, and
    // focus eligibility it was never supposed to get, all rendering
    // nothing once the window's actual content moves into the tray a
    // moment later. Not calling XMapWindow here matters just as much
    // as not calling m_repository.Add(): SystemTray::DockIcon() maps
    // this window itself, after reparenting it, as the last step of
    // the XEmbed handshake - mapping it here first would show it,
    // full-sized, in whatever tile it would have landed in, for
    // however many milliseconds pass until the dock request is
    // processed.
    if (m_connection.IsXEmbedWindow(id, m_atoms))
        return;

    ManagedWindow* window = m_repository.Add(id);

    window->SetTitle(m_connection.GetWindowTitle(id, m_atoms));

    std::string className;
    std::string instanceName;
    m_connection.GetWindowClass(id, className, instanceName);
    window->SetClassName(className);
    window->SetInstanceName(instanceName);
    window->SetRole(m_connection.GetWindowRole(id, m_atoms));
    window->SetPid(m_connection.GetWindowPid(id, m_atoms));

    // Provisional - a `windowrule=workspace:N` match or a transient's
    // parent (both resolved below, once we know them) can override
    // this; a plain top-level window keeps it as-is, landing on
    // whichever monitor/workspace the user was actually looking at
    // when they opened it.
    window->SetWorkspace(FocusedWorkspaceId());
    window->SetMonitor(FocusedMonitor().Id());
    window->SetBorderWidth(m_config.GetInt("general.border_size", 2));

    // Needed before TryTile() below - a client (TLauncher's real UI is
    // the reliable reproducer) that declares a minimum size larger
    // than the tile it ends up in will still get force-resized into
    // that tile regardless (MoveResizeWindow isn't bound by the hint -
    // only *interactive* resizing agents are expected to respect it),
    // and unlike a client that's small enough to just look cramped,
    // one whose layout can't actually fit below its own stated minimum
    // clips content outright - parts of the GUI become genuinely
    // inaccessible rather than just tight. TryTile()'s capacity check
    // needs this so it can recognise "no, this one specifically needs
    // more room than that" instead of only ever checking against the
    // one-size-fits-all general.min_tile_width/height.
    int minWidth = 0;
    int minHeight = 0;
    m_connection.GetMinSize(id, minWidth, minHeight);
    window->SetMinSize(minWidth, minHeight);

    m_connection.SelectInputFor(id, EnterWindowMask | PropertyChangeMask | FocusChangeMask);

    // Plain (no Super) click-to-focus: sync-grabbed so we always get
    // first refusal, then replayed so the client still sees the click.
    for (unsigned int variant : m_connection.LockVariants(0))
        m_connection.GrabButtonOnWindow(id, Button1, variant);

    WindowID transientOwner = 0;
    bool isTransient = m_connection.GetTransientFor(id, transientOwner);
    bool isFloatingType = m_connection.IsFloatingWindowType(id, m_atoms);

    // The managed window `id` is WM_TRANSIENT_FOR, if the hint
    // resolves to one - nullptr for a plain top-level window, for a
    // hint pointing at the root/an unmanaged window, and for a hint
    // that hasn't been set at all. Everything below that talks about
    // "the parent" means this, not the raw transientOwner id.
    ManagedWindow* parent = isTransient ? m_repository.Get(transientOwner) : nullptr;

    WindowRuleEffect rules = ResolveWindowRules(className, instanceName, window->Title());

    // Whether a `workspace<N>=` autostart line (RunAutostart()) was
    // the thing that launched this window's process in the first
    // place - resolved here, before it's needed below, since (like
    // the windowrule above) which workspace this window even competes
    // for tiling room on depends on it.
    int autostartWorkspace = ResolveWorkspaceAutostart(window->Pid());

    // Session Restore: a record here means this exact X11 Window ID
    // was already managed - and had its state saved - the last time
    // Kohiko ran (see SessionStore's header comment for why the ID
    // itself is a reliable enough match). `sessionPriority` reflects
    // session.restore_priority (config/session, default config) and
    // is reused below for the float/tile decision too - see
    // AdoptExistingWindows() for the startup call site that makes
    // most windows this actually fires for.
    const SessionWindowState* session = m_session.Find(id);
    bool sessionPriority = Utils::Lower(m_config.GetString("session.restore_priority", "config")) == "session";

    // A `windowrule=workspace:N` match ("open Telegram's media viewer
    // on its own dedicated workspace instead of cluttering whatever's
    // current" is the motivating example) has to land before the
    // tiling-capacity checks below, since which workspace this window
    // even competes for room on depends on it. An explicit rule like
    // that is a deliberate per-window override and wins over automatic
    // parent-attachment; absent one, a transient/dialog always belongs
    // on whichever workspace its parent actually lives on right now -
    // "the child always appears attached to its parent, never on the
    // wrong workspace" - rather than wherever happened to be current
    // when it was mapped. A `workspace<N>=` autostart placement slots
    // in between those two: it's specific to this one launch (not
    // just its class, the way a windowrule is), so it wins over
    // parent-attachment too, but an explicit windowrule is still the
    // more deliberate, more specific override and takes precedence
    // over it.
    bool hasConfigWorkspace = rules.forceWorkspace >= 1 && rules.forceWorkspace <= m_workspaces.Count();
    bool hasSessionWorkspace = session && session->workspace >= 1 && session->workspace <= m_workspaces.Count();

    if (sessionPriority && hasSessionWorkspace)
        window->SetWorkspace(session->workspace);
    else if (hasConfigWorkspace)
        window->SetWorkspace(rules.forceWorkspace);
    else if (hasSessionWorkspace)
        window->SetWorkspace(session->workspace);
    else if (autostartWorkspace >= 1 && autostartWorkspace <= m_workspaces.Count())
        window->SetWorkspace(autostartWorkspace);
    else if (parent)
        window->SetWorkspace(parent->Workspace());
    else if (int habitWorkspace = AdaptiveWorkspaceHabit(AppClassKey(window)); habitWorkspace != 0)
        window->SetWorkspace(habitWorkspace);

    // Window Placement: a transient/dialog always follows its parent's
    // *monitor*, not just its workspace - if the parent's workspace is
    // currently visible somewhere, that monitor is where this window
    // belongs too, full stop, regardless of which monitor is focused
    // right now. Everything else (a plain top-level window, or a
    // `windowrule=workspace:N` window whose forced workspace happens
    // to already be visible on some other monitor) follows the same
    // "wherever that workspace actually is right now" rule; only a
    // window landing on a workspace nothing is currently showing falls
    // back to FocusedMonitor() - there's no "correct" monitor for a
    // window that isn't visible anywhere yet, so the one the user is
    // actually looking at is the least surprising choice.
    Monitor* shownOn = MonitorShowing(window->Workspace());

    // Only reached when nothing is currently showing this window's
    // workspace at all - if some monitor already is, that's always
    // the right answer regardless of session state (see comment
    // above). A saved output name that isn't connected right now
    // (or wasn't saved at all) just falls through to FocusedMonitor()
    // exactly as before session restore existed.
    Monitor* sessionMonitor =
        (!shownOn && session && !session->monitorName.empty())
            ? m_monitors.FindByName(session->monitorName)
            : nullptr;

    Monitor& targetMonitor = shownOn ? *shownOn : (sessionMonitor ? *sessionMonitor : FocusedMonitor());
    window->SetMonitor(targetMonitor.Id());

    // `windowrule=tile` is an explicit "no matter what this
    // application's own hints say, force it into the tiling layout"
    // override - a Java/Swing launcher that insists on floating at its
    // own fixed size (and fights the layout it's given) is the
    // motivating example. It skips the transient/dialog auto-float
    // path below entirely and falls straight through to the same
    // TryTile() every ordinary window goes through - which, unlike a
    // ConfigureRequest a client might send afterward asking to be its
    // "own size" again, Kohiko simply never honours for a tiled window
    // (see HandleConfigureRequest): once tiled, it stays exactly the
    // size Kohiko's layout says, strictly, for good.
    //
    // Absent that override, every dialog/utility/splash/toolbar/popup
    // window type and every window with a WM_TRANSIENT_FOR set at all
    // floats - only a plain window with none of those (EWMH's
    // definition of NORMAL) ever reaches TryTile() below, so only
    // NORMAL windows ever enter the BSP tree.
    // `windowrule=tile`/`=float` are explicit per-window overrides;
    // session.restore_priority decides whether they or a saved
    // session state win when both exist and disagree - see this
    // function's session/sessionPriority comment above. Absent either
    // opinion, a plain window falls back to the client-type heuristics
    // (dialog/transient/etc.) exactly as before session restore
    // existed.
    bool configHasFloatOpinion = rules.forceFloat || rules.forceTile;
    bool configWantsFloat = !rules.forceTile && rules.forceFloat;
    bool sessionHasFloatOpinion = session != nullptr;

    bool wantsFloat;

    if (sessionPriority && sessionHasFloatOpinion)
        wantsFloat = session->floating;
    else if (configHasFloatOpinion)
        wantsFloat = configWantsFloat;
    else if (sessionHasFloatOpinion)
        wantsFloat = session->floating;
    else
        wantsFloat = (isTransient || isFloatingType);

    bool sessionPositionRestored = false;

    if (wantsFloat)
    {
        window->SetState(WindowState::Floating);

        // A saved floating geometry (real width/height, not the
        // struct's zero-initialized default) takes precedence over
        // re-centering it from scratch - restoring "where it actually
        // was" is the whole point of session restore for a floating
        // window. Falls back to the usual centered placement for a
        // window with no saved geometry (nothing in session, or one
        // whose class/type just started floating for the first time).
        Rect geometry =
            (session && session->floatingGeometry.width > 0 && session->floatingGeometry.height > 0)
                ? session->floatingGeometry
                : CenteredFloatingRectForWindow(id, attrs, targetMonitor, parent);

        window->SetGeometry(geometry);
        window->SetFloatingGeometry(geometry);

        m_connection.MoveResizeWindow(id, geometry);
        m_connection.SetBorderWidth(id, window->BorderWidth());
    }
    else if ((sessionPositionRestored = TryRestoreSessionPosition(window, session, targetMonitor)) ||
             TryTile(window, window->Workspace(), targetMonitor))
    {
        // Tiled on the current workspace - either restored to its
        // exact previous BSP position (see TryRestoreSessionPosition())
        // or, far more often, freshly placed by the common case.
    }
    else
    {
        int currentWorkspace = window->Workspace();
        int alternate = FindWorkspaceWithRoom(window, currentWorkspace, targetMonitor);

        if (alternate != 0 && TryTile(window, alternate, targetMonitor))
        {
            Logger::Info(
                "Workspace " + std::to_string(currentWorkspace) +
                " has no room for another tile without dropping below the "
                "configured minimum size - opened window " +
                std::to_string(static_cast<unsigned long>(id)) +
                " on workspace " + std::to_string(alternate) + " instead.");
        }
        else
        {
            // Every workspace's tiling layout is genuinely full (down
            // to minWidth x minHeight everywhere) - what happens next
            // is governed by general.tiling_misbehavior_fallback, the
            // same knob ApplyTilingMisbehaviorFallback() uses. It's
            // really the same question either way - "this window can't
            // have fixed tiled geometry right now, where does it go
            // instead?" - just asked at open time here rather than
            // after the fact.
            std::string fallback = Utils::Lower(m_config.GetString("general.tiling_misbehavior_fallback", "floating"));

            bool placed = false;

            if (fallback == "new_workspace")
            {
                int target = FindWorkspaceWithFewestWindows(currentWorkspace);

                if (target != 0 && target != currentWorkspace)
                {
                    window->SetWorkspace(target);
                    placed = TryTile(window, target, targetMonitor);

                    if (placed)
                    {
                        Logger::Info(
                            "No workspace has room for another tile without "
                            "dropping below the configured minimum size - moved "
                            "window " + std::to_string(static_cast<unsigned long>(id)) +
                            " to workspace " + std::to_string(target) +
                            " (fewest windows) instead.");
                    }
                    else
                    {
                        window->SetWorkspace(currentWorkspace);
                    }
                }
            }

            if (!placed)
            {
                // Either the fallback is "floating", or "new_workspace"
                // genuinely had nowhere better to go either - what
                // actually guarantees "no window ever goes beyond the
                // screen, or gets forced into an undersized tile" is
                // floating it instead.
                window->SetState(WindowState::Floating);

                Rect geometry = CenteredFloatingRectForWindow(id, attrs, targetMonitor, parent);
                window->SetGeometry(geometry);
                window->SetFloatingGeometry(geometry);

                m_connection.MoveResizeWindow(id, geometry);
                m_connection.SetBorderWidth(id, window->BorderWidth());

                Logger::Warning(
                    "No workspace has room for another tile without dropping "
                    "below the configured minimum size - opened window " +
                    std::to_string(static_cast<unsigned long>(id)) + " floating instead.");
            }
        }
    }

    // Adaptive Placement (see PlacementHabitStore's header comment):
    // only for a window that actually landed tiled, and only when
    // TryRestoreSessionPosition() above didn't already put it exactly
    // where it sat at last shutdown - that's a strictly more precise
    // answer than a general habit could ever be, so there's nothing
    // for the softer heuristic to improve on.
    if (window->IsTiled() && !sessionPositionRestored)
        ApplyAdaptivePlacementNudge(window);

    // A client's own EWMH fullscreen request, made before it was ever
    // mapped (some toolkits set _NET_WM_STATE directly instead of
    // sending a ClientMessage, since the message form is only
    // meaningful for an already-mapped window - see
    // HandleClientMessage() for that path) and `windowrule=fullscreen`
    // (flameshot's screenshot overlay - which needs to genuinely cover
    // the whole screen to work at all, not open at some fraction of it
    // - is the motivating example for both) mean the same thing here:
    // this window should already be fullscreen the instant it appears,
    // not tiled/floating-sized for one frame first. A saved session
    // fullscreen state means the same thing for a restored window.
    // `nofullscreen` wins over all three if a rule says this class
    // should never be allowed real fullscreen at all - there's no
    // real "priority" question here the way there is for
    // workspace/float above, since none of these three sources ever
    // actively says "must not be fullscreen" on their own; only
    // denyFullscreen does.
    bool wantsFullscreen =
        !rules.denyFullscreen &&
        (rules.forceFullscreen || (session && session->fullscreen) ||
         m_connection.HasNetWmStateFullscreen(id, m_atoms));

    if (wantsFullscreen)
    {
        window->SetPreviousState(window->State());
        window->SetState(WindowState::Fullscreen);
        m_connection.SetNetWmState(m_atoms, id, true);
    }

    // A transient/dialog child was pinned to its parent's workspace
    // (and, via targetMonitor above, its parent's monitor) already -
    // Window Placement's "transient/dialog windows must always follow
    // their parent monitor". What's left is making sure it's actually
    // *visible*: if the parent's workspace is already showing on some
    // monitor (the common case - the parent has to be visible for a
    // dialog to even make sense), just move focus there so the new
    // dialog gets keyboard input without touching what any other
    // monitor is showing; only a parent sitting on a background
    // workspace nobody's displaying needs that workspace actually
    // switched onto the focused monitor to bring both it and the new
    // dialog into view. Deliberately gated on `parent` (not just
    // "landed on a background workspace" in general) - an ordinary
    // `windowrule=workspace:N` window with no parent is *supposed* to
    // open quietly in the background without yanking focus away from
    // whatever the user is doing (see the comment on that rule in
    // ResolveWindowRules's caller above); only a transient explicitly
    // follows its parent into view.
    if (parent)
    {
        if (Monitor* visibleOn = MonitorShowing(window->Workspace()))
            FocusMonitor(*visibleOn);
        else
            SwitchWorkspaceOnMonitor(FocusedMonitor(), window->Workspace());
    }

    // A window the fallback above redistributed onto a *different*
    // (background) workspace must stay unmapped and unfocused, exactly
    // like every other window living on a workspace nothing is
    // currently displaying - Arrange() unmaps it below regardless
    // (see its comment), and it gets its chance at focus once some
    // monitor actually switches to that workspace. Mapping/focusing it
    // here regardless of visibility would show a window nobody asked
    // to see right now and steal focus from whatever *is* currently on
    // screen.
    if (IsWorkspaceVisible(window->Workspace()))
    {
        // Resize into its real tile slot *before* mapping, not after.
        // A client (Java/Swing apps like TLauncher are the reliable
        // reproducer, but it's not Java-specific) that gets mapped at
        // its own small requested size and is then immediately
        // resized larger can end up with its content stuck in the
        // corner corresponding to that original size - the newly
        // exposed area never gets painted, because the resize raced
        // the client's own realization instead of being the size it
        // was realized at in the first place. Configuring geometry on
        // a not-yet-mapped window is perfectly legal (and is exactly
        // what the floating branch above, and every other
        // geometry-then-Map site in this file, already does) - Arrange()
        // just needs to run before MapWindow() here too.
        Arrange();
        m_connection.MapWindow(id);

        // A window opened while the Launcher/Notepad is up should
        // still tile in and become visible - just not take keyboard
        // focus away from it. The Launcher deliberately never uses an
        // XGrabKeyboard (see its class comment), so it depends
        // entirely on actually holding X input focus to receive
        // Enter/Escape/typing at all; losing focus here used to leave
        // it visibly open but completely unreachable from the
        // keyboard, with no way to even close it. Re-asserting focus
        // on the modal (rather than just skipping the Focus(id) call)
        // also covers apps that request focus for themselves as part
        // of mapping - HandleFocusIn() below is the second line of
        // defence for ones that grab it a moment later instead.
        if (m_launcher.IsOpen())
        {
            m_connection.SetInputFocus(m_launcher.WindowId());
            RefreshBorderColor(window);
        }
        else if (m_notepad.IsOpen())
        {
            m_connection.SetInputFocus(m_notepad.WindowId());
            RefreshBorderColor(window);
        }
        else
        {
            Focus(id);
        }
    }
    else
    {
        Arrange();
    }

    RefreshClientList();
}

void WindowManager::Unmanage(WindowID id)
{
    ManagedWindow* window = m_repository.Get(id);

    if (!window)
        return;

    bool wasFocused = window->Focused();
    int workspace = window->Workspace();

    if (window->OccupiesTreeSlot())
        m_workspaces.Get(workspace).Tree().Remove(window);

    m_scratchpad.Forget(id);
    m_repository.Remove(id);

    RefreshClientList();

    if (wasFocused)
        FocusNextAvailable();

    Arrange();
}

void WindowManager::RefreshClientList()
{
    std::vector<::Window> ids;
    ids.reserve(m_repository.All().size());

    for (ManagedWindow* window : m_repository.All())
        ids.push_back(window->Id());

    m_connection.SetClientList(m_atoms, ids);
}

void WindowManager::Focus(WindowID id)
{
    ManagedWindow* window = m_repository.Get(id);

    if (!window)
        return;

    // Keeps "the focused monitor" honest with "the focused window" -
    // every path that ends up focusing a window (a click, the mouse
    // entering it, a workspace switch, a Swap drag landing somewhere,
    // a new window opening, ...) funnels through here, so this is the
    // one place that needs to know about it rather than every call
    // site separately remembering to. Skipped for a scratchpad window:
    // it's reachable from every workspace on every monitor, so its
    // Workspace() field is just wherever it'll restore to if it's ever
    // un-scratchpadded, not "where it lives" - MonitorShowing() on
    // that would be meaningless at best and actively wrong at worst
    // (jumping focus to some unrelated monitor that stale workspace id
    // happens to currently be showing). ToggleScratchpadForFocused()
    // already picks the right monitor for it directly.
    if (!window->IsScratchpad())
    {
        if (Monitor* monitor = MonitorShowing(window->Workspace()))
            m_monitors.SetFocused(monitor);
    }

    m_repository.ClearFocus();
    window->SetFocused(true);

    m_workspaces.Get(window->Workspace()).Tree().Focus(window);

    m_connection.SetInputFocus(id);
    m_connection.SetActiveWindow(m_atoms, id);

    RefreshBorderColor(window);

    for (ManagedWindow* other : m_repository.Visible(window->Workspace()))
    {
        if (other->Id() != id)
            RefreshBorderColor(other);
    }

    UpdateAllBars();
}

void WindowManager::FocusNextAvailable()
{
    FocusNextAvailableOn(FocusedMonitor());
}

void WindowManager::FocusNextAvailableOn(Monitor& monitor)
{
    Workspace* workspace = monitor.ActiveWorkspace();
    auto visible = workspace ? m_repository.Visible(workspace->Id()) : std::vector<ManagedWindow*>{};

    if (!visible.empty())
    {
        Focus(visible.front()->Id());
        return;
    }

    // Nothing left on that monitor's workspace. Only actually clear
    // real X input focus (and the bar's title) when it's the focused
    // monitor losing its last window - fixing up a *different*
    // monitor's now-empty workspace shouldn't steal focus away from
    // whatever the user is actually looking at.
    if (&monitor != m_monitors.Focused())
        return;

    // Same reasoning as FocusMonitor()'s identical check: a shown
    // scratchpad window doesn't count as "visible" for this workspace
    // (WindowRepository::Visible() excludes it on purpose), but it can
    // still be the only thing actually on screen for this monitor -
    // e.g. the window underneath it just closed - and shouldn't lose
    // real X focus just because Visible() came back empty.
    if (m_scratchpad.HasWindow() && m_scratchpad.IsVisible())
    {
        ManagedWindow* scratch = m_repository.Get(m_scratchpad.Window());

        if (scratch && scratch->Monitor() == monitor.Id())
        {
            Focus(scratch->Id());
            return;
        }
    }

    m_repository.ClearFocus();
    m_connection.SetInputFocus(m_connection.Root());
    m_connection.SetActiveWindow(m_atoms, None);
    UpdateAllBars();
}

void WindowManager::Arrange()
{
    RefreshMonitorWorkAreas();

    // The set of workspace ids visible *somewhere* right now - one
    // per monitor (MonitorManager guarantees no two monitors ever
    // share an ActiveWorkspace(), so this is also each monitor's
    // workspace exactly once). Everything below - which windows get
    // unmapped, which get laid out - is driven off this, not any
    // single "current" id.
    std::vector<int> visibleWorkspaceIds;

    for (const auto& monitor : m_monitors.All())
        if (monitor->ActiveWorkspace())
            visibleWorkspaceIds.push_back(monitor->ActiveWorkspace()->Id());

    auto isVisible = [&](int workspaceId)
    {
        return std::find(visibleWorkspaceIds.begin(), visibleWorkspaceIds.end(), workspaceId)
            != visibleWorkspaceIds.end();
    };

    // Unmap anything that isn't on a currently-visible workspace.
    // Centralising this here - rather than every call site that
    // changes what should be visible (a workspace switch, a window
    // moving to another workspace/monitor, a hotplug, ...) separately
    // remembering to unmap what it just hid - is what makes those
    // call sites safe to write as "mutate state, then call Arrange()"
    // without having to reason about mapping themselves; see
    // ArrangeMonitor() below for the mirror-image "map/position
    // whatever should be visible" half.
    //
    // Only touches windows X still actually reports as mapped -
    // repeating XUnmapWindow() on one that's already unmapped is a
    // silent no-op that generates no UnmapNotify, so it would never
    // get paired back off by ConsumeIgnoredUnmap() and the ignore
    // count kept below would just grow forever, eventually causing a
    // *real* future close to be wrongly swallowed as "just Kohiko
    // hiding it" instead of "the client actually went away".
    for (ManagedWindow* window : m_repository.All())
    {
        if (window->IsScratchpad() || window == m_activeDragWindow)
            continue;

        if (isVisible(window->Workspace()))
            continue;

        XWindowAttributes attrs{};

        if (m_connection.GetWindowAttributes(window->Id(), attrs) && attrs.map_state == IsUnmapped)
            continue;

        window->IgnoreNextUnmap();
        m_connection.UnmapWindow(window->Id());
    }

    for (const auto& monitor : m_monitors.All())
        ArrangeMonitor(*monitor);

    UpdateAllBars();

    // Every Raise() above (a newly-mapped window, a floating window,
    // a fullscreen window) happened *after* the Launcher/Notepad were
    // last raised, which - left alone - would leave whichever one is
    // currently open stacked underneath something else even though it
    // still holds real input focus. Always give it the last word on
    // stacking order.
    RaiseModalWindows();

    m_connection.Flush();
}

void WindowManager::ArrangeMonitor(Monitor& monitor)
{
    Workspace* workspace = monitor.ActiveWorkspace();

    if (!workspace)
        return;

    Rect tilingArea = TilingArea(monitor);

    LayoutEngine::Params params;
    params.innerGap     = m_config.GetInt("general.inner_gap", 6);
    params.outerGap     = m_config.GetInt("general.outer_gap", 6);
    params.borderWidth  = m_config.GetInt("general.border_size", 2);
    params.smartGaps    = m_config.GetBool("general.smart_gaps", true);
    params.smartBorders  = m_config.GetBool("general.smart_borders", true);

    if (!workspace->Tree().Empty())
        m_layout.Apply(workspace->Tree().Root(), tilingArea, params);

    ManagedWindow* fullscreenWindow = nullptr;

    for (ManagedWindow* window : m_repository.Visible(workspace->Id()))
    {
        // Bookkeeping (IPC/`kohikoctl clients` reporting, and Focus()'s
        // "which monitor does this window's workspace live on"
        // lookups) - kept current for every visible window on every
        // pass, not just when a window first lands here.
        window->SetMonitor(monitor.Id());

        if (window == m_activeDragWindow)
            continue; // being driven directly by the Swap drag right now - don't snap it back

        if (window->IsFullscreen())
        {
            fullscreenWindow = window;
            continue;
        }

        if (window->IsFloating())
        {
            m_connection.MoveResizeWindow(window->Id(), window->Geometry());
            m_connection.SetBorderWidth(window->Id(), window->BorderWidth());
            m_connection.MapWindow(window->Id());
            m_connection.Raise(window->Id());
            continue;
        }

        m_connection.MoveResizeWindow(window->Id(), window->Geometry());
        m_connection.SetBorderWidth(window->Id(), window->BorderWidth());
        m_connection.MapWindow(window->Id());
    }

    if (fullscreenWindow)
    {
        const Rect& full = monitor.Geometry();

        m_connection.MoveResizeWindow(fullscreenWindow->Id(), full);
        m_connection.SetBorderWidth(fullscreenWindow->Id(), 0);
        m_connection.MapWindow(fullscreenWindow->Id());
        m_connection.Raise(fullscreenWindow->Id());

        // Keep bookkeeping (IPC/`kohikoctl clients` reporting) in sync
        // with what's actually on screen - the tree still computed a
        // tiled-slot rect for this leaf above, but that's not where
        // the window really is right now.
        fullscreenWindow->SetGeometry(full);
    }
}

void WindowManager::RefreshMonitorWorkAreas()
{
    // Every monitor hosts its own bar now (see RebuildBars()), so every
    // one reserves space for it - unlike the single-global-bar days,
    // there's no "only Primary() loses space to it" special case left.
    // Kept as its own pass (rather than folded into ArrangeMonitor())
    // so TryTile()/CenteredFloatingRect()/FindWorkspaceWithRoom() can
    // all read a monitor's WorkArea() directly without needing to
    // know the bar even exists.
    int barHeight = m_config.GetInt("general.bar_height", 26);

    for (const auto& monitor : m_monitors.All())
    {
        Rect area = monitor->Geometry();

        area.y += barHeight;
        area.height -= barHeight;

        monitor->SetWorkArea(area);
    }
}

Rect WindowManager::TilingArea(Monitor& monitor)
{
    return monitor.WorkArea();
}

void WindowManager::RefreshWorkspaceGeometry(int workspaceId, Monitor& referenceMonitor)
{
    Workspace& workspace = m_workspaces.Get(workspaceId);

    if (workspace.Tree().Empty())
        return;

    LayoutEngine::Params params;
    params.innerGap     = m_config.GetInt("general.inner_gap", 6);
    params.outerGap     = m_config.GetInt("general.outer_gap", 8);
    params.borderWidth  = m_config.GetInt("general.border_size", 2);
    params.smartGaps    = m_config.GetBool("general.smart_gaps", true);
    params.smartBorders = m_config.GetBool("general.smart_borders", true);

    // Same computation ArrangeMonitor() would do for a workspace that
    // actually is some monitor's ActiveWorkspace() right now - just
    // without the X11 MoveResizeWindow/MapWindow side that only makes
    // sense for whatever's actually on screen right now.
    m_layout.Apply(workspace.Tree().Root(), TilingArea(referenceMonitor), params);
}

void WindowManager::SwitchWorkspace(int id)
{
    SwitchWorkspaceOnMonitor(FocusedMonitor(), id);
}

void WindowManager::SwitchWorkspaceOnMonitor(Monitor& monitor, int id)
{
    if (id < 1 || id > m_workspaces.Count())
        return;

    Workspace* current = monitor.ActiveWorkspace();

    if (current && current->Id() == id)
        return; // already showing it - nothing to do

    // Workspace conflicts: switching workspace on one monitor must
    // NOT affect other monitors, and a workspace already visible
    // somewhere else is never swapped, stolen, or silently
    // reassigned - i3-style, the request is simply rejected outright,
    // with a notification on the *requesting* monitor's own bar so
    // it's obvious why nothing happened.
    if (Monitor* other = MonitorShowing(id))
    {
        ShowNotificationOnMonitor(
            monitor,
            "Workspace " + std::to_string(id) + " is already visible on monitor " +
                std::to_string(other->Id()));

        return;
    }

    monitor.SetWorkspace(&m_workspaces.Get(id));

    // The wallpaper for this monitor may now be different (see
    // wallpaper.workspace= in WallpaperManager.h) - re-resolve and
    // redraw it before Arrange() below, not after, so there's no
    // visible flash of the previous workspace's wallpaper showing
    // through the gaps for even one frame while windows are still
    // being retiled.
    m_wallpaperManager.ApplyToRoot(m_connection, m_monitors);

    // Arrange() is what actually unmaps whatever either monitor was
    // just showing and maps/positions whatever it's showing now - see
    // its comment. Every window that was ever tiled onto `id` while it
    // sat in the background already has fresh cached geometry (from
    // RefreshWorkspaceGeometry(), called by TryTile() for exactly this
    // reason), so there's no "resize immediately after mapping"
    // flicker to worry about here the way a brand new window opening
    // has to guard against in Manage().
    Arrange();

    FocusMonitor(monitor);
}

void WindowManager::ShowNotificationOnMonitor(Monitor& monitor, const std::string& text)
{
    if (Bar* bar = BarFor(monitor))
    {
        bar->ShowNotification(text);
        bar->Redraw();
    }

    Logger::Info(text);
}

Monitor& WindowManager::FocusedMonitor() const
{
    Monitor* focused = m_monitors.Focused();

    return focused ? *focused : m_monitors.Primary();
}

int WindowManager::FocusedWorkspaceId() const
{
    Workspace* workspace = FocusedMonitor().ActiveWorkspace();

    return workspace ? workspace->Id() : 1;
}

Monitor* WindowManager::MonitorShowing(int workspaceId) const
{
    for (const auto& monitor : m_monitors.All())
        if (monitor->ActiveWorkspace() && monitor->ActiveWorkspace()->Id() == workspaceId)
            return monitor.get();

    return nullptr;
}

bool WindowManager::IsWorkspaceVisible(int workspaceId) const
{
    return MonitorShowing(workspaceId) != nullptr;
}

void WindowManager::FocusMonitor(Monitor& monitor)
{
    m_monitors.SetFocused(&monitor);

    Workspace* workspace = monitor.ActiveWorkspace();
    auto visible = workspace ? m_repository.Visible(workspace->Id()) : std::vector<ManagedWindow*>{};

    if (!visible.empty())
    {
        Focus(visible.front()->Id());
        return;
    }

    // A shown scratchpad window is a global overlay, not tied to any
    // one workspace - WindowRepository::Visible() deliberately excludes
    // it (see its own comment) so it never gets counted as "the"
    // window for a workspace it isn't really on. But while it's the
    // only thing actually on screen for this monitor, it still needs
    // real X input focus, or the very next workspace switch back to
    // this monitor silently drops focus to nothing: the window stays
    // visible but unfocused, and every focused-window command
    // (releasing it via toggle_floating, closing it, typing into it)
    // quietly stops working until it's hidden and shown again.
    if (m_scratchpad.HasWindow() && m_scratchpad.IsVisible())
    {
        ManagedWindow* scratch = m_repository.Get(m_scratchpad.Window());

        if (scratch && scratch->Monitor() == monitor.Id())
        {
            Focus(scratch->Id());
            return;
        }
    }

    // Nothing on this monitor to focus - clear real X input focus
    // (Focus(id) is never reached to do it for us) while still making
    // sure every bar reflects current state.
    m_repository.ClearFocus();
    m_connection.SetInputFocus(m_connection.Root());
    m_connection.SetActiveWindow(m_atoms, None);
    UpdateAllBars();
}

void WindowManager::FocusMonitorCommand(const std::string& arg)
{
    Monitor* target = nullptr;

    if (arg == "left")       target = m_monitors.Neighbor(FocusedMonitor(), Direction::Left);
    else if (arg == "right") target = m_monitors.Neighbor(FocusedMonitor(), Direction::Right);
    else if (arg == "up")    target = m_monitors.Neighbor(FocusedMonitor(), Direction::Up);
    else if (arg == "down")  target = m_monitors.Neighbor(FocusedMonitor(), Direction::Down);
    else
    {
        try
        {
            target = m_monitors.ByIndex(std::stoi(arg));
        }
        catch (...)
        {
            target = nullptr;
        }
    }

    if (!target || target == &FocusedMonitor())
        return;

    FocusMonitor(*target);
}

void WindowManager::MoveFocusedToWorkspace(int id)
{
    ManagedWindow* window = m_repository.Focused();

    if (!window || id == window->Workspace() || id < 1 || id > m_workspaces.Count())
        return;

    int oldWorkspaceId = window->Workspace();

    // Adaptive Placement: an explicit "move THIS window to workspace
    // `id`" is exactly the kind of repeated-by-hand signal
    // PlacementHabitStore learns from - see its header comment. Voted
    // on unconditionally (not just when the move actually lands
    // tiled) since the user's *intent* was "this app belongs on
    // workspace `id`" regardless of how the destination happens to be
    // laid out right now.
    RecordAdaptiveWorkspaceHabit(window, id);

    // Whichever monitor is currently showing the destination workspace
    // is what TryTile()'s capacity math (and the floating fallback's
    // CenteredFloatingRect()) should be sized against - falling back
    // to FocusedMonitor() only for a purely background destination
    // nothing is displaying right now (see TryTile()'s comment on why
    // that's a fine best-effort guess either way).
    Monitor* shownOn = MonitorShowing(id);
    Monitor& referenceMonitor = shownOn ? *shownOn : FocusedMonitor();

    bool wasTiled = window->IsTiled();
    bool wasFullscreenTile =
        window->IsFullscreen() && window->PreviousState() == WindowState::Tiled;

    if (wasTiled || wasFullscreenTile)
        m_workspaces.Get(oldWorkspaceId).Tree().Remove(window);

    window->SetWorkspace(id);

    if (wasTiled && !TryTile(window, id, referenceMonitor))
    {
        // TryTile() only fails the capacity check, so the window isn't
        // in any tree right now - land it floating instead of forcing
        // an undersized tile. Explicit user intent ("move THIS window
        // to workspace `id`") still wins: it lands on workspace `id`,
        // just not tiled there.
        window->SetState(WindowState::Floating);

        Rect geometry = window->FloatingGeometry();

        if (geometry.width <= 0 || geometry.height <= 0)
            geometry = CenteredFloatingRect(referenceMonitor, 0.5f, 0.5f);

        window->SetGeometry(geometry);
        window->SetFloatingGeometry(geometry);

        Logger::Warning(
            "Workspace " + std::to_string(id) +
            " has no room for another tile without dropping below the "
            "configured minimum size - moved window " +
            std::to_string(static_cast<unsigned long>(window->Id())) +
            " there floating instead.");
    }
    else if (wasFullscreenTile)
    {
        // A window mid-fullscreen (Telegram's media viewer, a video
        // player, ...) should stay fullscreen right through a move to
        // another workspace, exactly as if it had just stayed put -
        // nothing above touched State() for this branch. All that's
        // still needed is a freshly reserved tiled slot on the *new*
        // workspace for it to restore into whenever it eventually
        // un-fullscreens, the same one TryTile() would hand a window
        // that opened there directly (see OccupiesTreeSlot()'s
        // comment for why this can't just reuse the old workspace's
        // now-removed leaf).
        if (TryTile(window, id, referenceMonitor))
        {
            // TryTile() always sets Tiled - it's still genuinely
            // fullscreen right now, so put that straight back.
            window->SetState(WindowState::Fullscreen);
        }
        else
        {
            // No room even for a restore slot on the target workspace -
            // the graceful fallback is exactly what un-fullscreening
            // onto an already-full workspace does elsewhere: land
            // floating instead, whenever it does eventually un-fullscreen.
            window->SetPreviousState(WindowState::Floating);
        }
    }

    // A window that was already floating (neither branch above ran)
    // keeps its FloatingGeometry() as-is when moved onto a background
    // workspace - there's no "correct" monitor to place it on until
    // something actually shows that workspace (RelocateOrphanedFloatingWindows()
    // is the general safety net for a rect that ends up nowhere real).
    // But if `id` is already visible on some *other* monitor right
    // now, the window is about to actually appear there immediately -
    // re-home its absolute coordinates into that monitor's space, the
    // same way MoveWindowToMonitor() does for an explicit monitor move,
    // so it doesn't just vanish off both screens.
    if (!wasTiled && !wasFullscreenTile && window->IsFloating() && shownOn && shownOn != &FocusedMonitor())
    {
        Rect relocated = RelocateRectToMonitor(window->Geometry(), FocusedMonitor().WorkArea(), shownOn->WorkArea());
        window->SetGeometry(relocated);
        window->SetFloatingGeometry(relocated);
    }

    // No manual unmap here - Arrange() unmaps anything that ends up on
    // a workspace nothing is currently showing, and just as happily
    // maps it straight onto its new tile if `id` turns out to already
    // be visible on another monitor (moving a window *between* two
    // monitors that are both on screen right now is exactly the case
    // the old single-monitor "always unmap unconditionally" version of
    // this function could never actually hit).
    Arrange();
    FocusNextAvailable();
}

void WindowManager::MoveFocusedToMonitorCommand(const std::string& arg)
{
    ManagedWindow* window = m_repository.Focused();

    if (!window)
        return;

    Monitor* target = nullptr;

    if (arg == "left")       target = m_monitors.Neighbor(FocusedMonitor(), Direction::Left);
    else if (arg == "right") target = m_monitors.Neighbor(FocusedMonitor(), Direction::Right);
    else if (arg == "up")    target = m_monitors.Neighbor(FocusedMonitor(), Direction::Up);
    else if (arg == "down")  target = m_monitors.Neighbor(FocusedMonitor(), Direction::Down);
    else
    {
        try
        {
            target = m_monitors.ByIndex(std::stoi(arg));
        }
        catch (...)
        {
            target = nullptr;
        }
    }

    if (!target || target == &FocusedMonitor())
        return;

    MoveWindowToMonitor(window, *target);
}

void WindowManager::MoveWindowToMonitor(ManagedWindow* window, Monitor& target)
{
    if (!window)
        return;

    Workspace* targetWorkspace = target.ActiveWorkspace();

    if (!targetWorkspace || window->Workspace() == targetWorkspace->Id())
        return;

    Monitor* sourceMonitor = MonitorShowing(window->Workspace());

    int oldWorkspaceId = window->Workspace();
    int targetWorkspaceId = targetWorkspace->Id();

    bool wasTiled = window->IsTiled();
    bool wasFullscreenTile =
        window->IsFullscreen() && window->PreviousState() == WindowState::Tiled;
    bool wasFullscreenFloat =
        window->IsFullscreen() && window->PreviousState() == WindowState::Floating;

    if (wasTiled || wasFullscreenTile)
        m_workspaces.Get(oldWorkspaceId).Tree().Remove(window);

    window->SetWorkspace(targetWorkspaceId);
    window->SetMonitor(target.Id());

    if (wasTiled || wasFullscreenTile)
    {
        if (TryTile(window, targetWorkspaceId, target))
        {
            if (wasFullscreenTile)
                window->SetState(WindowState::Fullscreen);
        }
        else
        {
            // Moving Windows: "preserve floating state if floating" -
            // there's no floating state to preserve here (it was
            // tiled), but the same principle applies in spirit: a
            // window that can't keep the property it had (a real tile
            // slot) on the destination monitor still has to land
            // *somewhere* sane rather than fail the move outright, so
            // it floats there instead - exactly TryTile()'s own
            // capacity-exceeded fallback, just reached via a monitor
            // move instead of a fresh open.
            window->SetState(wasFullscreenTile ? WindowState::Fullscreen : WindowState::Floating);

            if (wasFullscreenTile)
                window->SetPreviousState(WindowState::Floating);

            Rect geometry = CenteredFloatingRect(target, 0.5f, 0.5f);
            window->SetGeometry(geometry);
            window->SetFloatingGeometry(geometry);
        }
    }
    else if (window->IsFloating() || wasFullscreenFloat)
    {
        // Moving Windows: "preserve floating state if floating" - kept
        // floating, but re-homed from the source monitor's coordinate
        // space into the destination's, at the same *relative*
        // position/size where that still fits, rather than always
        // recentering outright (a window docked to a corner stays
        // docked to the analogous corner) - see
        // RelocateRectToMonitor()'s comment.
        Rect fromArea = sourceMonitor ? sourceMonitor->WorkArea() : target.WorkArea();
        Rect relocated = RelocateRectToMonitor(window->Geometry(), fromArea, target.WorkArea());

        window->SetGeometry(relocated);
        window->SetFloatingGeometry(relocated);
    }
    // else: doesn't occupy a tree slot and isn't floating (e.g. a
    // scratchpad window, which ToggleScratchpadForFocused() handles
    // entirely on its own) - workspace/monitor bookkeeping only above
    // is all that's needed.

    // No manual unmap here - see the matching comment in
    // MoveFocusedToWorkspace(): Arrange() unmaps/maps everything based
    // on which workspaces are visible *after* the move, which for a
    // monitor move is unconditionally true for the destination (it's
    // that monitor's own ActiveWorkspace() by construction) and
    // depends on the source's own remaining state for the source.
    Arrange();

    // Focus stays on the source monitor (refocusing whatever's left
    // there, exactly like closing/moving-to-workspace a focused window
    // already does) rather than following the window across - matches
    // i3/bspwm's "move container to output" convention of not stealing
    // focus onto the destination just because something was thrown
    // over to it.
    if (sourceMonitor)
        FocusNextAvailableOn(*sourceMonitor);
}

void WindowManager::ToggleFloating()
{
    ManagedWindow* window = m_repository.Focused();

    if (!window || window->IsFullscreen())
        return;

    if (window->IsScratchpad())
    {
        // Release it from the scratchpad slot into an ordinary
        // floating window on the focused monitor's current workspace.
        m_scratchpad.Forget(window->Id());
        window->SetWorkspace(FocusedWorkspaceId());
        window->SetMonitor(FocusedMonitor().Id());
        window->SetState(WindowState::Floating);
    }
    else if (window->IsTiled())
    {
        m_workspaces.Get(window->Workspace()).Tree().Remove(window);

        Rect geometry = window->FloatingGeometry();

        if (geometry.width <= 0 || geometry.height <= 0)
            geometry = CenteredFloatingRect(FocusedMonitor(), 0.5f, 0.5f);

        window->SetGeometry(geometry);
        window->SetFloatingGeometry(geometry);
        window->SetState(WindowState::Floating);
    }
    else if (window->IsFloating())
    {
        window->SetFloatingGeometry(window->Geometry());

        Monitor* shownOn = MonitorShowing(window->Workspace());
        Monitor& referenceMonitor = shownOn ? *shownOn : FocusedMonitor();

        if (!TryTile(window, window->Workspace(), referenceMonitor))
        {
            // No room to tile it right now (bug #4's capacity check) -
            // simplest safe behaviour is to just leave it floating
            // rather than forcing an undersized tile; the user can
            // try again once something else closes.
            Logger::Warning(
                "Workspace " + std::to_string(window->Workspace()) +
                " has no room for another tile right now - window " +
                std::to_string(static_cast<unsigned long>(window->Id())) +
                " stays floating.");
        }
    }

    Arrange();
}

void WindowManager::ToggleFullscreen()
{
    ManagedWindow* window = m_repository.Focused();

    if (!window)
        return;

    if (window->IsFullscreen())
        window->SetState(window->PreviousState());
    else
    {
        window->SetPreviousState(window->State());
        window->SetState(WindowState::Fullscreen);
    }

    Arrange();
}

void WindowManager::ToggleScratchpadForFocused()
{
    if (!m_scratchpad.HasWindow())
    {
        ManagedWindow* window = m_repository.Focused();

        if (!window || window->IsScratchpad())
            return;

        if (window->OccupiesTreeSlot())
            m_workspaces.Get(window->Workspace()).Tree().Remove(window);

        window->SetPreviousState(window->State());
        window->SetState(WindowState::Scratchpad);

        m_scratchpad.Assign(window->Id());

        window->IgnoreNextUnmap();
        m_connection.UnmapWindow(window->Id());

        Arrange();
        FocusNextAvailable();
        return;
    }

    bool nowVisible = m_scratchpad.Toggle();
    ManagedWindow* window = m_repository.Get(m_scratchpad.Window());

    if (!window)
        return;

    if (nowVisible)
    {
        float widthFraction  = m_config.GetPercent("scratchpad.width",  0.7f);
        float heightFraction = m_config.GetPercent("scratchpad.height", 0.7f);

        // The scratchpad is global - reachable from any workspace on
        // any monitor - so unlike an ordinary floating window it has
        // no "own" monitor to remember between toggles; it simply
        // reappears whichever monitor the user is actually on right
        // now, every time.
        Monitor& monitor = FocusedMonitor();

        Rect geometry = CenteredFloatingRect(monitor, widthFraction, heightFraction);
        window->SetGeometry(geometry);
        window->SetMonitor(monitor.Id());

        m_connection.MoveResizeWindow(window->Id(), geometry);
        m_connection.SetBorderWidth(window->Id(), window->BorderWidth());
        m_connection.MapWindow(window->Id());
        m_connection.Raise(window->Id());
        RaiseModalWindows();

        Focus(window->Id());
        UpdateAllBars();
    }
    else
    {
        window->IgnoreNextUnmap();
        m_connection.UnmapWindow(window->Id());

        if (window->Focused())
            FocusNextAvailable();

        UpdateAllBars();
    }
}

void WindowManager::CloseFocused()
{
    ManagedWindow* window = m_repository.Focused();

    if (!window)
        return;

    m_connection.CloseWindow(window->Id(), m_atoms);
}

void WindowManager::FocusDirection(Direction direction)
{
    ManagedWindow* window = m_repository.Focused();

    if (!window || !window->IsTiled())
        return;

    ManagedWindow* neighbor =
        m_workspaces.Get(window->Workspace()).Tree().FindNeighbor(window, direction);

    if (neighbor)
        Focus(neighbor->Id());
}

void WindowManager::MoveFocusedDirection(Direction direction)
{
    ManagedWindow* window = m_repository.Focused();

    if (!window || !window->IsTiled())
        return;

    ManagedWindow* neighbor =
        m_workspaces.Get(window->Workspace()).Tree().FindNeighbor(window, direction);

    if (neighbor)
        SwapWindows(window, neighbor);
}

void WindowManager::FocusCycle(int step)
{
    Workspace* workspace = FocusedMonitor().ActiveWorkspace();

    if (!workspace)
        return;

    auto visible = m_repository.Visible(workspace->Id());

    if (visible.empty())
        return;

    ManagedWindow* current = m_repository.Focused();

    auto it = std::find(visible.begin(), visible.end(), current);

    int index = (it != visible.end())
        ? static_cast<int>(std::distance(visible.begin(), it))
        : 0;

    int count = static_cast<int>(visible.size());
    int next = ((index + step) % count + count) % count;

    Focus(visible[static_cast<std::size_t>(next)]->Id());
}

void WindowManager::RotateFocused()
{
    ManagedWindow* window = m_repository.Focused();

    if (!window || !window->IsTiled())
        return;

    m_workspaces.Get(window->Workspace()).Tree().Rotate(window);
    Arrange();
}

void WindowManager::FlipFocused()
{
    ManagedWindow* window = m_repository.Focused();

    if (!window || !window->IsTiled())
        return;

    m_workspaces.Get(window->Workspace()).Tree().Flip(window);
    Arrange();
}

void WindowManager::ReloadConfig()
{
    if (!m_config.Load(m_configPath))
        return;

    m_keyboard.Configure(m_config);
    m_mouse.Configure(m_config);

    m_windowRules = LoadWindowRules(m_config);
    m_monitors.SetRules(LoadMonitorRules(m_config));

    m_wallpaperManager.Configure(m_config);
    m_wallpaperManager.ApplyToRoot(m_connection, m_monitors);

    m_fileManager =
        m_config.GetString(
            "launcher.file_manager",
            "pcmanfm");

    // Re-applying layouts is harmless (setxkbmap just sets the same
    // thing again) so someone editing `keyboard.layouts` sees it take
    // effect on reload like everything else - RunAutostart() is
    // deliberately NOT called here, see its declaration in the header.
    ApplyKeyboardLayouts();

    Arrange();
}

void WindowManager::ApplyKeyboardLayouts()
{
    std::vector<std::string> layouts =
        Utils::SplitWhitespace(
            m_config.GetString("keyboard.layouts", "us"));

    if (layouts.empty())
        return;

    std::string joined;

    for (std::size_t i = 0; i < layouts.size(); ++i)
    {
        // `keyboard.layouts=` is meant to be comma-separated
        // (matching setxkbmap's own -layout syntax), but a stray
        // space after a comma shouldn't silently break the whole
        // list, so tokenize on whitespace first and then strip any
        // trailing/leading commas off each piece before rejoining.
        std::string token = layouts[i];

        while (!token.empty() && token.front() == ',')
            token.erase(token.begin());

        while (!token.empty() && token.back() == ',')
            token.pop_back();

        if (token.empty())
            continue;

        if (!joined.empty())
            joined += ",";

        joined += token;
    }

    if (joined.empty())
        return;

    std::string command = "setxkbmap -layout " + joined;

    // Only meaningful (and only valid setxkbmap syntax) once there's
    // more than one layout to switch between - with a single layout
    // there's nothing to toggle.
    if (joined.find(',') != std::string::npos)
    {
        std::string toggle =
            m_config.GetString(
                "keyboard.layout_toggle",
                "grp:alt_shift_toggle");

        if (!toggle.empty())
            command += " -option " + toggle;
    }

    Process::Spawn(command, m_connection.DisplayName());
}

void WindowManager::RunAutostart()
{
    for (const std::string& program :
         Utils::SplitWhitespace(
             m_config.GetString("auto_start_programs")))
    {
        Process::Spawn(program, m_connection.DisplayName());
    }

    RunXdgAutostartEntries();

    // `workspace<N>=` is autostart with a target workspace attached -
    // e.g.
    //
    //   workspace1=zen-browser
    //   workspace2=discord telegram-desktop
    //   workspace3=steam
    //
    // Every program listed launches exactly like `auto_start_programs`
    // above (space-separated, through `/bin/sh -c`, args and all) -
    // the only difference is that whichever window(s) it opens land on
    // workspace N once they actually show up, however long that takes
    // (Steam and friends are never instant), instead of wherever
    // happened to be focused at the moment Kohiko itself started.
    //
    // A window is only ever eligible for this for a bounded time after
    // its process launches - long enough to cover a genuinely slow
    // starter, not so long that the same process opening some unrelated
    // window an hour into the session gets silently redirected too.
    static constexpr auto kWorkspaceAutostartWindow = std::chrono::seconds(60);

    std::chrono::steady_clock::time_point expiry =
        std::chrono::steady_clock::now() + kWorkspaceAutostartWindow;

    for (int workspaceId = 1; workspaceId <= m_workspaces.Count(); ++workspaceId)
    {
        std::string key = "workspace" + std::to_string(workspaceId);

        for (const std::string& program : Utils::SplitWhitespace(m_config.GetString(key)))
        {
            pid_t pid = Process::Spawn(program, m_connection.DisplayName());

            if (pid <= 0)
                continue;

            PendingWorkspaceAutostart pending;
            pending.pid = static_cast<long>(pid);
            pending.workspace = workspaceId;
            pending.expiry = expiry;

            m_pendingWorkspaceAutostarts.push_back(pending);
        }
    }
}

void WindowManager::RunXdgAutostartEntries()
{
    // Desktop IDs already resolved by a higher-priority directory -
    // once a given basename has been decided (spawn it, or explicitly
    // don't), a same-named file further down the list is a system
    // fallback the user/admin has already overridden and must be
    // ignored outright, not merged with or fallen back to. See
    // Xdg::AutostartDirs()'s comment for why the list is ordered this
    // way in the first place.
    std::vector<std::string> seenIds;

    for (const std::filesystem::path& dir : Xdg::AutostartDirs())
    {
        std::error_code ec;

        if (!std::filesystem::is_directory(dir, ec))
            continue; // no autostart entries here - e.g. no ~/.config/autostart at all, which is normal

        std::filesystem::directory_iterator it(dir, ec);
        std::filesystem::directory_iterator end;

        for (; !ec && it != end; it.increment(ec))
        {
            const std::filesystem::path& path = it->path();

            if (path.extension() != ".desktop")
                continue;

            std::string desktopId = path.stem().string();

            if (std::find(seenIds.begin(), seenIds.end(), desktopId) != seenIds.end())
                continue;

            seenIds.push_back(desktopId);

            std::optional<DesktopEntry> entry = ParseDesktopFile(path, 0);

            if (entry && ShouldAutostart(*entry))
                Process::Spawn(entry->exec, m_connection.DisplayName());
        }
    }
}

int WindowManager::ResolveWorkspaceAutostart(
    long pid) const
{
    if (pid <= 0)
        return 0;

    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

    for (const PendingWorkspaceAutostart& pending : m_pendingWorkspaceAutostarts)
    {
        if (now >= pending.expiry)
            continue;

        if (pending.pid == pid || Process::IsDescendantOf(pid, static_cast<pid_t>(pending.pid)))
            return pending.workspace;
    }

    return 0;
}

Monitor& WindowManager::MonitorForModal() const
{
    Monitor* monitor =
        m_monitors.Containing(m_connection.QueryPointer());

    if (!monitor)
        monitor = m_monitors.Focused();

    if (!monitor)
        monitor = &m_monitors.Primary();

    return *monitor;
}

void WindowManager::ToggleLauncher()
{
    if (m_launcher.IsOpen())
    {
        CloseLauncher(false);
        return;
    }

    if (m_notepad.IsOpen())
        CloseNotepad();

    ManagedWindow* focused = m_repository.Focused();
    m_focusBeforeModal = focused ? focused->Id() : 0;

    m_launcher.Reposition(MonitorForModal().Geometry());
    m_launcher.Open();
    m_connection.SetInputFocus(m_launcher.WindowId());
}

void WindowManager::CloseLauncher(bool run)
{
    if (!m_launcher.IsOpen())
        return;

    bool isFile =
        m_launcher.SelectedIsFile();

    std::string command =
        m_launcher.SelectedCommand();

    std::string path =
        m_launcher.SelectedPath();

    m_launcher.Close();

    if (run)
    {
        if (isFile)
{
    namespace fs = std::filesystem;

    std::string folder =
        fs::path(path)
            .parent_path()
            .string();

    Process::Spawn(
        m_fileManager + " \"" + folder + "\"",
        m_connection.DisplayName());
}
        else if (!command.empty())
        {
            Process::Spawn(
                command,
                m_connection.DisplayName());
        }
    }

    RestoreFocusAfterModal();
}

void WindowManager::ToggleNotepad()
{
    if (m_notepad.IsOpen())
    {
        CloseNotepad();
        return;
    }

    if (m_launcher.IsOpen())
        CloseLauncher(false);

    ManagedWindow* focused = m_repository.Focused();
    m_focusBeforeModal = focused ? focused->Id() : 0;

    m_notepad.Reposition(MonitorForModal().Geometry());
    m_notepad.Open();
    UpdateAllBars();
    m_connection.SetInputFocus(m_notepad.WindowId());
}

void WindowManager::CloseNotepad()
{
    if (!m_notepad.IsOpen())
        return;

    m_notepad.Close();
    UpdateAllBars();

    RestoreFocusAfterModal();
}

void WindowManager::RestoreFocusAfterModal()
{
    if (m_focusBeforeModal != 0 && m_repository.Contains(m_focusBeforeModal))
        Focus(m_focusBeforeModal);
    else
        FocusNextAvailable();

    m_focusBeforeModal = 0;
}

std::string WindowManager::AppClassKey(ManagedWindow* window)
{
    if (!window)
        return std::string();

    return !window->ClassName().empty() ? window->ClassName() : window->InstanceName();
}

int WindowManager::AdaptiveWorkspaceHabit(const std::string& appClass) const
{
    if (appClass.empty() || !m_config.GetBool("general.adaptive_placement", true))
        return 0;

    int habitWorkspace = m_placementHabits.PredictWorkspace(appClass);

    return (habitWorkspace >= 1 && habitWorkspace <= m_workspaces.Count()) ? habitWorkspace : 0;
}

void WindowManager::RecordAdaptiveWorkspaceHabit(ManagedWindow* window, int workspace)
{
    if (!m_config.GetBool("general.adaptive_placement", true))
        return;

    std::string appClass = AppClassKey(window);

    if (!appClass.empty())
        m_placementHabits.RecordWorkspaceMove(appClass, workspace);
}

void WindowManager::RecordAdaptivePositionHabit(ManagedWindow* window)
{
    if (!window || !m_config.GetBool("general.adaptive_placement", true))
        return;

    std::string appClass = AppClassKey(window);

    if (appClass.empty())
        return;

    Monitor* monitor = m_monitors.Find(window->Monitor());

    if (!monitor)
        return;

    m_placementHabits.RecordPositionMove(appClass, window->Geometry(), monitor->WorkArea());
}

void WindowManager::ApplyAdaptivePlacementNudge(ManagedWindow* window)
{
    if (!window || !window->IsTiled())
        return;

    if (!m_config.GetBool("general.adaptive_placement", true))
        return;

    std::string appClass = AppClassKey(window);

    if (appClass.empty())
        return;

    std::vector<Direction> habitDirections = m_placementHabits.PredictPositions(appClass);

    if (habitDirections.empty())
        return;

    BSPTree& tree = m_workspaces.Get(window->Workspace()).Tree();

    // Best-effort, bounded walk: repeatedly trades places with
    // whichever neighbor sits one step further toward each confident
    // direction, strongest axis first, until there's nothing left in
    // that direction to trade with (window is now flush against that
    // edge) - exactly what a manual Super+Shift+h/j/k/l move all the
    // way to the edge would do by hand, just done automatically once
    // per newly-opened window of a class Kohiko has learned this
    // about. kMaxHopsPerDirection is purely a defensive bound against
    // any pathological tree shape; a normal handful-of-windows
    // workspace always runs out of neighbors long before hitting it.
    constexpr int kMaxHopsPerDirection = 16;

    for (Direction direction : habitDirections)
    {
        for (int hop = 0; hop < kMaxHopsPerDirection; ++hop)
        {
            ManagedWindow* neighbor = tree.FindNeighbor(window, direction);

            if (!neighbor)
                break;

            tree.Swap(window, neighbor);
            Arrange();
        }
    }
}

bool WindowManager::TryRestoreSessionPosition(
    ManagedWindow* window,
    const SessionWindowState* session,
    Monitor& referenceMonitor)
{
    if (!window || !session || !session->hasNeighbor)
        return false;

    ManagedWindow* neighbor = m_repository.Get(session->neighborId);

    // The neighbor has to already be tiled on the exact workspace
    // `window` is about to land on - a neighbor that hasn't reappeared
    // yet this session (its own process hasn't relaunched, or it
    // landed somewhere else entirely) simply means there's nothing to
    // splice next to right now; the caller falls back to ordinary
    // TryTile(), exactly as if this window had no session position
    // data at all - see SessionStore's header comment for why this is
    // inherently best-effort rather than a guarantee.
    if (!neighbor || !neighbor->IsTiled() || neighbor->Workspace() != window->Workspace())
        return false;

    if (!m_workspaces.Get(window->Workspace()).Tree().InsertNextTo(window, neighbor, session->neighborDirection))
        return false;

    window->SetState(WindowState::Tiled);
    window->ResetTilingMisbehavior();

    if (!IsWorkspaceVisible(window->Workspace()))
        RefreshWorkspaceGeometry(window->Workspace(), referenceMonitor);

    return true;
}

bool WindowManager::TryTile(ManagedWindow* window, int workspaceId, Monitor& referenceMonitor)
{
    if (!window || workspaceId < 1 || workspaceId > m_workspaces.Count())
        return false;

    int innerGap  = m_config.GetInt("general.inner_gap", 6);
    int outerGap  = m_config.GetInt("general.outer_gap", 8);

    // MIN_USABLE_TILE_WIDTH/HEIGHT: the floor no *other*, already-
    // tiled window is ever allowed to shrink past on `window`'s
    // account. `window`'s own (possibly larger) declared minimum is
    // handled by BSPTree::Insert() itself, via ManagedWindow::
    // MinWidth()/MinHeight() - see BSPTree.h's EffectiveMinSize() for
    // why the two are deliberately kept separate.
    int floorWidth  = m_config.GetInt("general.min_tile_width", 100);
    int floorHeight = m_config.GetInt("general.min_tile_height", 60);

    // Matches LayoutEngine::Apply's own Shrunk(outerGap) exactly, so
    // this and the real layout can never disagree about what a
    // freshly-split leaf's rect would be.
    Rect tilingArea = TilingArea(referenceMonitor).Shrunk(outerGap);

    // BSPTree::Insert()'s placement-aware overload is the single
    // choke point for "Try Current Layout -> Try Alternative Layouts
    // -> Shrink Existing Tiles" - it only mutates the tree (and only
    // ever down to the floor, never further) if it actually finds a
    // legal placement, and returns false untouched otherwise.
    if (!m_workspaces.Get(workspaceId).Tree().Insert(window, tilingArea, innerGap, floorWidth, floorHeight))
        return false;

    window->SetWorkspace(workspaceId);
    window->SetState(WindowState::Tiled);

    // A freshly-tiled window starts this tile's "is it fighting its
    // geometry" count at zero, regardless of whatever it racked up in
    // a previous tile.
    window->ResetTilingMisbehavior();

    // ArrangeMonitor() (called by every caller of TryTile() right
    // after, via Arrange()) only ever lays out a workspace that
    // actually IS some monitor's ActiveWorkspace() right now - for any
    // other, keep its cached node geometry fresh ourselves so the next
    // insertion's AnchorLeaf()/DirectionForRect() has something real
    // to work from instead of a stale {0,0,0,0}.
    if (!IsWorkspaceVisible(workspaceId))
        RefreshWorkspaceGeometry(workspaceId, referenceMonitor);

    return true;
}

int WindowManager::FindWorkspaceWithRoom(ManagedWindow* window, int excludeId, Monitor& referenceMonitor)
{
    int innerGap  = m_config.GetInt("general.inner_gap", 6);
    int outerGap  = m_config.GetInt("general.outer_gap", 8);
    int floorWidth  = m_config.GetInt("general.min_tile_width", 100);
    int floorHeight = m_config.GetInt("general.min_tile_height", 60);

    Rect tilingArea = TilingArea(referenceMonitor).Shrunk(outerGap);

    for (int id = 1; id <= m_workspaces.Count(); ++id)
    {
        if (id == excludeId)
            continue;

        if (m_workspaces.Get(id).Tree().HasSpaceForAnotherWindow(window, tilingArea, innerGap, floorWidth, floorHeight))
            return id;
    }

    return 0;
}

int WindowManager::FindWorkspaceWithFewestWindows(int excludeId)
{
    int best = 0;
    std::size_t bestCount = 0;

    // Ascending id order both gives the natural "first available one"
    // tie-break the spec asks for and makes the scan deterministic.
    for (int id = 1; id <= m_workspaces.Count(); ++id)
    {
        if (id == excludeId)
            continue;

        std::size_t count = m_repository.Workspace(id).size();

        if (best == 0 || count < bestCount)
        {
            best = id;
            bestCount = count;
        }
    }

    return best;
}

void WindowManager::ApplyTilingMisbehaviorFallback(ManagedWindow* window)
{
    if (!window)
        return;

    int oldWorkspace = window->Workspace();

    // Stop assigning fixed tiled geometry entirely - remove it from
    // the tree here and never put it back into any tile, on any
    // workspace, as part of this call. Simply moving it into a
    // *different* tile would leave it exactly as likely to fight that
    // one too; the whole point of this fallback is landing it
    // somewhere floating instead.
    if (window->OccupiesTreeSlot())
        m_workspaces.Get(oldWorkspace).Tree().Remove(window);

    window->ResetTilingMisbehavior();

    std::string fallback = Utils::Lower(m_config.GetString("general.tiling_misbehavior_fallback", "floating"));

    int targetWorkspace = oldWorkspace;

    if (fallback == "new_workspace")
    {
        int candidate = FindWorkspaceWithFewestWindows(oldWorkspace);

        if (candidate != 0)
            targetWorkspace = candidate;
    }

    window->SetWorkspace(targetWorkspace);
    window->SetState(WindowState::Floating);

    Monitor* shownOn = MonitorShowing(targetWorkspace);
    Monitor& referenceMonitor = shownOn ? *shownOn : (MonitorShowing(oldWorkspace) ? *MonitorShowing(oldWorkspace) : FocusedMonitor());

    XWindowAttributes attrs{};
    m_connection.GetWindowAttributes(window->Id(), attrs);

    Rect geometry = CenteredFloatingRectForWindow(window->Id(), attrs, referenceMonitor);
    window->SetGeometry(geometry);
    window->SetFloatingGeometry(geometry);

    Logger::Warning(
        "Window " + std::to_string(static_cast<unsigned long>(window->Id())) +
        (window->ClassName().empty() ? "" : " (" + window->ClassName() + ")") +
        " repeatedly sent geometry requests conflicting with its tiled slot - "
        "switching it to floating" +
        (targetWorkspace != oldWorkspace
            ? " on workspace " + std::to_string(targetWorkspace)
            : "") +
        " instead of continuing to force it back into a tile.");

    // No manual unmap here - Arrange() (below) unmaps anything left on
    // a workspace nothing is currently showing on its own; see the
    // matching comment in MoveFocusedToWorkspace().
    Arrange();
}

void WindowManager::RefreshBorderColor(ManagedWindow* window)
{
    if (!window || window->BorderWidth() <= 0)
        return;

    unsigned long activeColor   = ParseColor("general.border_color_active",   "0x89b4fa");
    unsigned long inactiveColor = ParseColor("general.border_color_inactive", "0x45475a");

    m_connection.SetBorderColor(window->Id(), window->Focused() ? activeColor : inactiveColor);
}

Rect WindowManager::CenteredFloatingRect(Monitor& monitor, float widthFraction, float heightFraction)
{
    const Rect& geometry = monitor.WorkArea();
    int borderWidth = m_config.GetInt("general.border_size", 2);

    Rect rect;
    rect.width  = static_cast<int>(static_cast<float>(geometry.width)  * widthFraction);
    rect.height = static_cast<int>(static_cast<float>(geometry.height) * heightFraction);
    rect.x = geometry.x + (geometry.width  - rect.width)  / 2;
    rect.y = geometry.y + (geometry.height - rect.height) / 2;

    return rect.ClampedTo(geometry, borderWidth);
}

Rect WindowManager::CenteredFloatingRectForWindow(
    WindowID id,
    const XWindowAttributes& attrs,
    Monitor& monitor,
    ManagedWindow* parent)
{
    int width = 0;
    int height = 0;

    // Prefer what the client itself actually asked for - WM_NORMAL_HINTS
    // where it set one, otherwise whatever size it already had when it
    // asked to be mapped (every toolkit creates its top-level window at
    // its real intended size before mapping it; attrs is read at that
    // exact moment by Manage()). A fixed-size dialog, an image viewer
    // sized to the picture it's showing, a settings window with a
    // sensible default size - all of them should open at *that* size,
    // centered, instead of being squashed or stretched into a flat 50%
    // of the screen regardless of what they actually contain.
    if (!m_connection.GetPreferredSize(id, width, height))
    {
        width = attrs.width;
        height = attrs.height;
    }

    const Rect& geometry = monitor.WorkArea();
    int borderWidth = m_config.GetInt("general.border_size", 2);

    // A handful of windows genuinely don't have any usable size to go
    // on (0, or something too small to be more than a sliver) - rather
    // than ever placing an unusable window, fall back to the old
    // half-the-screen default exactly like before this existed. A
    // transient parent still gets to say *where* (centered over it
    // rather than the whole monitor) even when it can't say *how big*.
    if (width < 50 || height < 30)
    {
        if (!parent)
            return CenteredFloatingRect(monitor, 0.5f, 0.5f);

        Rect rect;
        rect.width  = static_cast<int>(static_cast<float>(geometry.width)  * 0.5f);
        rect.height = static_cast<int>(static_cast<float>(geometry.height) * 0.5f);
        rect.x = parent->Geometry().CenterX() - rect.width  / 2;
        rect.y = parent->Geometry().CenterY() - rect.height / 2;

        return rect.ClampedTo(geometry, borderWidth);
    }

    // Never let a window's own idea of its size push it off-screen -
    // still centered, just clamped to comfortably fit (a soft 95% cap
    // first, so it visibly has room to breathe rather than butting
    // right up against the edge), then hard-clamped against the real
    // monitor bounds - accounting for the border ClampedTo() adds
    // outward from width/height - as the actual Geometry Rules
    // guarantee that must hold no matter what a misbehaving client's
    // own hints claimed.
    int maxWidth  = static_cast<int>(static_cast<float>(geometry.width)  * 0.95f);
    int maxHeight = static_cast<int>(static_cast<float>(geometry.height) * 0.95f);

    if (width  > maxWidth)  width  = maxWidth;
    if (height > maxHeight) height = maxHeight;

    Rect rect;
    rect.width  = width;
    rect.height = height;

    if (parent)
    {
        // Center on the parent's own current rect rather than the
        // monitor - see the header comment on this overload: a GIMP
        // color picker or a file-save dialog should appear right over
        // the window that spawned it, not wherever happens to be the
        // middle of the screen. Still clamped to the monitor below, so
        // a parent sitting near an edge can never push the child
        // partly off-screen.
        rect.x = parent->Geometry().CenterX() - width  / 2;
        rect.y = parent->Geometry().CenterY() - height / 2;
    }
    else
    {
        rect.x = geometry.x + (geometry.width  - width)  / 2;
        rect.y = geometry.y + (geometry.height - height) / 2;
    }

    return rect.ClampedTo(geometry, borderWidth);
}

Rect WindowManager::ResizedFloatingRect(
    ManagedWindow* window,
    const Point& cursor,
    Monitor& monitor) const
{
    int dx = cursor.x - m_resizeStartCursor.x;
    int dy = cursor.y - m_resizeStartCursor.y;

    // Same floor TryTile() enforces for a tiled window - a window's
    // own declared minimum (WM_NORMAL_HINTS) where it's stricter than
    // the configured general default, otherwise the configured default
    // itself.
    int floorWidth  = m_config.GetInt("general.min_tile_width", 100);
    int floorHeight = m_config.GetInt("general.min_tile_height", 60);

    int minWidth  = std::max(floorWidth,  window ? window->MinWidth()  : 0);
    int minHeight = std::max(floorHeight, window ? window->MinHeight() : 0);

    int borderWidth = window ? window->BorderWidth() : 0;
    int footprint = borderWidth * 2;

    const Rect& bounds = monitor.WorkArea();

    Rect rect = m_resizeStartRect;

    // Each axis is handled independently: find where the moving edge
    // wants to land, clamp it to the monitor first, then pull it back
    // toward the anchored edge if that would leave less than the
    // minimum size - the anchored edge itself is never touched either
    // way, which is what keeps it visually pinned in place.
    if (m_resizeFromLeft)
    {
        int rightEdge = m_resizeStartRect.Right();
        int leftEdge  = m_resizeStartRect.x + dx;

        if (leftEdge < bounds.x)
            leftEdge = bounds.x;

        if (rightEdge - leftEdge < minWidth)
            leftEdge = rightEdge - minWidth;

        rect.x     = leftEdge;
        rect.width = rightEdge - leftEdge;
    }
    else
    {
        int leftEdge  = m_resizeStartRect.x;
        int rightEdge = m_resizeStartRect.Right() + dx;

        int maxRightEdge = bounds.Right() - footprint;

        if (rightEdge > maxRightEdge)
            rightEdge = maxRightEdge;

        if (rightEdge - leftEdge < minWidth)
            rightEdge = leftEdge + minWidth;

        rect.width = rightEdge - leftEdge;
    }

    if (m_resizeFromTop)
    {
        int bottomEdge = m_resizeStartRect.Bottom();
        int topEdge    = m_resizeStartRect.y + dy;

        if (topEdge < bounds.y)
            topEdge = bounds.y;

        if (bottomEdge - topEdge < minHeight)
            topEdge = bottomEdge - minHeight;

        rect.y      = topEdge;
        rect.height = bottomEdge - topEdge;
    }
    else
    {
        int topEdge    = m_resizeStartRect.y;
        int bottomEdge = m_resizeStartRect.Bottom() + dy;

        int maxBottomEdge = bounds.Bottom() - footprint;

        if (bottomEdge > maxBottomEdge)
            bottomEdge = maxBottomEdge;

        if (bottomEdge - topEdge < minHeight)
            bottomEdge = topEdge + minHeight;

        rect.height = bottomEdge - topEdge;
    }

    if (rect.width  < 1) rect.width  = 1;
    if (rect.height < 1) rect.height = 1;

    return rect;
}

Rect WindowManager::RelocateRectToMonitor(
    const Rect& rect,
    const Rect& fromArea,
    const Rect& toArea) const
{
    int borderWidth = m_config.GetInt("general.border_size", 2);

    if (fromArea.width <= 0 || fromArea.height <= 0)
    {
        // No sane "from" to compute a relative position against -
        // just center it on the destination instead of dividing by
        // zero.
        Rect centered;
        centered.width  = std::min(rect.width,  toArea.width);
        centered.height = std::min(rect.height, toArea.height);
        centered.x = toArea.x + (toArea.width  - centered.width)  / 2;
        centered.y = toArea.y + (toArea.height - centered.height) / 2;

        return centered.ClampedTo(toArea, borderWidth);
    }

    // Keep the same position/size *relative* to each monitor's own
    // work area - a window sitting in the top-right quarter of a
    // 1920x1080 monitor ends up in the top-right quarter of whatever
    // it's relocated to as well, rather than always snapping back to
    // dead center. Size is carried across proportionally too, so a
    // window sized for a 4K monitor doesn't swallow an entire 1080p
    // one whole.
    float relX = static_cast<float>(rect.x - fromArea.x) / static_cast<float>(fromArea.width);
    float relY = static_cast<float>(rect.y - fromArea.y) / static_cast<float>(fromArea.height);
    float relW = static_cast<float>(rect.width)  / static_cast<float>(fromArea.width);
    float relH = static_cast<float>(rect.height) / static_cast<float>(fromArea.height);

    Rect relocated;
    relocated.width  = std::clamp(static_cast<int>(relW * static_cast<float>(toArea.width)),  1, toArea.width);
    relocated.height = std::clamp(static_cast<int>(relH * static_cast<float>(toArea.height)), 1, toArea.height);
    relocated.x = toArea.x + static_cast<int>(relX * static_cast<float>(toArea.width));
    relocated.y = toArea.y + static_cast<int>(relY * static_cast<float>(toArea.height));

    return relocated.ClampedTo(toArea, borderWidth);
}

void WindowManager::RelocateOrphanedFloatingWindows()
{
    for (ManagedWindow* window : m_repository.All())
    {
        bool floatingLike =
            window->IsFloating() ||
            (window->IsFullscreen() && window->PreviousState() == WindowState::Floating);

        if (!floatingLike)
            continue; // a tiled (or fullscreen-while-tiled) window's tree ratios adapt to whatever monitor lays it out - nothing to fix

        Point center(window->Geometry().CenterX(), window->Geometry().CenterY());

        if (m_monitors.Containing(center))
            continue; // still lands on some connected monitor - leave it exactly where it was

        Monitor* home = MonitorShowing(window->Workspace());
        Monitor& target = home ? *home : m_monitors.Primary();

        // Same flat default every other "couldn't figure out anything
        // better" floating placement in this file falls back to.
        Rect geometry = CenteredFloatingRect(target, 0.5f, 0.5f);

        // A genuinely client-sized rect (not just the 50/50 fallback
        // above) is worth preserving if it still fits the destination
        // monitor - only fall back to the flat default when it
        // wouldn't.
        const Rect& workArea = target.WorkArea();

        if (window->Geometry().width <= workArea.width && window->Geometry().height <= workArea.height)
        {
            Rect resized = window->Geometry();
            resized.x = workArea.x + (workArea.width  - resized.width)  / 2;
            resized.y = workArea.y + (workArea.height - resized.height) / 2;
            geometry = resized.ClampedTo(workArea, m_config.GetInt("general.border_size", 2));
        }

        window->SetGeometry(geometry);
        window->SetFloatingGeometry(geometry);

        Logger::Info(
            "Window " + std::to_string(static_cast<unsigned long>(window->Id())) +
            (window->ClassName().empty() ? "" : " (" + window->ClassName() + ")") +
            " was floating on a monitor that just disconnected - relocated onto monitor " +
            std::to_string(target.Id()) + " instead.");
    }
}

void WindowManager::HandleMonitorTopologyChanged()
{
    RebuildBars();
    RefreshMonitorWorkAreas();
    RelocateOrphanedFloatingWindows();

    // A monitor connecting, disconnecting, or changing resolution
    // always changes what the composite root-window wallpaper needs
    // to look like (a new region to cover, one less region, or an
    // existing region resized) - see WallpaperManager::ApplyToRoot()'s
    // own comment. Before Reposition()/Arrange() below, for the same
    // "no visible flash of stale content" reasoning as
    // SwitchWorkspaceOnMonitor().
    m_wallpaperManager.ApplyToRoot(m_connection, m_monitors);

    if (m_lockScreen.IsLocked())
        m_lockScreen.Reposition(m_monitors, m_wallpaperManager);

    Arrange();

    // Same reasoning as the call at the end of Initialize(): a monitor
    // that just appeared (or one whose geometry just moved) generates
    // no MotionNotify/EnterNotify of its own, so if the pointer already
    // happens to be sitting over it - plugging in a second monitor to
    // the side the pointer was already resting near is the common
    // case - it needs to be checked directly rather than waiting for
    // the pointer to move before it's recognized as focused.
    UpdateFocusedMonitorFromPointer(m_connection.QueryPointer());
}

void WindowManager::RebuildBars()
{
    // Every monitor MonitorManager still reports gets counted as "kept
    // or new" below - one raw-pointer scan, then a lookup per existing
    // entry, both O(number of monitors), never more than a handful.
    std::vector<Monitor*> current;

    for (const auto& monitor : m_monitors.All())
        current.push_back(monitor.get());

    // Tear down any bar whose monitor is gone.
    for (auto it = m_bars.begin(); it != m_bars.end(); )
    {
        if (std::find(current.begin(), current.end(), it->first) == current.end())
        {
            it->second->Hide();
            it = m_bars.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Create (or just reconfigure the geometry of) one for every
    // monitor that's here now.
    for (Monitor* monitor : current)
    {
        auto it = m_bars.find(monitor);

        if (it == m_bars.end())
        {
            auto bar = std::make_unique<Bar>(m_connection);
            bar->Configure(m_config, monitor->Geometry());
            bar->Show();

            it = m_bars.emplace(monitor, std::move(bar)).first;
        }
        else
        {
            it->second->Configure(m_config, monitor->Geometry());
        }
    }

    // The system tray is a single, X11-wide selection (see
    // SystemTray.h) - it can only ever live on one bar, so it always
    // follows whichever monitor is Primary() right now.
    if (Bar* primaryBar = BarFor(m_monitors.Primary()))
        primaryBar->AttachSystemTray(&m_tray);
}

Bar* WindowManager::BarFor(Monitor& monitor) const
{
    auto it = m_bars.find(&monitor);

    return it != m_bars.end() ? it->second.get() : nullptr;
}

Bar* WindowManager::BarWindowMatching(::Window id) const
{
    for (const auto& [monitor, bar] : m_bars)
        if (bar->WindowId() == id)
            return bar.get();

    return nullptr;
}

void WindowManager::UpdateAllBars()
{
    ManagedWindow* focusedWindow = m_repository.Focused();
    Monitor* focusedMonitor = m_monitors.Focused();

    bool scratchpadActive = m_scratchpad.HasWindow() && m_scratchpad.IsVisible();
    bool notepadActive = m_notepad.HasContent();

    for (const auto& monitor : m_monitors.All())
    {
        Bar* bar = BarFor(*monitor);

        if (!bar)
            continue;

        Workspace* workspace = monitor->ActiveWorkspace();

        bar->SetWorkspaces(m_workspaces.Count(), workspace ? workspace->Id() : 0);

        // Only the focused monitor has a genuine "currently focused
        // window" of its own right now - real X input focus is
        // singular. Showing the same title on every monitor's bar
        // (what a single global bar always effectively did) would
        // misrepresent every *other* monitor as if it were also
        // focused; showing nothing there instead is honest about it.
        bar->SetTitle(monitor.get() == focusedMonitor && focusedWindow ? focusedWindow->Title() : "");

        bar->SetScratchpadActive(scratchpadActive);
        bar->SetNotepadActive(notepadActive);

        bar->Redraw();
    }
}

WindowRuleEffect WindowManager::ResolveWindowRules(
    const std::string& className,
    const std::string& instanceName,
    const std::string& title) const
{
    WindowRuleEffect effect;

    for (const WindowRule& rule : m_windowRules)
    {
        if (!rule.Matches(className, instanceName, title))
            continue;

        switch (rule.action)
        {
            case WindowRule::Action::Float:       effect.forceFloat      = true;          break;
            case WindowRule::Action::Tile:        effect.forceTile       = true;          break;
            case WindowRule::Action::Fullscreen:  effect.forceFullscreen = true;           break;
            case WindowRule::Action::NoFullscreen: effect.denyFullscreen  = true;          break;
            case WindowRule::Action::Workspace:   effect.forceWorkspace  = rule.workspace; break;
        }
    }

    return effect;
}

void WindowManager::RaiseModalWindows()
{
    if (m_launcher.IsOpen())
        m_connection.Raise(m_launcher.WindowId());
    else if (m_notepad.IsOpen())
        m_connection.Raise(m_notepad.WindowId());
}

unsigned long WindowManager::ParseColor(const std::string& key, const std::string& fallback) const
{
    return std::strtoul(m_config.GetString(key, fallback).c_str(), nullptr, 0);
}

std::string WindowManager::DumpClientsJson() const
{
    std::ostringstream out;
    out << "[";

    bool first = true;

    for (ManagedWindow* window : m_repository.All())
    {
        if (!first)
            out << ",";

        first = false;

        const Rect& r = window->Geometry();

        out << "{"
            << "\"id\":" << static_cast<unsigned long>(window->Id()) << ","
            << "\"title\":" << Json::String(window->Title()) << ","
            << "\"class\":" << Json::String(window->ClassName()) << ","
            << "\"instance\":" << Json::String(window->InstanceName()) << ","
            << "\"pid\":" << window->Pid() << ","
            << "\"workspace\":" << window->Workspace() << ","
            << "\"monitor\":" << window->Monitor() << ","
            << "\"x\":" << r.x << ",\"y\":" << r.y
            << ",\"width\":" << r.width << ",\"height\":" << r.height << ","
            << "\"floating\":" << Json::Boolean(window->IsFloating()) << ","
            << "\"fullscreen\":" << Json::Boolean(window->IsFullscreen()) << ","
            << "\"scratchpad\":" << Json::Boolean(window->IsScratchpad()) << ","
            << "\"focused\":" << Json::Boolean(window->Focused())
            << "}";
    }

    out << "]";
    return out.str();
}

std::string WindowManager::DumpMonitorsJson() const
{
    std::ostringstream out;
    out << "[";

    bool first = true;

    for (const auto& monitor : m_monitors.All())
    {
        if (!first)
            out << ",";

        first = false;

        const Rect& r = monitor->Geometry();
        const Rect& w = monitor->WorkArea();
        Workspace* workspace = monitor->ActiveWorkspace();

        out << "{"
            << "\"id\":" << monitor->Id() << ","
            << "\"name\":" << Json::String(monitor->Name()) << ","
            << "\"x\":" << r.x << ",\"y\":" << r.y
            << ",\"width\":" << r.width << ",\"height\":" << r.height << ","
            << "\"workArea\":{"
                << "\"x\":" << w.x << ",\"y\":" << w.y
                << ",\"width\":" << w.width << ",\"height\":" << w.height
            << "},"
            << "\"workspace\":" << (workspace ? workspace->Id() : 0) << ","
            << "\"primary\":" << Json::Boolean(monitor.get() == &m_monitors.Primary()) << ","
            << "\"focused\":" << Json::Boolean(monitor.get() == m_monitors.Focused())
            << "}";
    }

    out << "]";
    return out.str();
}

std::string WindowManager::DumpActiveWindowJson() const
{
    ManagedWindow* window = m_repository.Focused();

    if (!window)
        return "null";

    const Rect& r = window->Geometry();

    std::ostringstream out;
    out << "{"
        << "\"id\":" << static_cast<unsigned long>(window->Id()) << ","
        << "\"title\":" << Json::String(window->Title()) << ","
        << "\"class\":" << Json::String(window->ClassName()) << ","
        << "\"workspace\":" << window->Workspace() << ","
        << "\"x\":" << r.x << ",\"y\":" << r.y
        << ",\"width\":" << r.width << ",\"height\":" << r.height
        << "}";

    return out.str();
}

::Window WindowManager::LauncherWindowId() const
{
    return m_launcher.WindowId();
}

}
