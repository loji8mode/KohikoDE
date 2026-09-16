# Kohiko Project History

This document traces the evolution of Kohiko, a C++20 / X11 tiling window
manager, across its released versions from 0.1.0 through 0.20.0. It is
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

Two things seem likely to matter next. First, this phase's own testing
gap - everything touching Xft, D-Bus, Imlib2, or a real X server was
verified by code review and close analogy to already-proven patterns,
not execution - means real-display/real-system verification of this
entire phase (window manager startup, the lock screen's new logind
integration, wallpaper rendering itself, the autologin flow against an
actual display manager) is still outstanding, the same category of gap
Phase 9/10/11 each flagged for whatever they couldn't exercise in
their own environment. Second, this phase's own "Extension points"
section in `docs/ARCHITECTURE.md` is deliberately source-level, not
dynamically loaded - worth revisiting only if a concrete need for
genuine runtime plugin loading materializes, not preemptively.

--------------------------------------------------------------------------

