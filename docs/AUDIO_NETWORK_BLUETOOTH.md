# `kohiko-audio` / `kohiko-network` / `kohiko-bluetooth`: design notes and status

This document covers the three newest companion apps specifically -
what their current design is, what changed to get there, and what's
left. For how they fit into the project as a whole (the shared UI
toolkit, the process model, configuration, rendering), see
[docs/ARCHITECTURE.md](ARCHITECTURE.md); that document is the one to
keep current as further changes land, this one is closer to a
snapshot of "what the 0.19.2 redesign did and where it left things."

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
`CHANGELOG.md`'s 0.19.2 entry:

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
  relying on this.**
- **Icons don't render in that sandbox** (no icon theme installed) -
  icon *names* are set correctly throughout (freedesktop-standard
  names matching the existing convention, e.g. `audio-card`,
  `network-wireless`, `bluetooth`), but their actual on-screen
  appearance couldn't be verified there.
- `kohiko-network`'s Wi-Fi row signal strength is shown via icon only
  in the list (matching the mockup, which doesn't show a numeric
  percent there); the numeric percent is shown in the details panel.
- `kohiko-bluetooth`'s NEARBY DEVICES list has no per-row signal
  indicator - the mockup shows one, but `BluetoothDevice` doesn't carry
  RSSI data, and fabricating a value would misrepresent real device
  state rather than just omit a nice-to-have.
- `NetworkManagerClient::SetAirplaneMode()` (toggling both Wi-Fi and
  WWAN radios together) isn't wired into the UI - it wasn't in the
  previous UI either, so this predates the 0.19.2 redesign and isn't a
  regression from it, just an existing gap worth knowing about.

## Recommendations for the next development session

1. **Smoke-test on a real KohikoWM desktop first**, before making
   further changes - this is the single highest-value thing to do
   next, given everything above was verified in a sandbox. Pay
   particular attention to icon rendering and to `kohiko-bluetooth`'s
   pairing flow (`BluezClient::PairDevice()`/`ConnectDevice()` are
   genuinely slow, blocking D-Bus calls - see `BluetoothWindow.h`'s own
   comment - and their real-world timing couldn't be tested here).
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
