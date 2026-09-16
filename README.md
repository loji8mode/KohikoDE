# Kohiko

A small, fast tiling window manager for X11, built around a Hyprland-style
BSP (binary space partitioning) layout: every new window splits the space
next to whatever's focused, `Super+LMB` drags pick a window up and swap it
with wherever you drop it, and `Super+RMB` drags resize a window and
everything next to it adjusts automatically. A native launcher (`Super+D`)
and a small scratch notepad (`Super+N`) round it out - both drawn the same
plain-Xlib way as the bar, with no extra process or toolkit dependency.

Kohiko is **not** a Hyprland replacement and it isn't Wayland - Hyprland is
a Wayland compositor, which means competing with it feature-for-feature
would mean writing a compositor, a renderer, and an input stack from
scratch. Kohiko instead borrows the parts of Hyprland's tiling *behaviour*
that make it pleasant to use (the BSP dwindle-style layout, `hyprctl`-style
IPC, sane defaults) and implements them as a plain X11 window manager, on
top of decades-old, extremely well understood APIs. The payoff: no
compositing, no GPU shaders, no decorative effects, nothing running that
doesn't have to - just gaps, borders, and tiling, plus the one animation
rule the project does follow: motion only ever plays to confirm an action
you just took (a swapped window sliding into its new tile), never as
decoration. It should run comfortably on hardware that would struggle with
a full compositor.

## Contents

