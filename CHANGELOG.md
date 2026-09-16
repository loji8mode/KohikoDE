# Changelog

## Version 0.20.0

### Added
- Wallpaper support: independently configurable per monitor and per
  workspace, applied the standard X11 way (one composite `Pixmap`
  spanning every connected monitor, set as the root window's
  background - the same technique `feh`/`nitrogen`/`xwallpaper` use, so
  gaps between tiled windows and empty workspaces show it through for
  free, no separate "wallpaper window" needed). New `wallpaper.default`/
  `wallpaper.mode`/`wallpaper.background_color`, plus repeatable
  `wallpaper.monitor=<name>,path=...,mode=...` and
  `wallpaper.workspace=<id>,path=...,mode=...` rules (workspace rule
  wins over monitor rule wherever both could apply to the same screen
  at once) using the exact same `key,key=value,...` shape `monitor=`/
  `windowrule=` already use. Four real scale modes (fill/fit/center/
  stretch) via a new shared `ImageRenderer` class, built on a single
  Imlib2 `imlib_render_image_part_on_drawable_at_size()` call per mode
  with different source/dest rectangle math - verified with 33 unit
  tests covering every mode against both the six shipped wallpapers'
  own aspect ratio and several target resolutions (including two cases
  where an initial test draft had Fit's letterbox-vs-pillarbox
  direction backwards; independently re-derived the math to confirm
  the implementation was correct and fixed the test, not the code).
  Live-reloads automatically on file changes (`inotify`, watching each
  resolved wallpaper's containing directory - the same
  atomic-replace-safe approach `AppDirWatcher` already uses - rather
  than the file itself, so both an in-place edit and an editor's
  atomic-save-via-rename are both caught correctly) via a new
  `WallpaperManager` class, whose parsing/priority-resolution logic
  (19 checks, `test_wallpapermanager`) is fully unit tested against
  real `Config`/`Monitor`/`Workspace` objects. Six wallpapers now ship
  with Kohiko itself (`assets/wallpapers/`, installed to
  `/usr/local/share/kohiko/wallpapers/`), with `wallpaper.default`
  already pointing at one of them in `config/default.conf` out of the
  box. New `wallpaper.monitor=`/`wallpaper.workspace=` sections in
  Kohiko Settings (raw-text editing, same as `bind=`/`exec.`).
- Lock screen background image gained the same four real scale modes
  above (`lockscreen.background_mode`) - it was previously always
  stretched, via its own separate, less capable Imlib2 loader, now
  replaced by the shared `ImageRenderer`. New
  `lockscreen.use_desktop_wallpaper`: when `lockscreen.background_image`
  is left empty, shows whatever the desktop wallpaper currently
  resolves to for each monitor instead of the plain
  `lockscreen.background_color` - an explicit `background_image` still
  always takes priority when set. `LockScreen::Lock()`/`Reposition()`
  previously each had their own copy of the per-monitor pixmap-loading
  loop; consolidated into one shared `RenderBackgroundPixmaps()`.
- `kohikoctl configure-autologin [--undo]`: opt-in, interactive
  display-manager autologin setup, backed by a new
  `AutologinConfigurator` class. Detects the active display manager
  (the systemd `display-manager` service symlink, falling back to
  known binaries), refuses outright if autologin is already configured
  for it anywhere (main config file or a drop-in directory - existing
  autologin configurations are never modified or overwritten), shows
  exactly what will be created, and only ever writes anything after an
  explicit confirmation prompt that **defaults to No**. LightDM and
  SDDM get full support, via a single new drop-in config file each
  (`.../lightdm.conf.d/60-kohiko-autologin.conf`,
  `.../sddm.conf.d/60-kohiko-autologin.conf`) rather than an in-place
  edit of the existing `lightdm.conf`/`sddm.conf` - the original file
  is never touched, and `--undo` is always exactly "delete that one
  file". GDM is detected but deliberately left to manual documentation
  (no equivalent drop-in mechanism for this setting - only one shared
  `custom.conf` this tool has no business assuming the rest of the
  structure of); any other or ambiguous case also falls back to
  documentation rather than guessing. No plaintext password is ever
  read, stored, or handled - only a username gets written, to a file
  the display manager already reads for exactly this purpose.
  `AutologinConfigurator`'s every filesystem path is parameterized by a
  `root` argument (defaulting to `/`) specifically so this could be
  genuinely, thoroughly tested (30 checks,
  `test_autologinconfigurator`, covering detection - including
  ambiguous/unknown cases - existing-config checks across main-file and
  drop-in variants, section-scoped INI parsing correctness, refusal
  behavior, and undo) against a real throwaway fake filesystem tree
  rather than reviewed by inspection alone; `tools/kohikoctl.cpp`'s
  actual CLI/prompt flow was additionally verified for real, end to
  end, inside an isolated mount namespace with a fake `/etc` bind-
  mounted over the real one (detection, full configure with the exact
  written file content, refuse-on-existing, undo, and the empty-input/
  explicit-`n` "default is No" cases all confirmed against the real
  binary, not just the library).
