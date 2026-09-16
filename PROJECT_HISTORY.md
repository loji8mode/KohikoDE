# Kohiko Project History

This document traces the evolution of Kohiko, a C++20 / X11 tiling window
manager, across its released versions from 0.1.0 through 0.19.0. It is
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

## Current Direction

As of 0.19.0, Kohiko presents itself as a largely self-contained X11
tiling window manager and minimal desktop session: its own bar, native
launcher and notepad, native lock screen with both Suspend-triggered and
idle-timeout automatic locking, a power menu, session restore that now
tracks BSP position as well as workspace/monitor/floating state, an
adaptive placement system that learns per-application habits during
ordinary use, multi-monitor support, standards-based display-sleep
inhibition, a native settings GUI with structured editors for its
two most syntax-heavy repeatable directives, and now native audio,
network, and Bluetooth applications with matching tray widgets built on
PipeWire/NetworkManager/BlueZ - all on a deliberately small and
still-optional-where-possible set of external dependencies (Xlib,
optionally XRandr/XScreenSaver/D-Bus, Imlib2, Xft/fontconfig, libpam,
and now libdbus-1/libpipewire-0.3 together for the three new apps
specifically - GTK3 was removed in 0.15.0).

The README's "Planned" section was empty going into this release, and
this phase's own scope (a fully-specified feature request rather than
something drawn from that list) is itself a data point: with the core
tiling/session/settings surface considered largely settled (per Phase
9's own observation), new scope is now arriving as complete, externally-
defined feature areas - "add native desktop integration" - rather than
emerging organically from friction with what already exists. Whether
that pattern continues or Kohiko returns to incrementally deepening its
existing surface is the open question this release doesn't answer by
itself.
