# Kohiko Project History

This document traces the evolution of Kohiko, a C++20 / X11 tiling window
manager, across its released versions from 0.1.0 through 0.20.4. It is
derived from a direct comparison of the source, configuration, and
documentation of each released version against the one before it.

--------------------------------------------------------------------------

## Phase 1 — Core Tiling Engine (0.1.0 - 0.3.0)

**Versions:** 0.1.0, 0.2.0, 0.3.0

**Goals:**
Establish the fundamental architecture: a BSP (binary space partitioning)
tree as the tiling model, a coordinating `WindowManager`, and the
supporting X11 plumbing (event loop, workspace/monitor bookkeeping,
window tracking).

**Major developments:**
- 0.1.0 shipped the initial skeleton: the BSP tree types, `LayoutEngine`,
  `WindowManager`, workspace/monitor managers, a basic bar, scratchpad,
  and an IPC server, but with several components (a separate
  `InputManager`, an unused `Renderer`) that were not fully wired
  together, and a `Config` limited to single-value keys.
- 0.2.0 was a near-total rewrite that turned the skeleton into a working
  tiling window manager: `Config` was rebuilt to support repeatable
  directives (`bind=`, `exec.<name>=`); a `Command`/`KeyCombo`/`IpcPath`/
  `Json` layer was introduced to give keybindings and IPC a shared,
  parsed representation; a `kohikoctl` CLI client and a BSP-tree unit
  test suite were added; and the unused `InputManager`/`Renderer`
  classes were removed in favor of `KeyboardManager`/`MouseManager`
  owning their own grabbing logic directly.
- 0.3.0 added the first native, non-shelled-out UI elements — a
  launcher (`Super+D`) and a notepad (`Super+N`) — along with an
  animated swap-drag gesture and a minimum-tile-size guard that could
  fall back to another workspace or floating rather than forcing an
  unusably small tile.

**Lessons visible from the repository:**
The 0.1.0 -> 0.2.0 gap is the largest single jump in the entire history,
suggesting 0.1.0 was published as an early architectural sketch rather
than a working release; the project's actual usable baseline begins at
0.2.0.

--------------------------------------------------------------------------

## Phase 2 — Desktop Integration (0.4.0 - 0.8.0)

**Versions:** 0.4.0, 0.5.0, 0.5.1, 0.6.0, 0.7.0, 0.8.0

**Goals:**
Turn the tiling core into something usable as a daily desktop session,
by adding the surrounding functionality a window manager is normally
expected to provide, and by making the launcher a genuine application
launcher rather than a plain command runner.

**Major developments:**
- 0.4.0 rebuilt the launcher into a full application launcher: `.desktop`
  file scanning, icon rendering (via Imlib2 and, at this stage, GTK3 for
  icon-theme lookup), fuzzy matching, a popularity/history-based ranking
  table, and IME-based (XIM/XIC) Unicode text entry. The same release
  added system tray support and an Arch Linux install script.
- 0.5.0 added EWMH/NetWM compliance (`_NET_SUPPORTING_WM_CHECK`,
  `_NET_CLIENT_LIST`, `_NET_ACTIVE_WINDOW`) so EWMH-aware tools would
  work correctly, plus a live launcher-cache reload command.
- 0.5.1 was a small bugfix release (resize-direction sign error, focus
  stealing while a modal is open, tray icon background color).
- 0.6.0 added autostart programs and multi-layout keyboard support
  (`setxkbmap`-based), both configurable and reload-safe.
- 0.7.0 replaced classic X11 core-font text rendering with Xft/
  fontconfig, adding automatic per-character font fallback so
  non-Latin scripts render correctly — the project's first explicit
  acknowledgment of a rendering dependency beyond plain libX11.
- 0.8.0 introduced per-application window rules (`windowrule=`) and
  real EWMH fullscreen support, plus natural-size (rather than
  fixed-fraction) floating window placement.

**Lessons visible from the repository:**
This phase substantially increased the project's dependency footprint
(GTK3, Imlib2, Xft/fontconfig) in exchange for desktop-environment-level
functionality (icons, fonts, tray). The GTK3 dependency in particular
was later identified as excessive for what it was used for and removed
in Phase 6.

--------------------------------------------------------------------------

## Phase 3 — Real-World Hardening (0.8.1 - 0.8.3)

**Versions:** 0.8.1, 0.8.2, 0.8.3

**Goals:**
Fix concrete bugs surfaced by specific real applications, rather than
add new features.

**Major developments:**
- 0.8.1 fixed a BSP tree bug where a tiled window that went fullscreen
  and was closed while still fullscreen left a permanently reserved,
  empty tree slot (a visible "dead space" bug), introducing the
  `OccupiesTreeSlot()` concept to distinguish "currently tiled" from
  "still holds a tiling reservation."
- 0.8.2 fixed a window-content rendering glitch by reordering geometry
  application (`Arrange()`) to happen before, not after, mapping a
  window — most reliably reproduced with Java/Swing applications such
  as TLauncher.
- 0.8.3 began respecting a client's own declared minimum size
  (`WM_NORMAL_HINTS`) in tiling capacity checks, and added a forced
  redraw (`ClearArea`) for clients that visually mis-render after a
  denied resize request.

**Lessons visible from the repository:**
Several specific applications (TLauncher/Java-Swing GUIs, Discord,
Telegram) recur repeatedly across this phase and the next as the
concrete motivating cases behind otherwise-generic-sounding fixes,
indicating the project's bug fixes were driven by hands-on daily use
rather than abstract correctness review.

--------------------------------------------------------------------------

## Phase 4 — Advanced Tiling and Multi-Monitor (0.9.0 - 0.13.2)

**Versions:** 0.9.0, 0.10.0, 0.11.0, 0.12.0, 0.13.0, 0.13.1, 0.13.2

**Goals:**
Make the tiling algorithm itself substantially smarter, and add
first-class multi-monitor support — arguably the largest single
feature of the project's history.

**Major developments:**
- 0.9.0 replaced simple "always split 50/50" insertion with an
  escalating placement algorithm (try the natural split direction, then
  the alternate direction, then shrink existing tiles via a bisection
  search), plus a "tiling misbehavior" detector that moves a window
  that keeps fighting its assigned geometry to floating or another
  workspace.
- 0.10.0 expanded automatic floating-window detection to cover every
  EWMH floating-style window type (not just dialogs) and made transient
  windows follow their parent's workspace and center over it.
- 0.11.0 added real multi-monitor support: independent per-monitor
  workspaces and BSP trees, a `monitor=` config directive, hotplug
  handling, and `focusmonitor`/`movetomonitor` commands.
- 0.12.0 refined multi-monitor support significantly: one bar per
  monitor, ambient (always-on) focus-follows-mouse across monitors,
  live floating-window drag across monitor boundaries, and replacing
  workspace-swap-on-conflict with an explicit rejection-plus-notification.
- 0.13.0 extended `Super+RMB` drag-resize to floating windows (previously
  tiled-only).
- 0.13.1 and 0.13.2 record a visible course-correction: 0.13.1 introduced a
  narrow, opt-in fix (`windowrule=tile` ignoring a window's own minimum
  size) for a specific application (Telegram), which 0.13.2 replaced one
  release later with a broader rule applied to every window
  unconditionally, after recognizing the narrow fix didn't address the
  general case.

**Lessons visible from the repository:**
The 0.13.1 -> 0.13.2 sequence is a clear example of the project favoring a
generally-correct rule over an accumulation of per-application special
cases once the general rule was understood; window rules and
application-specific tuning appear throughout the project's history but
are periodically superseded by broader fixes when a pattern is
recognized.

--------------------------------------------------------------------------

## Phase 5 — Workspace and Session Ergonomics (0.14.0 - 0.14.1)

**Versions:** 0.14.0, 0.14.1

**Goals:**
Smooth out remaining rough edges in startup behavior — where autostarted
programs land, and which monitor is considered focused at startup.

**Major developments:**
- 0.14.0 added `workspace<N>=` autostart, matching a spawned program's
  eventual window back to the process that launched it via process-tree
  ancestry (since shells and launcher scripts commonly interpose extra
  processes between the spawn call and the real application).
- 0.14.1 fixed focused-monitor detection immediately after startup and
  after a monitor hotplug (by directly querying the pointer instead of
  waiting for a motion event), and restored an autostart config section
  that had been inadvertently dropped in 0.14.0.

**Lessons visible from the repository:**
0.14.1's fixes to 0.14.0's own default configuration and monitor-detection
timing suggest this pair of releases functioned as a single feature
followed immediately by its own bugfix/cleanup pass.

--------------------------------------------------------------------------

## Phase 6 — Launcher Maturity and Dependency Reduction (0.15.0 - 0.15.2)

**Versions:** 0.15.0, 0.15.1, 0.15.2

**Goals:**
Refactor the launcher, which had grown substantially since 0.4.0, into
a properly modular implementation, while removing a dependency (GTK3)
that had been carried solely for one narrow purpose.

**Major developments:**
- 0.15.0 split the launcher's internals into dedicated modules
  (`AppIndex`, `DesktopEntry`, `IconResolver`, `IniFile`, `HistoryStore`,
  `LauncherScoring`, `Xdg`), replaced GTK3-based icon-theme lookup with
  a from-scratch implementation of the freedesktop Icon Theme
  Specification, and added a configurable web-search fallback with
  engine-specific smart prefixes.
- 0.15.1 was a performance and correctness pass on the new modules:
  precomputing word-splits for file search (removing the dominant cost
  of a launcher query) and fixing an incorrect list-separator
  assumption in icon-theme index parsing.
- 0.15.2 was a one-line fix bumping the on-disk application-index cache
  version to invalidate caches written before 0.15.1's schema changes.

**Lessons visible from the repository:**
This phase demonstrates a recurring pattern in the project: a period of
rapid feature growth in one area (the launcher, since 0.4.0) eventually
followed by a dedicated refactor once the growth outpaces the original
file/module structure.

--------------------------------------------------------------------------

## Phase 7 — Session Persistence and System Integration (0.16.0)

**Versions:** 0.16.0

**Goals:**
Extend Kohiko from a window manager into a more complete minimal
desktop session, covering functionality (locking, power control, session
continuity) that had previously been left to external tools or not
addressed at all.

**Major developments:**
- A native, PAM-authenticated lock screen with no dependency on
  external lockers such as `i3lock`, integrated with a keyboard/pointer
  grab and with the power menu's Suspend action.
- A power menu (Shutdown/Restart/Suspend) on every bar.
- Session restore across restarts, persisting each window's workspace,
  floating state/geometry, monitor, and fullscreen state, keyed by X11
  window ID.
- Adoption of already-mapped windows at startup, resolving a
  long-standing known limitation from earlier releases.
- `ConfigSchema`, a dormant metadata registry intended as groundwork for
  a possible future configuration GUI, explicitly not wired into any
  runtime behavior yet.
- The README's "Known limitations" section was restructured into
  "Done," "Planned," and "Intentionally unsupported" — a shift in how
  the project describes its own scope and maturity.

**Lessons visible from the repository:**
This release resolves two known limitations that had been carried and
explicitly documented since Phase 4 (single-monitor launcher/notepad,
lack of window adoption), and the README's reframing away from a
"known limitations" list suggests the project considers its core,
single-user desktop-session scope largely complete as of this release.

--------------------------------------------------------------------------

## Phase 8 — Configuration GUI (0.17.0)

**Versions:** 0.17.0