- [Building](#building)
- [Running it](#running-it)
- [Configuration](#configuration)
- [Kohiko Settings](#kohiko-settings)
- [Window rules](#window-rules)
- [Multi-monitor](#multi-monitor)
- [Autostart](#autostart)
- [Keyboard layouts / languages](#keyboard-layouts--languages)
- [Default keybindings](#default-keybindings)
- [System tray](#system-tray)
- [Audio, network, and Bluetooth](#audio-network-and-bluetooth)
- [EWMH support](#ewmh-support)
- [The mouse: swap, move, and resize](#the-mouse-swap-move-and-resize)
- [The launcher (Super+D)](#the-launcher-superd)
- [The notepad (Super+N)](#the-notepad-supern)
- [The power menu](#the-power-menu)
- [Native lock screen](#native-lock-screen)
- [Suspend integration](#suspend-integration)
- [Display sleep](#display-sleep)
- [Fonts and languages](#fonts-and-languages)
- [kohikoctl / IPC](#kohikoctl--ipc)
- [Architecture](#architecture)
- [Done](#done)
- [Planned](#planned)
- [Intentionally unsupported](#intentionally-unsupported)

See also: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) - a fuller
architecture reference covering the whole project (core WM, the
shared UI toolkit behind `kohiko-settings`/`kohiko-audio`/
`kohiko-network`/`kohiko-bluetooth`, IPC, configuration, and the
rendering pipeline), kept up to date as the project evolves rather
than describing one point in time.

## Building

You need a C++20 compiler and the X11 development headers, plus Imlib2
(icon loading and rendering for the launcher/notepad, and the lock
screen's optional background/logo images - icon-theme *lookup* is
Kohiko's own freedesktop Icon Theme Specification implementation, see
`include/IconResolver.h`, so no toolkit dependency is needed for that),
Xft/fontconfig (text rendering - see
[Fonts and languages](#fonts-and-languages)), and libpam (the lock
screen's authentication - see [Native lock screen](#native-lock-screen)).
There's no Qt, GTK, or compositor dependency of any kind, and no toolkit
is used for the bar/launcher/notepad/lock screen's own UI - just plain
Xlib shapes plus Xft text.

```sh
sudo apt install build-essential libx11-dev libimlib2-dev \
                  libxft-dev libfontconfig-dev libpam0g-dev fonts-dejavu-core
# optional, for multi-monitor geometry:
sudo apt install libxrandr-dev
# optional, for idle-timeout locking and the X11 fallback half of
# display-sleep inhibition (see Display sleep):
sudo apt install libxss-dev
# optional, for the primary (D-Bus) half of display-sleep inhibition:
sudo apt install libdbus-1-dev
# optional, for kohiko-audio/kohiko-network/kohiko-bluetooth and their
# tray widgets (see Audio, network, and Bluetooth) - both required
# together for those six binaries to build at all; everything else in
# this repository builds fine without them:
sudo apt install libdbus-1-dev libpipewire-0.3-dev
```

Also install the lock screen's PAM service file - without it, every
password is rejected (or, on some distros, PAM's own fallback silently
takes over instead - see [Native lock screen](#native-lock-screen) for
exactly what happens and why installing this matters either way):

```sh
sudo cp pam/kohiko /etc/pam.d/kohiko
```

Two build systems are provided; pick whichever you'd rather have installed.

**Plain `make`** (no cmake required):

```sh
make -j$(nproc)          # -> ./kohiko, ./kohikoctl, ./kohiko-settings, and (if libdbus-1-dev
                           #    + libpipewire-0.3-dev are present) ./kohiko-audio, ./kohiko-network,
                           #    ./kohiko-bluetooth, and their three tray widgets
make test                 # BSP tree, launcher scoring, adaptive-placement, DBusValue unit tests (no X server needed)
make test-monitors        # MonitorManager/XRandr tests (needs a real X server - skips gracefully without one)
sudo make install          # installs to /usr/local, incl. every app's .desktop entry + icon,
                           # and the tray widgets' autostart entries under /etc/xdg/autostart
```

**CMake:**

```sh
scripts/build.sh           # -> build/kohiko, build/kohikoctl, build/kohiko-settings, and
                           #    (same conditions as above) build/kohiko-audio, build/kohiko-network,
                           #    build/kohiko-bluetooth, and their tray widgets
```

`kohiko-settings` (see [Kohiko Settings](#kohiko-settings)) is Kohiko's
own settings GUI - part of this same repository and build, needing
nothing beyond the X11/Xft already listed above, not a separate project
or package.

If `libxrandr-dev` is present, both build systems automatically compile in
XRandr-based monitor detection (`KOHIKO_HAVE_XRANDR`); if it isn't, Kohiko
falls back to treating the whole X display as one monitor, which is
correct for the common single-monitor case regardless. `libxss-dev`
(`KOHIKO_HAVE_XSS`) and `libdbus-1-dev` (`KOHIKO_HAVE_DBUS`) are
detected and compiled in the same way, independently of each other and
of XRandr - see [Native lock screen](#native-lock-screen) and
[Display sleep](#display-sleep) for exactly what's disabled (gracefully -
never a build failure) without each one.

### Arch Linux: one-command install

```sh
scripts/install-arch.sh
```

Installs every pacman dependency Kohiko needs (`base-devel`, `libx11`,
`libxrandr`, `imlib2`, `xorg-fonts-misc`, `xorg-server`), builds
with `make -j$(nproc)`, runs `sudo make install`, drops a default config
in `~/.config/kohiko` if you don't have one yet, and registers Kohiko as
a session: an `xsessions` `.desktop` entry so it shows up in your display
manager's session list (SDDM/GDM/LightDM/...), plus a `~/.xinitrc` that
starts it - but only if you don't already have one, so it never
overwrites an existing setup. Run it as your normal user; it calls `sudo`
itself for the steps that need it.

## Running it

Kohiko is a normal X11 window manager, so it's started the same way as any
other (dwm, i3, ...): from your display manager's session list if you add
a `.desktop` entry for it, or directly from `~/.xinitrc`:

```sh
# ~/.xinitrc
exec /path/to/Kohiko/build/kohiko
```

To try it out **without** touching your real session, run it nested
inside a nested X server:

```sh
scripts/run-xephyr.sh 1     # opens a 1600x900 window running Kohiko on :1
```

On first run Kohiko looks for `~/.config/kohiko/kohiko.conf`; if it isn't
there yet, copy the shipped defaults to start from:

```sh
mkdir -p ~/.config/kohiko
cp config/default.conf ~/.config/kohiko/kohiko.conf
```

(You can also pass a config path explicitly: `kohiko /path/to/file.conf`.)

## Configuration

The format is deliberately simple: `key=value`, one per line, `#` for
comments. Four kinds of key are allowed to repeat (`bind=`, `exec.<name>=`,
`windowrule=` - see [Window rules](#window-rules) - and `monitor=` - see
[Multi-monitor](#multi-monitor)); every other key is a single setting
where the last occurrence wins. See
[`config/default.conf`](config/default.conf) for the full annotated set -
the highlights:

```ini
general.inner_gap=6              # gap between tiled windows
general.outer_gap=8              # gap around the edge of the screen
general.border_size=2
general.border_color_active=0x89b4fa
general.border_color_inactive=0x45475a
general.smart_gaps=true          # gaps collapse to 0 with only one tiled window
general.smart_borders=true       # same, for the border
general.min_tile_width=100       # a new tile - or any existing one - never shrinks below this...
general.min_tile_height=60       # ...tries the other split direction/shrinking first, then another workspace, then floating
general.tiling_misbehavior_threshold=3    # how many conflicting resize requests in a row counts as "fighting" tiled geometry
general.tiling_misbehavior_fallback=floating  # floating | new_workspace - where a misbehaving window goes instead
general.bar_height=26
general.focus_follows_mouse=true

workspace.count=10
scratchpad.width=70%
scratchpad.height=70%
notepad.width=40%
notepad.height=50%

exec.terminal=xterm              # referenced from `bind=... exec terminal`
exec.screenshot=flameshot gui    # referenced from `bind=Print exec screenshot`

auto_start_programs=telegram-desktop discord zen-browser  # launched once at startup

workspace1=zen-browser             # same idea, but pinned to a workspace - see Autostart below
workspace2=discord telegram-desktop
workspace3=steam

keyboard.layouts=us,ua           # XKB layouts, applied via setxkbmap
keyboard.layout_toggle=grp:alt_shift_toggle

mouse.swap=SUPER+BTN1
mouse.resize=SUPER+BTN3

bind=SUPER+RETURN exec terminal
bind=SUPER+Q close

windowrule=fullscreen class:flameshot   # see Window rules below
windowrule=tile class:tlauncher

monitor=HDMI-1,workspace=1              # see Multi-monitor below
```

### Kohiko Settings

A native settings GUI ships as part of Kohiko itself: `kohiko-settings`,
built from this same repository (`make`/`sudo make install` or the CMake
equivalent build and install it right alongside `kohiko`/`kohikoctl` - see
[Building](#building)), with no extra dependency beyond what Kohiko
itself already needs (plain X11/Xft - no GTK/Qt here either, same as
every other Kohiko-drawn window). Installing Kohiko always installs
Kohiko Settings; there's no separate package or opt-in step.

Unlike the bar/launcher/notepad/power menu/lock screen - all windows the
`kohiko` process draws for itself - `kohiko-settings` is a completely
ordinary, separate application, run and window-managed exactly like any
other program. It's discoverable the same way too: `sudo make
install`/CMake's `install()` drop a standard `.desktop` entry
(`desktop/kohiko-settings.desktop`) and icon
(`assets/icons/kohiko-settings.svg`) into the usual XDG locations, so it
shows up in Kohiko's own launcher (`Super+D`, type "Kohiko Settings") or
any other standards-compliant launcher/menu through completely ordinary
desktop-file indexing - nothing is hardcoded into `Launcher.cpp`/
`AppIndex.cpp` to make that happen. You can also just run
`kohiko-settings` directly from a terminal.

What it gives you, on top of hand-editing `kohiko.conf`:

- A **category sidebar** (General, Appearance, Launcher, Bar, Input,
  Monitors, Workspaces, Window Rules, Power, Notepad, Scratchpad,
  Developer, Lock Screen), with settings further **grouped under
  sub-headings** within a category (e.g. Appearance's Layout/Colors/Text)
  instead of one long flat list.
- **Search** (top of the window) that filters across every category at
  once by key, label, description, or group - clicking a category again
  clears it.
- An **(i) info icon** beside every setting; click it to expand a short
  panel with its description, default value, allowed values (for
  anything enum-like), and any recommendation, then click again to
  collapse it.
- **Inline validation** - a number field that doesn't parse, a color
  that isn't `0xRRGGBB`, a percentage missing its `%`, or a window
  rule/monitor rule/keybinding/named-command line that doesn't match its
  expected syntax gets a red outline and a short error message right
  there, rather than silently writing something Kohiko wouldn't
  understand.
- **Click-to-position caret** - clicking anywhere inside any text
  field, in a plain setting or a structured row alike, places the
  caret exactly where you clicked rather than always jumping to the
  start or end; arrow keys/Home/End/Tab still work exactly as before.
- **Save** and **Reset to Default** at the bottom. Save validates,
  writes every valid, changed setting to `kohiko.conf`, and asks a
  currently-running Kohiko to pick it up immediately (`kohikoctl
  reload`) - all without closing the window, so there's never a
  reason to restart Kohiko, or even leave Settings, after changing
  something; Reset to Default resets whatever's currently in view
  (the selected category, or the current search results) back to its
  shipped default - nothing is written to disk until you Save
  afterward.

`windowrule=` (under Window Rules) and `monitor=` (under Monitors) each
get a **structured, row-based editor**: one row per rule, with a
click-to-cycle action button (float/tile/fullscreen/no-fullscreen/
workspace for a window rule) plus a text field per selector
(`class:`/`instance:`/`title:`, or an output name and a workspace
number for a monitor rule) rather than needing to know that syntax by
hand even inside the GUI - Tab moves between a row's fields and on to
the next row, and each row gets its own `x` to remove it, plus an
"+ Add rule" button below the last one. What actually gets saved to
`kohiko.conf` is still exactly that same `class:foo instance:bar`/
`HDMI-1,workspace=2` syntax - the structured editor is purely a
friendlier way to produce it, never a different underlying format.

`bind=` (under Input's "Keybindings" group) and `exec.<name>=` (under
Developer) are, by contrast, each edited as their own small block of
raw config syntax (one entry per line, exactly like you'd write it in
the file) - a keybinding or a named command doesn't decompose into a
handful of independent selector fields the way a window/monitor rule
does, so a structured editor wouldn't actually be simpler than typing
the line itself. `workspace<N>=` autostart, on the other hand, *is* a
plain per-workspace text field (one per workspace, right under
`workspace.count` in the Workspaces category) since each is genuinely
its own distinct key rather than one repeated one.

Throughout, **`kohiko.conf` remains the actual source of truth**: Save
edits the file's existing lines in place - preserving every comment,
blank line, and setting you haven't touched through the GUI at all -
rather than regenerating it, so hand-editing the same file before, after,
or interleaved with using Kohiko Settings is always fully supported. A
setting the GUI has never touched keeps its exact original text (`yes`
stays `yes` rather than being silently rewritten to `true` the next time
you click Save, for instance); a genuinely new key Kohiko Settings adds
that didn't already have an active line gets appended under a clearly
marked `# --- Added by Kohiko Settings ---` section at the end of the
file instead of guessing where else it might belong.

Reload after editing `kohiko.conf` by hand without restarting: `kohikoctl
reload` (also bound to `Super+Shift+C` by default, and what Kohiko
Settings' own Save does automatically). `auto_start_programs` and
`workspace<N>=` are the one exception - both only ever run right after
Kohiko itself starts, never on reload, so reloading the config doesn't
relaunch every autostart program.

## Window rules

No amount of automatic window-type detection can guess every
application's intentions correctly, so - the same as i3's `for_window` or
Hyprland's `windowrule` - Kohiko lets you pin down exactly how a specific
application behaves, overriding whatever it asks for itself:

```ini
windowrule=float class:pavucontrol
windowrule=tile class:tlauncher
windowrule=fullscreen class:flameshot
windowrule=nofullscreen class:mpv
windowrule=workspace:4 class:telegram title:photo
```

- `float` - always open floating (centered, sized to its own natural
  size - see below), never tiled, regardless of window type.
- `tile` - always tile it, strictly, even if it's the kind of window
  (a dialog, or one that insists on opening at its own fixed size -
  Tlauncher is the canonical example of a Java/Swing app that does this
  and fights being resized) Kohiko would otherwise float automatically.
- `fullscreen` - open already fullscreen. flameshot's screenshot overlay
  needs this to work at all (a partial-screen overlay can't select the
  rest of the screen); it already asks for real EWMH fullscreen on its
  own and Kohiko honours that automatically the moment it's mapped (see
  [EWMH support](#ewmh-support)), so this mostly exists as an explicit
  belt-and-suspenders pin, or for apps that don't ask for it themselves.
- `nofullscreen` - never allow real fullscreen for this app, whether it
  asks for it itself or `fullscreen`/`Super+F` asks on its behalf; it
  stays tiled/floating instead.
- `workspace:N` - always open on workspace N regardless of whichever
  workspace is current - e.g. exiling a chat app's media-viewer windows
  to their own workspace ("a new virtual display") instead of covering
  whatever you're doing on the current one.

The selector after the action is one or more of `class:`, `instance:`,
`title:` (case-insensitive substring match against `WM_CLASS`'s
class/instance and the window title, space-separated - a window has to
match every one given to match the rule at all). `xprop` (click the
window after running it) shows you the actual class/instance/title to
match on if you're not sure.

Every window still gets sensible behaviour with **no** rule at all:
transient windows (anything with `WM_TRANSIENT_FOR` set) and every
dialog/utility/splash/toolbar/popup-menu `_NET_WM_WINDOW_TYPE` float
automatically and never enter the BSP tree - only a plain window with
neither (EWMH's definition of NORMAL) ever gets tiled. A transient
child additionally always lands on whatever workspace *and monitor*
its parent is actually on right now (bringing that workspace into view
on the focused monitor if the parent isn't visible anywhere), opens
centered over the parent's own window rather than the middle of the
monitor, and takes focus the moment it appears - so a GIMP color
picker, an IntelliJ/Android Studio dialog, a file picker, a
confirmation prompt, or TLauncher's own update/login prompts always
show up attached to the window that spawned them instead of floating
in the wrong place or getting silently left behind on another
workspace. Any floating window - automatic or `float` above - opens
sized at whatever it actually asked for (its `WM_NORMAL_HINTS`, or
failing that its own size at the moment it asked to be mapped) rather
than a flat fraction of the screen, so it doesn't get squashed or
stretched into a size it was never designed for. `windowrule=` exists
for the apps that don't play along with that on their own.

## Multi-monitor

Kohiko detects every connected output via XRandr (falling back to
treating the whole X display as one monitor if XRandr isn't available)
and gives each one its own independent workspace, exactly like
i3/bspwm/Hyprland: switching workspace on one monitor never touches
what any other monitor is showing, each has its own completely
separate `BSPTree`, its own bar, and fullscreen only ever covers the
monitor it was toggled on.

```
Monitor 1 (HDMI-1) -> Workspace 1        Monitor 1: Workspace 1 -> Workspace 2
Monitor 2 (DP-1)   -> Workspace 5   ->   Monitor 2: Workspace 5 (unchanged)
```

**Focus follows the mouse, monitor for monitor** - whichever monitor
the cursor is currently over is "the focused one": where a new window
opens, which one `workspace <N>`/`movetoworkspace <N>` operate on, and
so on. Just move the pointer there - onto a window, or onto bare
desktop - there's no keybind for it and nothing to press first. (This
is a separate, always-on mechanism from `general.focus_follows_mouse`,
which only controls whether hovering a *specific window* steals its
focus within a monitor; which monitor is focused follows the cursor
either way.) `focusmonitor`/`movetomonitor <left|right|up|down|N>`
still exist as plain commands if you'd rather bind explicit keyboard
control yourself (`kohikoctl dispatch focusmonitor right`, or a `bind=`
line) - nothing is bound to them by default anymore.

**Every monitor has its own bar**, each showing that monitor's *own*
active workspace - Monitor 1's bar highlights workspace 1 while
Monitor 2's highlights workspace 5, independently, exactly matching
whatever `kohikoctl monitors` reports for each. Only the currently-
focused monitor's bar shows a window title (real X input focus is
singular, so that's the only one with a genuine "focused window" to
show); the clock and the scratchpad/notepad indicators appear on every
bar. The system tray is a single X11-wide selection, so it can only
ever dock on one bar - it stays on whichever monitor is Primary().

**Dragging a floating window (Super+LMB) across a monitor boundary
transfers it live** - grab it, drag past the edge, and it belongs to
the new monitor's workspace immediately, still glued to the cursor;
releasing the button just clamps its final position to whatever
monitor it's over so it can't end up partly off-screen. (Super+LMB on
a *tiled* window keeps doing what it always did - pick it up and drop
it onto another tile to swap the two - dragging is only "move it
around" for a floating one.)

**Workspace conflicts are rejected, never swapped or stolen:** asking
a monitor to switch to a workspace that's already visible on a
*different* monitor does nothing to either monitor except show a
notification (right there on the requesting monitor's own bar, for a
few seconds) explaining why:

```
Workspace 3 visible on monitor 1
                                        Result: nothing changes -
User asks monitor 2 to switch to it -> monitor 2's bar shows
                                        "Workspace 3 is already
                                         visible on monitor 1"
```

**New windows** open on whichever monitor is currently focused (i.e.
under the cursor), except a transient/dialog child, which always
follows its *parent's* monitor instead (see [Window rules](#window-rules)
above) - so a launcher on monitor 2 always gets its own update dialog
on monitor 2 too, even if the cursor is over monitor 1 at that instant.

**Hotplug** is handled automatically: connecting or disconnecting a
monitor re-detects the whole layout, gives any newly-connected output
its own workspace (see `monitor=` below), rebuilds each monitor's own
bar to match, and re-homes any floating window whose position no
longer lands on *any* remaining monitor onto whichever one now shows
its workspace, so nothing is ever left positioned in space that no
longer exists. A tiled window needs no such fixup - its layout ratios
simply apply to whatever monitor ends up showing that workspace next.

Pin a specific output to a specific starting workspace with
`monitor=<name>,workspace=<N>` (run `kohikoctl monitors` to see the
exact output names XRandr knows your monitors by):

```ini
monitor=HDMI-1,workspace=1
monitor=DP-1,workspace=2
```

This only affects an output the *next* time it connects (plugging it
in, or starting Kohiko with it already attached) - it deliberately
never yanks a workspace already on screen out from under a monitor
just because a rule for it changed underneath it. The `monitor=`
syntax is intentionally the same `key,key=value,...` shape
`windowrule=` uses, so more per-monitor settings can be added later
without a new config syntax.

`kohikoctl monitors` dumps the full live state as JSON - id, XRandr
output name, geometry, `workArea` (geometry minus that monitor's own
bar), which workspace is active there, and whether it's the
primary/focused one:

```json
[{"id":1,"name":"HDMI-1","x":0,"y":0,"width":1920,"height":1080,
  "workArea":{"x":0,"y":26,"width":1920,"height":1054},
  "workspace":1,"primary":true,"focused":true}]
```

## Autostart

`auto_start_programs=` takes a space-separated list of commands, each
launched once, right after the bar/tray/launcher finish starting up -
no keybind needed:

```ini
auto_start_programs=telegram-desktop discord zen-browser
```

Each entry runs exactly the way `exec.<name>=` does (through `/bin/sh -c`,
forced onto this session's `DISPLAY`), so anything you could put after
`exec.terminal=` works here too. Leave it empty, or delete the line, to
autostart nothing.

### Autostart on a specific workspace

`workspace<N>=` (N from 1 to `workspace.count`) is the same thing, except
each program listed also gets pinned to workspace N once its window
actually appears, instead of landing on whichever workspace happened to
be focused when Kohiko itself started:

```ini
workspace1=zen-browser
workspace2=discord telegram-desktop
workspace3=steam
```

List a program under `workspace<N>=` instead of `auto_start_programs=`,
not in addition to it - either one launches it, and `workspace<N>=` is
just `auto_start_programs=` with a destination attached. Same
space-separated, `/bin/sh -c`-through-DISPLAY syntax either way, and a
program can only be pinned to one workspace at a time.

Kohiko places it by matching the window's own `_NET_WM_PID` back to the
process it just spawned for that line, walking up the process tree to
find it (`/bin/sh -c` itself already forks one extra level before ever
running your command, and a program with its own launcher script - e.g.
Steam - adds more on top of that), which is also why this only ever
affects a window that shows up within a minute or so of Kohiko
starting - long enough to cover even a slow starter, but not so long
that the same program opening some unrelated window an hour into the
session gets redirected too. A `windowrule=workspace:N` for the same
window (see [Window rules](#window-rules)) always wins over this if both
apply, since that's a more specific, deliberate override.

## Keyboard layouts / languages

Kohiko applies `keyboard.layouts=` via `setxkbmap` once at startup (and
again on reload), so it isn't limited to English - list any XKB layouts
you want, comma-separated:

```ini
keyboard.layouts=us,ua
keyboard.layout_toggle=grp:alt_shift_toggle
```

This is real XKB, so every layout listed works everywhere - in every
window, not just some of them - the same as it would under any other
window manager. `keyboard.layout_toggle` is any `setxkbmap` `grp:`
option; the default above cycles through the listed layouts with
Alt+Shift and is ignored while only one layout is configured.

Kohiko's own keybinds are unaffected by any of this either way:
`KeyboardManager` grabs `bind=` entries by physical keycode (see
[Architecture](#architecture)), so `Super+H/J/K/L` and the rest keep
working identically no matter which layout above is currently active -
only what other programs see you type changes.

## Default keybindings

| Bind                  | Action                                   |
|-----------------------|-------------------------------------------|
| `Super+Return`         | launch `exec.terminal`                    |
| `Super+D`              | toggle the native launcher                |
| `Super+N`              | toggle the native notepad                 |
| `Super+Q`              | close the focused window                  |
| `Super+Space`          | toggle floating                           |
| `Super+F`              | toggle fullscreen                         |
| `Super+F1`             | toggle scratchpad                          |
| `Super+R`              | rotate the focused split (vertical <-> horizontal) |
| `Super+Shift+R`        | flip the focused split (mirror the two panes) |
| `Print`                | screenshot via `exec.screenshot` (flameshot by default) |
| `Super+Shift+D`        | re-scan applications/files for the launcher (live update) |
| `Super+H/J/K/L`        | move focus left/down/up/right             |
| `Super+1..0`           | switch to workspace 1-10                  |
| `Super+Shift+1..0`     | send the focused window to workspace 1-10 |
| `Super+Shift+C`        | reload the config                         |
| `Super+Shift+Q`        | quit Kohiko                               |

Which *monitor* is focused isn't a keybind at all - it just follows
the mouse pointer (see [Multi-monitor](#multi-monitor)); `Super+LMB`
drag on a floating window also carries it across monitor boundaries
live. `focusmonitor`/`movetomonitor` remain available as commands for
anyone who wants an explicit keybind of their own.

All of the above are just entries in `kohiko.conf` - remove, remap, or add
to them freely; nothing is hardcoded.

## System tray

The bar implements the freedesktop System Tray Protocol, so applets that
dock an icon there show up at the right edge of the bar, just left of
the clock, the same way they would in any other status bar. No
configuration needed - Kohiko takes ownership of the tray selection on
startup and lays out whatever docks itself with it, left to right, in
the order it arrived. On a multi-monitor setup (every monitor has its
own bar - see [Multi-monitor](#multi-monitor)) the tray is a single
X11-wide selection, so it can only ever live on one of them - it stays
on whichever monitor is Primary(), following it if a hotplug changes
which one that is.

Kohiko ships its own native audio/network/Bluetooth tray widgets (see
the next section) rather than expecting a third-party applet like
`nm-applet` or `blueman-applet` - those still dock into the same tray
just fine if you run them instead, since this is a completely ordinary
implementation of the standard protocol either way, but there's no
need to for these three in particular.

## Audio, network, and Bluetooth

Three standalone native applications, installed and built alongside
`kohiko`/`kohiko-settings` (see [Building](#building)) but not part of
the window manager process itself - each is an ordinary X11 client
you can also launch, alt-tab to, or window-rule like any other. All
three share one visual language with `kohiko-settings` (same dark
violet-accented theme, same card/row/badge/button styling - see
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#ui-toolkit-uiwidget--friends))
and are laid out for a tiling window manager first: no fixed-size
assumptions, comfortable from a ~420px-wide tiled sliver up through a
fullscreen ultra-wide monitor. A narrow window gets a single stacked
column; a wide one splits a list from its details panel (network/
Bluetooth) or reflows a row onto two lines instead of clipping
(audio); very wide windows cap their content width and center it
rather than stretching every row into one long line. Each app's main
page deliberately mirrors its own design mockup closely - anything the
mockup doesn't have room for (level meters, Ethernet/VPN management,
per-device Bluetooth trust) lives one level down, behind that page's
own "Advanced Settings" link, rather than being dropped.

- **`kohiko-audio`** - OUTPUT DEVICES and INPUT DEVICES lists (icon,
  name/type, a Default badge, live volume, and a Mute/Unmute button
  per device - click a row anywhere else to make it the default
  device), backed entirely by PipeWire (and whatever WirePlumber
  policy is in effect for it) - Kohiko doesn't talk to ALSA or
  PulseAudio directly, and doesn't replace WirePlumber's own session/
  policy management, just the UI in front of it. Live output/
  microphone level meters and a "notify when the default device
  changes" toggle are one level down, on Advanced Settings.
- **`kohiko-network`** - a Wi-Fi/VPN toolbar, a live-filtering search
  field, an AVAILABLE NETWORKS list, and a details panel for whichever
  network is selected (status, signal strength, IPv4/gateway/DNS,
  Disconnect/Connect). Backed entirely by NetworkManager over D-Bus.
  Ethernet (connect/disconnect, IPv4/IPv6/DNS/gateway/MAC info) and
  VPN (activate/deactivate existing profiles) management are one level
  down, on Advanced Settings. Setting up a brand-new VPN profile isn't
  in scope either way - that's `nm-connection-editor` or your VPN
  provider's own setup tool's job; kohiko-network manages profiles
  that already exist.
- **`kohiko-bluetooth`** - adapter power/discoverable toggles, a scan
  button, CONNECTED DEVICES and NEARBY DEVICES lists, and a details
  panel for whichever device is selected (connection status, battery
  where the device reports one, trusted status, and Pair/Connect/
  Disconnect/Forget actions appropriate to that device's current
  state). Backed entirely by BlueZ over D-Bus. The actual trust
  on/off toggle (the details panel only ever *shows* trusted status)
  is one level down, on Advanced Settings.

Each has a matching tray widget (`kohiko-audio-tray`,
`kohiko-network-tray`, `kohiko-bluetooth-tray`) that docks into the
system tray described above and autostarts by default (see
[Autostart](#autostart) for the general mechanism - these use the
same `/etc/xdg/autostart` convention as any other autostarted
application, installed automatically by `make install`/`cmake
--install`). Left-clicking `kohiko-audio-tray` opens `kohiko-audio`,
and right-click opens a quick mute/device-switch menu; left-clicking
`kohiko-network-tray`/`kohiko-bluetooth-tray` opens a quick popup, and
right-click opens the full app - this asymmetry matches how each one
is most often used (audio's quick actions are reached for constantly;
network/Bluetooth's aren't). The audio tray widget's scroll wheel
adjusts the default output's volume directly. Clicking a tray widget's
"open the full app" action raises the already-running window instead
of launching a second one, the same single-instance behavior as
opening the app any other way.

None of these three apps or their tray widgets are built at all if
`libdbus-1-dev`/`libpipewire-0.3-dev` aren't present at build time
(see [Building](#building)) - `kohiko`, `kohikoctl`, and
`kohiko-settings` build and work exactly as before either way.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for how these three
apps are put together internally (the shared `UiWindow`/`Widget`
toolkit, the responsive layout conventions, current limitations, and
recommendations for anyone picking this up next).

## EWMH support

Kohiko advertises itself as a compliant window manager on startup, per
the EWMH/NetWM spec: it creates an invisible check window and publishes
`_NET_SUPPORTING_WM_CHECK` (proof there's a *live* WM, not a stale
property left over from one that already exited) and `_NET_SUPPORTED`
(the list below). It then keeps two more properties current for the
lifetime of the session:

- `_NET_CLIENT_LIST` - every window Kohiko currently manages, updated
  the moment one is added or removed.
- `_NET_ACTIVE_WINDOW` - whichever window is currently focused (unset
  when nothing is).

`_NET_WM_NAME` is honoured both ways: Kohiko reads it from client
windows for their title (shown in the bar), and sets it on its own
check window for the round-trip above.

Kohiko also implements the one EWMH window *state* it needs to:
`_NET_WM_STATE_FULLSCREEN`. A client can ask for it either of the two
ways the spec allows - a `_NET_WM_STATE` `ClientMessage` (add/remove/
toggle) once it's already mapped, or setting the property directly
before it's ever mapped at all (the form some toolkits use instead,
since the `ClientMessage` form is only meaningful for an already-mapped
window) - and Kohiko honours both, updating the property back to match
reality either way. `windowrule=fullscreen`/`nofullscreen` (see
[Window rules](#window-rules)) sit on top of this same mechanism.

This is what lets EWMH-aware tools that check for a compliant window
manager before trusting anything - flameshot's screenshot overlay is
the motivating example - work correctly under Kohiko instead of falling
back to less reliable window discovery, *and* actually receive the real
fullscreen they ask for once they do.

## The mouse: swap, move, and resize

These are the gestures the whole layout is built around:

- **`Super` + left-click and drag on a *tiled* window** picks it up: it
  detaches and follows the cursor exactly, and nothing else on screen
  moves while you're dragging - this is deliberately not a live
  swap-on-hover, so the only thing that happens *is* the thing you're
  doing. Whichever tiled window you're currently over gets a highlighted
  border as a preview of what you're about to swap with. Release over a
  window and the two trade places, each sliding smoothly into its new
  tile; release over empty space (or back over the window you picked up)
  and it slides back home instead. Geometry itself never changes - only
  which window occupies which tile does - the sliding motion is just
  confirming that for you, not decoration.
- **`Super` + left-click and drag on a *floating* window** just moves
  it, 1:1 with the cursor, the classic i3-style floating drag - and on
  a multi-monitor setup, dragging it across a monitor boundary
  transfers it onto the destination monitor's workspace immediately,
  live, mid-drag (see [Multi-monitor](#multi-monitor)); releasing the
  button clamps it to stay fully on whichever monitor it ends up over.
- **`Super` + right-click and drag** resizes: on a *tiled* window, the
  window you grabbed grows or shrinks in whichever direction you drag
  it, and its neighbour shrinks or grows to match, exactly like
  dragging the divider between them - updated on every reported mouse
  movement with no queued-up lag, even under a fast/laggy pointer
  (Kohiko collapses a backlog of motion events down to the latest one
  rather than working through it one at a time). On a *floating*
  window it resizes that window directly instead - whichever edge(s)
  you grabbed nearest to (an edge, or a corner for both axes at once)
  grow or shrink with the cursor while the opposite edge(s) stay put,
  clamped to the window's own minimum size and to whichever monitor
  it's on.

Both left-click gestures share one bind (`mouse.swap=`, default
`SUPER+BTN1`) - which one happens just depends on whether the window
under the cursor is tiled or floating. Resize is separately rebindable
via `mouse.resize=` (e.g. `SUPER+BTN2` for the middle button).

## The launcher (Super+D)

`Super+D` opens a small centered input box - about a twelfth of the
screen's area, wide rather than square, so it reads as "a line to type
into" rather than a dialog. It opens with the cursor already in the field;
type a command, press `Enter`, and it runs (via the same `/bin/sh -c`
mechanism as `exec.*=` entries) and the box closes itself. `Escape` cancels
without running anything, and clicking anywhere outside the box also
dismisses it.

This replaced the earlier default of shelling out to `dmenu_run` - it's
Kohiko's own window, drawn the same plain-Xlib way as the bar, with no
external launcher binary required. If you'd rather use dmenu, rofi, or
similar instead, the general `exec.<name>=` + `bind=... exec <name>`
mechanism is still there for it; see the commented-out example in
`config/default.conf`.

It also stays raised above everything else for as long as it's open -
including a program that opens *while* it's up. The launcher deliberately
never uses an `XGrabKeyboard` (see its own header comment for why), so it
depends entirely on actually holding X input focus, on top, to be usable
at all; Kohiko re-raises it after every single window-stacking change for
exactly that reason, the same way `Super+N`'s notepad below does.

The application list (from `/usr/share/applications/*.desktop`) and the
file index (from `$HOME`) are both cached in memory rather than
re-scanned on every `Super+D` - walking your entire home directory on
every keystroke would make the launcher feel slow. Install something
new, or add/remove a file, and it won't show up until that cache is
refreshed - either `Super+Shift+D` or `kohikoctl reloadlauncher` does
that immediately, live, with no restart required.

## The notepad (Super+N)

`Super+N` toggles a small native scratch-notes box: free-form multi-line
text (typing, `Enter` for a new line, `Backspace`/`Delete`,
`Ctrl+Backspace`/`Ctrl+Delete` for whole-word deletes, arrow keys,
`Home`/`End`), saved automatically to `~/.config/kohiko/notepad.txt` and
restored from there on every restart. `Escape` hides it again (saving on
the way out) without closing or discarding anything.

The bar shows a `[N]` indicator whenever the notepad has any saved content
or is currently open - a bracketed-letter indicator in the same style as
the scratchpad's `[S]`, rather than an actual emoji glyph, since relying on
one specific emoji being installed (and rendering as more than a
missing-glyph box) is fragile in a way a plain letter never is.

Deliberately, this is *not* "spawn a text editor into a hidden special
workspace and toggle it," which is the common pattern in the Hyprland
ecosystem for this kind of quick-notes utility - it's a genuinely native
widget with its own tiny text buffer, again with no toolkit or external
process involved. The trade-off is a correspondingly small feature set: no
undo, no selection, no copy/paste - it's meant for jotting something down,
not replacing a real editor.

Both the launcher and the notepad always open centered on whichever
monitor currently has the mouse cursor (falling back to the focused
monitor, then the primary one, if the cursor is outside every monitor) -
not pinned to a single monitor regardless of where you're actually
working.

## The power menu

Every bar has its own `[Power]` button on the right, next to the clock.
Clicking it opens a small popup directly under the button with exactly
three rows - Shutdown, Restart, Suspend - each running a plain shell
command configured via `power.shutdown_command`/`power.restart_command`/
`power.suspend_command` (`systemctl poweroff`/`reboot`/`suspend` by
default; see `config/default.conf`). Click a row to run it, or click
anywhere else / press `Escape` to dismiss the menu without running
anything. Whether Suspend locks the screen first is governed by
`lockscreen.after` - see [Suspend integration](#suspend-integration).

## Native lock screen

`Super+Shift+L`, `kohikoctl dispatch lock`, automatically right before
Suspend, or automatically after a period of no input (depending on
`lockscreen.after`/`lockscreen.idle_timeout_minutes` - see below) locks
the screen: full-screen on every monitor, a password field with hidden
input, `Escape` clears whatever's typed (it never unlocks), `Enter`
authenticates. No dependency on `i3lock`, `betterlockscreen`, or any
other external locker.

`lockscreen.idle_timeout_minutes` (0 by default, disabled) locks
automatically after that many minutes with no keyboard/mouse input
anywhere on the display, independent of `lockscreen.after`'s own
Suspend/startup triggers - so, for instance, `lockscreen.after=manual`
plus `lockscreen.idle_timeout_minutes=10` locks after 10 idle minutes
or on request, but never merely for suspending. Idle time is measured
via the X11 XScreenSaver extension (`XScreenSaverQueryInfo` - not
Kohiko watching its own windows' input, which would miss activity in
any other application entirely), checked roughly once a second; if
that extension isn't available at build or run time, idle-timeout
locking simply never fires; every other lock trigger is unaffected.
Watching a video (see [Display sleep](#display-sleep) below) keeps the
*display* on, but doesn't by itself reset this timer or keep the
session unlocked forever. Still fully subject to `lockscreen.after=
never`, which disables locking altogether, on-demand or automatic.

Authentication goes through PAM, using a service named `kohiko` - install
`pam/kohiko` from this repo as `/etc/pam.d/kohiko` first (see
[Building](#building)). Without it, PAM falls through to
`/etc/pam.d/other`, and what happens next depends on your distro:
Arch/Fedora/RHEL ship a strict deny-everything `other`, so the lock
screen shows up and runs but no password is ever accepted; Debian/Ubuntu's
default `other` just includes the same common-auth stack a real password
would already pass, which means an *unconfigured* lock screen could still
silently accept the right password there. Either way, install the file
rather than relying on whatever your distro's fallback happens to do.

If the current user's account has no password configured at all, locking
unlocks immediately - checked by attempting to authenticate with an empty
password right then, the same way a real attempt would be, rather than
parsing `/etc/shadow` directly.

While locked, Kohiko holds an active keyboard and pointer grab - not just
window stacking and real X input focus the way the launcher/notepad get
away with. Stacking alone wouldn't stop Kohiko's own global hotkeys
(bound as passive grabs on the root window) from still firing underneath,
or a misbehaving client that calls `XSetInputFocus` on itself directly
from stealing keystrokes - including the password itself.

Whatever's typed is wiped from memory (overwritten with zeros, not just
`.clear()`'d - see `Utils::SecureErase`) the moment it's no longer
needed: right after a successful authentication, right after a failed
one, and on `Escape`. This is a best-effort mitigation, not a guarantee -
it can only ever reach Kohiko's own copy of the string, not anything a
prior reallocation or swap may already have copied elsewhere - but it
means a plaintext password doesn't just sit in Kohiko's own heap for the
rest of the session the way a plain `clear()` would leave it.

## Suspend integration

`lockscreen.after` controls when the lock screen engages automatically:

- `never` - not even the manual lock command/keybind will lock (a
  deliberate full opt-out - e.g. a trusted single-user machine that
  doesn't want a lock screen at all).
- `manual` - only locks when you ask it to (`Super+Shift+L`/`kohikoctl
  dispatch lock`); Suspend does not lock automatically.
- `suspend` (the default) - also locks automatically right before
  Suspend.
- `always` - also locks once at Kohiko startup, on top of `suspend`'s
  automatic-before-Suspend behavior - so a freshly started (or just
  restarted) session is never reachable unlocked either.

Suspend integration itself doesn't depend on `logind`/DBus sleep
signals: since Kohiko itself is what spawns `power.suspend_command` (see
[The power menu](#the-power-menu)), it locks the screen *before* issuing
that command rather than trying to detect the resume afterward. Suspending
freezes the whole machine - X server, Kohiko, and every active grab
included - and resumes it exactly as it was, so "lock right before
suspending" and "the lock screen is already there the instant it wakes
back up" end up being the same thing, with no separate wake-detection
needed at all. If the account has no password configured, the lock screen
still briefly appears then unlocks immediately, same as any other time it
locks - see above.

Configurable via `lockscreen.*` in `config/default.conf`: background
color, an optional background image (stretched to fill each monitor) and
an optional logo (centered above the password field, drawn at a fixed
size), foreground/field/error colors, and font (falls back to
`general.font` if unset); a clock and date (each independently toggled
on/off, with their own `strftime()` format string); and independent
toggles for showing the username and/or hostname above the password
field (shown as `user@host` if both are on).

**A security note, same as every other X11 screen locker's own
documentation says of itself:** this locks the X *session* - it's exactly
as trustworthy as X11 itself is against someone with hardware access to
the machine (a VT switch, physically power-cycling it, or ptrace-level
access to the process from another account), which is a limitation of
the X11 security model generally, not something specific to Kohiko's own
implementation.

## Display sleep

By default (`general.inhibit_sleep_during_playback=true`), Kohiko keeps
the display from powering off via DPMS while there's a reason it
shouldn't, and otherwise leaves DPMS's own configured timeout (set with
`xset dpms <standby> <suspend> <off>`, same as on any other X11 window
manager - Kohiko doesn't set or manage that timeout itself) completely
alone:

- **D-Bus inhibit (the primary mechanism).** Kohiko provides the
  standard `org.freedesktop.ScreenSaver` and `org.freedesktop.
  PowerManagement` `Inhibit(application_name, reason) -> cookie`/
  `UnInhibit(cookie)` interfaces on the session bus - the same ones
  browsers (a YouTube tab in Firefox or Chromium), video players (VLC,
  mpv), and presentation software already call into on every major
  desktop specifically to keep the screen awake while they're actively
  playing, whether or not their window happens to be fullscreen. If
  something else - a full GNOME/KDE session's own screensaver service,
  say - already owns those names, Kohiko backs off entirely rather
  than fight over them; it only becomes the provider on a session that
  doesn't already have one (a bare `startx`, for instance). If an
  inhibiting application crashes without calling `UnInhibit`, its
  inhibit is released automatically the moment Kohiko notices that
  application disappear from the bus, so a crash can never wedge the
  display awake indefinitely.
- **Fullscreen fallback.** Whether or not the D-Bus service above is
  available, Kohiko also inhibits sleep any time a currently-visible
  window is fullscreen - covering content that doesn't (or, running an
  older version, can't yet) make the D-Bus call itself.

Either way, inhibiting works by periodically telling the X server
"activity just happened" (`XResetScreenSaver`) - exactly what real
keyboard/mouse input would do, and exactly the same idle counter DPMS's
own power-down timers are driven by - rather than ever touching DPMS's
configuration directly. The moment neither condition above still holds
(the video stops, the fullscreen window closes, every D-Bus inhibit is
released), this simply stops happening, and whatever idle time has
genuinely elapsed since resumes counting completely normally toward the
display's real timeout - the display still sleeps right on schedule once
you're actually away from the keyboard, exactly as if this feature
didn't exist at all.

## Fonts and languages

The bar, launcher, and notepad all draw their own text through Xft/
fontconfig (`include/Font.h`), the one dependency this project takes on
beyond plain libX11 - not for decoration, but because classic X11 core
fonts (the old `XCreateFontSet`/`Xutf8DrawString` approach this replaced)
essentially never have real glyph coverage for anything beyond Latin text
on a modern system, which meant Cyrillic, CJK, and most other scripts
silently rendered as missing-glyph boxes no matter what font name was
requested.

`general.font=` names one font, as a fontconfig pattern - not an XLFD name:

```ini
general.font=monospace:pixelsize=14
```

That font only has to cover whatever script *you* mostly type in - any
character it doesn't have (Ukrainian on a Latin-only font, Chinese on
almost any font, ...) is automatically looked up against every other font
installed on the system and drawn with whichever one actually has that
glyph, the same per-character fallback technique dwm's own Xft patch uses.
There's nothing to configure per-language: install a font that covers the
script you need (e.g. `noto-fonts-cjk` on Arch, `fonts-noto-cjk` on
Debian/Ubuntu, for Chinese/Japanese/Korean) and it's picked up automatically
the next time Kohiko starts.

If a codepoint genuinely isn't covered by any installed font, it still
falls back to drawing with `general.font=`'s own font, which typically
means a missing-glyph "tofu" box - that's a "no such font is installed"
problem, not something Kohiko's fallback logic can work around further.

## kohikoctl / IPC

Kohiko listens on a Unix socket (`/tmp/kohiko_<DISPLAY>.sock`) for simple
line-based commands, in the spirit of `hyprctl`:

```sh
kohikoctl dispatch workspace 3        # anything you could put after `bind=... `
kohikoctl dispatch close
kohikoctl dispatch focusmonitor right # see Multi-monitor above
kohikoctl clients                      # JSON: every managed window
kohikoctl monitors                     # JSON: detected monitors
kohikoctl activewindow                 # JSON: the focused window, or null
kohikoctl tree                         # JSON: the focused monitor's workspace's BSP tree (or `tree <id>` for any workspace)
kohikoctl reload
kohikoctl reloadlauncher              # re-scan applications/files live, no restart needed
kohikoctl quit
```

`dispatch` accepts exactly the same action text as a `bind=` line (minus
the key combo), so anything you can bind to a key you can also trigger
from a script or a keybinding daemon that doesn't know about Kohiko
directly.

## Architecture

This section covers the WM core specifically. For how this fits
together with the shared UI toolkit, the companion apps
(`kohiko-settings`/`kohiko-audio`/`kohiko-network`/`kohiko-bluetooth`),
IPC, and configuration as a whole, see
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

Roughly the file layout the project was designed around, one responsibility each:

| File                       | Responsibility |
|----------------------------|----------------|
| `BSPTree` / `BSPNode` / `BSPLeaf` / `BSPSplit` | The tree itself: placement-aware insert-next-to-focused (natural direction, then the other direction, then shrinking other tiles - never below `general.min_tile_*`), remove-and-collapse, swap, resize, rotate, flip, neighbor search, hit-testing, JSON dump |
| `LayoutEngine`             | Walks the tree and turns ratios into pixels (gaps, borders, smart gaps/borders) |
| `WindowManager`            | Coordinator - owns everything else, sequences the actual X11 event handling |
| `KeyboardManager`          | Config binds -> grabbed keys -> `Command`s. No other logic. |
| `MouseManager`             | The Super+drag state machine described above, plus routing ambient pointer motion to monitor-focus-follow when no drag is active |
| `Animator`                 | The small rect-tween stepper behind the Swap-drop animation |
| `Launcher`                 | The native `Super+D` input box |
| `Notepad`                  | The native `Super+N` scratch-notes box, with disk persistence |
| `EventDispatcher`          | One `switch` over every X11 event type |
| `XConnection`              | The only file that calls Xlib directly (almost) |
| `WorkspaceManager` / `Workspace` | Owns every workspace (and its own independent `BSPTree`) for the process lifetime - no notion of a single "current" one; see [Multi-monitor](#multi-monitor) |
| `WindowRepository`         | `WindowID -> ManagedWindow*`, plus the `Focused()/Floating()/Scratchpad()/Visible()` filters |
| `ManagedWindow`            | Everything Kohiko knows about one window |
| `Config` / `ConfigParser`  | The `key=value` file, with repeatable keys for `bind=`/`exec.*=`/`windowrule=`/`monitor=` |
| `WindowRule`                | Parses/matches `windowrule=` lines - see [Window rules](#window-rules) |
| `IPCServer` / `kohikoctl`  | The Unix-socket control protocol and its CLI client |
| `Bar`                      | One instance per monitor - its own workspaces/active-highlight, a title (focused monitor only), scratchpad/notepad indicators, clock, transient notifications - plain Xlib text, no toolkit |
| `MonitorManager` / `Monitor` / `MonitorRule` | XRandr detection and hotplug, one monitor's geometry/`WorkArea()`/active workspace, `monitor=` rule parsing - see [Multi-monitor](#multi-monitor) |
| `PowerMenu`                | The bar's `[Power]` popup - exactly Shutdown/Restart/Suspend, see [The power menu](#the-power-menu) |
| `LockScreen` / `Authenticator` | The native lock screen and its PAM authentication (run in a short-lived forked child, never inline in the main process) - see [Native lock screen](#native-lock-screen) |
| `ConfigSchema`             | Metadata about `Config`'s keys (category/group/type/default/description/allowed values) - what Kohiko Settings actually renders from; `Config` itself doesn't reference this at all, see [Kohiko Settings](#kohiko-settings) |
| `ConfigWriter`             | Kohiko Settings' write path back into `kohiko.conf` - edits existing lines in place rather than regenerating the file, see [Kohiko Settings](#kohiko-settings) |
| `SettingsWindow`           | `kohiko-settings` itself - a separate ordinary application, not part of the `kohiko` process, see [Kohiko Settings](#kohiko-settings) |

A `BSPNode`'s `Geometry()` is its raw tree-partition rect (no gaps - used
for hit-testing and neighbor search because it tiles the workspace with no
dead zones); a `ManagedWindow`'s `Geometry()` is the actual on-screen rect
after gaps and border are subtracted. Swapping re-points which window a
leaf refers to rather than touching any rect, which is what makes it safe
to lay out again immediately afterward - `Animator` then plays that
transition back over a couple of frames instead of applying it instantly,
which is the only place Kohiko's "no decorative animation" rule allows
motion at all.

Before a window is ever inserted, `BSPTree::Insert()`'s placement-aware
overload (and `HasSpaceForAnotherWindow()`, its non-mutating probe of the
same logic) tries, in order: the anchor's natural split direction; the
*other* split direction (a wide-but-shallow or tall-but-narrow anchor can
easily fit one way and not the other); and finally shrinking other tiles
- nearest to the anchor first, walking outward toward the root - but
never past `general.min_tile_width/height`, or a leaf's own declared
`WM_NORMAL_HINTS` minimum if it's larger. If none of that finds room,
`WindowManager` falls back to another workspace or, failing that,
floating, instead of ever tiling a window into an unusably small space.
The same escalating logic protects tiled windows afterward, too: a
window that keeps sending `ConfigureRequest`s asking for something other
than the tile it was actually given (`general.tiling_misbehavior_threshold`
in a row) gets pulled out of the tree and switched to floating -
`general.tiling_misbehavior_fallback` - rather than being forced back
into a tile it's already shown it won't render into correctly.

## Done

- **BSP tiling** with `Super+LMB` swap-drag and `Super+RMB` resize-drag,
  rotate/flip, and the misbehaving-client fallback described above.
- **Multi-monitor**: independent per-monitor workspaces, focus-follows-
  mouse per monitor, one bar per monitor, and live hotplug - connecting,
  disconnecting, or reconfiguring a monitor re-detects the whole layout,
  rebuilds bars, and re-homes any floating window left positioned off
  every remaining monitor, without a restart.
- **Adoption of already-mapped windows at startup** - restarting Kohiko
  (or starting it into a session where other windows are already open)
  manages whatever's already there exactly like a window opened normally:
  tiled or floated per the same window rules, skipping only
  override-redirect windows and other panels/docks (`_NET_WM_WINDOW_TYPE_DOCK`).
- **The launcher (`Super+D`) and notepad (`Super+N`)** open centered on
  whichever monitor the cursor is on - falling back to the focused
  monitor, then the primary one, if the cursor is outside every monitor.
- **The scratchpad** reliably keeps real X input focus across workspace
  switches, so releasing it (`Super+Space`) or typing into it keeps
  working no matter how many times you've switched away and back since
  showing it.
- **The notepad**: persisted multi-line text, a stable caret (drawn as
  its own overlay rather than an inline character, so it doesn't reflow
  the line or vanish depending on font), Home/End, and Ctrl+Backspace/
  Ctrl+Delete word deletion.
- **`kohikoctl` / IPC**: `reload`, `workspace <N>`, `movetoworkspace <N>`,
  `launcher`, `notepad`, `scratchpad`, `close`, `focus <left|right|up|down|next|prev>`,
  `move <left|right|up|down>`, `rotate`, `flip`, `focusmonitor`/
  `movetomonitor <left|right|up|down|N>`, `exec`, `quit` - see
  [kohikoctl / IPC](#kohikoctl--ipc).
- **EWMH support**, the system tray, autostart, per-window rules
  (`windowrule=`), per-monitor rules (`monitor=...,workspace=N`), and
  keyboard layout switching - see their own sections above.
- **Session restore** - every window's workspace, tiled/floating
  state, floating geometry, monitor, fullscreen state, and - for a
  tiled window - its actual position in the BSP layout relative to
  its neighbours are all remembered across a restart and reapplied
  when it comes back up, so the layout you see right after logging
  back in closely resembles what you actually had before logging out.
  Keyed off the window's own X11 ID (stable across a Kohiko-only
  restart - see `session.restore_priority` in `config/default.conf`
  for how a conflicting `windowrule=` and saved session are
  reconciled). Restoring a tiled window's exact former position is
  necessarily best-effort: if the window it was next to hasn't
  reopened yet this session, it's placed the ordinary way instead,
  same as any other new window. Also covers being stopped via
  `SIGTERM`/`SIGINT` (a session manager restarting it, or a plain
  `pkill kohiko`), not just a clean `kohikoctl quit`.
- **Adaptive placement** - learns, per application, which workspace
  and which side of the tiling layout you repeatedly move its windows
  to by hand (Super+Shift+`<N>`, a Super+LMB drag, or
  Super+Shift+`h`/`j`/`k`/`l`), and starts placing new windows of that
  application there automatically once the pattern is clear and
  consistent - during an ordinary session, not just after a restart.
  A one-off manual move never immediately becomes a habit; it takes a
  genuinely repeated, consistent pattern (see `general.
  adaptive_placement` in `config/default.conf`) before Kohiko starts
  predicting anything, and it only ever fills in when neither an
  explicit `windowrule=` nor a precise Session Restore match already
  has an opinion. Turn off with `general.adaptive_placement=false` to
  place every new window using only the ordinary rules, with no
  learned habits at all.
- **A power menu on every bar** - shutdown/restart/suspend, nothing
  else - see [The power menu](#the-power-menu).
- **A native lock screen** - PAM-authenticated, no `i3lock`/
  `betterlockscreen` dependency, configurable clock/date/hostname/
  username display, a `lockscreen.after` setting governing exactly when
  it engages automatically (`never`/`manual`/`suspend`/`always`), and a
  best-effort secure wipe of the typed password from memory right after
  it's used - see [Native lock screen](#native-lock-screen) and
  [Suspend integration](#suspend-integration).
- **Kohiko Settings** (`kohiko-settings`) - a native configuration GUI,
  installed automatically alongside `kohiko`/`kohikoctl` with its own
  `.desktop` entry and icon: a category sidebar, settings grouped under
  sub-headings, search, an (i) info icon per setting (description,
  default, allowed values, recommendation), inline validation, and
  Save/Reset to Default - editing `kohiko.conf`'s existing lines
  in place rather than regenerating the file, so hand-editing the same
  file remains fully supported alongside it. See
  [Kohiko Settings](#kohiko-settings). Every text field - a plain
  setting, or a structured row below - places the caret exactly where
  you click, not just at the start or end.
- **Structured `windowrule=`/`monitor=` editors in Kohiko Settings** -
  each rule is its own row (an action button you click to cycle
  through float/tile/fullscreen/no-fullscreen/workspace, plus
  `class:`/`instance:`/`title:` text fields for a window rule, or an
  output-name field and a workspace number for a monitor rule), with
  its own "+ Add rule" button and a `x` to remove a row - no need to
  remember the underlying syntax by hand, even though what actually
  gets saved to `kohiko.conf` is exactly that same syntax.
- **Idle-timeout locking** - `lockscreen.idle_timeout_minutes` locks
  the screen automatically after that many minutes with no keyboard/
  mouse input anywhere on the display, independent of `lockscreen.
  after`'s own Suspend/startup triggers, and of whether anything is
  actively inhibiting display sleep (see the next item) - watching a
  video keeps the *display* on, but doesn't by itself keep the session
  unlocked forever. 0 (the default) disables it; still fully subject
  to `lockscreen.after=never`.
- **Display-sleep inhibition** - Kohiko provides the standard
  `org.freedesktop.ScreenSaver`/`org.freedesktop.PowerManagement`
  D-Bus `Inhibit`/`UnInhibit` interfaces itself (whenever nothing
  else - a full desktop environment's own session services, say - is
  already providing them), so any application that already knows how
  to ask a desktop not to sleep the screen while it's active - browsers
  playing video (a YouTube tab in Firefox or Chromium), VLC, mpv,
  presentation software - keeps the display awake for exactly as long
  as it says to, whether or not its window happens to be fullscreen.
  If an inhibiting application crashes without releasing its inhibit,
  it's released automatically the moment Kohiko notices that
  application disappear from the bus, so a crash can never wedge the
  display awake indefinitely. A plain X11 check ("is a currently-
  visible window fullscreen") is the fallback for content that isn't
  D-Bus-aware. Never disables DPMS itself, and never delays a genuinely
  idle display from sleeping on schedule - turn off with `general.
  inhibit_sleep_during_playback=false` if you'd rather the display's
  configured timeout applied completely unconditionally.
- **Native audio, network, and Bluetooth apps** (`kohiko-audio`,
  `kohiko-network`, `kohiko-bluetooth` - see
  [Audio, network, and Bluetooth](#audio-network-and-bluetooth)) with
  matching tray widgets, built on PipeWire, NetworkManager, and BlueZ
  respectively - Kohiko still doesn't replace any of those, just adds
  a native UI in front of each.

## Planned

Nothing currently planned - see [Done](#done) for what shipped most
recently.

## Intentionally unsupported

- **The scratchpad holds one window at a time, by design** - assign a
  new one only after closing (or `Super+Space`-releasing) the current
  occupant. This isn't planned to change; a single scratchpad slot is
  the whole point of it staying simple.
- **The notepad is intentionally minimal** - no undo, no text
  selection, no copy/paste. It's a quick-notes box, not a text editor,
  and won't grow into one.
- **Moving a floating window around isn't bound to anything, by
  design** - it spawns centered and stays wherever the client puts it
  or wherever you leave it via its own controls.
- **`general.tiling_misbehavior_*` only ever detects fighting via
  repeated, conflicting `ConfigureRequest`s** (see `config/default.conf`)
  - the only signal X11 actually gives a window manager for "this
  client isn't happy with the geometry it was given." It can't see
  rendering glitches directly, so a client that visually misrenders
  without ever asking to be resized won't trip it; `windowrule=float`
  (or `=tile`) on that specific application remains the direct fix for
  those.
