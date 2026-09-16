# Kohiko: Architecture

This is the project-wide architecture reference: how the window
manager core, the shared UI toolkit, the companion applications, IPC,
configuration, and rendering all fit together. It's meant to be kept
up to date as the project evolves, not to describe one point in time -
if you change something this document describes, update the relevant
section in the same change.

For the WM core's own internals in more depth (the BSP tree, layout
algorithm, placement/misbehavior fallback logic), see the
[Architecture section of the README](../README.md#architecture) - that
table and its surrounding text aren't duplicated here. For a
chronological account of *how* the project got here, see
[PROJECT_HISTORY.md](../PROJECT_HISTORY.md). For a closer look at the
three newest companion apps specifically (per-app design notes,
current limitations, recommendations), see
[docs/AUDIO_NETWORK_BLUETOOTH.md](AUDIO_NETWORK_BLUETOOTH.md).

## Contents

- [Process map](#process-map)
- [Core window manager](#core-window-manager)
- [UI toolkit (`UiWidget` & friends)](#ui-toolkit-uiwidget--friends)
- [Companion applications](#companion-applications)
- [Configuration system](#configuration-system)
- [IPC](#ipc)
- [Rendering pipeline](#rendering-pipeline)
- [Build system](#build-system)
- [Known limitations / recommendations for the next session](#known-limitations--recommendations-for-the-next-session)

## Process map

Kohiko isn't one binary - it's a small family of independent
processes that agree on file formats and D-Bus/system-service
contracts, never on shared memory or a shared address space:

| Process | What it is | Talks to |
|---|---|---|
| `kohiko` | The window manager itself - the only process that speaks X11 as a *window manager* (`SubstructureRedirect` etc.) | X server directly (Xlib); `kohikoctl` over a Unix socket; `kohiko-settings` only indirectly, via the config file |
| `kohikoctl` | A CLI client - sends one line, reads one response, exits | `kohiko`'s `IPCServer`, over a Unix socket |
| `kohiko-settings` | A structured GUI editor for `kohiko.conf` | The config file directly (reads via `Config`, writes via `ConfigWriter`); tells `kohiko` to reload via IPC after saving |
| `kohiko-audio` / `kohiko-network` / `kohiko-bluetooth` | Ordinary X11 client apps, each a full settings page for one subsystem | PipeWire / NetworkManager (D-Bus) / BlueZ (D-Bus) respectively - never `kohiko` itself |
| `kohiko-audio-tray` / `kohiko-network-tray` / `kohiko-bluetooth-tray` | Small system-tray clients | The same backend as their matching full app, plus `AppInstanceLock` to raise the full app instead of launching a second instance |

None of these processes share code by linking against each other -
they share code by linking against the same **static libraries of
source files** (the UI toolkit, the D-Bus client layer, `AppConfigStore`,
`AppInstanceLock`, ...), each compiled fresh into every binary that
uses it. There's no Kohiko shared library (`.so`) anywhere - see
[Build system](#build-system).

## Core window manager

The `kohiko` process itself: `WindowManager` coordinates a BSP-tree
tiling engine (`BSPTree`/`BSPNode`/`BSPLeaf`/`BSPSplit`), one `Bar` per
monitor (plain Xlib drawing, no toolkit - see
[Rendering pipeline](#rendering-pipeline)), `KeyboardManager`/
`MouseManager` for input, `WorkspaceManager`/`MonitorManager` for
multi-monitor/multi-workspace state, and a single-threaded
`EventLoop` multiplexing the X11 connection with the IPC socket via
`select()`. See the
[Architecture section of the README](../README.md#architecture) for
the full file-by-file table and the placement/tiling algorithm's own
description - that content lives there because it's tightly coupled
to the user-facing behavior the rest of the README documents (window
rules, multi-monitor, session restore, ...), and duplicating it here
would just create two copies to keep in sync.

`kohiko-settings` (`SettingsWindow`) is a separate ordinary
application - not part of the `kohiko` process - with its own
hand-rolled widget code, predating the shared UI toolkit described
below and deliberately never migrated onto it (see
[Companion applications](#companion-applications)'s note on this).

## UI toolkit (`UiWidget` & friends)

Everything under `include`/`src` prefixed `Ui` - `UiWindow`,
`UiWidget` (which most concrete widgets live in), `UiListRow`,
`UiScrollView`, `UiSidebar`, `UiPopupMenu`, `UiIconCache`, plus
`UiTheme` for the shared color palette and `Font` for text
measurement/drawing. Built once, used by all three of
`kohiko-audio`/`kohiko-network`/`kohiko-bluetooth` - **not** by
`kohiko-settings`, which has its own separate, older, hand-rolled
widget code that was deliberately left alone rather than migrated (see
Phase 10 in `PROJECT_HISTORY.md`: "new infrastructure for new apps,
not a refactor of something that already works"). This means
`UiTheme`'s palette can be changed freely without affecting
`kohiko-settings`'s look - and has been (see
`docs/AUDIO_NETWORK_BLUETOOTH.md`).

### `Widget` basics

Every widget is a `Widget` subclass with a public `Rect bounds` (in
window-absolute pixel coordinates - see
[The coordinate convention](#the-coordinate-convention-and-why-it-matters)
below) and an owned list of child widgets. The base class provides:

- `Draw(UiWindow&)` - draw self, then children (`DrawChildren()`)
- `WantsInput()` / `OnPress`/`OnRelease`/`OnMotion`/`OnScroll` - mouse
  interaction, opt-in per widget (a plain `Widget` wants none of it)
- `WantsFocus()` / `OnFocus`/`OnBlur`/`OnKeyInput` - keyboard focus,
  added for `TextField` (see below); every other existing widget's
  defaults mean this doesn't affect them
- `AddChild()` / `Children()` / `ClearChildren()` / `ReleaseChild()` -
  child ownership; `ReleaseChild()` (added this session) hands a
  specific child back to the caller *without* destroying it - see
  [Deferred destruction](#deferred-destruction) below for why that
  exists

Concrete widgets in `UiWidget.h`/`.cpp`: `Label`, `IconView`,
`Separator`, `ToggleSwitch`, `Slider`, `Badge`, `Button` (with a
`Tone`: Neutral/Accent/Danger outline styles, or a filled `primary`
style), `IconButton`, `Card` (a bordered box - the container every
list/details panel in the three newer apps sits inside), `TextField`
(a real single-line text input - see below), and the
`MakePageHeader()`/`MakeSettingsRow()`/`MakeDetailRow()` composer
functions for shapes reused across pages (a title/subtitle/trailing-
control header; a label-plus-one-trailing-control settings row; a
label/value details-panel line). `UiListRow.h`/`.cpp` has `ListRow`
(icon, title, optional subtitle, one trailing control) with two
display modes - its own rounded card (the original shape), or `flat`
(a thin bottom divider and a left accent bar when `selected`, meant to
sit packed inside a shared `Card` alongside sibling rows - the shape
`kohiko-audio`'s device lists, `kohiko-network`'s network list, and
`kohiko-bluetooth`'s device lists all use). `UiScrollView.h`/`.cpp`
has `ScrollView` (mouse-wheel scrolling over a single content
`Widget`, replaced wholesale via `SetContent()`). `UiSidebar.h`/`.cpp`
has `Sidebar` (a vertical list of selectable items - currently used by
`kohiko-audio`/`kohiko-network`/`kohiko-bluetooth`'s Advanced Settings
sub-navigation, and by earlier layouts before the mockup-driven
redesign). `UiPopupMenu.h`/`.cpp` has `PopupMenu`, a standalone
override-redirect popup window with its own modal event loop - not
part of the `Widget` tree at all, used by tray widgets.

### The coordinate convention, and why it matters

Every `Widget::bounds` is in **absolute window pixel coordinates** -
there is no parent-relative transform anywhere in `Draw()`. A
container's own `bounds` only ever affects what *it itself* draws
(its background, border); it does nothing to reposition its children.
This is a real footgun found twice during this session's work (see
`CHANGELOG.md`'s "Fixed" entries for 0.19.2) - the fix in both cases
was the same shape: whatever computes a widget's final on-screen
position must do so *once*, using the coordinates that position will
actually have, rather than positioning children relative to a
container assumed to later "become" some other position.

In practice, every page-building function in
`AudioWindow`/`NetworkWindow`/`BluetoothWindow` follows one rule:
**compute every child's `bounds` using its true final absolute-in-content
coordinates at the moment you create it.** A `Card` containing device
rows, say, is built with `card->bounds = {0, y, width, height}` where
`y` is the running vertical offset already accumulated from everything
above it on the page - and each row inside that card is given
`{0, y + i * rowHeight, width, rowHeight}` using that *same* `y`, not
a row-local `{0, i * rowHeight, ...}` that assumes the card will later
shift its children into place. Nothing ever will.

The one place a whole-subtree translation *does* happen is
`ScrollView::SetContent()`, once, when content is first attached: the
caller builds the whole page assuming `(0, 0)` is the ScrollView's own
top-left, and `SetContent()` recursively shifts every descendant by
the ScrollView's actual on-screen position in a single pass. This is
the *only* place a translate-after-construction happens, specifically
because it needs to (a page can't know its ScrollView's absolute
position while it's being built as a detached tree) - and even this
one exception created its own bug: see
[Deferred destruction](#deferred-destruction) and the note on cached
layout below.

**A trap this convention creates**: any widget that caches a *derived*
absolute rect at layout time (rather than computing it fresh from
`bounds` on demand) will silently go stale if `bounds` is ever changed
afterward - by a translate, or by a caller deliberately resizing it
post-layout. `ListRow` and `Sidebar` both did exactly this and were
fixed to store *offsets* from `bounds` instead, recomputed every
`Draw()`/hit-test (see `CHANGELOG.md`). **If you add a new widget that
precomputes any layout in a "Layout()"-style method, store it relative
to `bounds`, not as an absolute `Rect`**, or it will have this same bug
the moment something moves it after the fact.

### Keyboard focus

Added this session for `TextField` (`kohiko-network`'s live search
field is currently the only user). `UiWindow` tracks one
`m_focusedWidget`; a click routes focus via `HitTest()` + `WantsFocus()`
(see `UiWindow::HandleEvent()`'s `ButtonPress` case), and app code can
grant it directly with `UiWindow::SetFocus(Widget*)` - needed when a
widget rebuilds its own containing page on every keystroke (to
re-filter a list, say) and must hand focus straight back to the fresh
widget instance that rebuild just created, or it would only ever
receive one keystroke before losing focus to nothing. `KeyPress`
events are translated to a small `KeyAction` enum
(`Printable`/`Backspace`/`Enter`/`Escape`/arrows/`Home`/`End`) before
being routed to `OnKeyInput()`, so widgets don't need to touch
`<X11/keysym.h>` themselves.

`UiWindow::ClearFocus()` exists for callers about to destroy the
current widget tree who need to drop a focus reference *before* doing
so as a defensive measure - `SetRoot()` calls it unconditionally. See
[Deferred destruction](#deferred-destruction) for why holding a stale
focus pointer across a rebuild is also handled at a lower level.

### Deferred destruction

Found and fixed this session (`CHANGELOG.md`'s use-after-free entry).
Both `ScrollView::SetContent()` and `UiWindow::SetRoot()` retire the
*outgoing* widget tree for one extra generation - moved into a
`m_retiredContent`/`m_retiredRoot` member via the new
`Widget::ReleaseChild()`, actually freed only at the start of the
*next* call - rather than destroying it immediately. This matters
because both are frequently called from inside a click/keypress
callback belonging to a widget that is itself part of the tree being
replaced (a list row's `onClick` selecting itself and calling straight
back into the page's own rebuild function is the common case
throughout all three newer apps). Destroying that widget - and the
`std::function` currently executing on its behalf - while its own call
stack is still unwinding is a real use-after-free in practice, caught
here with an AddressSanitizer-instrumented build; deferring the actual
free to the next call, by which point execution is always back at the
top of a fresh, unrelated event, avoids it without requiring every
`onClick`/`onChange` call site to know to defer anything itself.

**If you add a third place that destroys a live widget subtree**
(there are currently exactly two), it needs the same treatment unless
you can prove nothing inside that subtree can ever synchronously
trigger its own destruction.

## Companion applications

`kohiko-audio` (`AudioWindow`), `kohiko-network` (`NetworkWindow`),
`kohiko-bluetooth` (`BluetoothWindow`) - see
[docs/AUDIO_NETWORK_BLUETOOTH.md](AUDIO_NETWORK_BLUETOOTH.md) for the
current design of each specifically. All three follow one shared shape:

- **`Initialize()`**: acquire `AppInstanceLock`, create the `UiWindow`,
  connect the relevant backend client (`PipeWireClient`/
  `NetworkManagerClient`/`BluezClient`) and register its change
  handler (which just sets a `m_<x>Dirty` flag - see below), build the
  initial chrome, register a resize handler, and start a periodic
  `SetInterval()` timer.
- **`RebuildChrome()`**: builds the window's root `Widget` (currently
  just margins around a single `ScrollView` in all three apps) and
  calls `RebuildPage()`. Called from `Initialize()` and the resize
  handler.
- **`RebuildPage()`**: the dispatcher - builds a fresh content `Widget`
  for whichever internal `Page` enum value is active (`Main` or
  `Advanced` in all three apps currently), sets `content->bounds`, and
  calls `m_scrollView->SetContent(std::move(content))`.
  `NetworkWindow`'s also does the search-field focus-preservation dance
  described above.
- **`RebuildMainPage()`/`RebuildAdvancedPage()`**: build the actual
  widget tree for one page, following the coordinate convention above -
  an accumulating `int y` running down the page, with helper functions
  (`AppendDeviceSection`, `AppendToolbar`, `BuildDeviceRow`, ...)
  appending one section at a time and returning the new `y`.

**The periodic-rebuild pattern**: none of the three apps rebuild
directly from their backend client's change callback. Instead that
callback just sets a dirty flag; a `SetInterval()` timer (66-300ms
depending on the app) checks the flag and calls `RebuildPage()` only
if `!m_window.IsInteracting()` - i.e. only when nothing is mid-drag or
holding keyboard focus. This is what stops an async backend event
(PipeWire volume ticking, a new Wi-Fi network appearing) from yanking
the widget tree out from under an in-progress slider drag or a
half-typed search query.

**Responsive layout**: no generic layout system - every page computes
pixel `Rect`s directly (see
[the coordinate convention](#the-coordinate-convention-and-why-it-matters)),
branching on `contentWidth` at a handful of named breakpoints per app
(a narrow-toolbar breakpoint, a list/details split breakpoint, a
device-row two-line breakpoint - see each app's own `.cpp` for its
exact constants). Content width is capped and centered past
~900-960px so a very wide window gains margin instead of stretching
every row into one long line. This has been verified comfortable from
~420px content width up through fullscreen/ultra-wide, but see
`docs/AUDIO_NETWORK_BLUETOOTH.md`'s limitations section - it hasn't
been tested on a real display, only via Xvfb screenshots.

**Why `kohiko-settings` doesn't use any of this**: it predates the
toolkit and already works; see Phase 10 in `PROJECT_HISTORY.md` and
the note in [UI toolkit](#ui-toolkit-uiwidget--friends) above.

## Configuration system

Two entirely separate configuration mechanisms, for two different
kinds of settings:

- **`kohiko.conf`** (the window manager's own config) - `Config` loads
  a repeatable `key=value` file preserving duplicates (needed for
  `bind=`/`exec.*=`/`windowrule=`/`monitor=` which each appear many
  times); `ConfigParser` does the line-level parsing; `ConfigSchema`
  is metadata (category/group/type/default/description/allowed
  values) describing `Config`'s keys for `kohiko-settings` to render
  editors from - `Config` itself never references `ConfigSchema`, the
  dependency only goes one way; `ConfigWriter` is `kohiko-settings`'s
  write path back into the file, editing existing lines in place
  rather than regenerating the whole file (preserving comments/
  ordering/anything Kohiko itself doesn't understand).
- **`AppConfigStore`** - a deliberately tiny read/write `key=value`
  store for `kohiko-audio`/`kohiko-network`/`kohiko-bluetooth`'s own
  small UI preferences (currently just `kohiko-audio`'s "notify when
  the default device changes" toggle) - one flat file per app under
  `Xdg::ConfigDir()`, no sections, no repeatable keys, no relationship
  to `Config`/`ConfigSchema`/`ConfigWriter` at all. See its own header
  comment in `include/AppConfigStore.h` for why it's kept separate
  from `IniFile` (shaped around parsing *other people's* freedesktop
  files) as well as from `Config`.

Neither of these is the UI toolkit's `UiTheme` (a plain in-memory
struct of colors with no persistence - see
[UI toolkit](#ui-toolkit-uiwidget--friends)).

## IPC

`kohiko`'s `IPCServer` listens on a Unix domain socket; `kohikoctl`
connects, sends one line, reads one response, disconnects - simple
enough to run out of the same `select()`-based `EventLoop` as the X11
connection, so every request is answered on the main thread with no
locking anywhere. `IpcPath` computes the socket's location; `Json` is
a small hand-rolled encoder used for structured IPC responses (`kohikoctl`
commands that return more than a bare status).

Separately, `AppInstanceLock` (shared by all six of the newer
binaries - the three full apps and their three tray widgets) is a
single-instance-plus-raise mechanism, unrelated to `IPCServer`/
`kohikoctl`: it lets a tray widget's "open the full app" action raise
an already-running instance instead of launching a second one. See its
own header comment for the listening-socket/fork mechanics.

## Rendering pipeline

Two independent rendering paths, matching the two independent widget
systems:

- **The WM's own `Bar`** draws directly with Xlib (`XDrawString`/
  `XFillRectangle` and friends) - no toolkit, no backing pixmap
  abstraction beyond what Xlib itself does. See `Bar.h`'s file-level
  comment.
- **`UiWindow`** (the shared toolkit, and separately
  `kohiko-settings`'s own hand-rolled equivalent) renders to an
  off-screen `Pixmap` the same size as the window
  (`XCreatePixmap`/`XftDrawCreate` against that pixmap, not the window
  itself), then `Redraw()` copies the whole thing to the real window
  in one `XCopyArea` call. Every `Fill*`/`Draw*` method on `UiWindow`
  (`FillRect`, `FillRoundedRect`, `DrawBorder`, `DrawTextClipped`,
  `DrawIcon`, ...) draws into that backing pixmap; widgets never touch
  Xlib directly. A `m_dirty` flag (set by `RequestRedraw()`, checked
  once per `Run()` loop iteration) means a frame is only actually
  composited when something changed, not on a fixed timer. Icons
  route through `UiIconCache` (Imlib2-backed, freedesktop icon-theme
  lookup, graceful no-op if a name doesn't resolve - relevant if
  you're testing in an environment with no icon theme installed, see
  `docs/AUDIO_NETWORK_BLUETOOTH.md`); text goes through `Font` (Xft/
  fontconfig, with the same per-character fallback the WM's own `Bar`
  uses).

Resizing recreates the backing pixmap at the new size
(`UiWindow::ResizeBacking()`, called from the `ConfigureNotify`
handler) and calls the app's registered `SetResizeHandler()` callback,
which is what lets `AudioWindow`/`NetworkWindow`/`BluetoothWindow`
recompute their whole layout from scratch on every resize rather than
just stretching whatever they last drew.

## Build system

Plain `Makefile` plus a parallel `CMakeLists.txt` (kept in sync by
hand - there's no generation step from one to the other). Neither
produces a Kohiko shared library: every binary explicitly lists which
`src/*.cpp` files it needs and compiles/links them directly, so e.g.
`kohiko-audio` and `kohiko-bluetooth` each get their own compiled copy
of `UiWidget.cpp`, `DBusClient.cpp`, etc. rather than sharing a `.so`.
`libdbus-1-dev`/`libpipewire-0.3-dev` gate whether the six newer
binaries (three apps, three tray widgets) are built at all; everything
else builds and works identically either way (see
[Audio, network, and Bluetooth](../README.md#audio-network-and-bluetooth)
in the README).

The `Makefile` now generates and includes per-`.o` header-dependency
files (`-MMD -MP`, `build/*.d`, `-include $(wildcard build/*.d)` at
the bottom of the `Makefile`) - added this session after a missing
dependency-tracking bug caused stale object files to link against
fresh ones with no warning (see `CHANGELOG.md`). **If you add a new
object-file-producing rule to the `Makefile`, give it `$(DEPFLAGS)`
too**, or it'll silently regress back to the same bug for that one
rule. `CMakeLists.txt` was not affected - CMake's Makefile/Ninja
generators already track header dependencies automatically.

## Known limitations / recommendations for the next session

- **No generic layout system.** Every page in
  `AudioWindow`/`NetworkWindow`/`BluetoothWindow` hand-computes pixel
  `Rect`s with an accumulating `y` offset and a handful of named width
  breakpoints (see [Companion applications](#companion-applications)).
  This works and is now used consistently across all three apps, but
  it's manual - a fourth app, or a substantially different page shape
  in one of the existing three, means writing that arithmetic again
  rather than composing existing layout containers. Worth a real
  `VerticalLayout`/`HorizontalLayout`/similar if a fourth app or a
  significantly different page shape is ever added; not worth
  retrofitting onto the existing three pages for its own sake.
- **No automated tests for the UI toolkit or the three newer apps.**
  The WM core has a BSP-tree unit test suite (see Phase 1 in
  `PROJECT_HISTORY.md`); the UI toolkit and
  `kohiko-audio`/`kohiko-network`/`kohiko-bluetooth` have none. All
  verification this session was manual (Xvfb screenshots, `xdotool`-
  driven interaction, an AddressSanitizer build for the use-after-free
  bug). A lightweight widget-tree test harness - even just "build page
  X with fake backend data, assert on the resulting `Rect`s or a
  rendered screenshot's pixels" - would have caught the coordinate and
  use-after-free bugs this session found by hand, faster and before
  they shipped.
- **The coordinate convention is a manual discipline, not an enforced
  one.** See [The coordinate convention](#the-coordinate-convention-and-why-it-matters) -
  nothing stops a future widget from caching an absolute rect the way
  `ListRow`/`Sidebar` used to. Worth a comment at the top of
  `UiWidget.h` calling this out explicitly for anyone adding a new
  widget, if one isn't there already by the time you read this.
- **Real-hardware/real-display testing is still outstanding.** See
  `docs/AUDIO_NETWORK_BLUETOOTH.md`'s own limitations section - this
  session's environment had no display, no icon theme, and no
  PipeWire/NetworkManager/BlueZ daemons, so icon rendering and
  real-device behavior are unverified.
- **`kohiko-settings` still doesn't use the shared UI toolkit.** This
  is a deliberate, repeatedly-reaffirmed choice (Phase 10; this
  phase's own notes), not an oversight - but if a future session finds
  a *concrete* reason `kohiko-settings` needs something the toolkit
  now has (the new `TextField`, say), that's the point at which
  migrating it becomes worth reconsidering, rather than before.