**Goals:**
Deliver the configuration GUI flagged as "Planned" in the previous
release, built directly on top of the `ConfigSchema` metadata registry
that had been kept dormant and in sync with every new setting since it
was introduced.

**Major developments:**
- Kohiko Settings (`kohiko-settings`): a native settings GUI shipped as
  its own standalone application, not part of the `kohiko` process,
  installed automatically alongside `kohiko`/`kohikoctl` with its own
  `.desktop` entry and icon. It presents every setting from
  `ConfigSchema` grouped by category and sub-heading, with search, a
  per-setting info panel, inline validation, and Apply/Save/Reset to
  Default.
- `ConfigWriter`, a dedicated write path that edits `kohiko.conf`'s
  existing lines, comments, and ordering in place, so hand-editing the
  config file remains fully supported alongside the GUI rather than
  being superseded by it.
- `lockscreen.after` (`never`/`manual`/`suspend`/`always`), replacing
  the previous plain on/off suspend-lock toggle with finer-grained
  control over when the lock screen engages automatically, plus clock/
  date/hostname/username display options and a best-effort secure wipe
  of the typed password from memory after use.

**Lessons visible from the repository:**
This release directly follows through on the previous release's own
stated plan rather than introducing an unrelated feature, and does so
by finishing work (`ConfigSchema`) that had been deliberately kept
current release-over-release specifically so this GUI could be built
without a separate audit pass first. The GUI was also kept
architecturally separate from the window manager process itself
(a distinct binary, built from a small shared subset of source files)
rather than folded into `kohiko` directly, consistent with the
project's general preference for keeping the core WM process narrowly
scoped.

--------------------------------------------------------------------------

## Phase 9 — Polish and Predictive Placement (0.18.0)

**Versions:** 0.18.0

**Goals:**
Follow through on 0.17.0's own stated direction — finish the three
narrower Kohiko Settings/lock-screen refinements it had explicitly left
as "Planned" — while also substantially deepening Session Restore and
closing out a display-sleep gap that had never had dedicated support at
all, all without touching the tiling/insertion algorithm itself.

**Major developments:**
- Kohiko Settings' Apply/Save/Reset trio collapsed to just Save/Reset:
  Save now validates, writes, and tells a running Kohiko to reload, all
  without closing the window — Apply and the old closing Save are now
  the exact same action, so there's no longer a reason to leave Settings
  just to confirm a change actually took.
- Session Restore now remembers a tiled window's actual position in its
  workspace's BSP tree relative to its neighbours (`BSPTree::
  CollectPlacementRules()`/`InsertNextTo()`), not just which workspace
  it was on — best-effort by nature, since it depends on whichever
  neighbour a window was recorded against having already reappeared
  this session.
- Adaptive placement: a new, independent learning system
  (`PlacementHabitStore`) that watches manual workspace moves and manual
  tiled repositioning, and — once a genuinely repeated, consistent
  pattern emerges, never from a single observation — starts placing new
  windows of that same application accordingly, during an ordinary
  session and not only after a restart.
- Kohiko Settings: click-to-position text caret (mapping a click's pixel
  position back to the nearest character boundary via the same font
  metrics already used to draw the caret) in every text field, plus
  structured, row-based editors for `windowrule=`/`monitor=` in place of
  the raw one-line-per-rule text block — a click-to-cycle action button
  and per-selector text fields that still write out the exact same
  underlying config syntax.
- `lockscreen.idle_timeout_minutes`: genuine idle-timeout locking,
  independent of `lockscreen.after`'s Suspend/startup triggers, using
  the X11 XScreenSaver extension to measure idle time server-wide.
- Display-sleep inhibition: Kohiko now provides the standard
  `org.freedesktop.ScreenSaver`/`org.freedesktop.PowerManagement` D-Bus
  `Inhibit`/`UnInhibit` interfaces itself whenever nothing else on the
  session bus already does, so browsers, video players, and
  presentation software already built to ask a desktop not to sleep the
  screen — regardless of whether their window is fullscreen — work with
  Kohiko out of the box; a plain "is a visible window fullscreen" X11
  check remains as a fallback for anything that isn't D-Bus-aware. Ties
  into the exact same idle counter DPMS's own timers read, via
  `XResetScreenSaver`, rather than touching DPMS configuration directly.
- Two new optional, gracefully-degrading build dependencies following
  the project's existing XRandr precedent: `libxss-dev` (idle detection
  and the fullscreen-fallback half of sleep inhibition) and
  `libdbus-1-dev` (the primary half of sleep inhibition) — Kohiko still
  builds and runs completely normally without either, just without the
  one feature each backs.

**Lessons visible from the repository:**
Every one of 0.17.0's three "Planned" refinements shipped in the very
next release, in the order they were listed — a much tighter follow-
through loop than several earlier phases, where a stated direction
sometimes took multiple releases to materialize (multi-monitor support
was flagged well before Phase 4 actually delivered it). Session Restore
and adaptive placement were not on the "Planned" list at all, suggesting
this release's scope came from two different sources at once: closing
out a known backlog item and independently deepening an existing
feature area. The BSP collapse behaviour and the underlying tiling
insertion algorithm were both explicitly left untouched throughout —
the project's tiling core has now gone several releases without a
structural change, with new work consistently landing in the layers
built on top of it (session persistence, learned habits, the
configuration surface) rather than in the BSP tree itself.

--------------------------------------------------------------------------

## Phase 10 — Native Desktop Integration (0.19.0)

**Versions:** 0.19.0

**Goals:**
Replace reliance on third-party tray applets (`nm-applet`,
`blueman-applet`, a PulseAudio/PipeWire mixer) for the three most common
pieces of desktop-session functionality a tiling WM doesn't itself
provide - audio, networking, and Bluetooth - with native Kohiko
applications and tray widgets, while continuing to use the same
underlying system services (PipeWire/WirePlumber, NetworkManager, BlueZ)
as their backend rather than reimplementing any of them.

**Major developments:**
- Three new standalone GUI applications - `kohiko-audio`,
  `kohiko-network`, `kohiko-bluetooth` - each a completely ordinary X11
  client (not part of the `kohiko` WM process), following the same
  "standalone binary, no toolkit dependency" precedent `kohiko-settings`
  set in Phase 8.
- A new shared UI toolkit (`UiWindow`/`UiWidget`/`UiListRow`/
  `UiScrollView`/`UiSidebar`/`UiPopupMenu`/`UiIconCache`) used by all
  three, deliberately kept separate from `kohiko-settings`'s own
  existing hand-rolled widget code rather than migrating that working,
  already-shipped code onto a shared base - new infrastructure for new
  apps, not a refactor of something that already works.
- A new D-Bus layer (`DBusValue`/`DBusClient`) generalizing the
  request/reply and property/signal patterns `ScreenSaverInhibitor`
  (Phase 9's predecessor, Phase 7 originally) already used in miniature
  for one narrow purpose, now reused by `NetworkManagerClient` and
  `BluezClient`; a separate `PipeWireClient` for `kohiko-audio`'s
  backend, since PipeWire has no D-Bus interface of its own.
- A tray-docking framework (`TrayIconClient`) implementing the client
  side of the same freedesktop System Tray Protocol `SystemTray.cpp`
  already implements the host side of (Phase 2), used by three new tray
  widgets - `kohiko-audio-tray`, `kohiko-network-tray`,
  `kohiko-bluetooth-tray` - plus a small single-instance/raise-existing-
  window mechanism (`AppInstanceLock`) shared by all six new binaries.
- `libpipewire-0.3-dev` joins `libdbus-1-dev` as a build dependency for
  these six binaries specifically (both required together, unlike
  `kohiko`'s own already-optional `libdbus-1-dev` usage) - the rest of
  the project is entirely unaffected if either is absent.

**Lessons visible from the repository:**
This is the first phase whose new code deliberately does *not* build on
an existing shared foundation where one already existed for a similar
purpose (`kohiko-settings`'s widget code) - a sign that "shared
infrastructure" is being scoped to genuine reuse across the *new* apps
being added, rather than treated as an excuse to refactor already-stable
code paths, consistent with Phase 9's observation that Kohiko's core
increasingly changes only where real friction is found. It's also the
first phase to add a hard (non-optional-feature) external dependency
pair to part of the build - a narrower version of the same "gracefully
degrade, never fail the whole build" precedent XRandr/XScreenSaver/D-Bus
established in earlier phases, just applied at the level of "skip six
binaries" instead of "skip one feature inside an existing binary".

--------------------------------------------------------------------------

## Phase 11 — Desktop Integration Redesign and Toolkit Hardening (0.19.1 - 0.19.2)

**Versions:** 0.19.1, 0.19.2

**Goals:**
Take the three apps Phase 10 introduced from "functional, backed by
the right services" to "matches a specific visual design", without
touching any of the backend clients Phase 10 built - and, along the
way, harden the one-phase-old shared UI toolkit against bugs that
heavier use of it exposed.

**Major developments:**
- 0.19.1 was a first layout pass: the original single-column stacked
  rows became a responsive card/grid layout (1-3 columns depending on
  width), full-width page headers, and a growing Ethernet info grid -
  plus `UiWindow::SetResizeHandler()`, since until this release only
  the backing pixmap resized and a tiled or maximized window kept
  whatever layout it opened with.
- 0.19.2 replaced each app's main page again, this time to match a
  supplied visual mockup closely rather than an original design: flat
  divided-row lists inside one bordered `Card` (replacing the grid of
  separate cards 0.19.1 had just introduced), a violet accent
  replacing the previous blue, a live-filtering `TextField` search
  field in `kohiko-network` (the toolkit's first real text-input
  widget, with its own keyboard-focus system added to `UiWindow`), and
  a selected-item details panel in `kohiko-network`/`kohiko-bluetooth`
  sitting beside their list above ~720px content width. Anything the
  new mockups didn't have room for (level meters, Ethernet/VPN
  management, per-device Bluetooth trust) moved to a new per-app
  "Advanced Settings" sub-page rather than being dropped, and every
  page's responsive floor dropped to a genuinely usable ~420px.
- Four bugs were found and fixed in the shared toolkit itself during
  0.19.2, none specific to any one app: `ScrollView::SetContent()`
  never actually translated its content into position (latent since
  0.19.1's resize work, only surfaced by 0.19.2's heavier `ScrollView`
  use); `ListRow`/`Sidebar` cached icon/text layout as absolute rects
  that went stale once the above was fixed and content started
  actually moving; a widget whose own callback synchronously rebuilt
  its containing page could destroy itself mid-callback (fixed with a
  one-generation-deferred destruction scheme in `ScrollView` and
  `UiWindow`, plus a new `Widget::ReleaseChild()`); and the `Makefile`
  never tracked header dependencies, so an incremental rebuild after
  editing a shared header could silently link stale and fresh object
  code together. See `docs/ARCHITECTURE.md` and `CHANGELOG.md` for the
  full detail on each.

**Lessons visible from the repository:**
This phase is the first to substantially rework UI Phase 10 had *just*
shipped, rather than build net-new surface - and the bugs it found were
all latent in Phase 10's original toolkit code, not introduced by the
redesign itself, just never exercised by that phase's lighter,
scroll/callback-light original layouts. The general shape - a new
capability (heavier `ScrollView` usage, a real text-input widget,
callbacks that rebuild their own page) immediately surfacing
correctness gaps nothing before it had reason to hit - matches Phase 9
and Phase 10's own observations that this codebase's remaining bugs
tend to be found by genuinely new usage patterns rather than further
scrutiny of unchanged code. Fixing all four at the toolkit level
(rather than working around each in the one app that happened to hit
it first) keeps with the project's established preference for shared
infrastructure over per-call-site patches wherever the bug isn't
actually app-specific.

--------------------------------------------------------------------------

## Phase 12 — Persistence, Recovery, Session Integration, and Wallpaper (0.20.0)

**Versions:** 0.20.0

**Goals:**
Shift from adding features to polishing Kohiko into a complete desktop
environment, per an explicit brief covering eight named areas
(persistent learning, launcher auto-refresh, tray integration, the BSP
algorithm, taskbar rendering, wallpaper, login experience, general
polish) plus seven cross-cutting requirements (config migration,
recovery mode, session robustness, plugin-architecture preparation,
background-work auditing, memory/resource cleanup, a full consistency
review) - with an explicit instruction to verify before changing
anything, and a follow-up brief specifically for the login experience:
autologin into a Kohiko session with no username ever typed, backed by
Kohiko's own lock screen rather than a second one, integrated with the
system's real authentication and session-lock mechanisms rather than
duplicating either.

**Major developments:**
- **Verification-first, several times over.** Four of the eight named
  areas turned out to already be correct and were deliberately left
  unchanged rather than rewritten for its own sake: the BSP
  split-direction algorithm (proved mathematically optimal for
  minimizing child-tile distortion), session restore's robustness
  across a changed workspace count, a disconnected monitor, a missing
  application, and a slow-launching one (all four already handled by
  the existing `Manage()`/`SessionStore` design), the tray integration
  (already fully automatic status/click/scroll/right-click, confirmed
  by reading the actual widget source rather than trusting its own
  documentation), and the codebase's memory/resource discipline (zero
  raw `new`/`delete` anywhere; every sampled X11 resource-creation call
  already paired with a free).
- **Persistent learning's one real gap** - a monitor's *starting*
  workspace after a restart came only from static `monitor=` rules or
  "first available", never from what it last actually showed - closed
  by extending `SessionStore` (not a new store) with
  `LastWorkspaceForMonitor()`.
- **Config migration and recovery mode**, working together without
  either needing to know the other exists: `ConfigMigration` appends
  any `ConfigSchema` key missing from an existing `kohiko.conf` (never
  touching a key that's already there, including one a user
  deliberately left blank - a real bug in an early draft, caught by a
  test before it shipped, since `GetScalar().empty()` can't tell
  "unset" from "set to empty on purpose" the way the `ConfigWriter::
  Contains()` written to fix it can); `RecoveryMode` detects a crash
  before a previous startup reached "ready" and falls back to built-in
  defaults, after backing up the suspect file - the same backup
  `ConfigMigration` itself already writes.
- **Launcher auto-refresh** via a new `inotify`-based `AppDirWatcher`,
  feeding `EventLoop`'s existing `select()` loop the same way
  `ScreenSaverInhibitor` already does - no polling thread, no new
  timer.
- **Two real CPU-wakeup bugs found and fixed** while auditing
  background work: `TrayIconClient::Run()` and `UiWindow::Run()` (the
  event loops behind seven separate processes between them) each had
  an unconditional 250-500ms `poll()` timeout floor that fired forever
  regardless of whether anything was actually pending - neither loop
  needed it, since redraws were already dirty-flag-driven and the only
  genuine periodic needs were already computed correctly and just
  getting clamped down to the floor.
- **`Bar::Redraw()` stopped drawing straight onto the live window** - a
  classic X11 flicker source (full clear immediately followed by
  several draw calls, no coalescing guarantee) - in favor of an
  off-screen backing `Pixmap` composited in one `XCopyArea`, the same
  pattern `UiWindow.cpp` already used elsewhere in the project but
  `Bar` itself never had.
- **The login/session experience**, the largest single workstream:
  a generic (not Arch-only) `xsessions` install; a new `kohiko-session`
  wrapper as the actual session entry point, providing crash-restart
  supervision (exponential backoff, a hard burst limit, SIGTERM/SIGINT
  forwarded with a SIGKILL fallback) that complements rather than
  duplicates `RecoveryMode` - the only component this phase could
  meaningfully test against real running processes rather than by
  inspection, which caught a real bug (`kill` called without its
  required `-` signal-name prefix, silently breaking signal forwarding
  entirely); a new `SessionLockBridge` integrating the existing native
  `LockScreen` with `systemd-logind`'s session-lock D-Bus interface
  (`loginctl lock-session` now reaches it; lock/unlock state is
  reported back via `SetLockedHint()`) deliberately one-way - logind's
  own `Unlock` signal is never honored, since doing so would mean a
  second, PAM-bypassing way to unlock the session; and an opt-in
  `kohikoctl configure-autologin` (`AutologinConfigurator`) for
  LightDM/SDDM (a dedicated drop-in file, never an in-place edit of an
  existing one; GDM detected but deliberately left to documentation,
  for lack of an equivalent safe drop-in mechanism) that defaults to
  No, refuses outright if autologin is already configured anywhere,
  and was verified for real - not just by inspection - inside an
  isolated mount namespace with a fake `/etc` bind-mounted over the
  real one. Investigating the desired "never type a username" flow
  found the architecture already supported it: `LockScreen` already
  resolves its username via `getpwuid(getuid())`, never prompts for
  one, so the only genuinely new work was the pieces around it.
- **Wallpaper support**, the second-largest workstream: independently
  configurable per monitor and per workspace, applied as a single
  composite `Pixmap` spanning every connected monitor set as the root
  window's background (the same technique `feh`/`nitrogen`/
  `xwallpaper` use, so gaps between tiled windows show it through for
  free), four real scale modes via a new shared `ImageRenderer` used by
  both the new `WallpaperManager` and - replacing its previous
  stretch-only loader - `LockScreen`'s own background image, which
  also gained the option to simply mirror the desktop wallpaper
  instead of needing its own. Live-reloads on file changes via
  `inotify`, watching each resolved wallpaper's containing directory
  (the same atomic-replace-safe approach `AppDirWatcher` already
  established, reused rather than watching files directly). Six
  supplied wallpaper images now ship with Kohiko itself.
- **Extension points formalized**, per an explicit "no `dlopen()` yet"
  brief: a new `docs/ARCHITECTURE.md` section documenting the concrete,
  already-proven shapes to follow for a new background subsystem, a
  new setting (scalar or repeatable), a new tray icon, a new full
  companion app, or a new `kohikoctl` command - each with a real,
  already-shipped example to point to, several of them added by this
  very phase.

**Lessons visible from the repository:**
This phase's testing story splits cleanly by what the sandbox it ran
in could and couldn't do: this environment had no Xft, no D-Bus
headers, no Imlib2, and no working X server, so every actual
`kohiko`/`kohiko-settings` compile was blocked at the first Xft
`#include`. Everything with no such dependency (config parsing,
session-state resolution, the aspect-ratio math behind the four scale
modes, `AutologinConfigurator`'s detection/write/undo logic) got real,
thorough unit tests instead - and caught real bugs doing it, including
one in a test itself (an initial Fit-mode test had letterbox and
pillarbox backwards; independently re-derived the math before
concluding the implementation, not the test, was right). The one
component built as a plain POSIX shell script - `kohiko-session` -
could be exercised as a real, running process regardless of any of
that, and was the single highest-value thing tested this phase as a
result, catching a bug (the missing `kill` signal-name prefix) that
would have silently broken session-end handling in production.
`AutologinConfigurator`'s system-file-touching logic went a step
further, verified inside an isolated mount namespace against a fake
`/etc` rather than only by code review - worth remembering as a
technique for anything future work needs to test that would otherwise
require root and a real system to exercise safely. Everything that
genuinely could only be verified by code review (Imlib2 rendering
calls, raw libdbus message construction) was written by close analogy
to already-proven code elsewhere in the same codebase rather than from
scratch, and flagged plainly as reviewed-not-executed rather than
described as tested.