- `kohiko-session`: a new, minimal POSIX shell wrapper that's now the
  actual session entry point a display manager (or `~/.xinitrc`)
  launches (`Display Manager -> kohiko-session -> kohiko`), providing
  session-level crash supervision on top of - not instead of -
  `RecoveryMode`'s own internal, config-focused recovery: on a clean
  exit, propagates it and stops (a deliberate logout); on a crash,
  restarts with exponential backoff (1s, 2s, 4s, ... capped at 30s),
  up to 5 consecutive failures before giving up and letting the
  session fail rather than looping forever; a crash-free run of at
  least 30s resets that count, so one crash after hours of normal use
  still gets the full restart budget. Forwards SIGTERM/SIGINT to the
  running `kohiko` (falling back to SIGKILL if it doesn't exit within
  10s) rather than treating an intentional session-end signal as a
  crash to restart from. The two systems need zero coordination:
  `RecoveryMode` decides what a *given* start of `kohiko` does; this
  script only decides whether there's a next one at all. Without a
  wrapper at all, a single crash simply ends the session outright -
  `RecoveryMode`'s own safe-defaults logic would never even get a next
  launch to run on. Thoroughly tested against real fake-`kohiko`
  stand-ins covering every branch (`tests/test_kohiko_session.sh`,
  wired into `make test`/`ctest`) - a genuine, non-mocked functional
  test, unlike most of this phase's other C++ changes, which this
  sandbox can't compile against real Xft/D-Bus headers to test
  end-to-end.
- The `xsessions` `.desktop` entry (letting Kohiko appear as a
  selectable session in display managers) is no longer Arch-only: a
  checked-in `desktop/kohiko.desktop` (`Exec=kohiko-session`, not
  `kohiko` directly - see above) is now installed by the plain
  `make install`/`cmake --install` path too, at the fixed
  `/usr/share/xsessions` every display manager actually scans -
  deliberately not `$PREFIX`-relative, the same kind of exception
  already made for the pixmaps icon fallback just below.
  `scripts/install-arch.sh` no longer generates its own separate copy
  of this file (it now comes from `make install` like everything
  else) and points its `~/.xinitrc` fallback at `kohiko-session` too.
- Native lock screen now integrates with systemd-logind's session-lock
  mechanism instead of being an isolated WM feature: a new
  `SessionLockBridge` class (same raw-libdbus/private-connection shape
  as `ScreenSaverInhibitor`) subscribes to the current session's own
  `org.freedesktop.login1.Session` `Lock` signal - so `loginctl
  lock-session`, a suspend hook, or any other standard tool asking
  this session to lock reaches Kohiko's real `LockScreen`, the same as
  every other way of locking it - and calls `SetLockedHint()` after
  every genuine lock/unlock transition (via a new
  `LockScreen::SetLockStateChangedCallback()`, since `Unlock()` is
  only ever called internally from `HandleKeyPress()`), so
  `loginctl session-status`/a display manager's user switcher sees the
  truth. Deliberately one-way: does not subscribe to logind's own
  `Unlock` signal, since honoring an external unlock request would
  mean bypassing `LockScreen`'s PAM-backed `Authenticator` entirely -
  a second, weaker authentication path this exists specifically to
  avoid creating. No new lock screen, no duplicated authentication, no
  plaintext-password handling beyond what `Authenticator` already did.
  Resolves this session's own logind session via
  `Manager.GetSessionByPID(getpid())`, not `$XDG_SESSION_ID`, so it
  still works even where that variable isn't set. Gracefully
  unavailable (falls back to exactly today's local-only lock screen
  behavior) with no logging on any system without a session bus,
  without logind running, or where this process isn't a logind-managed
  session at all - none of which are errors.
