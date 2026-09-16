# `kohiko-audio` / `kohiko-network` / `kohiko-bluetooth`: design notes and status

This document covers the three newest companion apps specifically -
what their current design is, what changed to get there, and what's
left. For how they fit into the project as a whole (the shared UI
toolkit, the process model, configuration, rendering), see
[docs/ARCHITECTURE.md](ARCHITECTURE.md); that document is the one to
keep current as further changes land, this one is closer to a
snapshot of "what the 0.20.0 redesign did and where it left things."

## Tray widget autostart (this session's fix)

The three sections above are entirely about the earlier UI redesign of
the standalone `kohiko-audio`/`kohiko-network`/`kohiko-bluetooth`
apps' own windows. This section is about a separate, later fix: **the
audio/network/Bluetooth *tray widgets* (`kohiko-audio-tray` etc.)
never actually launched in a real Kohiko session**, despite the
0.19.0 CHANGELOG entry that introduced them describing them as
"autostarted by default." That claim wasn't tested against a real
session - see the "Developed and tested entirely in a sandboxed
container... no window manager" limitation below, which predates this
fix and is exactly why the gap went unnoticed.

### The actual bug

`desktop/kohiko-*-tray.desktop` were (correctly) installed to
`/etc/xdg/autostart/`, but nothing in Kohiko ever read that directory.
Kohiko's own `auto_start_programs=`/`workspace<N>=` mechanism
(`WindowManager::RunAutostart()`) only ever parsed those two config
keys - it never scanned `/etc/xdg/autostart` or `~/.config/autostart`,
and nothing else in the codebase (`AppDirWatcher`, `AppIndex`,
`Xdg::ApplicationDirs()`) covers that directory either; those all
cover `$XDG_DATA_DIRS/applications` for the launcher's own index, a
different spec, different variable, different subdirectory. Kohiko has
no session manager of its own (`gnome-session`, `xfce4-session`, `dex`,
...) to fall back on, so on a real Kohiko session - launched by a
display manager or via `startx`/`~/.xinitrc`, made no difference - the
three tray processes were simply never started. This wasn't an X11
system-tray-hosting bug, a D-Bus bug, or a process-crash bug:
`SystemTray.cpp`/`TrayIconClient.cpp` and all three
`tools/kohiko-*-tray.cpp` were, and remain, unmodified and correct;
they just never ran.