--------------------------------------------------------------------------

## Phase 13 — Tray Integration Verification (0.20.1)

**Versions:** 0.20.1

**Goals:**
Diagnose and fix a specific, concrete real-world report - the audio/
network/Bluetooth tray icons did not appear on a real Arch Linux
Kohiko session - by verifying the actual runtime behavior end to end
rather than trusting what the code, or the existing documentation,
claimed.

**Major developments:**
- Root cause: the three tray widgets' `.desktop` autostart entries
  (introduced back in 0.19.0, described there and in `README.md` as
  "autostarted by default") were correctly installed to
  `/etc/xdg/autostart`, but nothing in Kohiko - which has never had a
  session manager of its own - ever actually read that directory.
  `WindowManager::RunAutostart()` gained a second mechanism alongside
  its existing config-string one: it now also runs every `.desktop`
  entry under `Xdg::AutostartDirs()`, filtered through a new
  `ShouldAutostart()` predicate. Kohiko also now sets
  `$XDG_CURRENT_DESKTOP` itself if nothing already has, closing a
  display-manager-vs-manual-launch environment difference the
  investigation surfaced along the way.
- A second, independent bug in the same investigation: `IconResolver`
  only ever tried an icon's exact name, which silently failed for
  standard status-icon names (`network-wireless-signal-excellent` and
  siblings) against a real, currently-installed Adwaita theme - found
  by checking actual rendered pixel data, not by reading the code -
  since Adwaita only ships those under a `-symbolic` suffix. Fixed
  with a same-again-with-a-suffix retry.
- A third finding, investigated but explicitly *not* fixed: loading
  several different real status-icon SVGs in succession, within one
  process, reproducibly corrupts the heap somewhere inside Imlib2's
  own SVG loader, isolated as precisely as reasonably possible (a raw-
  Imlib2, no-Kohiko-code reproduction hits the same crash; librsvg's
  own CLI renders the same files fine standalone) but not resolved -
  fixing a third-party rendering library's own internals was judged
  well outside a "smallest robust fix" for this report, and the crash
  is unconfirmed on the actual target system's library versions. A
  standard, low-risk mitigation (disabling Imlib2's own internal
  cache) was applied anyway, but documented plainly as unproven rather
  than claimed as a fix.
- An unrelated, pre-existing build-system bug was fixed as a
  prerequisite for trusting `make test`'s own output at all: two test
  targets were silently missing `-lXrandr`, so `make test` stopped
  partway through - on any system with XRandr headers present, which
  includes a default Arch install - without the remaining test
  binaries ever running, and without failing loudly enough to make
  that obvious.

**Lessons visible from the repository:**
This phase is a direct, fairly stark answer to the exact gap Phase 11
flagged for itself: "real-display/real-system verification... is still
outstanding." For the tray-specific slice of that gap, actually setting
one up - a real (if headless) Xvfb X server, a real `dbus-daemon
--session`, a real PipeWire/WirePlumber stack with a dummy sink, a
dedicated non-root user, `kohiko-session` itself as the entry point -
surfaced two genuine bugs that a code read alone had not (the autostart
gap was introduced in 0.19.0 and had gone unnoticed for multiple
releases; the icon-name gap likely predates even that). It also
demonstrates the flip side of the same lesson: even with a real X
server and a real backend stack available, some things still can't be
verified without the user's own actual machine (real Wi-Fi/Bluetooth
hardware, a real display manager specifically, the exact `imlib2`/
`librsvg` package versions Arch itself ships) - and the honest response
to hitting that wall was to disclose the gap precisely, not to either
stop short of the fix that was achievable or overstate the fix that
wasn't. Two small, purpose-built unit tests
(`test_desktopentry`/`test_iconresolver`) were added for the two pieces
of newly-added pure logic, following the project's existing pattern of
real-execution tests wherever a dependency permits them, and mock/
review-only where it doesn't - the Imlib2 crash specifically falls into
the latter category, and is recorded as such rather than quietly
dropped.