- `SessionStore` now also remembers the workspace each monitor (by
  XRandr output name) was actually showing as of the last clean
  shutdown, independently of any window's own saved state. On restart,
  `MonitorManager::Detect()` consults this - via a new
  `SetSessionWorkspaceLookup()` callback, wired up in
  `WindowManager::Initialize()` - as a fallback for a monitor's
  starting workspace: below an explicit `monitor=` rule (which is
  still a deliberate pin and always wins), but above "first workspace
  nothing else is showing". Previously an unpinned monitor reset to
  whatever workspace happened to be free on every single restart, even
  if the user had been using it on a completely different one for
  weeks; adaptive placement's per-window and per-app-class learning
  already survived restarts (`PlacementHabitStore`), but this
  per-monitor piece of "don't make the user teach Kohiko twice" did
  not. The on-disk session file format gains a trailing `MONITORS
  <count>` section for this (still `KOHIKO_SESSION_V2` - purely
  additive, so an older file with no such section still loads its
  window records exactly as before). New `test_sessionstore`
  regression suite (`make test` / `ctest`) covers the parsing directly,
  including that backward-compatibility case.
- Automatic config migration: on every normal startup, `Application::Run()`
  now calls the new `ConfigMigration::MigrateIfNeeded()` before loading
  `kohiko.conf`, which appends `key=default` (from `ConfigSchema`,
  already Kohiko Settings' own source of truth for every setting) for
  any schema key genuinely missing from the file - never a key that's
  already there, active or (deliberately) commented-out, and never a
  key the user set to an explicit empty value on purpose (several
  schema defaults, e.g. `lockscreen.background_image`, are themselves
  `""` - `ConfigWriter` gained a proper `Contains()` to tell "never
  set" apart from "set to empty on purpose", since `GetScalar().empty()`
  alone can't). Reuses `ConfigWriter` - the same line-preserving engine
  Kohiko Settings' own Save() already relies on - rather than a second
  way of editing `kohiko.conf`; `ConfigWriter.cpp` moved out of the
  Makefile/CMakeLists' GUI-only source list accordingly, since it's now
  linked into the main `kohiko` binary too, not just `kohiko-settings`.
  A config that's already fully up to date is left with its mtime
  completely untouched. New `test_configmigration` regression suite.
- Recovery mode: a new `RecoveryMode` class detects a Kohiko that
  crashed before finishing startup last time (a single small marker
  file, set at the very top of `Application::Run()` and cleared right
  after `Logger::Info("ready")` - see `RecoveryMode.h` for the full
  mechanism) and, on the next launch, skips loading/migrating the
  user's config entirely in favor of built-in defaults, after backing
  up the suspect file to `kohiko.conf.bak` (the same backup
  `ConfigMigration` itself writes before altering anything, via the
  now-shared `ConfigMigration::BackupConfig()`) and logging a clear,
  actionable `Logger::Warning`. `kohikoctl` gained a `restore-config
  [path]` subcommand that copies that backup back over `kohiko.conf` -
  deliberately a plain filesystem operation rather than going through
  `IpcServer` like every other `kohikoctl` command, since it has to
  work precisely when Kohiko isn't running at all. New
  `test_recoverymode` regression suite. A bad configuration can no
  longer make Kohiko unusable.
- Launcher auto-refresh: the application list now watches every
  directory it scans (`Xdg::ApplicationDirs()` -
  `/usr/share/applications`, `~/.local/share/applications`, and
  anywhere else `$XDG_DATA_DIRS` points) via Linux `inotify(7)`, and
  automatically re-scans - the exact same path `kohikoctl
  reloadlauncher` already used - the moment a `.desktop` file is
  installed, edited, or removed. New `AppDirWatcher` class feeds its
  fd into `EventLoop`'s existing `select()` multiplexing (identical
  treatment to `ScreenSaverInhibitor`'s D-Bus fd) - no polling timer,
  no extra thread. Gracefully unavailable (falls back to exactly
  today's manual-refresh-only behavior) if `inotify_init1()` itself
  ever fails, which is not expected on Linux but is never treated as
  fatal. New `test_appdirwatcher` regression suite exercises this
  against real inotify with throwaway temp directories - no X server
  needed.

### Fixed
- Several new files (`SessionLockBridge.cpp`, `AppDirWatcher.cpp`,
  `WallpaperManager.cpp`, `RecoveryMode.cpp`) used `std::uint32_t`/
  `std::error_code` without including `<cstdint>`/`<system_error>`
  directly, relying on another header happening to provide them
  transitively - which held in the sandbox this phase was developed in,
  but not on every real toolchain (reported as a build failure in
  `SessionLockBridge.cpp` specifically: `'uint32_t' is not a member of
  'std'`). Fixed by including what each file actually uses directly,
  rather than relying on transitive availability - also applied
  defensively to `TrayIconClient.cpp` (missing `<algorithm>` for
  `std::min`/`std::max`, same risk, not yet reported broken) and
  `UiWindow.h` (a pre-existing `std::uint32_t` usage that turned out to
  already be covered transitively via `Types.h`, so not a live bug,
  but now stated explicitly rather than left implicit).
- `TrayIconClient::Run()` (shared by `kohiko-audio-tray`/
  `kohiko-network-tray`/`kohiko-bluetooth-tray`) and `UiWindow::Run()`
  (shared by `kohiko-settings`/`kohiko-audio`/`kohiko-network`/
  `kohiko-bluetooth`'s own windows) each had a hardcoded 250-500ms
  `poll()` timeout floor that fired unconditionally forever, even
  fully idle with no timer registered at all, waking every one of
  these seven processes several times a second for no reason. Neither
  needed it: redraws are already driven directly by the fd/click/
  scroll callbacks that set the dirty flag (checked at the very next
  loop iteration regardless of what woke it), and the only genuine
  periodic need in either loop - the dock-retry attempt in
  `TrayIconClient` and each window's own registered timers - was
  already being computed correctly and just getting clamped down to
  that floor. Both now compute the actual next deadline and pass it
  straight to `poll()`, blocking indefinitely (`-1`) when nothing is
  pending. Verified with isolated unit tests of the exact
  timeout-computation logic (covering idle/dock-retry-due/timer-due/
  overdue-clamps-to-zero/soonest-of-two-competing-deadlines), since
  neither file builds without Xft/libpipewire/libdbus.
- `Bar::Redraw()` drew straight onto the live bar window - a full
  `XClearWindow` immediately followed by several `XftDrawString`
  calls repainting it - on every relevant event *and* once a second
  for the clock, which is a classic X11 flicker source: the window
  briefly shows blank/background-colored before the following draw
  calls paint it back in, with no guarantee the X server coalesces
  that into one visible frame. Now draws into an off-screen backing
  `Pixmap` (created/resized alongside the window - width follows
  monitor geometry and can change on a hotplug/resolution change,
  height never does) and composites the finished frame onto the real
  window with a single `XCopyArea` at the end - the exact same
  backing-pixmap pattern `UiWindow.cpp` already used, just not
  something `Bar` itself had. The window is now touched exactly once
  per `Redraw()`, so there's no intermediate state left for the X
  server to ever actually display. Verified by isolating the exact
  `XCreatePixmap`/`XFillRectangle`/`XCopyArea` call shapes into a
  standalone compile-only check against the real `<X11/Xlib.h>`
  prototypes, since `Bar.cpp` itself needs Xft to build.
- Investigated the "hover title" behavior described as needing
  removal in favor of the border highlight alone indicating focus -
  traced every `Bar::SetTitle()` call site and found the bar's title
  is already driven purely by `WindowManager`'s own focus state
  (`focusedWindow->Title()`), not mouse hover; there is no separate
  hover-driven title path anywhere in the codebase to remove. No code
  change made here - existing behavior already matches the goal.

### Changed
- Rebuilt `kohiko-audio`/`kohiko-network`/`kohiko-bluetooth`'s main
  pages to match their design mockups: flat divided-row lists inside
  a single bordered card (replacing the previous grid-of-separate-
  cards layout) for device/network/Bluetooth lists, a violet accent
  in place of the previous blue, and a live-filtering search field in
  `kohiko-network`. `kohiko-network` and `kohiko-bluetooth` gained a
  details panel for whichever list row is selected, sitting beside
  the list above ~720px content width and stacked below it otherwise.
  See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full
  per-app breakdown.
- Moved features not present in the new mockups down to a new
  "Advanced Settings" sub-page in each app, rather than dropping them:
  level meters and the default-device-change notification toggle
  (`kohiko-audio`); Ethernet device management and VPN profile
  activation (`kohiko-network`); per-device Bluetooth trust
  (`kohiko-bluetooth`, previously exposed inline on each paired
  device's card).
- Every page's responsive floor dropped to a genuinely usable ~420px
  content width (narrow single-column layouts, two-line device rows
  in `kohiko-audio`, a wrapping toolbar in `kohiko-network`/
  `kohiko-bluetooth`), and content width is now capped and centered
  past ~900-960px so a fullscreen/ultra-wide window gains margin
  instead of stretching every row into one long line.

### Added
- Shared toolkit: `Card` (the bordered-box container the above lists
  and details panels sit inside), `TextField` (a real single-line
  text input with keyboard focus - see `UiWindow::SetFocus()`/
  `ClearFocus()`/`WantsFocus()`/`OnKeyInput()`), `IconButton`, and a
  `Button::Tone` (Accent/Danger outline styles, for e.g. Connect vs.
  Disconnect). `ListRow` gained a `flat` display mode (a thin divider
  and a left accent bar when selected, instead of its own rounded
  card) for the "one bordered card, many divided rows" list shape the
  new mockups use throughout.

### Fixed
- `ScrollView::SetContent()` never actually positioned its content -
  it pinned the content widget's own top-left to the ScrollView's
  position but never translated any of its children, so scrolled
  content rendered at the window's absolute top-left instead of
  inside the ScrollView. This had been latent since 0.19.1's
  `SetResizeHandler()`-driven reflow work; nothing exercised it
  visibly enough to notice until this session's heavier use of
  `ScrollView` surfaced it.
- `ListRow`/`Sidebar` cached icon/text layout as absolute rects
  computed once, which went stale relative to a widget's later-
  translated `bounds` (see above) - now stored as offsets from
  `bounds`, recomputed every `Draw()`/hit-test.
- A widget whose own `onClick`/`onChange` synchronously triggers a
  page rebuild (row selection, tab switches, navigation links - all
  common in the new pages) could destroy itself mid-callback -
  `ScrollView::SetContent()` and `UiWindow::SetRoot()` now retire the
  outgoing widget tree for one extra generation instead of destroying
  it immediately (see the new `Widget::ReleaseChild()`).
- `Makefile` never tracked header dependencies (no `-MMD`/`-MP`), so
  an incremental build after editing a shared header could link stale
  object code against fresh code with no warning - intermittent,
  hard-to-reproduce crashes with no code-level cause. Fixed by adding
  `-MMD -MP` and including the generated `build/*.d` files.
- `TrayIconClient::Run()` (shared by `kohiko-audio-tray`/
  `kohiko-network-tray`/`kohiko-bluetooth-tray`) had a hardcoded
  250ms/500ms `poll()` timeout floor that fired unconditionally
  forever, even fully idle and already docked, waking each of the
  three tray-widget processes 2-4 times a second for no reason - none
  of the events it existed to catch actually needed that granularity:
  redraws are already driven directly by the fd/click/scroll callbacks
  that set the dirty flag (checked at the very next loop iteration
  regardless of what woke it), and the only genuine periodic need -
  the dock-retry attempt while not yet docked, plus each widget's own
  registered timers - was already being computed correctly and just
  getting clamped down to that floor. Now computes the actual next
  deadline (the sooner of the dock retry and every registered timer)
  and passes it straight to `poll()`, blocking indefinitely (`-1`) if
  there's genuinely nothing to wait for. Verified with an isolated
  unit test of the exact timeout-computation logic (five edge cases -
  fully idle, dock-retry due, a timer due, an already-overdue timer
  clamping to 0 rather than going negative, and picking the sooner of
  two competing deadlines), since `TrayIconClient.cpp` itself needs
  Xft/libpipewire/libdbus to build.
- `UiWindow::Run()` - the shared event loop `kohiko-settings`,
  `kohiko-audio`, `kohiko-network`, and `kohiko-bluetooth` are all
  actually built on - had the exact same unconditional 250ms `poll()`
  floor as `TrayIconClient::Run()` above, and for the same reason
  wasn't needed: most windows built on `UiWindow` never call
  `SetInterval()` at all, so they were waking up 4 times a second with
  nothing whatsoever to check. Same fix: block indefinitely when no
  timer is registered, otherwise wake exactly when the soonest one is
  actually due - `kohiko-audio`'s 66ms level-meter animation and
  `kohiko-network`/`kohiko-bluetooth`'s 300ms live-refresh timers are
  completely unaffected either way. Also isolated-unit-tested for the
  same reason as the entry above.

## Version 0.19.1

Release date: 2026-08-03

### Changed
- Redesigned `kohiko-audio`/`kohiko-network`/`kohiko-bluetooth`'s
  content layout from a single narrow column of stacked rows into a
  desktop-first, card-based layout that actually uses a wide window's
  width: responsive device/tile grids (1-3 columns depending on
  available width and item count), full-width page headers
  (title/subtitle/primary control), and - for Ethernet - a labeled
  Address/Gateway/DNS/MAC info grid that grows in from a single
  summary line as width allows, instead of a cramped one-line
  subtitle regardless of window size.
- Windows now genuinely reflow on resize: `UiWindow` gained a
  `SetResizeHandler()` hook that fires on real size changes, which
  every one of the three apps uses to recompute its whole layout
  (column counts, card widths, how much inline detail to show) from
  scratch - previously only the backing pixmap resized, so a tiled
  half-screen or maximized window kept whatever layout it opened
  with. The minimum window size dropped from "locked to the initial
  size" to a real usable floor (420x360), now that resizing to it
  actually looks intentional rather than just clipping.
- New shared toolkit widgets backing the above: `Badge` (a small
  rounded status pill - "Connected", "Default Device", "Good
  signal"), `IconView` (deduplicated from two apps' own copies), and
  a `MakePageHeader()` composer for the title/subtitle/trailing-
  control header every page now opens with.
- Hardened `UiWindow`'s drawing primitives (`FillRect`,
  `FillRoundedRect`, `DrawBorder`, `SetClip`, `DrawIcon`) against
  non-positive rectangle dimensions, reachable now that windows can
  be resized much narrower than before.

## Version 0.19.0

Release date: 2026-08-02

### Added
- **`kohiko-audio`**: a native audio control center backed by PipeWire/
  WirePlumber - output/input device lists, per-device volume and mute,
  default-device switching, and live output/microphone level meters.
  Future-ready page architecture (a `Sidebar` of pages built from a
  single `PipeWireClient::Nodes()` snapshot) so per-app volume,
  equalizer, and similar pages can be added later without reshaping the
  window itself.
- **`kohiko-network`**: Wi-Fi (scan, connect to open/secured networks,
  saved-network reuse, signal strength), Ethernet (connect/disconnect,
  IPv4/IPv6/DNS/gateway/MAC), VPN (activate/deactivate existing
  profiles), and an airplane-mode toggle (NetworkManager's
  `WirelessEnabled`/`WwanEnabled` together) - all backed by
  NetworkManager over D-Bus via a new `NetworkManagerClient`.
- **`kohiko-bluetooth`**: adapter power/discoverable, scanning, pairing,
  connecting/disconnecting, trusting, removing, and (where available)
  battery level - backed by BlueZ over D-Bus via a new `BluezClient`.
- Three matching tray widgets - `kohiko-audio-tray`,
  `kohiko-network-tray`, `kohiko-bluetooth-tray` - docking into the
  existing System Tray Protocol implementation (`SystemTray.cpp`) via a
  new `TrayIconClient`, autostarted by default
  (`desktop/kohiko-*-tray.desktop`, installed to `/etc/xdg/autostart`).
  The audio tray widget's scroll wheel adjusts the default output
  volume directly.
- New shared infrastructure backing all six binaries above: a
  D-Bus marshalling/connection layer (`DBusValue`/`DBusClient`), a
  small UI toolkit (`UiWindow`/`UiWidget`/`UiListRow`/`UiScrollView`/
  `UiSidebar`/`UiPopupMenu`/`UiIconCache`) distinct from
  `kohiko-settings`'s own existing widget code, a single-instance/
  raise-existing-window mechanism (`AppInstanceLock`), a desktop
  notification helper (`NotificationClient`), and a small per-app
  settings store (`AppConfigStore`).
- New build dependency: `libpipewire-0.3-dev`, required together with
  `libdbus-1-dev` to build the six binaries above - both build systems
  skip just those six targets (with a warning), rather than failing the
  whole build, if either is missing; `kohiko`/`kohikoctl`/
  `kohiko-settings` are entirely unaffected either way.
- New `DBusValue` unit test suite (`tests/test_dbusvalue.cpp`, added to
  both `make test` and `ctest`).

## Version 0.18.0

Release date: 2026-07-31

### Added
- Session Restore now remembers a tiled window's actual position in its
  workspace's BSP tree relative to its neighbours, not just which
  workspace/monitor/floating-geometry/fullscreen state it had: two new
  `BSPTree` methods, `CollectPlacementRules()` (derives a replayable
  "place window X next to window Y, split direction D" rule list from
  any tree shape) and `InsertNextTo()` (replays one such rule), plus a
  session-file format bump (`KOHIKO_SESSION_V2`) to persist it. A tiled
  window whose recorded neighbour hasn't reappeared yet this session is
  placed the ordinary way instead - this is inherently best-effort, not
  a guarantee, since windows can map in in an unpredictable order.
- **Adaptive placement**: a new `PlacementHabitStore` learns, per
  application class, which workspace and which side of the tiling
  layout you repeatedly move its windows to by hand (`Super+Shift+<N>`,
  a `Super+LMB` drag, or `Super+Shift+h/j/k/l`), and starts placing new
  windows of that application accordingly once the pattern is clearly
  consistent (not from a single observation) - during an ordinary
  session, not only after a restart. New `general.adaptive_placement`
  config key (default `true`); only ever fills in when neither an
  explicit `windowrule=` nor a precise Session Restore match already
  has an opinion.
- Kohiko Settings: click-to-position text caret in every text field
  (`SettingsWindow::CaretIndexForClick()`, mapping a click's X
  coordinate to the nearest UTF-8 character boundary via the same font
  metrics already used to draw the caret) - clicking anywhere in a
  field now places the caret exactly there instead of always jumping to
  one end.
- Kohiko Settings: structured, row-based editors for `windowrule=` and
  `monitor=`, replacing their previous raw one-line-per-rule text
  block - a click-to-cycle action button
  (float/tile/fullscreen/nofullscreen/workspace) plus `class:`/
  `instance:`/`title:` text fields per window rule, or an output-name
  field and a workspace-number field per monitor rule, each row with
  its own remove button and a shared "+ Add rule" button below the
  last one. `bind=`/`exec.<name>=` are unchanged (still the raw
  text-block editor) - a keybinding or named command doesn't decompose
  into independent selector fields the way a window/monitor rule does.
  The generated `kohiko.conf` syntax is unchanged either way.
- `lockscreen.idle_timeout_minutes` config key (default `0`, disabled):
  locks the screen automatically after that many minutes with no
  keyboard/mouse input anywhere on the display, independent of
  `lockscreen.after`'s own Suspend/startup triggers. New `IdleWatcher`
  class wraps the X11 XScreenSaver extension (`XScreenSaverQueryInfo`)
  to measure idle time server-wide, not just input directed at one of
  Kohiko's own windows; a no-op wherever the extension isn't available.
- Display-sleep inhibition: new `ScreenSaverInhibitor` class provides
  the standard `org.freedesktop.ScreenSaver` and `org.freedesktop.
  PowerManagement` D-Bus `Inhibit(application_name, reason) -> cookie`/
  `UnInhibit(cookie)` interfaces on the session bus - the primary
  mechanism, letting any D-Bus-aware browser (a YouTube tab in Firefox
  or Chromium), video player (VLC, mpv), or presentation tool keep the
  display awake whether or not its window is fullscreen, exactly as it
  already does under a full desktop environment. Backs off entirely if
  either bus name is already owned (e.g. running inside GNOME/KDE's own
  session) rather than fighting over it; automatically releases a
  cookie if the holding application disconnects from the bus without
  calling `UnInhibit` (a crash can never wedge the display awake
  indefinitely). A plain X11 "is a visible window fullscreen" check
  (`WindowManager::IsAnyVisibleWindowFullscreen()`) is the fallback for
  content that isn't D-Bus-aware. New `general.
  inhibit_sleep_during_playback` config key (default `true`). Neither
  mechanism ever touches DPMS's own configuration directly - both work
  by periodically resetting the same server-side idle counter DPMS's
  power-down timers already read (`XResetScreenSaver`), so the display
  still sleeps exactly on schedule once genuinely idle.
- Two new optional, gracefully-degrading build dependencies, detected
  and compiled in independently of each other and of the existing
  XRandr detection: `libxss-dev` (`KOHIKO_HAVE_XSS` - idle-timeout
  locking and the fullscreen-fallback half of sleep inhibition) and
  `libdbus-1-dev` (`KOHIKO_HAVE_DBUS` - the primary half of sleep
  inhibition). Kohiko still builds and runs normally without either,
  just without the one feature each backs.
- `test_placementhabits`: a new standalone unit test suite for
  `PlacementHabitStore`'s voting/confidence-threshold logic, run as
  part of `make test`/`ctest` alongside the existing BSP tree and
  launcher-scoring suites - redirects `XDG_DATA_HOME` to a throwaway
  temp directory so it never reads or overwrites a real user's actual
  learned habits.
- `tests/test_bsptree.cpp`: new regression coverage explicitly locking
  in that closing a tiled window always expands only its immediate
  spatial sibling into the freed slot (e.g. `A | B/C` closing `B`
  becomes `A | C`, never a full re-stack of `A` over `C`), and that
  `CollectPlacementRules()`/`InsertNextTo()` can always successfully
  reconstruct a tree's exact neighbour relationships from scratch.

### Changed
- Kohiko Settings: Apply/Save/Reset collapsed to Save/Reset. Save now
  does everything Apply previously did (validate, write, tell a running
  Kohiko to reload) *and* keeps the window open, exactly like Apply
  always did - there is no longer a separate "Save and close" action,
  since there was never a good reason to leave Settings just to confirm
  a change actually took effect.
- `SessionStore::Save()` now takes a `WorkspaceManager` reference (to
  read each workspace's BSP tree for `CollectPlacementRules()`) in
  addition to its previous window-list/monitor-manager parameters.

### Fixed
- Kohiko Settings' top-level `Tab` key handler didn't know about the
  new structured `windowrule=`/`monitor=` rows and was clearing focus
  entirely instead of moving to the next cell within a row - found and
  fixed during this release's own testing of the structured editor
  added above, before it ever shipped.
- `BSPTree::CollectPlacementRules()`'s first implementation emitted a
  deeply-nested split's own connecting rule *before* the outer rule
  that actually places its neighbour in the tree, which
  `InsertNextTo()` couldn't replay (the referenced neighbour wasn't
  placed yet); switched to pre-order emission (parent rules before the
  splits nested inside either of their own two sides) - caught by this
  release's own 5,000-trial round-trip test before shipping, now
  permanently covered by a regression test (see Added above).

### Notes
- The BSP collapse behaviour (what happens when a tiled window is
  removed) was reviewed in depth against a specific "should merge
  sideways, not re-stack" example, plus a 20,000-trial randomized fuzz
  test checking for overlap/count invariants - both confirmed the
  existing implementation already collapses correctly; no change was
  needed there beyond the new regression tests that now lock the
  existing, correct behaviour in. The window insertion algorithm
  itself was not touched at all this release.
- Session Restore's BSP-position tracking and adaptive placement are
  deliberately independent, best-effort systems layered on top of the
  ordinary placement rules (`windowrule=` > a precise session match >
  autostart workspace > parent-window attachment > a learned adaptive
  habit, in that priority order) - neither can ever override an
  explicit rule, and both degrade gracefully (falling straight back to
  ordinary placement) whenever their own data doesn't apply.

## Version 0.17.0

Release date: 2026-07-26

### Added
- Kohiko Settings (`kohiko-settings`): a native configuration GUI, built as its own standalone application (not part of the `kohiko` process) and installed automatically alongside `kohiko`/`kohikoctl`, with its own `.desktop` entry (`desktop/kohiko-settings.desktop`) and icon (`assets/icons/kohiko-settings.svg`) so it's discoverable through the launcher or any standards-compliant menu. It renders every setting described by `ConfigSchema` (previously dormant scaffolding, now actually used) grouped into a category sidebar with sub-headings, offers a search box filtering by key/label/description/group, an (i) info icon per setting showing its description/default/allowed values, inline validation of typed values (numbers, colors, percentages, and rule/keybinding syntax), and Apply/Save/Reset to Default actions.
- `ConfigWriter` (`include/ConfigWriter.h`/`.cpp`): the write path Kohiko Settings uses to save changes back into `kohiko.conf`, editing existing lines, comments, and ordering in place rather than regenerating the file, so hand-editing the config remains fully supported alongside the GUI. A new key with no existing line is appended under a `# --- Added by Kohiko Settings ---` marker instead of being inserted elsewhere in the file.
- `lockscreen.after` config key (`never`/`manual`/`suspend`/`always`), replacing the previous plain on/off `lockscreen.lock_on_suspend` toggle: `never` disables locking entirely, including the manual lock command; `manual` allows locking on demand only; `suspend` (the default) also locks automatically before Suspend; `always` additionally locks once at Kohiko startup.
- Lock screen clock and date display (`lockscreen.show_clock`, `show_date`, `clock_format`, `date_format`), and optional hostname/username display above the password field (`lockscreen.show_hostname`, `show_username`).
- `Utils::SecureErase()`: overwrites a string's buffer with zeros before clearing it; used by the lock screen to wipe the typed password from memory immediately after every authentication attempt (successful, failed, or cancelled via `Escape`), as a best-effort mitigation beyond what a plain `clear()` provides.

### Changed
- `ConfigSchema::ValueType::Bool` renamed to `Boolean`.
- Default config: `session.restore_priority` default changed from `session` back to `config`; `workspace1=`/`workspace2=`/`workspace3=` example autostart lines commented out again; `auto_start_programs` default restored to `Telegram discord zen-browser flameshot`.
- `scripts/install-arch.sh` updated to note that `kohiko-settings` is installed automatically alongside `kohiko`, with no additional dependency.

### Removed
- `lockscreen.lock_on_suspend` config key, replaced by `lockscreen.after` with no automatic migration; a config file still setting `lockscreen.lock_on_suspend` has no effect under this release.

### Notes
- `kohiko-settings` is built from a deliberately small, shared subset of the project's own source files (`Config`, `ConfigSchema`, `Utils`, `Font`, plus its own `SettingsWindow`/`ConfigWriter`) rather than linking against the full `kohiko` binary, so it carries none of the window-manager-only code (BSP layout, EWMH handling, XRandr monitor detection, and so on) and needs nothing beyond the X11/Xft dependencies `kohiko` itself already requires.
- The four repeatable config directives (`bind=`, `exec.<name>=`, `windowrule=`, `monitor=`) are edited in Kohiko Settings as raw, one-line-per-entry text blocks rather than individually-typed fields; `workspace<N>=` autostart is the exception, shown as one plain text field per workspace since each is a distinct key.