**Fix**: `WindowManager::RunAutostart()` now also scans
`Xdg::AutostartDirs()` (`$XDG_CONFIG_HOME/autostart` then
`$XDG_CONFIG_DIRS/autostart`, `/etc/xdg/autostart` by default) and
spawns whatever `ShouldAutostart()` (`DesktopEntry.h`/`.cpp`) accepts -
see the [Autostart](../README.md#xdg-autostart-desktop-entries)
section of the README for the user-facing behavior. Also: Kohiko now
sets `$XDG_CURRENT_DESKTOP=Kohiko` at startup if nothing already has
(`Application::Run()`), so `OnlyShowIn=`/`NotShowIn=` on any autostart
entry behaves the same way under a display manager and under a manual
`startx` launch, instead of silently differing between the two -
found while investigating this bug specifically because it's an
"environment differs between a DM session and a manual one" issue in
the same spirit as the main bug, even though none of Kohiko's own
three tray `.desktop` files happen to set those keys.

### Real-session testing actually performed

Unlike the 0.20.0 redesign work below, this fix *was* tested against
something much closer to a real session, not just a sandboxed
container with no display: Xvfb (a real, if headless, X11 server - not
a mock), a real `dbus-daemon --session`, and a real PipeWire +
WirePlumber + `pipewire-pulse` stack (with a dummy sink, since the
container has no audio hardware), all running as a dedicated non-root
user, with `kohiko-session` (the actual script a display manager or
`~/.xinitrc` would run) as the entry point rather than invoking
`kohiko` directly. This is still not the user's own real Arch Linux
machine, and that gap matters most for exactly the two things a
headless container can't provide: real Wi-Fi/Bluetooth hardware and a
real display's visual proofreading. Specifically confirmed:

- **A pristine pre-fix build, against the exact same installed
  `/etc/xdg/autostart` entries, launches zero tray processes** -
  direct, reproduced confirmation of the bug as reported, not just a
  read of the code.
- The fixed build launches all three tray processes automatically as
  part of an ordinary `kohiko-session` startup, with no manual
  intervention.
- All three successfully complete the XEmbed dock handshake into the
  bar's tray container - confirmed via the X window tree (the
  container's child count goes from 0 to 3) and via
  `_NET_SYSTEM_TRAY_S0` selection ownership, not just "the process
  exists."
- `kohikoctl reload` does not relaunch or duplicate them (autostart is
  correctly a startup-only step, unchanged from before this fix).
- Killing the WM process outright correctly triggers
  `kohiko-session`'s own crash-restart logic, and the new `kohiko`
  process correctly re-runs autostart, so a fresh set of tray icons
  reliably re-appears and re-docks after a crash too - **with one
  caveat**: the *previous* run's three tray processes are not killed
  by this and are left running, no longer docked anywhere (their
  window was destroyed along with the old bar). This is pre-existing
  behavior of how `kohiko-session`'s supervisor and `RunAutostart()`'s
  fire-and-forget spawning already worked together, for *any*
  autostart program, not something this fix changed - fixing it would
  mean changing `kohiko-session`'s own supervisor to track and restart
  autostart children, which is unrelated-enough WM functionality that
  it's left alone here and just noted as a known characteristic.
- The audio tray's scroll wheel was tested against the real dummy
  PipeWire sink end-to-end: scrolling up on its docked window moved
  the sink from 50% to 56%; scrolling down moved it back to 50%. Left-
  click and right-click were exercised too (process survives either);
  the exact resulting menu/window behavior wasn't independently
  re-verified beyond that, since `TrayIconClient.cpp`/the three
  `tools/kohiko-*-tray.cpp` files are byte-for-byte unchanged from
  before this fix (confirmed by diff against the pre-fix source).
- `make`/`make test` and the CMake+`ctest` equivalent both still build
  clean (zero warnings) and pass in full, including two new test
  binaries (`test_desktopentry`, `test_iconresolver`, 34 checks
  between them) added for the two pieces of new pure logic
  (`ShouldAutostart()`/`Xdg::AutostartDirs()`, and see "Icon loading"
  below) - `make test` itself was previously broken (missing `-lXrandr`
  on two unrelated test targets, predating this fix) and had to be
  fixed first just to get a full run; see `CHANGELOG.md`.

Not tested, and worth being explicit about: real Wi-Fi/Bluetooth
hardware and a real NetworkManager/BlueZ install (the container has
neither), a real display manager specifically (SDDM/GDM/LightDM were
approximated by directly setting the environment a DM would set, not
run), and multi-monitor tray placement.

### Icon loading

Investigating "icons don't appear" surfaced two more issues in icon
*loading*, once the autostart fix above got the tray widgets running
at all - both found by actually looking at rendered pixels (an
ImageMagick histogram of the tray container's own pixels, not just
"the code looks right"), not by inspection alone:

1. **Exact-name-only icon lookup missed real, standard icon names.**
   `IconResolver::Resolve()` only ever tried an exact filename match.
   That's correct per the base freedesktop icon theme spec, but
   `network-wireless-signal-excellent` and friends - genuine,
   standard freedesktop status-icon names, exactly what
   `kohiko-network-tray` asks for - turned out to not exist under
   that exact bare name in Adwaita (confirmed: a real, currently-
   installed copy of Adwaita 46.0 only ships it as
   `network-wireless-signal-excellent-symbolic`), and Adwaita is
   about as mainstream an icon theme as exists. Confirmed empirically,
   not assumed: with a real Adwaita theme configured and all three
   tray widgets successfully docked (per the autostart fix above), the
   tray container's own pixels were a single uniform color - zero
   glyph pixels anywhere - across the entire container. **Fix**:
   `IconResolver::Resolve()` now retries with a `-symbolic` suffix if
   the exact name isn't found anywhere in the theme chain (skipped if
   the name already ends in `-symbolic`, so it can't double up). Unit-
   tested (`tests/test_iconresolver.cpp`) and confirmed against the
   real installed Adwaita theme: all of `audio-volume-high`,
   `bluetooth-active`, and `network-wireless-signal-excellent` now
   resolve to real files (the `-symbolic` ones, for this theme), and
   the tray container's pixels are no longer uniform - at least one
   real glyph shape now renders where before there was none. This is a
   change to `IconResolver.cpp` only, used by the launcher too as well
   as these tray/app windows; it only ever adds a fallback attempted
   after every existing lookup has already failed, so it can't change
   the result of any lookup that already succeeded.

2. **An unresolved, pre-existing Imlib2/librsvg crash risk, found (not
   caused) by fix #1 above.** Once real icon files were actually being
   loaded (they mostly weren't before fix #1, since resolution failed
   first), loading a specific sequence of several different real
   Adwaita status-icon SVGs in a row - within one long-running process,
   the same pattern a tray widget uses over its lifetime as its status
   changes - reproducibly corrupted the heap (`corrupted double-linked
   list`) partway through, in this project's own sandbox's installed
   Imlib2/librsvg (Ubuntu 24.04's `libimlib2-1.12.4`/
   `librsvg2-2.58.0`). Isolated as precisely as reasonably possible
   before stopping:
   - Not the SVG files themselves - `rsvg-convert` (librsvg's own CLI,
     same library) renders the exact same files with no issue,
     standalone.
   - Not `UiIconCache`'s own code, `IconResolver`, or anything from
     this fix - a minimal, direct, raw Imlib2-API-only reproduction
     (no Kohiko code at all) hits the same crash on the same specific
     files.
   - Deterministic given a fixed load sequence (crashed 3/3 repeated
     runs), but sequence-dependent (a *different* sequence loading the
     same files did not crash), and specific to particular files
     (`audio-volume-medium-symbolic.svg` and
     `network-wireless-signal-excellent-symbolic.svg` specifically;
     the other 11 real icon names all three tray widgets can request
     were individually confirmed not to trigger it).
   - `imlib_set_cache_size(0)` (disabling Imlib2's own internal image
     cache, on top of `UiIconCache`'s own separate pixmap-level cache,
     which is unaffected and unchanged) is a standard mitigation for
     this general class of Imlib2 bug and was added defensively to
     `UiIconCache`'s constructor, but **did not reliably eliminate
     it** in further testing - the specific 9-icon sequence above
     still crashed with it in place, deterministically, across 3
     repeated runs. It is included anyway since it's a standard,
     zero-downside, one-line mitigation, but should not be read as a
     fix.

   **This is disclosed, not fixed.** Root-causing or working around a
   crash inside a third-party image-rendering library's own internals
   is a fundamentally different, larger, and riskier undertaking than
   "why don't these processes launch," and well outside "smallest
   robust fix" for that bug - especially for a crash this environment-
   and version-dependent, unconfirmed on the actual target system
   (Arch Linux's `imlib2`/`librsvg` package versions are almost
   certainly different from this sandbox's Ubuntu 24.04 ones, and may
   not be affected at all). `Launcher.cpp` has its own separate,
   independent Imlib2 usage for app icons (`DrawIcon()`, not routed
   through `UiIconCache`) that was not touched or tested here, but
   shares the same underlying Imlib2, so may carry the same class of
   risk - worth checking in a future session, but explicitly not part
   of this fix (unrelated WM functionality). **If you hit an
   audio/network tray widget disappearing unexpectedly, especially
   after the volume or signal-strength icon has changed state several
   times, this is the most likely explanation** - restarting the
   affected tray widget is the only known workaround for now, and a
   theme whose status icons exist under their bare (non-`-symbolic`)
   names sidesteps it entirely for whichever specific icons it
   provides that way.

## What was implemented

All three apps' main pages were rebuilt to match a supplied visual
mockup closely, and their content/interaction model changed to match:

- **`kohiko-audio`**: OUTPUT DEVICES and INPUT DEVICES sections, each a
  `Card` containing one flat divided row per device - icon, name,
  device type (Stereo/Mono, derived from channel count), a Default
  badge when applicable, a live volume percentage, a slider, and a
  Mute/Unmute button (turns red-outlined "Unmute" when muted). Clicking
  a row anywhere other than its own controls makes that device the
  default. Below ~560px content width, each row reflows onto two lines
  (name/badge on top, slider/percent/mute below) instead of clipping.
- **`kohiko-network`**: a toolbar (Wi-Fi toggle, VPN on/off status, a
  refresh button - wraps to a second line below ~560px), a working
  live-filtering search field, an AVAILABLE NETWORKS list, and a
  details panel for whichever network is selected (Status, Signal
  Strength, IPv4 Address, Gateway, DNS, and Disconnect/Connect buttons -
  both always shown side by side, with only the one that applies to
  the current connection state enabled, matching the mockup's own
  two-buttons-side-by-side layout while avoiding a "Connect" that
  would do nothing on an already-active network). List and details
  panel sit side by side above ~720px content width and stack into one
  column below it.
- **`kohiko-bluetooth`**: a toolbar (Bluetooth/Discoverable toggles, a
  "Scan for devices" button, a refresh button), CONNECTED DEVICES and
  NEARBY DEVICES lists (bucketed by `BluetoothDevice::connected`, not
  by paired status - "nearby" covers both paired-but-out-of-range and
  never-paired-but-visible devices alike), and a details panel showing
  Connection/Battery/Trusted plus buttons appropriate to the selected
  device's actual state: a single "Pair" button if unpaired; "Connect"
  + "Forget Device" if paired but not connected; "Disconnect" +
  "Forget Device" + "Disconnect and Forget" if connected (matching the
  mockup's three-stacked-buttons shape for that state specifically).

All three: content width caps and centers past ~900-960px instead of
stretching every row wider as the window grows, and every page has
been checked down to ~420px content width without controls overlapping
or becoming unreachable.

## What was redesigned (vs. the previous, 0.19.1 layout)

0.19.1 had already moved these three apps from a single narrow stacked
column to a responsive card/grid layout - see Phase 10/11 in
`PROJECT_HISTORY.md`. This session's redesign replaced that layout
again, this time to match a specific supplied mockup rather than an
original design:

- Each device/network/Bluetooth-device list changed from a **grid of
  separate rounded cards** (1-3 columns depending on width) to a
  **single bordered `Card` containing flat divided rows** - a
  genuinely different visual shape, not just a resize of the same one
  (see `ListRow`'s new `flat` mode in `docs/ARCHITECTURE.md`).
- `kohiko-network`/`kohiko-bluetooth` gained a **details panel** for a
  selected list item - previously, a network/device's actions lived
  inline on its own card; now selecting a row updates a separate panel,
  and the list itself no longer carries per-row action buttons beyond
  the row's own selection click.
- The shared theme's accent color changed from blue to violet, and
  surfaces got darker, to match the mockup - see `UiTheme.h`. This is
  scoped to the toolkit these three apps use; `kohiko-settings`'s own
  separate widget code is unaffected (see `docs/ARCHITECTURE.md`).
- Features the new mockups don't show moved to a per-app **"Advanced
  Settings" sub-page** rather than being dropped: `kohiko-audio`'s
  level meters and "notify on default device change" toggle;
  `kohiko-network`'s Ethernet device management and VPN profile
  activation (a small Ethernet/VPN tab switcher, currently two
  `Button`s rather than a `Sidebar` - see
  [Remaining TODOs](#remaining-todos)); `kohiko-bluetooth`'s per-device
  Trust toggle (the main details panel now only ever *shows*
  trusted status, matching the mockup, rather than also controlling
  it).
- `kohiko-network` gained a **working search field** with real
  keyboard focus - previously there was no text input anywhere in any
  of the three apps.

## Bugs fixed

Four bugs were found in the shared UI toolkit while building the
above - all now fixed at the toolkit level, all detailed with the
"why" in `docs/ARCHITECTURE.md` (see its "UI toolkit" section) and in
`CHANGELOG.md`'s 0.20.0 entry:

1. `ScrollView::SetContent()` not actually positioning its content.
2. `ListRow`/`Sidebar` caching absolute layout rects that went stale
   once (1) was fixed.
3. A use-after-free when a widget's own callback synchronously rebuilds
   its containing page (found via an AddressSanitizer build).
4. The `Makefile` not tracking header dependencies, letting a stale
   object file link against fresh ones with no warning.

None of these were specific to one app - each was hit by (and fixed
once, for) all three.

## Toolkit improvements

New shared widgets/mechanisms, all in `docs/ARCHITECTURE.md`'s UI
toolkit section in more detail: `Card`, `TextField` (with a new
keyboard-focus system on `UiWindow`), `IconButton`, `Button::Tone`,
`ListRow`'s `flat` mode, `MakeSettingsRow()`/`MakeDetailRow()`, and
`Widget::ReleaseChild()` (supporting the deferred-destruction fix).

## Architectural changes

- Selection state (`m_selectedApPath` in `NetworkWindow`,
  `m_selectedDevicePath` in `BluetoothWindow`) is now a first-class
  member, re-validated on every rebuild (`EnsureSelection()`): kept if
  still present in the current list, else the first connected/active
  item, else the first item overall. This is new - the previous inline-
  actions-per-card design had no concept of "the selected item"
  distinct from "the item whose button you just clicked."
- `NetworkWindow` gained a live text-filtering query
  (`m_searchQuery`), re-applied on every rebuild rather than filtering
  once at data-fetch time - see `AppendNetworkSection()`.
- Every app's `Page` enum (`Main`/`Advanced`) and, for `NetworkWindow`,
  an `AdvancedTab` enum (`Ethernet`/`Vpn`) are new - previously
  `kohiko-audio` had an Output/Input page split and `kohiko-network`
  had a Wi-Fi/Ethernet/VPN sidebar-driven split; both collapsed into
  "one main page focused on what the mockup shows, one Advanced page
  for the rest."

## Files that changed

- `include/UiTheme.h`
- `include/UiWidget.h`, `src/UiWidget.cpp`
- `include/UiListRow.h`, `src/UiListRow.cpp`
- `include/UiWindow.h`, `src/UiWindow.cpp`
- `include/UiScrollView.h`, `src/UiScrollView.cpp`
- `include/UiSidebar.h`, `src/UiSidebar.cpp`
- `include/AudioWindow.h`, `src/AudioWindow.cpp` (full rewrite)
- `include/NetworkWindow.h`, `src/NetworkWindow.cpp` (full rewrite)
- `include/BluetoothWindow.h`, `src/BluetoothWindow.cpp` (full rewrite)
- `Makefile` (header dependency tracking)
- `README.md`, `CHANGELOG.md`, `PROJECT_HISTORY.md`,
  `docs/ARCHITECTURE.md`, `docs/AUDIO_NETWORK_BLUETOOTH.md`
  (documentation)

No new source files were added - every new widget/helper lives in an
existing toolkit file (see `docs/ARCHITECTURE.md`). `CMakeLists.txt`
needed no changes as a result. No backend files changed:
`PipeWireClient`, `NetworkManagerClient`, `BluezClient`, and the D-Bus
layer underneath them are exactly as they were.

## Remaining TODOs

- `kohiko-network`'s Advanced Settings Ethernet/VPN switcher is
  currently two plain `Button`s standing in for a tab control, not the
  `Sidebar` widget (which would need to live inside the scrollable
  page, not as fixed chrome, to fit this session's single-`ScrollView`
  page structure - doable, just not done). Low priority: it works and
  looks reasonable, just isn't using the "best fit" existing widget.
- Nothing currently calls `BluezClient::SetAdapterPairable()` or
  exposes it in the UI (mirroring `kohiko-network`'s pre-existing gap
  around `NetworkManagerClient::SetAirplaneMode()` - see
  [Known limitations](#known-limitations)) - not part of either
  mockup, not previously exposed, so not a regression, but worth
  noting as unexercised backend surface.
- No automated regression coverage for any of the three apps or the
  toolkit itself - see `docs/ARCHITECTURE.md`'s recommendations.

## Known limitations

- **Developed and tested entirely in a sandboxed container**: no real
  display, no icon theme installed, no PipeWire/NetworkManager/BlueZ
  daemons running, no window manager. All layout, interaction (search
  typing, row selection, page navigation, focus preservation across
  rebuilds), and responsiveness testing was done via Xvfb screenshots
  and `xdotool`, cross-checked against the mockups, plus temporary
  fake-data builds (removed before finalizing) to exercise the device-
  list/details-panel code paths without real hardware. **Real-hardware
  smoke testing on an actual KohikoWM desktop is recommended before
  relying on this.** (Still true for this section's own scope - the
  three apps' own page navigation, search, and selection interactions.
  A later session *did* add real-display/real-D-Bus/real-PipeWire
  testing, but specifically for the tray widgets' autostart/docking/
  icon-loading/scroll-volume behavior, not for these apps' own UI
  interactions - see [Tray widget autostart](#tray-widget-autostart-this-sessions-fix)
  above for exactly what that covered and didn't.)
- **Icons don't render in that sandbox** (no icon theme installed) -
  icon *names* are set correctly throughout (freedesktop-standard
  names matching the existing convention, e.g. `audio-card`,
  `network-wireless`, `bluetooth`), but their actual on-screen
  appearance couldn't be verified there. (A later session verified
  icon rendering specifically for the tray widgets' status icons
  against a real installed theme, found and fixed a real gap, and
  found an unresolved, disclosed rendering-crash risk - see [Icon
  loading](#icon-loading) above. These three apps' own icon names,
  e.g. `audio-card` for a device row, weren't part of that check and
  remain unverified on-screen.)
- `kohiko-network`'s Wi-Fi row signal strength is shown via icon only
  in the list (matching the mockup, which doesn't show a numeric
  percent there); the numeric percent is shown in the details panel.
- `kohiko-bluetooth`'s NEARBY DEVICES list has no per-row signal
  indicator - the mockup shows one, but `BluetoothDevice` doesn't carry
  RSSI data, and fabricating a value would misrepresent real device
  state rather than just omit a nice-to-have.
- `NetworkManagerClient::SetAirplaneMode()` (toggling both Wi-Fi and
  WWAN radios together) isn't wired into the UI - it wasn't in the
  previous UI either, so this predates the 0.20.0 redesign and isn't a
  regression from it, just an existing gap worth knowing about.

## Recommendations for the next development session

1. **Smoke-test on a real KohikoWM desktop first**, before making
   further changes to these three apps' own pages - a later session
   did this for the tray widgets specifically (autostart, docking,
   icon loading, scroll-volume - see [Tray widget
   autostart](#tray-widget-autostart-this-sessions-fix) above), but
   this section's own recommendation - these apps' own page
   navigation, search, row selection, and especially
   `kohiko-bluetooth`'s pairing flow (`BluezClient::PairDevice()`/
   `ConnectDevice()` are genuinely slow, blocking D-Bus calls - see
   `BluetoothWindow.h`'s own comment - and their real-world timing
   still hasn't been tested) - remains open.
2. **Read `docs/ARCHITECTURE.md`'s "coordinate convention" and
   "deferred destruction" sections before touching any page-building
   code** in any of the three apps, or before adding a new widget to
   the shared toolkit - both describe real bugs this session found the
   hard way, and the patterns that avoid them aren't enforced by the
   type system, only by convention.
3. If adding a fourth page/app or a substantially different page shape
   to an existing one, consider whether it's finally worth a real
   layout-container abstraction (see `docs/ARCHITECTURE.md`'s
   recommendations) rather than more hand-computed `Rect` arithmetic -
   but don't retrofit it onto the existing three pages just for its
   own sake.
4. If you add anything to the `Makefile` that produces a new `.o` file,
   remember `$(DEPFLAGS)` (see `docs/ARCHITECTURE.md`'s build-system
   section) - it's easy to copy an older rule that predates the fix.
5. **Investigate the Imlib2/librsvg crash risk noted in [Icon
   loading](#icon-loading) above** on the actual target system (Arch
   Linux's own `imlib2`/`librsvg` package versions) before assuming
   it's specific to this project's sandbox - if it reproduces there
   too, it's worth an upstream Imlib2 report; if not, it's worth
   recording here that it doesn't, so the next person doesn't have to
   re-investigate from scratch. `Launcher.cpp`'s own separate Imlib2
   usage (`DrawIcon()`) shares the same underlying library and is
   worth the same check.