--------------------------------------------------------------------------

## Phase 14 — Two Real Regressions, Fixed Properly (0.20.2)

**Versions:** 0.20.2

**Goals:**
Fix two problems reported from a real Kohiko 0.20.1 session, explicitly
not as "known limitations" - a window-classification bug that let tray
icon windows get managed like normal application windows, and the
Imlib2 SVG-loading crash risk Phase 13 had disclosed but deliberately
not fixed.

**Major developments:**
- Root cause of the classification bug: `TrayIconClient::TryDock()`
  maps its own window immediately after requesting a dock, without
  waiting for `SystemTray` to reparent it first - a deliberate choice
  (so a tray icon still shows *something* even if the dock request
  goes briefly unanswered) that raced `WindowManager::Manage()`'s
  ordinary `MapRequest` handling, which had no way to recognize a
  window already mid-handshake for embedding elsewhere. Fixed
  generically - `Manage()` now skips any window carrying an
  `_XEMBED_INFO` property (new: `XConnection::IsXEmbedWindow()`), the
  freedesktop XEmbed specification's own required marker for exactly
  this situation, rather than special-casing any particular
  application. `TrayIconClient::Create()` had already been setting
  this property correctly since 0.19.0; nothing on the WM side had
  ever checked it before now. This bug could only ever manifest once a
  tray icon window actually existed on a real session - Phase 13's own
  fix is what first made that true, which is most of why it surfaced
  only in this phase.
- The Imlib2 SVG-loading risk: reproduction of the *original* specific
  crash Phase 13 found failed this time, on the same Imlib2 version,
  despite substantially more instrumented effort (ASan, UBSan, glibc's
  `MALLOC_CHECK_`) than Phase 13 had applied - suggesting it may be
  heap-layout-sensitive rather than strictly deterministic, a real
  limit on how precisely this could be pinned down. A *different*,
  highly reproducible failure was found in its place - rapid creation/
  destruction of many different SVG-sourced Pixmaps through Imlib2's
  API directly produced X11 `BadPixmap` errors on the large majority of
  free attempts, while the same operations through `UiIconCache`'s own
  actual usage pattern (load once, hold, free at the end) produced
  none. Rather than continue chasing an increasingly narrow reproduction
  window, the fix taken was structural: a new module, `SvgRenderer`,
  renders `.svg`/`.svgz` icons directly through librsvg and Cairo,
  bypassing Imlib2's own SVG loader plugin entirely for that one format
  (every other format is untouched). This is not a novel approach -
  it's the same pair of libraries virtually the rest of the Linux
  desktop already renders SVG content through, just without Imlib2's
  own bridging code in between, which is where the demonstrated
  unreliability turned out to live. The unproven 0.20.1 mitigation
  (`imlib_set_cache_size(0)`) was removed rather than kept alongside
  the real fix, per an explicit instruction not to depend on it.
- Two new regression tests (`test_windowclassification`,
  `test_svgrenderer`), both following `test_monitormanager`'s existing
  "real X connection, gracefully skip if none available" pattern - the
  SVG one's stress section specifically reproduces the adversarial
  many-different-SVGs-in-one-process pattern that broke Imlib2's own
  loader, now asserting zero X errors through the new path instead.
- A numbering error from Phase 13's own entry (two phases both titled
  "Phase 12") was corrected while editing this same document.

**Lessons visible from the repository:**
Phase 13 closed a real, reported gap and, for the one part of it that
genuinely couldn't be fixed within that phase's own reasonable scope,
said so plainly instead of either pretending otherwise or quietly
leaving Kohiko exposed to it - and that disclosure is exactly what
made this phase possible to scope correctly: the instruction to "fix
this properly" pointed at a specific, already-documented, already-
partially-investigated problem rather than an open-ended one. The
window-classification bug is a cleaner illustration of a pattern this
project's own history keeps repeating (Phase 10 and Phase 13 both hit
versions of it too): a code path that looks complete by inspection can
still have a real gap that only a previously-impossible runtime
condition ever exercises - here, specifically, that no tray icon
window had ever existed on a real session before Phase 13's own fix
made that possible for the first time. Chasing the exact original
Imlib2 crash to a dead end, and then finding a different, more
reproducible failure in the same subsystem instead of stopping there,
is a useful example of when *not* to keep narrowing down a specific
repro and instead ask the more structural question ("should this code
path be avoided entirely") the surrounding evidence already supported
asking.

--------------------------------------------------------------------------

## Phase 15 — A Wi-Fi Reconnection Bug, and a Kernel Regression That Wasn't Kohiko's (0.20.3)

**Versions:** 0.20.3

**Goals:**
Diagnose a live report of a specific WPA2 network repeatedly failing
to connect, from real `nmcli`/`journalctl` evidence plus (in a related
but separate thread) a real hardware Wi-Fi association failure on a
specific chipset - establishing root cause for each from logs and
source code rather than assumption, and fixing only what was actually
Kohiko's to fix.

**Major developments:**
- The reported handshake failure (`psk mismatch reported by
  supplicant`) reproduced identically via plain `nmcli`, with
  Kohiko's own process confirmed, by PID, not to be the actor behind
  any of the specific failed attempts in the provided logs - so that
  specific failure was correctly not attributed to Kohiko's code, a
  conclusion reached from evidence rather than assumed in Kohiko's
  favor.
- Tracing the relevant code anyway turned up a real, separate,
  verified bug: `NetworkManagerClient::BytesToString()` read a
  Variant-wrapped D-Bus value's `.Items()` without unwrapping it
  first, silently returning `""` instead of a real SSID - confirmed
  by direct execution, not inspection alone, including a before/after
  run of the new regression test against the pre-fix code. The
  practical effect was total: `ConnectToAccessPoint()`'s "is this
  network already saved" check never matched anything, for any
  network, ever - every reconnection to an already-known network
  silently created a new connection profile instead of reusing one,
  and (a necessarily paired second fix, since the first fix alone
  would have made the user-visible symptom worse, not better) a
  freshly-typed password for an existing profile was never actually
  written back to it.
- A separate, real-hardware Wi-Fi association failure (RTL8822BU,
  5GHz specifically, `failed to get tx report from firmware` →
  deauth) was investigated via web research against the actual kernel
  version, firmware version, and dmesg output provided, matched to a
  specific, recent, unresolved upstream regression report (same chip,
  same firmware version, explicitly described as breaking starting at
  kernel 6.18.4 after working on 6.17 and 6.12 LTS), and left
  genuinely open - no confirmed upstream fix exists to point to, and
  none was invented. Neither NetworkManager nor Kohiko were modified
  in relation to it, since neither is where the problem lives.
- A deeper, mocked-D-Bus-service integration test for the full
  `Update()`+`ActivateConnection()` call sequence was attempted but
  not completed (repeated sandbox environment resets interrupted the
  attempt) - the shipped regression test covers `BytesToString()`
  directly and by execution, not the full round trip through a live
  or mocked NetworkManager. This is disclosed rather than implied.

**Lessons visible from the repository:**
The most important thing this phase got right was resisting the pull
toward treating "the user is running my software and hit a bug" as
evidence that the bug is in that software. The provided evidence -
specifically, that the failure reproduced via bare `nmcli` and that
the specific failed attempts in the logs weren't Kohiko's own process
- pointed away from Kohiko for the *headline* symptom, and that
conclusion held up under actually reading the code, rather than being
revised to fit an assumption either way. At the same time, tracing the
relevant code path thoroughly *anyway*, rather than stopping at "not
my bug," is what surfaced a real, independent, previously-unknown
defect in that exact area - a useful reminder that "this isn't the
cause of the specific report in front of me" and "this code is
correct" are different claims, and only investigating enough to
support the first one leaves the second one unverified. The rtw88
thread is a clean example of the flip side of the same discipline:
research turned up a very close match to a real upstream report, but
not a confirmed fix, and the honest answer was to say so plainly and
lay out real trade-offs rather than manufacture a specific version
number or patch to point to.

--------------------------------------------------------------------------

## Phase 16 — An Absolute-Deadline Fix for a Starved Clock, and Two False Premises Declined (0.20.4)

**Versions:** 0.20.4

**Goals:**
Diagnose a live report that the taskbar/clock could go stale until
some unrelated event forced a repaint, from a real screenshot and a
`startx`-launched-session detail, while separately evaluating two
other claims presented alongside the original request against the
actual repository before acting on either of them.

**Major developments:**
- Both accompanying premises were checked directly against the
  repository and found not to hold, before any related code was
  touched: this project has never been a git repository at any point
  in its recorded history, so a `git diff`-based investigation method
  proposed for a suspected Telegram Desktop regression could not have
  been carried out as described; and `Bar.cpp`/`Bar.h`'s existing
  backing-`Pixmap` double-buffering predates this project's entire
  multi-session engagement (present already in the original 0.20.0
  archive), so no session recorded here ever touched those files
  before this phase's own fix below. Both findings were reported
  plainly rather than acted on or silently set aside, and the
  underlying concern - the taskbar genuinely misbehaving - was
  investigated on its own merits regardless of the disputed backstory.
- That investigation found a real, previously-unknown bug:
  `EventLoop::Run()`'s `select()` timeout was reset to a fresh
  interval on every loop iteration regardless of what triggered it, so
  `WindowManager::Tick()` - the only thing driving every bar's
  periodic redraw, the clock chief among what it drives - only ever
  ran when a full second passed with zero file descriptor activity of
  any kind. Ordinary background D-Bus traffic (in particular
  `ScreenSaverInhibitor`'s deliberately bus-wide `NameOwnerChanged`
  subscription, needed so it can clean up after a crashed inhibitor
  automatically) could starve it indefinitely on a real desktop
  session, while every genuine X11 event kept being serviced
  completely normally in the meantime - matching the reported shape
  exactly: workspace and focus changes redrew instantly, since neither
  depends on `Tick()`, but the clock specifically could freeze for a
  long time. Directly demonstrated with a live session under synthetic
  D-Bus churn, not just reasoned about from reading the code: 70+ real
  seconds of continuous churn against the pre-fix code left the clock
  showing the exact same frozen time throughout; the identical test
  against the fix showed the clock advancing correctly by the full
  real elapsed time.
- Fixed by tracking an absolute deadline for the next tick instead of
  resetting a relative timeout every iteration, with the pure
  scheduling logic factored into small, header-only, unit-tested
  functions (`TickSchedule::PullForward()`/`IsDue()`/`TimeUntilDue()`
  in `EventLoop.h`) specifically so the fix's core correctness
  property - other fd activity can delay a due tick by however long it
  takes to service it, but can never postpone it indefinitely - is
  checked independently of a real, blocking, X11-connected event loop.
- Separately, per explicit request, the bar stopped displaying the
  focused window's title. Confirmed first, via direct testing (moving
  the mouse to several positions along the bar produced no text tied
  to cursor position anywhere), that this was an intentional existing
  feature rather than genuine hover text - then removed it in a
  deliberately minimal, reversible way: `Bar::SetTitle()` and
  `m_title` are both left in place, `Redraw()` simply no longer draws
  either.
