# Changelog

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