- The proposed git-diff-based Telegram Desktop investigation method
  itself was never viable (see above), but the underlying "Telegram
  and several other applications look like they fail to open" concern
  was picked back up later the same phase, once the user clarified it
  wasn't about tray icons/XEmbed at all but full application windows -
  ruling out the earlier `IsXEmbedWindow()` hypothesis outright, since
  that early-return only ever applies to a window carrying
  `_XEMBED_INFO`, not an application's actual main window. Investigated
  from `Manage()`'s own logic outward rather than from Telegram
  inward, and found a real, reproducible mechanism: a plain top-level
  window landing on a workspace nothing is currently displaying (via
  `workspace<N>=` autostart, `windowrule=workspace:N`, or a learned
  adaptive-placement habit) stays unmapped and unfocused - correct,
  deliberate, documented behaviour - but with no on-screen signal
  that it happened at all, unlike a transient/dialog, which always
  gets pulled into view. Confirmed directly against the shipped
  `config/default.conf` itself, not a contrived case: it already
  autostarts multiple applications via one shared `workspace2=discord
  Telegram` line, onto a workspace not visible at session start - the
  exact "several different applications, all launching the same way"
  shape originally reported. Reproduced live under Xvfb (a third
  application added to that identical line came up alive per `ps` but
  entirely absent from the visible workspace's screenshot, then
  present and normal on switching to workspace 2) and fixed with a bar
  notification reusing the existing `ShowNotificationOnMonitor()`
  mechanism, re-verified against the same live reproduction afterward.
- A separate, new report the same phase - "the clock is running fast"
  - was investigated and found not to be a Kohiko defect at all:
  `Bar::Redraw()` reads `std::time(nullptr)` fresh on every redraw,
  with no counter or accumulated state anywhere in the path, so the
  displayed value cannot itself drift from whatever the OS clock
  currently reports - confirmed both by reading the code and
  empirically, five live screenshots at 5-second intervals matching
  `date -u` exactly throughout. A "clock runs fast" report almost
  always points at the underlying *system* clock (common in
  virtualized/sandboxed environments under CPU contention), which sits
  entirely outside anything a window manager reads or controls -
  documented as such, with the actual OS-level check (`timedatectl
  status`) pointed to, rather than inventing a Kohiko-side change that
  couldn't address a cause it has no access to.
- A third report the same phase, this time accompanied by a concrete
  screenshot rather than only a description: a BSP layout with two
  windows sharing a short, wide slot under a third stayed side by
  side - now visibly, unnaturally narrow - once closing that third
  window promoted the pair into a much taller freed column.
  Investigated the *insertion* algorithm first, deliberately, before
  assuming that's where the fix belonged: six windows opened one after
  another, live under Xvfb, in both the natural most-recently-focused
  pattern and forcibly re-anchored to one fixed window each time,
  produced sensible, non-degenerate proportions throughout -
  `BSPTree`'s existing insert-time direction heuristic was already
  sound. The actual defect was narrower and specific to
  `BSPTree::Remove()`'s collapse step: promoting a survivor straight
  into its freed slot is exactly right when that survivor is a single
  leaf (both existing Collapse-on-remove tests cover this and still
  pass unmodified), but when the survivor is itself a further-split
  pair, its own internal direction was left completely untouched, even
  though the area it now occupies can be a very different shape than
  whatever it was originally tuned for. Fixed with a new
  placement-aware `Remove()` overload that recursively re-derives
  direction within a promoted subtree, at any depth, against the area
  it actually inherits - reusing the same heuristic fresh inserts
  already use, and the same direction-toggle `Rotate()` already uses -
  while leaving ratios untouched. Reproduced the user's exact reported
  scenario live under Xvfb both before and after the fix (before:
  narrow side-by-side strips; after: stacked, full-width panes,
  confirmed by screenshot), and added 16 new checks covering the exact
  scenario, a direct comparison against the unchanged old overload
  (confirming the fix itself, not incidental test setup, is what
  changes the outcome), ratio preservation across the flip, a
  no-flip-needed case, and a 6-window/2-removal scenario asserting no
  leaf's aspect ratio exceeds 3:1 at any depth.
- A fourth report the same phase, framed broadly ("focus and the mouse
  cursor get out of sync") but with five specific cases named to check:
  opening a new window, switching workspaces, switching monitors,
  rearranging windows, and restoring a session. Investigated each on
  its own, live under Xvfb wherever that was possible, rather than
  assuming all five shared one cause or all five were even genuinely
  broken. One (a new window grabbing focus regardless of pointer
  position) turned out to already be correct, intended policy -
  confirmed directly, not assumed - and was deliberately left alone.
  Three reproduced cleanly: switching workspaces away from and back to
  one without moving the pointer restored whichever window had been
  focused *before* leaving rather than whatever the still-stationary
  pointer was now resting over; a Swap drag left focus on whichever
  window held it before the drag started, regardless of where the two
  swapped windows ended up (`EndSwapDrag()` never touched focus at
  all); and restoring a session after a restart focused whichever
  survived window happened to be last in the root's own child z-order,
  matching neither the pointer nor any saved "was this focused" state
  (nothing records one). The fifth - switching monitors - couldn't be
  reproduced empirically in this sandbox's single-output Xvfb, but code
  reading surfaced something more specific than the other four: since
  Kohiko never warps the pointer, an explicit monitor switch under
  focus-follows-mouse would get silently undone by the very next
  incidental mouse movement on the monitor just switched away from.
  Fixed the first four with one new `WindowManager::SyncFocusToPointer()`,
  called explicitly right after each transition, reusing
  `HandleEnterNotify()`'s own existing guards; fixed the fifth in the
  opposite direction - moving the *pointer* to match an explicit focus
  change via a new `XConnection::WarpPointer()`, rather than moving
  focus to match the pointer, since the latter would have just undone
  the switch. Re-verified all three empirically reproducible fixes live
  afterward, each against an explicit baseline rather than merely "it
  changed to something" - the session-restore check in particular first
  established what the pointer-*less* default outcome was, then showed
  the pointer-aware result differed from it with the identical pointer
  position, to rule out coincidence.
- A fifth report the same phase: a visible rendering glitch during a
  Swap drag's slide-into-place animation, described as a stale-image
  trail. Ordinary floating-window dragging was ruled out first, tested
  directly and thoroughly - both a slow and a rapid many-step drag,
  against both a solid colour and the actual varied wallpaper image
  specifically so any staleness would be impossible to miss visually -
  and came back completely clean every time, which redirected the
  investigation toward the one thing structurally different about a
  Swap: the ~160ms eased slide `Animator` plays after a drop, rather
  than the window just snapping straight to its new tile. Confirmed
  live rather than assumed: catching a 160ms transition needed a rapid
  burst of screenshots fired immediately after the drop rather than a
  single capture after a `sleep` (which would only ever show the
  already-settled end state) - the very first capture in that burst
  caught it, and precise pixel measurement (not just a visual glance)
  confirmed the dragged window's edge was cut off well short of the
  screen edge, with a gap of bare wallpaper neither window was
  covering yet. Root cause: the dragged window's animation starts from
  wherever it visually was under the cursor at the moment of the drop
  - correct and, once traced through, deliberately chosen so the
  transition reads as "it settles where you dropped it" rather than
  teleporting back to its old tile first - but that rect is tracked at
  a fixed grab-offset from the cursor for the whole drag, so it can
  legitimately extend off-screen if grabbed away from its own edge and
  dragged far enough, which is harmless while actually dragging
  (nothing renders past the screen edge regardless) but wrong as an
  animation's own starting point. Fixed with `Rect::ClampedTo()` -
  pre-existing logic, already used elsewhere for floating windows, but
  never covered by a test of its own until this same fix added one -
  applied to that rect before either animation starts, in both of
  `EndSwapDrag()`'s branches (a successful swap, and the no-target
  snap-back) since both draw from the same rect. Re-verified with the
  identical rapid-burst-of-screenshots reproduction afterward, and the
  snap-back branch specifically re-checked too, not just the swap case
  the original report described.
- A sixth report the same phase: startup applications visually
  covering Kohiko's own lock screen. Confirmed live under Xvfb before
  writing any fix, and worse than the report implied - a single plain
  `xterm` opened after locking hid the entire lock surface, not just
  part of it, with nothing left on screen to show the session was even
  still locked. Root cause, found by reading `RaiseModalWindows()`
  (called at the end of every `Arrange()`, itself called by `Manage()`
  before mapping any newly-opened window, autostart included, "to give
  something the last word on stacking order" per its own existing
  comment): it only ever considered Launcher and Notepad, never
  LockScreen, which only ever raised itself once, at the moment of
  locking. Keystroke safety itself was never actually at risk either
  way - `LockScreen::Lock()`'s exclusive `XGrabKeyboard`/`XGrabPointer`
  routes input to it regardless of what's stacked where, a deliberately
  stronger guarantee than the cooperative stacking+focus model
  Launcher/Notepad get away with (see that class's own header comment)
  - but a lock screen a user can't *see* is a real problem independent
  of that. Fixed by having `RaiseModalWindows()` check
  `LockScreen::IsLocked()` first and unconditionally win if so. Verified
  beyond the original report's own scope: re-ran the identical live
  reproduction, then specifically tested the multi-application case the
  report called for (three windows opened one after another while
  locked, lock screen intact throughout all three), and finally tested
  past locking entirely - a real, PAM-authenticated unlock (which
  needed correcting this sandbox's own PAM stack for Ubuntu, a
  pre-documented distro difference in `pam/kohiko` itself, not a Kohiko
  defect) correctly restored full normal stacking and focus, the
  window that had been hidden underneath the whole time appearing
  properly tiled and focused the instant the screen unlocked.
- A seventh item the same phase, framed as verification rather than a
  known bug: the login/autologin architecture, checked end to end
  against its intended shape (display-manager/autologin starts Kohiko
  with no username prompt of its own; Kohiko's own LockScreen gates
  access with a password only) and found already correct - confirmed
  by code reading (no second username prompt anywhere; the one place a
  username gets typed, `kohikoctl configure-autologin`, is a one-time
  administrative command run once with `sudo`, not something an end
  user sees during ordinary login) and then by live end-to-end testing
  through the actual entry point a display manager would invoke
  (`kohiko-session`, not a shortcut), including a real,
  PAM-authenticated unlock completing the flow. That same live testing
  - specifically, testing past where the architecture check alone
  would have stopped - surfaced a real, separate, security-relevant
  bug that had nothing to do with usernames at all: a Kohiko that
  crashed while locked resumed, on `kohiko-session`'s own automatic
  restart, into a completely exposed desktop, confirmed by directly
  locking, then `kill -9`-ing the running process to simulate a crash.
  Root cause: nothing about a restarted process remembers what state
  the previous one was in - `SessionStore` never saved lock state, and
  `RecoveryMode`'s own existing crash marker is deliberately scoped to
  startup alone, cleared long before a much later crash mid-session
  would happen. Fixed with a new `LockRecovery` class, the same shape
  as `RecoveryMode` itself: a marker written on lock and removed on a
  normal unlock (via the lock-state-changed callback `Initialize()`
  already had wired up for an unrelated purpose), checked once at
  startup to decide whether to lock again immediately - independent of
  `lockscreen.after` entirely, since this is safety recovery rather
  than a preference. Verified live in both directions specifically so
  the fix couldn't be mistaken for a blanket "always relock on any
  crash" over-correction: crashing while locked resumes locked (the
  log confirms it explicitly); crashing while unlocked resumes
  unlocked, with nothing spurious.

**Lessons visible from the repository:**
This phase is as much about what wasn't changed as what was. Two
specific, plausible-sounding premises accompanying the original
request - a git-diff-based investigation method, and a "some earlier
session already touched `Bar.cpp` for this" backstory - both failed
direct inspection against the actual repository, and the response
demonstrated here was neither blind compliance nor blanket dismissal:
check the claim against the code, report the discrepancy plainly, and
keep investigating the underlying concern - which was genuine - on its
own merits. That discipline is arguably what surfaced the real bug:
had the "some previous fix broke this" framing been accepted
uncritically, the actual cause - a starvation bug with no prior fix to
have broken anything - would likely have gone unfound entirely. The
empirical churn test is worth calling out on its own too: reasoning
about the bug from reading `EventLoop.cpp` alone would have been
plausible but unconfirmed; actually reproducing the freeze under
synthetic load, then confirming its absence under identical load after
the fix, is what turned a plausible theory into a verified one -
consistent with this project's established pattern (0.20.1's tray
icons, 0.20.2's `BadPixmap`/heap-corruption reproduction, 0.20.3's
`BytesToString()` before/after test) of trusting direct execution over
code review alone wherever a real environment makes that possible. The
same discipline cut two directions on the two reports investigated
later in the phase: it led *toward* a real code change for the
"applications won't open" report (once the original XEmbed hypothesis
was ruled out by the user's own clarification, following the evidence
to a different, genuine mechanism instead of forcing the original
theory to fit), and *away* from one for the "clock runs fast" report,
where the same live-verification standard applied honestly pointed
outside the codebase entirely. Treating "investigate thoroughly" and
"a code change is the correct outcome" as two separate questions,
rather than assuming the second follows automatically from the first,
is what made both calls right instead of just one of them. The BSP
layout report that followed adds a third variant of the same
discipline: not "does this need a fix" (a screenshot already settled
that) but "where, specifically" - checking the *insertion* algorithm
first against six windows opened live under Xvfb before touching any
code, finding it already sound, and only then locating the actual
defect one layer over, in collapse. A less targeted response might
have taken "the BSP layout has a narrowness problem" as licence to
rewrite the insertion heuristic too, on the reasonable-sounding theory
that a "real redesign" ought to touch the whole algorithm - which
would have risked the two already-passing Collapse-on-remove tests and
the six-window live reproduction's own good behaviour for no actual
benefit, since neither was where the reported problem came from. The
focus/mouse report that followed splits five named cases three ways
rather than two: one investigated and found already correct, three
investigated and confirmed broken (fixed with the same mechanism
applied uniformly), and one confirmed broken by reasoning rather than
live reproduction, fixed with the *opposite* mechanism once it became
clear "make focus match the pointer" was actually the wrong direction
for that specific case - a good reminder that a single fix pattern
found to work for most of a family of related reports isn't
guaranteed to be right for all of them, and checking that at each
individual case matters more than the convenience of one uniform rule.

--------------------------------------------------------------------------

## Current Direction

As of 0.20.0, Kohiko adds persistence, recovery, and desktop-session
integration on top of the largely self-contained window manager Phase
11 left off with: monitor/workspace state and adaptive placement
habits both now survive not just a restart but a monitor's own
starting workspace across one; a bad config can no longer make Kohiko
unusable, between automatic migration of missing settings and
automatic recovery from a crash before startup finished; the launcher
notices new/changed/removed applications live; and Kohiko now installs
as a real, autologin-capable desktop session with its native lock
screen properly integrated into `systemd-logind` rather than sitting
beside it as an island - all on the same deliberately small,
still-optional-where-possible set of external dependencies Phase 11
left it with (Imlib2 now load-bearing for the main `kohiko` binary
itself, not just `kohiko-settings`, on account of wallpaper support).

That release flagged its own biggest gap plainly: everything touching
Xft, D-Bus, Imlib2, or a real X server had been verified by code
review and close analogy, not execution. 0.20.1 (Phase 13) closed the
specific slice of that gap a real user actually hit first - tray
autostart and icon loading - by setting up exactly the kind of real
execution environment that gap called for (a real X server, a real
D-Bus session bus, a real PipeWire stack) and finding two genuine bugs
neither code review nor the existing documentation had caught, though
one of the two (an Imlib2 SVG-loading crash risk) was disclosed rather
than fixed at the time. 0.20.2 (Phase 14) fixed that one properly -
bypassing Imlib2's own SVG loader for a direct librsvg/Cairo path
instead - and fixed a second, independent real-session regression the
same real-execution testing style surfaced: tray icon windows being
managed as normal application windows, a race between
`TrayIconClient`'s own docking handshake and the WM's ordinary
`MapRequest` handling that could only ever manifest once a real tray
icon window existed to race against, which 0.20.1 is what first made
true.

The rest of the gap 0.20.0 originally flagged is still not fully
closed: the lock screen's logind integration, wallpaper rendering
itself, and the autologin flow against an actual display manager
remain outside the scope of any of the last three phases (each was a
specific, reported regression, not a general audit) and were not
touched or re-verified in any of them. 0.20.3 (Phase 15) added one
more data point to the same pattern the last two phases already
established - a defect (kohiko-network never actually reusing or
updating a saved Wi-Fi connection's password) that had gone unnoticed
because nothing had traced that exact code path end to end against a
real, reported failure before - while also, in the same investigation,
correctly declining to attribute a real hardware/kernel-level Wi-Fi
failure to Kohiko just because it showed up in the same troubleshooting
session. A few things seem likely to matter next. First,
`Launcher.cpp`'s own, separate Imlib2 usage for app icons
(`DrawIcon()`, not routed through the now-fixed `UiIconCache`/
`SvgRenderer` path) still goes through Imlib2's SVG loader and was
deliberately left untouched in Phase 14 (unrelated WM functionality,
not part of what was reported) - whether it carries the same class of
risk is still an open question. Second, 0.20.3's own regression test
covers `BytesToString()` directly and by execution, but not the full
`Update()`+`ActivateConnection()` round trip through a live or mocked
NetworkManager - an attempt at exactly that (a mocked D-Bus service)
was started but not finished in that phase; picking it back up would
close a real, disclosed gap rather than a hypothetical one. Third,
this project's own "Extension points" section in
`docs/ARCHITECTURE.md` is deliberately source-level, not dynamically
loaded - worth revisiting only if a concrete need for genuine runtime
plugin loading materializes, not preemptively.

0.20.4 (Phase 16) fixed a defect in the same family as the last three
phases' - a real bug (`Tick()`'s scheduling starving under ordinary
background D-Bus traffic) that had gone unnoticed because nothing had
exercised the event loop under realistic load before, only in a
comparatively quiet sandbox - while also, twice in the same phase,
declining to act on a plausible-sounding premise that didn't survive
checking against the actual repository. Several items remain open
after this phase, carried forward rather than resolved: the
git-diff-based Telegram Desktop investigation was never actually
carried out (the git-repository premise it was framed around doesn't
hold, but the underlying question - whether 0.20.2's XEmbed
early-return interacts with Qt's own system-tray-icon usage - is
still real and still needs actual evidence from an affected machine,
not further reasoning from this repository alone); `Launcher.cpp`'s
own, separate Imlib2 usage for app icons (`DrawIcon()`, not routed
through the `UiIconCache`/`SvgRenderer` path 0.20.2 fixed) remains an
open question first raised in Phase 14 and still not investigated;
and 0.20.3's incomplete mocked-D-Bus-service integration test for
`NetworkManagerClient`'s full `Update()`+`ActivateConnection()` round
trip is still just started, not finished. None of these three are
new to this phase - they're listed here specifically so they don't
quietly drop out of view the way an unrepeated to-do item can.

Two more open items from later in the same phase, for the same
reason. First, the monitor-switch half of the focus/mouse consistency
fix (`FocusMonitorCommand()` warping the pointer onto the
newly-focused monitor under `general.focus_follows_mouse`) is verified
by code reading and by the fact that it builds and the rest of the
suite still passes, not by actually watching it happen - this
sandbox's Xvfb only ever exposes a single RandR output, so a genuine
two-monitor `focusmonitor left`/`right` switch has never actually been
run. The other four focus/mouse cases (and the BSP collapse fix
before them) were all confirmed by watching them happen, with an
explicit before/after or baseline comparison each time; this one
wasn't, purely on account of the environment, and is worth an explicit
live check the next time a real or better-emulated multi-monitor
session is available, rather than resting on reasoning alone
indefinitely. Second, the placement-aware `BSPTree::Remove()` overload
only re-derives direction *within* the promoted subtree, deliberately
never touching the grandparent split it gets promoted into (preserving
the existing "everything above the grandparent is left completely
untouched" guarantee the two original Collapse-on-remove tests already
lock in) - whether a grandparent's *own* direction could, in some
tree shape not yet hit in practice, also end up poorly suited to
what's now on either side of it after a promotion is a real, if
narrower, question the same fix deliberately didn't attempt to answer.

Continuing the same phase into the next round of reported work, the
first item handed over was framed as "Task 1: BSP rewrite" - and
turned out to be a third instance of the pattern the two items above
already established, checked the same way before any code was
touched: `CHANGELOG.md`'s own 0.20.4 entry already describes the
narrow-strip collapse defect as fixed, with a targeted `Remove()`
overload rather than a rewrite, and 82/82 checks in `test_bsptree`
(rebuilt and run directly from the restored source, not assumed from
the changelog's own claim) confirmed that's still true in this exact
checkpoint. A "rewrite" would have put 16 checks specifically written
to lock in that fix at risk for a problem that no longer exists;
asked directly rather than guessed at, the answer was to skip it and
move on to the next item, which is what happened.

That next item - kohiko-audio's GUI responsiveness - is where testing
several real window sizes, as asked for rather than skipped, actually
earned its keep: the layout/reflow work itself (from 0.19.1/0.20.0)
held up cleanly at every size tried, no overlap or clipping anywhere,
but scrolling a device list short enough to overflow turned out to be
close to unusable, for a reason the original report never
mentioned and that only showed up by actually resizing a live window
with real PipeWire nodes behind it rather than the empty "no devices"
state. See the CHANGELOG entry for the full root-cause chain (debug
instrumentation added temporarily to confirm it, not just reasoned
about from reading `HitTest()`) and the fix. Two things about it are
worth carrying forward rather than letting drop. First, `ScrollView`
is shared by kohiko-network's and kohiko-bluetooth's own lists, so
this same fix already applies to both the next time either is built -
confirmed only as far as a clean launch under Xvfb with no crash after
the change, not with the kind of dedicated resize/scroll testing
Tasks 7/8 still owe each app individually with its own real backend
data (mocked NetworkManager, mocked BlueZ). Second, a real but
separate gap turned up in the same investigation and was deliberately
left alone rather than folded in: long device/network names are
hard-clipped with no ellipsis, but the primitive responsible
(`UiWindow::DrawTextClipped()`) is shared across every widget in every
one of these apps, not something specific to audio's own layout -
fixing it properly belongs as its own change, not scope creep into a
task framed around resizing.

kohiko-network's own turn - GUI responsiveness plus the actual Wi-Fi
credential behaviour - repeated the "numeric-only password field"
claim from the handoff almost verbatim, and it was checked directly
against the restored source again rather than taken on faith, exactly
as it already had been once before this same version (see this
version's CHANGELOG entry): still false. What the report was actually
describing turned out to be one level up from the field itself -
`NetworkWindow.cpp`'s connect handler called the password prompt for
every secured access point unconditionally, with nothing checking
whether NetworkManager could already supply a secret for that SSID,
so the field being perfectly capable of accepting a real password was
beside the point when the user had to sit through the prompt every
time regardless. The fix needed a way to ask NetworkManager that
question directly - `GetSecrets()`, deliberately a separate,
permission-gated call from `GetSettings()` in NetworkManager's own
D-Bus API - and verifying it actually worked needed more than reading
the code: a small mock NetworkManager D-Bus service
(`tests/mock_networkmanager.py`) was written from scratch, running on
a private throwaway system bus a real `NetworkManagerClient` could
talk to for real, with a saved network that has a working secret, one
that doesn't, and a brand new one. It caught a real bug in itself
within the first few minutes - a `dbus.service.BusName` handle
constructed but never assigned to anything gets garbage-collected
immediately, silently releasing the service name before a single call
could arrive - which is worth remembering the shape of: the failure
looked exactly like the fix under test not working, and would have
been easy to misdiagnose as such rather than as a bug in the test
harness. Once fixed, the same mock also stood in for live GUI
testing - real fake access points, a real `xclip`-owned CLIPBOARD
selection to paste from - since the "must also accept pasted text"
half of the request turned out to name a feature that plainly did not
exist at all (no `SelectionNotify` handling anywhere), not a
restriction to loosen. Confirming *that* fix meant watching the actual
masked-character count in a screenshot change by exactly the length of
what was pasted, both from Ctrl+V and from a middle click - the kind
of check that only means something once you've also confirmed a
different point (a Slider or Button) produces no change, which is
what most of the equivalent audio work the task before this one was
about besides.

kohiko-bluetooth's own responsiveness pass closed out the run of
three, and - after a BSP task that turned out not to need a rewrite,
an audio task that found a real scroll-routing bug, and a network task
that found a real credential-prompting bug - came back clean. That
outcome deserved the same live-testing effort as the other two rather
than less: a third mock service (`mock_bluez.py`, following BlueZ's
own ObjectManager-based `GetManagedObjects()` shape rather than
NetworkManager's per-object Properties calls, since that's genuinely
how the two D-Bus APIs differ), real devices with a deliberately long
name, five window sizes including one short enough to force scrolling.
Nothing overlapped, nothing clipped, nothing sat in unexplained empty
space, and the scroll-routing fix from two tasks earlier - built and
tested against kohiko-audio's own device list, never against a
Bluetooth-shaped one specifically - turned out to already cover this
app too, confirmed rather than assumed. Reporting "this already works"
honestly, backed by the same live evidence a real bug would have
needed, is a different kind of finding than the two before it, not a
lesser one - manufacturing a fix here to have something to report
would have repeated the exact mistake the BSP task's own handoff
almost caused two tasks ago. The one thing worth changing wasn't a bug
at all: the details panel's height was computed twice, once to size
its card ahead of time and once implicitly by the sequence of widgets
actually built into it, agreeing today with nothing enforcing that
they keep agreeing tomorrow. Collapsing that into one function both
call, checked against a pixel-diff of the live screenshot before and
after to confirm the change genuinely changed nothing a user could
see, is the kind of change worth making even when nothing is currently
wrong - the same reasoning as writing a regression test for a fix,
aimed one step earlier, at a duplication that has the *shape* of a
future bug rather than an actual one yet.

The Telegram investigation was a different kind of task from the six
before it in this same version - not a GUI-responsiveness pass through
one of the three new apps, but a real window-manager bug, and the
CHANGELOG's own 0.20.3 entry had already half-solved it before this
phase began, in the sense that mattered most: it named
`IsXEmbedWindow()`'s early return as the specific suspect and recorded,
honestly, that it couldn't be checked without a real machine to run a
window manager on. That's exactly what this phase had that the last
one didn't. Confirming the suspicion turned out to be the easy half -
a small real X11 client, one property added or not, told the story in
two screenshots. The harder half was the same question this whole
version keeps circling back to in different clothes: once you know
*why* a check exists, is removing it actually safe, or does it just
trade one bug for the one it was written to prevent? Reading
`SystemTray.cpp` answered that a check on `_XEMBED_INFO` alone was
never really testing the right thing - the real signal was always the
separate `SYSTEM_TRAY_REQUEST_DOCK` message, arriving on its own
schedule - but proving a *timeout* was the right shape of fix, rather
than just deleting the check and hoping, meant deliberately breaking
things on purpose: disabling the check, watching a genuine tray icon
get tiled before it got docked, in a log, in black and white. That's
the same discipline as the BSP task's 82 passing checks or the
scroll-routing fix's before/after screenshots, pointed at a question
those techniques don't usually have to answer - not "does my fix work"
but "am I confident I understand what the code I'm about to stop
relying on was actually doing." A test client bug (closing itself on
any `ClientMessage`, not just the one that meant "please close") made
a working fix look broken for a few minutes before the log line under
it - `DockIcon()` sending a real `XEMBED_EMBEDDED_NOTIFY` - explained
why. Worth remembering: a "the fix isn't working" moment is sometimes
about the fix, and sometimes about the thing watching it.

The next task handed over against this same restored checkpoint was a
live-production report, not a code-review finding: idle `kohiko`
pinned one thread at ~94% CPU, with a specific `strace` command
already run against it and its output already in hand - a continuous
`inotify_rm_watch()`/`inotify_add_watch()`/`openat()`/`close()` cycle
against the wallpaper directory, 15-25 times a second, the watch
descriptor incrementing every cycle. That's a different starting point
from every regression this version had chased so far, closer to the
`Tick()`-starvation bug's own shape than to the "does this
plausible-sounding premise actually hold" pattern the two declined BSP
items followed - a mechanism to find, not a claim to check - and it
was treated that way: before touching `WallpaperManager.cpp` at all, a
standalone C reproducer confirmed directly against the kernel (not
assumed from `inotify(7)`'s wording, though it agrees) that
`inotify_rm_watch()` alone generates a real, readable `IN_IGNORED`
event on the same fd, with a fresh, incrementing watch descriptor from
each subsequent `inotify_add_watch()` - the exact signature already in
the strace. From there the loop read directly out of
`WallpaperManager.cpp`: `ApplyToRoot()` called `RefreshWatches()`
unconditionally on every invocation, including ones `Poll()` itself
had just triggered, and `RefreshWatches()` tore down and rebuilt every
watch unconditionally on every call regardless of whether anything
about what needed watching had actually changed - so a `Poll()`
misreading its own watch-teardown notification as a real file change
fed directly back into another teardown, closed, needing no actual
filesystem activity to sustain itself once started. Fixed the same way
the `Tick()` regression was: not by adding a delay or disabling the
mechanism the bug lived in, but by making the two places actually
responsible - `RefreshWatches()` unconditionally churning watches, and
`Poll()` not distinguishing `IN_IGNORED` from a real content event -
correct on their own terms, with `AppDirWatcher`'s own established
"watch the containing directory, install once, leave alone" pattern
(already the model `WallpaperManager` was supposed to be following,
per this project's own Phase 12 notes) as the reference rather than a
new invention. Confirmed four separate ways rather than any one taken
as sufficient alone: a standalone kernel-level reproducer first: a
regression test built to fail against the original logic before it
was allowed to pass against the fix (run both ways, not just the one
that was expected to succeed, exactly the same discipline as the
XEmbed timeout task's deliberately-broken-first check two tasks
earlier); the entire existing suite re-run clean afterward, nothing
elsewhere disturbed; and a live install under this sandbox's Xvfb,
`kohiko` actually running from `/usr/local/bin` against the real
`/usr/local/share/kohiko/wallpapers` path the report named, idle CPU
watched flat over repeated sampling windows and the reporter's own
exact `strace` command re-run against it producing zero matching
output where it had previously shown a continuous loop - then, for a
direct before/after rather than an inferred one, the original
unmodified source rebuilt and reinstalled in the same live session and
shown to reproduce actual watch-descriptor churn before the fix was
restored. A real wallpaper file replaced under the running process
(both a write-then-`rename()` and an in-place edit) confirmed the one
property most worth losing sight of while chasing a CPU regression:
the fix cannot just make the loop stop, it has to leave the feature
the loop was supposed to be serving still working, and both still
triggered exactly one re-render each, watched directly in the same
strace output, with the watch itself left untouched throughout.

The task after that was broader and open-ended by design: a full
performance audit of the entire running desktop, not a single
reported symptom - "profile everything, rank what you find by
measured cost, fix the real bottlenecks one at a time, prove each one
with a before/after measurement, and don't touch anything a
measurement didn't justify." That last clause turned out to matter
more than it might have read on first pass. A live session was built
the same way the wallpaper task's had been - Xvfb, a real installed
`kohiko`, this time also two private D-Bus buses with the existing
`mock_networkmanager.py`/`mock_bluez.py` test harnesses actually
running behind them, real PipeWire, real X11 client windows tiled by
the BSP - and profiled with whatever tools the sandbox actually had:
`strace -c`/`-f -y`, `pidstat -t`, raw `/proc/[pid]/stat` tick
sampling. `perf` turned out to not be an option at all - this
sandbox's kernel has no matching package available anywhere - which
meant leaning on syscall-level and tick-level evidence throughout
rather than a profiler's call graph, and saying so plainly rather than
quietly working around it. The sandbox itself rebooted mid-session
five separate times before this task was done (every background
process lost each time, the filesystem and installed packages
untouched) - enough that a small idempotent recovery script earned its
keep the same way `bringup`-style scripts have elsewhere in this
project's history, and enough that some planned live scenarios (a
literal mouse-drag window-move, chiefly) never got a fully reliable
reproduction in the time available and are recorded as exactly that -
an unverified item, not a quietly-assumed pass - rather than papered
over.

Almost everything measured turned out to already be efficient.
Workspace switching, focus cycling, window creation and close, a
keyboard-driven BSP move, the launcher's first open (background-
thread app-index build, already an intentional design choice, not
something this session added), settings and lock triggering, and
launching the network/Bluetooth tray tools against their real mock
backends - every one of these cost roughly what the action it performs
should cost, with nothing recurring or unconditional found underneath
any of them. That's a real finding in its own right, in the same
spirit as the BSP-rewrite and generic-drag-and-drop items declined
earlier in this same version: absence of a bottleneck, checked for
directly rather than assumed, is worth writing down with the same
confidence as one that was found. Exactly one genuine, measured,
*recurring* cost turned up anywhere in the whole audit: `Bar::Redraw()`
repainting the entire bar - every workspace label, both mode
indicators, the tray, the power button, the background fill, and the
clock - in full, and flushing it to the X server, on every single
`Tick()`, once a second, forever, for the entire life of the process,
almost always to update nothing but the clock's own seconds digit.
`strace -f -y` on a genuinely idle running session showed it directly:
the same `poll`/`writev`/`recvmsg` shape once a second, indefinitely,
needing no user interaction at all to keep happening - the same kind
of "the mechanism sustains itself with nothing external required"
signature the wallpaper regression had, just two orders of magnitude
cheaper per cycle and entirely intentional-looking rather than a
feedback loop, which is exactly why it had gone unnoticed rather than
reported: nothing about it looked broken from a user's seat, only from
underneath.

The fix followed the same discipline as everything else this session:
smallest change that addresses the actual measured cause,
everything else byte-for-byte unchanged. `Redraw()` now compares the
bar's current state against what it last actually painted - workspace
count and which one's active, the two mode indicators, the
notification text, the tray's width, and the clock text's own pixel
width - and when only the clock differs, repaints just the clock's
rectangle instead of the whole bar. Two correctness edges got the same
"prove it, don't assume it" treatment as the rest of this project's
history: `Show()` needed to force a full repaint on its very next
`Redraw()`, because X11 doesn't guarantee a plain window's prior
pixels survive being unmapped and remapped even though the backing
pixmap itself is untouched by that - and `Configure()` needed the same,
since a resolution change or hotplug moves the goalposts the cached
state was compared against. Both were worked out by tracing what X11
actually guarantees rather than by a failure report, the same way the
inotify mechanism itself was worked out two tasks ago rather than
guessed at. Verified four ways, matching the same "more than one kind
of evidence, not just the one that was expected to succeed" discipline
as the two tasks before it: a dedicated regression test
(`tests/test_bar.cpp`) that measures X11 request counts directly via
`XNextRequest()`'s own sequence counter rather than peeking at private
state, and specifically proves both that the cheap path engages *and*
that a real change - including the `Show()`/`Configure()` edges above -
still forces a full repaint every time; the entire existing suite
re-run clean on both the Makefile and, for the first time this
session, a from-scratch CMake build (`ctest`, 27/27); a live
functional pass against the fix installed for real - workspace
switching, focus cycling, a BSP move, a window close with real BSP
retiling, launcher open/close, and a lock trigger, all watched
directly rather than assumed unaffected; and the same live idle
session re-measured afterward, where the exact same `strace -f -y`
capture that had shown 836 bytes flushed every second before the fix
showed 368 bytes after it - real, on the same running process, not
estimated. Raw CPU-tick sampling couldn't tell before from after here
- both were already below what 10ms ticks on a single core can
resolve at true idle - and that gap between "the underlying X11 work
measurably dropped" and "the crude CPU counter available in this
sandbox couldn't see it" is itself worth being explicit about rather
than rounding up to a CPU-percentage claim the measurements don't
actually support.

One adjacent thing was measured and deliberately left alone:
`SystemTray::Reposition()` unconditionally issues its own
`XMoveResizeWindow` every time it's called, the identical
regardless-of-whether-anything-changed shape `Bar::Redraw()` had - but
`Redraw()`'s fix already removes its own once-a-second call into it
whenever the tray's width hasn't changed, which was the actual
measured, unconditional, forever-running cost. What's left calls it
only from genuine dock/undock events, not a recurring tick - and
fixing it anyway, with no measurement showing it currently costs
anything, would have been exactly the kind of "looks expensive so fix
it anyway" work this task's own instructions ruled out from the
start.

The Bar fix wasn't quite the end of it. One thread left loose while
tracing it - a `mmap`/`munmap` pattern noticed during a workspace
switch but not yet explained - turned out to lead somewhere real once
followed all the way through, rather than being noted and dropped. A
workspace switch, even one where neither the old nor the new workspace
had its own `wallpaper.workspace=` rule, was mmap-ing the wallpaper
PNG twice and allocating (then immediately freeing) an 8MB-class XSHM
buffer, every single time - `WallpaperManager::ApplyToRoot()`
re-rendering the entire composite unconditionally, the identical
shape of waste `Bar::Redraw()` had, just discovered second rather than
ranked first going in. This session's own instructions had drawn a
clear line around the wallpaper subsystem - the 0.20.5 inotify
regression was explicitly someone else's fixed, closed problem, not to
be reopened - but they'd also drawn the line in exactly the right
place to allow this: not the same mechanism, not the same bug, an
independent measurement finding an unrelated cost in the same file.
Reopening `WallpaperManager.cpp` under that specific permission, and
only that permission, mattered enough to state plainly rather than
blur - the alternative was either ignoring a real, measured finding
because it lived in a file with history attached, or touching that
file on weaker grounds than the instructions actually allowed.

The fix mirrored `Bar::Redraw()`'s almost exactly: compare what
`ResolveFor()` says now against what was last actually composited -
path, mode, geometry, background color, all of it - and skip the
decode/render when nothing differs. But this one had a sharper edge
the Bar fix didn't: `HandleWallpaperFileChanged()` - the *other* half
of the 0.20.5 fix, still completely untouched itself - calls
`ApplyToRoot()` precisely when `Poll()` has detected a real on-disk
edit, and a real on-disk edit doesn't change the *path* ResolveFor()
returns, only what's actually in the file at that path. A same-state
comparison would have looked at that call and seen nothing different,
and quietly broken live-reload - the exact feature the previous
session had gone to such lengths to prove still worked. Catching this
before it shipped rather than after came from the same habit as
everywhere else in this project's history: trace what a change
actually touches, in this case by reading `HandleWallpaperFileChanged()`
itself rather than assuming the render path's callers were
interchangeable, before assuming a fix is complete. The answer was a
`forceRerender` parameter, default false, with exactly one caller ever
passing true - explicit at the one call site that needs it rather than
something implicit a future change could silently lose.

Verified the same four ways again: a dedicated test section added to
the existing `test_wallpapermanager.cpp` (gracefully skipped without a
display, like the file's other X11-optional sections, rather than a
new standalone target, since this file - unlike `test_bar` - is
already wired into the unconditional `make test` run) measuring X11
request counts through the same `XNextRequest()` technique as the Bar
test: full render around 13-14 requests, the skip path 1, a genuine
wallpaper change and `forceRerender=true` both back to full-render
magnitude every time; the entire suite re-run clean on both build
systems again; live confirmation that a workspace switch between two
workspaces sharing a wallpaper dropped from 6 mmap/munmap calls to
zero, using the identical `strace -f -y` capture that had found the
problem in the first place; and, most carefully, both live-reload
directions checked by hand rather than inferred from the test alone -
an actual `wallpaper.workspace=` rule still renders the right image on
switching to that workspace, and editing the wallpaper file in place
still triggers a full re-render, confirmed by the mmap'd byte count
matching the new file's own size rather than assuming a fresh decode
happened just because something got redrawn.

The performance-audit checkpoint went out, and a live report came
back within the day - screenshots, not a description: tray icons and
the active-workspace highlight in the bar rendering "only when
affected by something... and only sometimes when they want."
Someone had actually built 0.20.6 and run it. That's a different kind
of signal than anything caught during the audit itself, and it
deserved being treated as more trustworthy than any measurement taken
in the sandbox that produced the fix - a real desktop, used for real,
had found something the whole verification pass had missed.

The two symptoms in the screenshots - frozen tray icons, an
active-workspace number never highlighted - turned out to be one
mechanism, and re-reading `Bar::Redraw()`'s own call sites in
`WindowManager.cpp` found it directly rather than needing to guess
from the screenshots alone: `HandleExpose()` still called `Redraw()`
plain, with no way to bypass the state-comparison fast path the
previous entry had just added. An `Expose` event doesn't mean the
bar's *logical* state changed - it means some region of the window
was just uncovered (most commonly something had briefly overlapped
it) and X11 makes no promise about what's left behind in that region.
`Show()` and `Configure()` had already gotten this exact reasoning
right two entries ago; this one call site was the gap. On a tick where
nothing logical had changed - the ordinary case - the fast path
correctly-by-its-own-rules touched only the clock, leaving whatever
the Expose event had actually exposed (which, on a bar, is most of
it) stale until something unrelated forced a full repaint. "Only
rendering when affected by something, only sometimes" describes that
mechanism almost exactly once you know to look for it.

The fix followed the same shape as `ApplyToRoot()`'s `forceRerender`
from the same session: a `forceFullRepaint` parameter on `Redraw()`,
default false, every existing call site untouched, with
`HandleExpose()` as the one caller passing true. Verifying it asked
for something the earlier `test_bar.cpp` checks hadn't needed -
request *counts* prove work happened, not that the work was *correct*
- so the new check reads real pixels back off the live window with
`XGetImage`: paint an obviously-wrong color directly onto the bar's
window (not the backing pixmap - simulating exactly what real Expose
damage leaves, since the pixmap itself is never touched by a window
being overlapped), confirm an ordinary `Redraw()` genuinely leaves it
damaged - proving the bug's mechanism stood up on its own before
claiming the fix - then confirm `Redraw(forceFullRepaint=true)`
reliably restores the real, correct pixel. One honest gap: a live
end-to-end reproduction, overlapping an actual window over the
running bar and moving it away, wasn't achieved in this sandbox -
Kohiko's own BSP tiling keeps ordinary windows from ever overlapping
the bar's reserved space, so a genuine live repro would need the lock
screen or a bar-adjacent popup specifically. Recorded as unverified
live end-to-end rather than folded into "verified" alongside the
pixel-level mechanism test, which does stand on its own.

The Expose fix landed, and a follow-up came back with something more
precise than the first report: the workspace indicator was fixed, but
the tray icons weren't, and - stated explicitly, worth taking at face
value rather than re-litigating - this wasn't another `Bar::Redraw()`
problem. That framing turned out to matter: it redirected the
investigation immediately toward the tray-specific code rather than
back into a file that had already been gone over twice. The next
detail mattered even more: third-party tray icons rendered fine, only
Kohiko's own three tray tools didn't. Since `SystemTray.cpp`'s XEmbed
docking treats every icon identically regardless of which process owns
it, that single fact ruled out the host side of the mechanism entirely
and pointed straight at the one thing genuinely different about
Kohiko's own tools: they all draw through the same shared
`TrayIconClient`, looking up freedesktop-standard status-icon names
through `UiIconCache`/`IconResolver` - something no third-party tray
icon depends on.

A live screenshot, cropped and enlarged, showed the actual bug
directly rather than left to describe secondhand: plain dark squares,
no glyph, nothing wrong with the icon *windows* themselves - they were
correctly sized, positioned, and docked, just empty. Rather than guess
at why, a standalone diagnostic linked straight against `IconResolver`
and asked it, one name at a time, whether any of the fourteen icon
names the three tray tools use actually resolved in this sandbox. None
did. Forcing a real theme (`Humanity`, confirmed present on disk with
exactly these files) made every one resolve instantly - which mattered
as much as the negative result, since it ruled out a bug in the
resolution algorithm itself and pinned this on `hicolor` (the required
fallback, and the *only* fallback `IconResolver::DetectSystemThemeName()`
checks for, since it only ever reads one specific GTK settings file)
being close to empty of actual status icons. That's a shape of failure
particularly likely for Kohiko's own actual users - people running a
bare window manager rather than a full desktop environment, who would
have no particular reason to have GTK configured at all.

`UiIconCache::Get()` returning false is documented, expected behavior,
not itself the bug - the bug was that none of the three tray tools had
anything to fall back to when it did. The fix mirrored `Bar.cpp`'s own
established pattern for "this can't be found, don't leave it visibly
broken" - the same reasoning behind `[Power]`/`[S]`/`[N]` being drawn
as plain text rather than left unrendered when a symbol might not be
in the loaded font - extended into a small `TrayIconClient::DrawText()`
and a short, always-rendering glyph per tray tool, varying with the
same state each tool already computes for its (possibly unresolvable)
icon name.

Building the regression test the fix was asked for - one that reads
real pixels back from a real window rather than only asserting a
function got called, matching this project's usual standard for
rendering-correctness claims - surfaced something else entirely: a
`TrayIconClient` that gets cleanly destroyed (a test doing exactly
that, rather than the real tray tools' own always-killed-abruptly
lifecycle) segfaults, in `Font`'s own destructor, using a display
connection `TrayIconClient` had already closed one line earlier. Nobody
had built a test that cleanly tore one down before, so nothing had ever
hit it. Fixed in the same change, for the same reason the earlier
`forceRerender`/`forceFullRepaint` parameters exist: trace what a
change actually touches - here, exactly what runs during destruction
and in what order - rather than assume a destructor that had simply
never been exercised was fine.

--------------------------------------------------------------------------

