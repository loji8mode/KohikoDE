# Changelog

## Version 0.20.7

Release date: 2026-09-10

### Fixed
- **Regression from 0.20.6's `Bar::Redraw()` optimization: the bar's
  tray icons and active-workspace highlight could get stuck showing
  stale or blank content - "only rendering when affected by
  something, and only sometimes" - until an unrelated state change
  (a workspace switch, a notification, a tray change) happened to
  trigger a full repaint anyway.** Reported live (with screenshots)
  after building and running 0.20.6. Root cause: 0.20.6 made
  `Bar::Redraw()` skip the full bar repaint when nothing about the
  bar's *logical* state (workspace, tray width, notification text,
  etc.) had changed since the last full repaint, repainting only the
  clock's own rectangle instead. `WindowManager::HandleExpose()` -
  called whenever an X11 `Expose` event reports that some region of
  the bar's window was just uncovered (most commonly: something had
  briefly overlapped the bar and moved away or closed) - was still
  calling `Redraw()` with no way to bypass that skip. An Expose event
  says nothing about whether the bar's *logical* state changed; it
  only means the window's actual on-screen pixels in that region can
  no longer be trusted, which 0.20.6's `Show()`/`Configure()` handling
  had already correctly accounted for, but this call site was missed.
  When an Expose event landed on a tick where nothing logical had
  changed, the fast path correctly-by-its-own-rules left everything
  except the clock stale, exactly matching the report. Fixed by adding
  a `forceFullRepaint` parameter to `Bar::Redraw()` (default `false`,
  every existing call site unaffected), with `HandleExpose()` as its
  one caller - the same "force past the state comparison" pattern
  0.20.6 already used for `WallpaperManager::ApplyToRoot()`'s
  `forceRerender`, applied here for the same class of reason (X11
  makes no content guarantee) rather than a different, more surprising
  one. Verified with a new pixel-level regression test
  (`tests/test_bar.cpp`, 5 new checks) that reproduces the exact bug
  mechanism directly: deliberately paints garbage onto the live bar
  window (simulating what real Expose damage leaves behind) and
  confirms via `XGetImage` pixel readback that an ordinary `Redraw()`
  correctly leaves it damaged (proving the bug's mechanism, not just
  asserting the fix), while `Redraw(forceFullRepaint=true)` - what
  `HandleExpose()` now calls - reliably restores the real, correct
  pixel every time. A live attempt to reproduce the original report
  end-to-end by overlapping a real window over the running bar and
  moving it away was not achieved in the sandbox used for this fix -
  Kohiko's own BSP tiling keeps ordinary windows from ever overlapping
  the bar's reserved screen space, so triggering a genuine Expose
  event on it live would need the lock screen or a bar-adjacent popup
  specifically, not a plain tiled window; this is noted honestly as
  unverified live end-to-end, distinct from the pixel-level mechanism
  test above, which does directly verify the fix.

## Version 0.20.6

Release date: 2026-09-09

### Performance

Full performance audit of the running desktop (idle, workspace
switching, focus, window create/close/move, launcher, settings, lock,
network/Bluetooth tray, and more - see this session's full findings in
`PROJECT_HISTORY.md`). Environment: single-core sandbox VM, Xvfb, real
`kohiko` 0.20.5 with real `mock_networkmanager.py`/`mock_bluez.py`
D-Bus backends, real PipeWire, real X11 client windows tiled by the
BSP. `perf` was unavailable (no matching kernel package for the
sandbox's kernel); `strace -c`/`strace -f -y`, `pidstat -t`, and
`/proc/[pid]/stat` tick sampling were used instead.

Every scenario measured except one turned out to already be
efficient - appropriately costed for what it does, with no recurring
or unconditional background waste. The one genuine, measured,
**recurring** bottleneck found:

- **`Bar::Redraw()` (src/Bar.cpp) unconditionally repainted the
  *entire* bar - background fill, every workspace label,
  scratchpad/notepad indicators, tray reposition, power button, and
  the clock - on every single call, including the once-a-second call
  `WindowManager::Tick()` makes purely to keep the clock's seconds
  live.** In the idle steady state - by far the most common reason
  `Redraw()` runs at all - everything except the clock's own digits is
  pixel-identical to what's already on screen. `strace -f -y` on a
  live, fully idle session confirmed one real, unconditional X11
  round trip every second, forever, for the life of the process:
  `poll` -> `writev` (836 bytes, a batch of queued drawing requests) ->
  `recvmsg` (32 bytes, a reply some call in this path expects) ->
  `recvmsg` (drain). Fixed by having `Redraw()` compare the bar's
  current state (workspace count/active, scratchpad/notepad flags,
  notification text, tray width, and the clock text's own pixel
  width) against what it last actually painted; when only the clock's
  digits differ, it repaints just the clock's own rectangle instead of
  the whole bar. Any real change - including the bar having been
  hidden and shown again, since X11 doesn't guarantee a plain window's
  pixels survive an unmap/remap, and any geometry change via
  `Configure()` - still takes the exact original full-repaint code
  path, unmodified. Measured live, on the same running process, after
  the fix: the idle tick's `writev` dropped from 836 bytes to 368
  bytes (a real `strace -f -y` capture, not an estimate) - roughly
  56% less X11 protocol traffic per idle tick. A dedicated regression
  test (`tests/test_bar.cpp`, `test-bar`/`Bar` in the Makefile/CMake)
  measures the same property more precisely via `XNextRequest()`'s
  request-sequence counter: a full repaint issues on the order of
  20-40 X11 requests; a clock-only tick against the same running bar,
  around 11 - and confirms the fast path never engages when something
  real changed (a workspace switch, `Hide()`+`Show()`, or a `Configure()`
  geometry change all correctly force a full repaint on the very next
  call). Raw CPU-tick measurements (`/proc/[pid]/stat`, 10ms
  resolution) could not distinguish before from after in this
  single-core sandbox - both were already below that measurement
  floor at idle - so this is reported honestly as an X11-protocol-
  traffic reduction, not a raw CPU-percentage one; the same
  unconditional-wakeup pattern is a well-understood real-hardware
  battery-life cost independent of what it registers as in a
  synthetic single-core VM.
- **`WallpaperManager::ApplyToRoot()` (src/WallpaperManager.cpp)
  unconditionally re-rendered the entire root wallpaper composite on
  every call - including a workspace switch where neither the old nor
  new workspace has its own `wallpaper.workspace=` rule, by far the
  common case, where the resolved wallpaper is provably identical to
  what's already on screen.** Found independently while investigating
  the `Bar::Redraw()` fix above - a different mechanism from the
  0.20.5 inotify watch-recreation regression (that fix, and its tests,
  are untouched by this one). `strace -f -y` on a live session showed
  the wallpaper PNG mmap'd *twice* (Imlib2's own loader behavior, not
  something this code controls) plus an 8MB-class XSHM-style buffer
  allocated and freed, on every single workspace switch, even when
  nothing about the resolved wallpaper actually changed. Fixed the
  same way as `Bar::Redraw()`: `ApplyToRoot()` now compares every
  monitor's resolved wallpaper (path, scale mode, geometry) plus the
  overall composite size and background color against what it last
  actually composited, and skips the decode/render entirely when
  nothing differs. A new `forceRerender` parameter (default `false`)
  exists for exactly one caller, `HandleWallpaperFileChanged()` -
  `Poll()` reporting a real on-disk change means the *file's contents*
  changed, which the resolved path/mode comparison alone can't detect
  (it's still the same path either way), so that one call site always
  forces a real render rather than relying on the state comparison -
  this is what keeps live-reload (0.20.5's own fix) working correctly
  after this change. Measured live: a workspace switch between two
  workspaces sharing the same wallpaper dropped from 6 mmap/munmap
  calls (2 renders' worth) to 0, confirmed via the identical
  `strace -f -y` capture used to find the problem. A dedicated test
  (`tests/test_wallpapermanager.cpp`'s "Skipping a genuinely redundant
  re-render" section, 8 new checks, X11-optional/gracefully-skipped
  like the Bar test) measures the same property via `XNextRequest()`:
  a full render costs ~13-14 X11 requests, the skip path costs 1 (a
  ~93% reduction) - and separately proves a genuine wallpaper change
  and `forceRerender=true` both still force a full render every time.
  Verified live in both directions: a real `wallpaper.workspace=`
  override still correctly renders the different image on switching
  to that workspace, and editing the wallpaper file in place still
  triggers a full re-render of the new content (confirmed via the
  mmap'd file's byte count matching the new file's actual size, not a
  stale cache hit).

Two related things were measured and deliberately left untouched,
since neither showed a demonstrated recurring cost once the above was
fixed - see `docs/ARCHITECTURE.md`'s "Known limitations" section for
the full reasoning on each: `SystemTray::Reposition()` itself still
unconditionally issues an `XMoveResizeWindow` on every call, but its
only remaining call sites after this fix are real tray dock/undock
events, not a per-second cost; and a literal mouse-drag window-move
was not reliably reproducible live in this sandbox (a keyboard-driven
BSP-relayout proxy was used instead and measured negligible cost).

## Version 0.20.5

Release date: 2026-09-08

### Fixed
- **Idle CPU regression introduced by the 0.20.4 disk-loss/restoration
  cycle: `kohiko` pinned one thread at ~94% CPU on a completely idle
  desktop, with `strace -p <pid> -e trace=inotify_add_watch,
  inotify_rm_watch,openat,close -tt` showing a tight, self-sustaining
  loop 15-25 times a second - `inotify_rm_watch()`, then
  `inotify_add_watch()` on `/usr/local/share/kohiko/wallpapers` with a
  new, incrementing watch descriptor each time, then an `openat()`/
  `close()` of the current wallpaper file - with zero actual
  filesystem activity required to sustain it.** Root cause:
  `WallpaperManager::ApplyToRoot()` called `RefreshWatches()`
  unconditionally on every single invocation, including ones
  triggered only because `Poll()` reported a change, and
  `RefreshWatches()` unconditionally tore down every existing inotify
  watch and rebuilt it from scratch every time it ran, regardless of
  whether the set of directories that needed watching had actually
  changed. `inotify_rm_watch()` generates a real, readable
  `IN_IGNORED` event on the same inotify fd it was called against
  (confirmed directly against the kernel with a standalone reproducer
  - this isn't a Kohiko-specific quirk, it's documented inotify(7)
  behaviour) - and `WallpaperManager::Poll()` treated *any*
  successfully-read bytes as "the wallpaper changed on disk, re-render
  it", without ever inspecting the event's `mask` to tell that
  self-generated bookkeeping notification apart from a genuine file
  change. The result was a closed loop entirely internal to Kohiko:
  `Poll()` reports the `IN_IGNORED` as a change -> `WindowManager`
  calls `ApplyToRoot()` again -> `RefreshWatches()` tears down the
  watch it just built (generating a fresh `IN_IGNORED`) and rebuilds
  it (hence the ever-incrementing watch descriptor in the strace) ->
  repeat, bounded only by how fast the event loop could cycle. Only
  two `ApplyToRoot()` calls anywhere near each other - entirely normal
  during ordinary startup - were needed to seed the first
  `IN_IGNORED`; from there the loop needed no external trigger at all,
  which is why it reproduced on a desktop nobody was touching. Fixed
  two ways: `RefreshWatches()` (now taking a plain resolved-path list
  rather than a `MonitorManager`, so it's unit-testable without X11)
  is now idempotent - it diffs the desired directory set against
  `m_watchedDirectories` and only calls `inotify_rm_watch()`/
  `inotify_add_watch()` for directories that actually need to stop or
  start being watched, leaving an already-correct watch set completely
  untouched; and `Poll()` now parses each `inotify_event` properly and
  no longer counts `IN_IGNORED` towards "something changed", as
  defense in depth for the rare, legitimate case where a watch really
  does need to move (e.g. `wallpaper.default` edited to point at a
  different directory). Live-reload itself is untouched - directory-
  level watching, an in-place edit, and the write-then-`rename()`
  "save" pattern are all still caught exactly as before; see
  `tests/test_wallpapermanager.cpp`'s "Idle-CPU regression" section for
  the regression coverage, which reproduces the exact cascade above
  against real inotify and confirms it terminates immediately rather
  than recreating the watch indefinitely.

## Version 0.20.4

Release date: 2026-08-20

### Fixed
- **The bar's clock (and everything else `WindowManager::Tick()`
  drives - idle-lock/sleep-inhibition checks, launcher/notepad cursor
  blink, notification expiry) could go arbitrarily long without
  updating on a real, ordinarily-busy desktop session, while every
  genuine X11 event kept being handled completely normally in the
  meantime.** Root cause: `EventLoop::Run()`'s `select()` timeout was
  reset to a fresh full interval (1s idle, ~8ms during an animation)
  on *every* loop iteration, regardless of why that iteration ran.
  `Tick()` only ever fired when `select()` returned 0 - a full,
  uninterrupted interval with zero file descriptor activity of any
  kind. Any other fd going ready before that interval elapsed - the
  IPC socket, the app-dir watcher, the lock bridge, the wallpaper file
  watch, and especially `ScreenSaverInhibitor`'s own D-Bus connection,
  which deliberately subscribes bus-wide to every `NameOwnerChanged`
  signal on the session bus rather than filtering to names it actually
  cares about (so it can notice and clean up after a crashed inhibitor
  automatically) - caused the loop to dispatch that fd and restart the
  wait from a full interval again, with no memory of how much real
  time had already passed. `NameOwnerChanged` traffic is ordinary,
  frequent background noise on any real, living session bus (systemd
  user session activation, portals, various helper daemons), so this
  was easy to hit on an actual desktop and easy to miss in a minimal
  test sandbox with almost nothing else on the bus - which is exactly
  why earlier live-session testing in this project's own sandbox never
  caught it. Fixed by tracking an absolute deadline
  (`std::chrono::steady_clock::time_point`) for the next `Tick()`
  call instead of resetting a relative timeout: the pure scheduling
  logic behind this (`TickSchedule::PullForward()`/`IsDue()`/
  `TimeUntilDue()`) was pulled out into small, header-only `inline`
  functions in `EventLoop.h` specifically so it's unit-testable
  without linking the rest of `EventLoop.cpp`'s dependency chain.
  Other fd activity can still make an individual `select()` call
  return early exactly as before, but can no longer push the deadline
  itself later - only real elapsed time reaching it does that.
  Directly demonstrated, not just reasoned about: a live `kohiko-session`
  was run under synthetic D-Bus churn (a background process
  claiming/releasing a bus name every ~150ms, generating a
  `NameOwnerChanged` signal each cycle). Against the pre-fix code, the
  clock stayed frozen at the exact same time across 70+ real seconds
  of continuous churn; against the fixed code, under identical churn,
  the clock advanced correctly by the full real elapsed time (e.g.
  `10:12:52` → `10:14:13` across 81 real seconds).
- **An application launched onto a workspace nothing is currently
  displaying looked exactly like it had failed to open at all** - a
  real process, a real X11 window, correctly tiled/placed and fully
  functional the moment you happened to switch to that workspace, but
  with zero on-screen signal that it had opened anywhere. Root cause:
  `Manage()`'s "a plain top-level window landing on a background
  workspace stays unmapped and unfocused" behaviour is itself correct
  and deliberate (a `workspace<N>=` autostart line, a
  `windowrule=workspace:N`, or a learned adaptive-placement habit is
  specifically meant to open quietly without stealing focus from
  whatever the user is doing right now - see Window Placement in
  `README.md`) - but unlike a transient/dialog, which always gets
  pulled into view because it needs its parent to make sense, an
  ordinary top-level window had no equivalent "at least let the user
  know" step at all. Reproduced directly against the shipped default
  config, not a contrived case: `config/default.conf` already
  autostarts multiple applications via one shared line,
  `workspace2=discord Telegram`, onto workspace 2, while a fresh
  session opens showing workspace 1 - the exact "affects several
  different applications that all launch the same way" shape
  originally reported. Confirmed live under Xvfb: adding a third
  application to that identical config line and starting a clean
  session left it running (confirmed via `ps`) but completely absent
  from workspace 1's screenshot; switching to workspace 2 showed it
  sitting there, fully rendered and normal. Fixed by adding a bar
  notification (`WindowManager::ShowNotificationOnMonitor()`, already
  used elsewhere for exactly this class of "something happened
  silently, the user should know" situation - see
  `SwitchWorkspaceOnMonitor()`'s own use of it) whenever a non-
  transient window lands on a workspace nothing is showing: reran the
  identical live reproduction afterward and the bar on workspace 1
  now reads "XTerm opened on workspace 2" for the notification's
  usual ~2.5s window. The placement decision itself is unchanged -
  this fixes the silence, not the (correct, existing) behaviour.
- **Closing a window could leave its surviving neighbours unnaturally
  narrow or stretched**, specifically whenever the neighbour being
  promoted into the freed space was itself a further-split pair, not
  a single window. Root cause, found from a direct user report and
  reproduced exactly as described before any code changed: `A |
  (B / (C|D))` - C and D side by side, sharing a short, wide slot
  under B - closing B promotes the whole `(C|D)` pair as a unit into
  B's old slot. `BSPTree::Remove()`'s collapse step has always
  promoted a survivor straight into its freed slot verbatim, which is
  exactly right when the survivor is a single leaf (see the two
  existing Collapse-on-remove tests in `tests/test_bsptree.cpp`, both
  still passing unmodified), but when the survivor is itself a split,
  its own internal direction was left exactly as it was, even though
  the area it now occupies can be a completely different shape - here,
  B's old slot is much *taller* than the short, wide space C and D
  were originally sized for, so they stayed side by side, each
  squeezed to roughly half the width they should have. Investigated
  the *insertion* side of the algorithm first, deliberately, before
  assuming the fix belonged there: six windows opened one after
  another (both in the natural most-recently-focused pattern and
  forcibly re-anchored to a single fixed window each time) produced
  sensible, non-degenerate proportions throughout, live under Xvfb -
  `DirectionForRect()`'s existing wide-slot-splits-vertically/
  tall-slot-splits-horizontally rule already self-corrects fine on
  fresh inserts. The defect was specifically in collapse, not
  insertion. Fixed with a new placement-aware `BSPTree::Remove(window,
  tilingArea, innerGap)` overload (the original geometry-agnostic
  `Remove(window)` is unchanged and still used wherever geometry
  genuinely isn't available) that, after promoting a survivor,
  recursively re-derives the direction of every split *within* that
  survivor - at any depth - against the area it actually inherited,
  using the exact same `DirectionForRect()` rule fresh inserts already
  use, via the same direction-toggle `Rotate()` uses. Ratios are never
  touched, only direction, so a manually-resized pair keeps its
  relative proportions, just applied to whichever axis now applies.
  All 6 `WindowManager.cpp` call sites updated to the new overload.
  Reproduced live under Xvfb exactly as reported - closing B in `A |
  (B / (C|D))` - before and after: before, C and D stayed side by
  side, each about half their column's width; after, they came out
  stacked top/bottom, each spanning the column's full width, visually
  confirmed via screenshot. 16 new checks in `tests/test_bsptree.cpp`
  cover the exact reported scenario (including a direct side-by-side
  comparison against the old overload, to confirm the fix - not
  incidental test setup - is what changes the outcome), a manually-
  resized ratio surviving the flip, a case where no flip is needed
  (confirming nothing spurious happens), and a 6-window,
  multiple-removal scenario asserting no leaf's aspect ratio ever
  exceeds 3:1 at any depth.
- **Focus could end up on a different window than whatever the mouse
  was actually resting over**, whenever the mismatch was caused by a
  transition where the pointer's own screen position never moved -
  only the content under it did - so neither `HandleEnterNotify()` nor
  `HandlePointerMotion()` ever had anything to react to and reconcile
  the two. Investigated (and live-reproduced under Xvfb, moving the
  pointer to a specific window first, then triggering the transition
  without moving it again) five specific cases, per the original
  report: opening a new window (checked, and deliberately left alone -
  a fresh window grabbing focus regardless of pointer position is
  itself the intended policy, not a bug); switching workspaces
  (confirmed: switching away from and back to a workspace restored
  whichever window was focused *before* leaving, discarding a pointer
  move made in between - `SwitchWorkspaceOnMonitor()`); rearranging
  via a Swap drag (confirmed: `EndSwapDrag()` never touched focus at
  all, so whichever window held it before the drag simply kept it,
  regardless of where the two swapped windows ended up); session
  restore (confirmed: `AdoptExistingWindows()` re-manages each
  surviving window in root's own child z-order, focusing each in turn
  exactly like any freshly opened window, so whichever happened to be
  *last* in that order - not the pointer, and not necessarily whatever
  was focused before Kohiko last exited, which nothing records - is
  what ends up focused); and switching monitors (not empirically
  reproducible in this sandbox's single-output Xvfb, but confirmed by
  code reading: `FocusMonitorCommand()` never moved the pointer, so
  under focus-follows-mouse the very next incidental bit of mouse
  motion on the monitor just switched *away* from would immediately
  switch focus straight back, silently undoing the switch). Fixed with
  a new `WindowManager::SyncFocusToPointer()`, called explicitly right
  after each of the four confirmed transitions: queries the pointer's
  real current position directly (`XConnection::QueryPointer()`, the
  same reasoning already established for `Initialize()`'s and
  `HandleMonitorTopologyChanged()`'s own pre-existing use of it, both
  of which this also upgrades to reconcile *window* focus, not only
  monitor focus, while it's there) and focuses whichever window is
  actually there - floating over tiled, matching every other hit-test
  in this file - under the exact same guards `HandleEnterNotify()`
  itself already uses (`general.focus_follows_mouse`, and Launcher/
  Notepad holding input focus for typing). The monitor-switch case
  needed the opposite direction instead - moving the *pointer* to
  match an explicit focus change, not focus to match the pointer,
  since syncing focus to wherever the pointer already is would just
  undo the very switch being made - so `FocusMonitorCommand()` now
  warps the pointer (`XConnection::WarpPointer()`, new) to the center
  of the newly-focused monitor instead, under the same
  `focus_follows_mouse` guard. Re-verified all three empirically
  testable cases live under Xvfb after the fix, each against an
  explicit before/after or default-baseline comparison, not just "it
  changed to *something*": workspace switch and session restore both
  now correctly focus whichever window the still-stationary pointer
  was actually resting over (session restore specifically checked
  against a same-pointer-position default-adoption baseline first, to
  rule out coincidence); the swap-drag case now correctly focuses
  whichever of the two swapped windows the drop point actually landed
  on. Also confirmed `general.focus_follows_mouse=false` correctly
  suppresses every one of these (session restore checked directly:
  focus stayed on the adoption-order default even with the pointer
  resting over a different window).
- **A Swap drag (rearranging tiled windows) could show a visible
  rendering glitch during the slide-into-place animation** - a chunk
  of one or both windows missing, cut off well short of where it
  should reach, briefly exposing bare wallpaper that neither window
  was actually covering yet. Root cause: the dragged window's
  animation starts from `m_dragCurrentRect`, which tracks the cursor
  at a fixed grab-offset for the whole drag - correct and expected
  while actually dragging (a window carried this way can legitimately
  extend partially off-screen if grabbed away from its own edge and
  dragged far enough - harmless in the moment, since nothing renders
  past the screen edge regardless), but wrong as an animation's
  *starting* rect: the clipped, off-screen portion isn't "there" for
  the tween to visibly grow out of, so its early frames render
  noticeably smaller than they should, and since the swap target is
  sliding in from the opposite direction at the same time, the two can
  visibly fail to meet in the middle. Confirmed live under Xvfb before
  writing any fix: a rapid burst of screenshots taken immediately after
  a drop (this needed catching a ~160ms window, not something a single
  screenshot after a `sleep` would ever show) caught it precisely -
  precise pixel measurement of the captured frame confirmed the dragged
  window's visible edge cut off well short of the screen edge, with a
  gap of untouched wallpaper on the far side. Fixed by clamping
  `fromRect` to the drop monitor's own work area
  (`Rect::ClampedTo()`, pre-existing, already used elsewhere for
  floating windows - this is its first use here) before either
  animation starts, in both the successful-swap and the
  no-valid-target snap-back branches of `EndSwapDrag()`, which share
  the same underlying rect. Re-verified with the identical live
  reproduction afterward: the dragged window's visible width now
  matches its true width from the very first captured frame, with no
  gap anywhere - confirmed by the same precise pixel measurement, not
  just a visual glance. Also specifically re-checked the snap-back
  case (drag far off-screen, release over no valid target) - clean
  there too.
- **A startup/autostart application opening while the screen was
  locked could completely cover Kohiko's own LockScreen** - not
  partially, not a corner peeking through, but the entire lock surface
  (clock, username, password field, all of it) hidden behind an
  ordinary application window, with nothing on screen indicating the
  session was even locked anymore. Confirmed live under Xvfb before
  writing any fix, and it was worse than the report alone suggested: a
  single plain `xterm` opened after locking was enough to hide the
  lock screen completely. Root cause: `LockScreen::Lock()` raises
  itself once, at the moment the screen locks, but nothing ever raised
  it again afterward - `RaiseModalWindows()`, called at the end of
  every `Arrange()` (which `Manage()` itself calls before mapping any
  newly-opened window, autostart included) specifically to give
  something the "last word" on stacking order, only ever considered
  Launcher and Notepad, never LockScreen. Real keystrokes were never
  actually at risk either way - `Lock()`'s exclusive `XGrabKeyboard`/
  `XGrabPointer` (see its own comment for why a cooperative focus
  model isn't enough for a lock screen specifically) means input goes
  to LockScreen regardless of what's stacked where - but a user who
  can't *see* the password field they're typing into is exactly the
  kind of broken this was supposed to prevent. Fixed by having
  `RaiseModalWindows()` check `LockScreen::IsLocked()` first,
  unconditionally, and return immediately if so - nothing else gets a
  say in stacking order while locked, Launcher/Notepad included (never
  reachable while locked in the first place given the exclusive grab,
  but this is the belt to that braces). Re-verified with the identical
  live reproduction afterward, plus the multi-application case the
  original report specifically called for: three separate windows
  opened one after another while locked, lock screen fully intact and
  visible throughout every single one. Also verified end to end past
  where the original report stopped: a real, PAM-authenticated unlock
  correctly restores full normal desktop stacking and focus afterward
  - the window that had been hidden underneath the whole time appeared
  correctly tiled and focused the moment the screen unlocked.
- **A Kohiko that crashed (or was killed) while the screen was locked
  resumed into a completely unlocked desktop** on the automatic
  restart `kohiko-session` performs - no lock screen, no password
  prompt, nothing on screen to say a crash had even happened.
  Discovered while verifying the LockScreen stacking fix just above,
  investigating startup ordering more broadly rather than stopping
  once that specific report was closed: confirmed directly by locking,
  then `kill -9`-ing the running Kohiko process to simulate a crash,
  and watching kohiko-session's own automatic restart come up fully
  exposed - workspaces, bar, everything. Root cause: a restarted
  Kohiko process has no memory of its own of what state the previous
  one was in: `SessionStore` saves window/workspace layout but never
  lock state, and `RecoveryMode`'s own existing crash marker (see its
  header comment) is specifically scoped to startup - it's cleared the
  moment a session reaches a stable running state, long before a much
  later crash mid-session would ever happen. Fixed with a new,
  narrowly-scoped `LockRecovery` class, deliberately the same shape as
  `RecoveryMode` itself: a small marker file, written whenever
  `LockScreen` locks (any trigger - keybind, `kohikoctl`, idle
  suspend, `loginctl lock-session`) and removed whenever it unlocks
  again normally, via the lock-state-changed callback `Initialize()`
  already wires up for an unrelated purpose (relaying to logind). If
  the marker is still there the next time Kohiko starts - the previous
  process ended without ever reaching the "removed" half - Kohiko
  locks again immediately, independent of the `lockscreen.after`
  config setting entirely (this is a safety recovery, not a matter of
  preference). Also cleared unconditionally on a clean
  `WindowManager::Shutdown()`, so a deliberate logout while still
  locked doesn't cause the next, entirely fresh login to come up
  pre-locked for no reason - this is purely about surviving a crash,
  not about remembering lock state across an intentional session end.
  Re-verified live in both directions, not just the one the bug
  report described: locking, then crashing, correctly resumes locked
  (log confirms: "the screen was locked when the previous session
  ended unexpectedly - locking again"); unlocking normally, then
  crashing, correctly resumes unlocked, with no spurious re-lock. 5
  new checks in `tests/test_lockrecovery.cpp`.
- **Mouse-wheel scrolling in kohiko-audio's device list (and,
  sharing the same `ScrollView`, kohiko-network's/kohiko-bluetooth's
  lists) only worked over the sliver of "dead space" between rows -
  hovering over a volume `Slider`, a "Mute"/"Unmute" `Button`, or a
  device row itself and scrolling did nothing.** On a window short
  enough for a device list to overflow (easy to hit well above the
  documented ~420x360 floor with more than two or three devices
  present), this made rows below the fold effectively unreachable:
  input devices, "Advanced Settings", anything past whatever fit in
  the visible area. Root cause, confirmed by instrumenting the actual
  dispatch path in a live Xvfb session rather than assumed from
  reading the code: `UiWindow::HandleEvent()`'s `Button4`/`Button5`
  branch reused the same `HitTest()` ordinary clicks use, which -
  correctly, for a click - always resolves to the *most specific*
  `WantsInput()` widget under the pointer, and called `OnScroll()` on
  that widget. Only `ScrollView` overrides `OnScroll()`; every other
  widget (`Slider`, `Button`, `ListRow`, `TextField`) silently no-ops
  it via the `Widget` base class default. Debug logging added
  temporarily to both the dispatch site and `ScrollView::OnScroll()`
  itself showed the real sequence directly: scrolling over a "Mute"
  button produced a valid, non-null hit target and zero
  `ScrollView::OnScroll()` calls; scrolling over genuine dead space
  produced a null target (a *different*, previously-unnoticed gap -
  see below) and also zero calls. Fixed with a second, purpose-built
  traversal, `Widget::WantsScroll()` (default `false`, overridden
  `true` only by `ScrollView`) and `UiWindow::HitTestForScroll()`,
  which looks for the nearest enclosing widget with `WantsScroll()`
  instead of the deepest `WantsInput()` one - so a wheel event finds
  the scrollable container regardless of what's drawn on top of it at
  that exact pixel. Both traversals are `static` (they never touched
  `UiWindow`'s own state to begin with) and made public specifically
  so they're callable from a test without a live X11 `Display`.
  Separately, `ScrollView` had no visual indicator of any kind that
  content overflowed - even with scrolling now working, a user had no
  way to *discover* there was more below the fold except by trying it
  blindly. Added a minimal overlay scrollbar thumb (`ScrollView::
  ComputeThumbRect()` - pure geometry, factored out for the same
  testability reason as the traversal above - drawn in `Draw()` only
  when content actually overflows), sized proportionally to the
  visible fraction and positioned by scroll offset, with no drag
  interaction of its own (a cue, not a second scrolling mechanism to
  keep in sync with `OnScroll()`'s own clamping). Live-verified in
  Xvfb against a real `kohiko-audio` with actual PipeWire nodes (three
  `Audio/Sink` + two `Audio/Source` virtual devices created via
  `pw-loopback`, not the empty "no devices" state): before the fix,
  eight scroll-wheel events over the "Mute" button at 420x500 produced
  an unchanged screenshot; after, the same input scrolled the list
  down far enough to reveal "USB Condenser Microphone" and "Advanced
  Settings", scrolled back up cleanly, and ordinary clicks (confirmed
  separately: "Mute" correctly flips to a red-toned "Unmute") were
  unaffected, since the click path itself was never touched. 13 new
  checks in `tests/test_scrollhittest.cpp`, covering the exact
  Button-inside-ScrollView reproduction, the dead-space case, a point
  genuinely outside the `ScrollView`, a `Button` with no enclosing
  `ScrollView` at all, and the thumb geometry (no-thumb-when-it-fits,
  top/bottom clamping, and the minimum-size floor for an extreme
  content/viewport ratio).
- **kohiko-network's "Connect" button prompted for a Wi-Fi password on
  every single manual connect to a secured network, even when
  NetworkManager already had a saved, working secret for it - the
  prompt had to be clicked/typed through (even submitting it blank
  happened to work, but only after going through it) every time,
  while an autoconnect reconnection to the same network worked
  silently in the background.** A prior report that the password
  field itself was numeric-only was checked directly against the
  restored source rather than assumed, and found false again, exactly
  as an earlier phase's own investigation already concluded (see this
  same version's "Investigated, not changed" section):
  `PromptForPassword()`'s `KeyPress`
  handling accepts any byte `XLookupString()` returns that isn't a C0
  control character, confirmed both by reading the code and, this
  time, by direct live input - typing `abCD123!@#` into a real modal
  under Xvfb produced ten masked characters, not a truncated/rejected
  result. The *actual* reported behavior traced to
  `NetworkWindow.cpp`'s connect handler, not the field: it called
  `PromptAndConnect()` for literally every access point with
  `secured == true`, with nothing checking whether NetworkManager
  could already supply a secret for that exact SSID. The backend half
  of this - `ConnectToAccessPoint()` reusing an existing saved
  connection's secret when given an empty password rather than
  creating a duplicate - has worked correctly since 0.20.3's
  `BytesToString()` fix; the UI simply never took advantage of it,
  showing the prompt unconditionally regardless of whether anything
  meaningful would happen with what the user typed into it. Fixed
  with `NetworkManagerClient::HasUsableSavedSecret(ssid)`: finds a
  saved `802-11-wireless` connection matching `ssid` the same way
  `ConnectToAccessPoint()` already does, then calls the connection's
  own `GetSecrets("802-11-wireless-security")` - a separate,
  permission-gated D-Bus call NetworkManager deliberately excludes
  from `GetSettings()` for the same reason a settings file wouldn't
  normally embed its own passwords in plain sight - and returns
  whether a non-empty `psk` actually came back. The connect handler
  now only calls `PromptAndConnect()` when this returns false;
  otherwise it takes the same direct-connect path an open network
  already used. Separately, and part of the same "letters, digits,
  symbols, **and pasted text**" requirement: the password modal had no
  paste support of any kind - no `SelectionNotify` handling existed at
  all. Added both conventional X11 paste gestures (Ctrl+V from
  CLIPBOARD, middle-click from PRIMARY), sharing a
  `FilterPastedPasswordText()` helper (pulled out as pure logic, not
  inlined into the X11 handler, specifically so it's testable without
  a live selection round trip) that strips C0 control characters and
  DEL - a trailing newline, near-universal when copying from a
  terminal or password manager, would otherwise become a literal
  character in the password - while passing multi-byte UTF-8 through
  untouched. Live-verified end to end against a purpose-built mock
  NetworkManager D-Bus service (`tests/mock_networkmanager.py`, a real
  `dbus-python`/GLib service on a private, throwaway system bus - not
  a real NetworkManager or any real Wi-Fi hardware) with two saved
  connections (one with a usable secret, one without) and four scanned
  access points: connecting to the one with a saved secret produced no
  second window at all and the call log showed `GetSecrets` -in
  `ActivateConnection` directly; connecting to the one without a
  usable secret produced a real second top-level window (found by
  diffing the X11 window tree - the modal, it turns out, sets no
  `_NET_WM_NAME`/`XStoreName` at all, so searching for it by name finds
  nothing, a real if inconsequential gap noted below); typing directly
  into that modal, pasting into it from a real `xclip`-owned CLIPBOARD
  selection, and middle-clicking a real PRIMARY selection into it all
  correctly appended masked characters matching the source text's
  length. 8 new checks in `tests/test_networkwindow.cpp`
  (`FilterPastedPasswordText()` - control characters, a trailing
  `\r\n`, embedded tabs, UTF-8, DEL, empty input) and 7 new checks in
  `tests/test_networkmanager_live.sh` (a genuine round trip through
  the mock covering all four cases this fix needs to get right: an
  existing usable secret skips the prompt and reuses without a
  duplicate profile; a saved-but-unusable secret still prompts; a
  freshly typed password is persisted via `Update()` on the *same*
  connection rather than a new one; a genuinely new network still
  correctly uses `AddAndActivateConnection()`), gracefully skipped if
  `python3-dbus`/PyGObject aren't available rather than failing the
  suite over missing optional infrastructure.
- **Telegram, FeatherPad, Flameshot, and any other application
  launched the same way could fail to open a usable window at all -
  not a layout glitch, a completely invisible, unmanaged X11 window,
  permanently.** Not assumed to be Telegram-specific, and it wasn't:
  the actual mechanism, once found, applies identically to any
  application, confirmed with a synthetic reproduction rather than
  any specific real one (see the note at the end of this entry on
  why). Root cause, traced through `WindowManager::Manage()` down to
  `XConnection::IsXEmbedWindow()`: 0.20.2's fix for a real, separate
  problem (a soon-to-be-docked system tray icon flashing into a full
  tile for a few frames before `SystemTray::DockIcon()` reparents it
  away) checked only whether a window carried the `_XEMBED_INFO`
  property, and treated that as *permanent, unconditional* proof a
  dock request was coming - if it never actually arrived, `Manage()`
  had already returned early, before adding the window to the
  repository or calling `MapWindow()`, and nothing else in the
  codebase would ever revisit that decision. Confirmed directly: a
  small, configurable, real X11 client
  (`tests/live/x11_test_client.cpp`, built for this investigation, not
  a mock of anything) creating an ordinary top-level window was
  correctly tiled full-screen by a real `kohiko` WM under Xvfb; an
  otherwise byte-for-byte identical window additionally carrying
  `_XEMBED_INFO` stayed frozen at its original creation geometry,
  invisible, indefinitely. Reading `SystemTray.cpp` confirmed the
  *actual* trigger for real tray docking is a separate, asynchronous
  `SYSTEM_TRAY_REQUEST_DOCK` `ClientMessage` sent to the
  `_NET_SYSTEM_TRAY_S<screen>` selection owner - nothing to do with
  the `MapRequest`/`Manage()` path at all - so the property's presence
  was never actually proof a dock request would follow: `_XEMBED_INFO`
  is a general-purpose X11 embedding protocol, not exclusive to system
  trays, and even a genuine tray icon's dock message is a fundamentally
  separate, asynchronous event that nothing guarantees will ever
  arrive (no tray host, a slow/buggy client, a race). Fixed with a
  bounded fallback rather than either extreme: `Manage()` still holds
  off mapping a window carrying `_XEMBED_INFO` immediately (preserving
  the original, legitimate no-flash behavior for the common, fast-dock
  case - proven still necessary, not just assumed, per the "what
  breaks" check below), but now records it with a 2-second deadline
  (`m_pendingXEmbedWindows`, checked once per `Tick()` via the new
  `CheckPendingXEmbedWindows()`); if a real dock request hasn't
  claimed it by then, it falls back to managing the window normally
  (`Manage(id, /*skipXEmbedCheck=*/true)`). The reverse race - a dock
  request arriving *after* the timeout already tiled the window - is
  also handled: `WindowManager::HandleClientMessage()` now unmanages a
  window `SystemTray::HandleClientMessage()` (whose return type
  changed from `void` to the docked window, or `0`/`None`) reports as
  just-docked, if it finds that window already sitting in the
  repository; `Unmanage()` only ever touches Kohiko's own bookkeeping,
  never the window's actual X11 state, so calling it after
  `DockIcon()` has already reparented the same window elsewhere is
  safe. A new `SystemTray::IsDocked()` lets the timeout sweep
  recognize a window that got claimed in the meantime, as a second,
  redundant line of defense alongside the immediate cleanup in
  `HandleClientMessage()`. Per this investigation's own explicit
  instruction not to simply remove the original check without proving
  what breaks: temporarily disabled it (`if (false && ...)`) and
  re-ran the genuine-dock scenario with sequencing debug logs at both
  `m_repository.Add()` and `DockIcon()` - confirmed directly, not
  reasoned about, that the tray icon window really does get added to
  the repository (tiled as an ordinary window) *before* `DockIcon()`
  ever runs on it, exactly the cosmetic regression the original check
  existed to prevent, and exactly why this fix keeps the fast path
  rather than replacing it outright. Live-verified (real `kohiko`
  under Xvfb, three real windows: an ordinary control, an
  `_XEMBED_INFO` window that never receives a dock request, and a
  genuine tray icon that sends a real `SYSTEM_TRAY_REQUEST_DOCK` via
  `XSendEvent`): the control tiles immediately; the never-docked
  window stays untouched for the first second, then is correctly
  tiled once the timeout fires - no longer permanently invisible;
  the genuine tray icon is correctly reparented into the tray
  container, resized to fit (confirmed via `xwininfo -id`, parented
  under the tray's own container window, `Map State: IsViewable`,
  resized from its 24px request down to the tray's icon size), never
  once observed at the root level at its raw creation size, and
  *stays* correctly docked even after the same 2-second window
  elapses - the fallback doesn't second-guess a dock that already
  succeeded. One test-tooling bug turned up and was fixed along the
  way, worth naming so it isn't mistaken for a Kohiko bug later: the
  first version of `x11_test_client` closed itself on *any*
  `ClientMessage`, not just a real `WM_DELETE_WINDOW` - which meant it
  exited the instant `DockIcon()` correctly sent it a spec-compliant
  `XEMBED_EMBEDDED_NOTIFY`, briefly looking like docking had crashed
  the client rather than succeeded. 6 new checks in the live-session
  `tests/test_xembed_dock_timeout.sh` (gracefully skipped if
  Xvfb/xwininfo aren't available), covering exactly the scenarios
  above against a real WM. On testing the specific named applications:
  Telegram, FeatherPad, and Flameshot aren't available in this
  environment (no network access to install them, and Telegram/
  FeatherPad are heavy GUI binaries beyond what a minimal
  reproduction needs) - instructed, at the top of this same
  investigation, to look at the common mechanism first rather than
  assume anything Telegram-specific, and that mechanism (a window
  carrying `_XEMBED_INFO` with no dock request following it) is what
  was actually reproduced and fixed, independent of which application
  triggers it. Whether any of these three specifically hit this exact
  path (rather than some other cause) is not something this
  environment can confirm directly - flagged here rather than claimed.

### Changed
- **The bar no longer displays the focused window's title.** This was
  a deliberate, working, intentional feature (`Bar::SetTitle()`/
  `Redraw()` drawing `m_title`), not a bug - confirmed by direct
  testing (mouse moved to multiple positions along the bar produced no
  text tied to cursor position anywhere, ruling out genuine *hover*
  text) and by reading the code, which shows it as a straightforward
  status-bar display of `_NET_WM_NAME` for whichever window currently
  holds real X input focus. Removed per explicit request, since it
  read as unwanted "what's under the mouse" information rather than
  the intended status display. The change is deliberately minimal and
  reversible: `Bar::SetTitle()` and `m_title` are both left in place -
  `WindowManager` still calls `SetTitle()` on every focus/title change
  via `UpdateAllBars()`, it's just a harmless no-op as far as anything
  visible goes now - so no call sites needed touching, and the display
  can be wired back into `Redraw()` again in one line if this is ever
  wanted back. `_NET_WM_NAME` itself is still read and still used
  internally (`windowrule=title:` matching, and to know when a redraw
  is worth triggering); only the on-screen display in the bar changed.

### Added
- `tests/test_eventloop.cpp` (9 checks, part of `make test`/`ctest`,
  unconditional - no X11/D-Bus dependency): covers
  `TickSchedule::PullForward()`/`IsDue()`/`TimeUntilDue()` directly
  with synthetic, offset-based timestamps, in a fraction of a second.
  It cannot exercise `EventLoop::Run()` itself (a real, blocking,
  X11-connected loop), which is why the live-session churn test
  described above, not this unit test alone, is what actually confirms
  the fix end to end.
- `tests/test_windowplacementnotice.cpp` (6 checks, unconditional): the
  new `WindowPlacementNotice::BackgroundPlacementText()` is a small,
  free, header-only function (`include/WindowPlacementNotice.h`) with
  no `WindowManager`/X11 dependency, deliberately factored out the
  same way `TickSchedule` was, purely so this is directly testable.
  It covers the text formatting only, including the empty-`WM_CLASS`
  fallback and a multi-word app name; it cannot exercise the actual
  *decision* of when `Manage()` calls it, which is what the live
  Xvfb reproduction described above under `### Fixed` confirms
  instead.
- 16 new checks in `tests/test_bsptree.cpp` (unconditional, no X11
  dependency, pure tree/geometry logic) for the collapse-on-remove
  direction fix above: the exact reported scenario end to end, a
  direct comparison against the unmodified old `Remove(window)`
  overload confirming the fix (not incidental test setup) is what
  changes the outcome, a manually-resized ratio surviving the flip
  onto the other axis, a case that needs no flip at all, and a
  6-window/2-removal scenario asserting no leaf's aspect ratio exceeds
  3:1 at any depth in the tree.
- `tests/test_rect_clamping.cpp` (11 checks, unconditional, pure
  struct/arithmetic logic - no dependency beyond `Types.h` itself):
  the first dedicated test coverage for `Rect::ClampedTo()`, which
  already existed and was already used elsewhere (floating windows)
  but had none before - written primarily to lock in the exact
  scenario behind the Swap-drag animation fix above (an off-screen
  position whose size already fits gets repositioned, never resized),
  plus the right/bottom-edge equivalent, a genuinely oversized rect
  actually needing a resize, and `borderWidth` being accounted for.

### Investigated, not changed
- The login/autologin architecture (`AutologinConfigurator`,
  `Authenticator`, `LockScreen`'s username handling) was checked
  end to end against the intended flow - display-manager/autologin
  starts Kohiko with no username prompt of its own, Kohiko's own
  LockScreen gates access with a password only - and found already
  correct, not requiring a code change. Confirmed by direct code
  reading (`LockScreen` resolves its displayed username via
  `getpwuid(getuid())` once at lock time, purely for display - never
  an editable field anywhere; no second username prompt exists
  anywhere in the codebase outside of `kohikoctl configure-autologin`
  itself, which is a one-time *administrative* setup command run once
  with `sudo` to tell the display manager which user to auto-login as,
  not something an end user sees during ordinary day-to-day login, and
  therefore not "the DM's user-selection functionality" being
  replaced) and then verified live end to end via the actual entry
  point a display manager invokes (`kohiko-session`, not a shortcut):
  with `lockscreen.after=always` set, launching `kohiko-session`
  showed the lock screen immediately, with "kohikouser" already
  correctly filled in with no prompt of any kind, and a real,
  PAM-authenticated password-only unlock completed the flow correctly.
  See the LockRecovery fix above for what this same investigation did
  turn up.
- The bar's clock cannot itself "run fast" independent of the
  system clock, and does not here: `Bar::Redraw()` calls
  `std::time(nullptr)` fresh on every single redraw (`src/Bar.cpp`)
  with no counter or incremental state anywhere in the display path -
  read directly, then confirmed empirically, not just by inspection:
  five screenshots of the running bar's clock taken at 5-second
  intervals under a live Xvfb session matched `date -u`'s own output
  exactly every time, with zero drift across the whole 20-second
  window. A report that the displayed clock runs fast almost always
  means the underlying *system* clock is the thing drifting - a
  common symptom in virtualized/sandboxed environments specifically,
  where the guest's timekeeping can lag or run ahead of real time
  under CPU contention - which is outside anything a window manager
  reads or controls; Kohiko only ever displays whatever the OS clock
  currently says. The standard fix lives at the OS level: confirming
  a time-sync service (`systemd-timesyncd`, `chrony`, or `ntpd`) is
  installed, enabled, and actually synchronized - `timedatectl status`
  on any systemd-based system shows this directly (`System clock
  synchronized: yes/no`). No Kohiko code change would address this
  even if made, since the reported symptom sits entirely upstream of
  anything Kohiko reads.
- A document accompanying this release's original request described
  investigating a "Telegram Desktop regression between Kohiko 0.20.1
  and 0.20.3" starting from `git diff <0.20.1> <0.20.3>`. Checked
  directly: this project has never been a git repository at any point
  across its whole history (`git status`/`git log --all`/a full-
  filesystem `find -iname .git` all confirm this, independently,
  multiple times), so that specific investigation step could never
  have been carried out on this project as described. The user
  subsequently clarified they meant changes made on their own end, not
  by a prior session. Whether 0.20.2's `IsXEmbedWindow()` early-return
  in `WindowManager::Manage()` could plausibly affect a real
  application that also uses XEmbed for its own purposes (e.g. Qt's
  system-tray-icon implementation, which Telegram Desktop does use) is
  a genuine, still-open question, not confirmed true or false - it
  needs real evidence (`xprop`/debug output from an affected machine)
  to investigate further, which wasn't available this release.
- A separate claim that a prior Kohiko session had added
  double-buffering to `Bar.cpp`/`Bar.h` that "removed visible flicker"
  but "did not preserve correct redraw behavior" was also checked
  directly: `Bar`'s existing backing-`Pixmap` double-buffering
  predates this project's entire multi-session engagement (present
  in the original 0.20.0 archive, before any work described in this
  changelog's history began) - no session recorded here ever touched
  `Bar.cpp`/`Bar.h` before the real fix described above under
  `### Fixed`. The actual, real, current redraw behavior of
  `Bar`/`EventLoop` was investigated on its own merits regardless,
  independent of this disputed premise, and is what turned up the
  genuine tick-scheduling bug above.
- A claim that the Wi-Fi password field in `NetworkWindow.cpp`
  "accepts only numeric input" was checked directly against
  `PromptForPassword()`, which uses `XLookupString()` and already
  accepts any printable character (any byte `>= 0x20`). A
  codebase-wide search found the only numeric-only input validation
  anywhere is in `SettingsWindow.cpp`, for an unrelated window-rule
  workspace-number field. No fix was needed or made for this specific
  claim.
- Of the five focus/mouse consistency cases investigated above, one -
  a newly opened window grabbing focus regardless of where the pointer
  happens to be resting - was deliberately left as-is after checking
  it live: this is itself the intended, ordinary policy (matching
  essentially every tiling WM's own convention, Kohiko's own included),
  confirmed by moving the pointer over an existing window first and
  then opening a second one elsewhere - the new window correctly took
  focus, with internal state and real X11 input focus agreeing the
  whole time. Forcing focus back onto whatever the pointer happens to
  be resting on instead would have been the actual regression here,
  not a fix.
- A first look at `kohiko-audio` at an enlarged (1600x950) window
  size, by eye, suggested the content column had drifted off-center -
  a visibly larger gap on the right than the left. Measured precisely
  instead of going on that impression (`PIL`, scanning for the
  leftmost/rightmost non-background pixel on several content rows):
  the actual margins were 350px and 351px - centered, matching
  `AudioWindow.h`'s documented "content width is capped and centered
  past ~900-960px" design. No fix was needed or made; the discrepancy
  was in the eyeballed read, not the layout.
- Long device/network names (`AudioWindow`'s device rows, and by the
  same shared `DrawTextClipped()` primitive, `NetworkWindow`'s/
  `BluetoothWindow`'s own lists) are hard-clipped at the row's right
  edge with no ellipsis, cutting off mid-word at narrow widths (e.g.
  "Kohiko USB Gaming Headset with a Genuinely Long Product Name"
  becomes "...with a Genui" at 420px). Confirmed live, not fixed:
  `UiWindow::DrawTextClipped()` has no truncation-with-ellipsis logic
  at all - it draws the full string and relies entirely on the X11
  clip region to cut it off. This is a real, pre-existing gap, but in
  a primitive shared across every widget in every one of kohiko-audio/
  network/bluetooth/settings, not something specific to audio's own
  layout - fixing it well (ellipsis truncation against actual text
  metrics) belongs as its own change against `UiWindow`/`Font`, not
  folded into an audio-scoped responsiveness pass. Left as a known,
  documented gap for now rather than expanding this task's blast
  radius into a shared rendering primitive.
- kohiko-network's Wi-Fi password modal (`PromptForPassword()`)
  creates its window with `XCreateWindow()` but never calls
  `XStoreName()`/sets `_NET_WM_NAME` - the "Password for ..." text
  visible in the dialog is only ever drawn as canvas content, not set
  as the window's actual X11 name. Noticed only because it broke the
  first attempt at finding the window for live testing (`xdotool
  search --name "Password for"` found nothing even though the modal
  was genuinely open and working - diffing the full window tree by
  geometry instead confirmed it was there). Left alone: the window
  grabs the keyboard and pointer for as long as it's open, so there's
  nothing to alt-tab to or list in a taskbar regardless of what name
  it has, and giving it one is a cosmetic change unrelated to either
  half of this task (layout or credential handling).
- **kohiko-bluetooth's GUI responsiveness, unlike Audio's and
  Network's own turns, checked out clean with no bug to fix.** Live
  Xvfb testing against a purpose-built mock BlueZ D-Bus service
  (`tests/mock_bluez.py` - the same `dbus-python`/GLib approach as
  `mock_networkmanager.py`, one adapter and five devices spanning
  connected/paired-but-out-of-range/freshly-discovered, one with a
  deliberately long advertised name) at five window sizes (420x600
  narrow, 680x700 - between the toolbar-wrap and list/details-split
  breakpoints, 1500x900 enlarged, 500x950 tall-narrow, 820x380 short
  enough to overflow) found no overlapping widgets, no clipping, and
  no unexplained empty space at any of them - the toolbar wraps its
  Discoverable toggle and Scan button cleanly below 640px, the device
  list/details panel stacks below 720px, content caps and centers
  past 960px, and (confirmed directly, not assumed from the shared
  code) this same version's scroll-routing fix already applies here
  too: scrolling with the pointer directly over the "Disconnect"
  button in a short window moved the whole list rather than doing
  nothing. The Advanced Settings/Trusted-devices page was checked
  too, at both a normal and a narrow width, with the same result.
  One real, if currently harmless, thing did turn up along the way:
  `AppendDeviceSection()` had to know the details panel's height
  *before* `BuildDetailsPanel()` builds it (to size the card's own
  bounds up front), which meant two independently-maintained copies
  of the same "18 + 40 + 18 + 3\*30 + 10 + buttonsHeight + 18"
  arithmetic - one in each function, currently in agreement but with
  nothing stopping a future edit to one from silently not being made
  to the other, which is exactly the shape a real clipping/excess-
  space bug in this exact spot would take. Not a bug today, so not
  written up as a fix - but replaced both copies with a single
  `BluetoothWindow::ComputeDetailsPanelHeight()` both call, a
  behavior-preserving change confirmed via pixel diff (`PIL.
  ImageChops.difference`, empty bounding box) against a live
  screenshot taken before the change. 12 new checks in
  `tests/test_bluetoothwindow.cpp`, for all three paired/connected
  button-count states: not a fresh re-derivation of the same formula,
  but construction of the *actual* widget tree `BuildDetailsPanel()`
  produces, checked against `ComputeDetailsPanelHeight()`'s own
  output for both failure modes this task cares about - nothing
  extends past the computed height (clipping), and nothing falls
  meaningfully short of it either (excess unused space).

## Version 0.20.3

Release date: 2026-08-16

### Fixed
- **kohiko-network never actually reused or updated a saved Wi-Fi
  connection's stored password - it silently discarded every
  freshly-typed password for a network NetworkManager already had a
  profile for, and (as a direct consequence) never detected that a
  profile already existed in the first place.** Found while
  investigating a live report of a WPA2 network ("Linux for best")
  repeatedly failing with `psk mismatch reported by supplicant`. The
  handshake failure itself reproduced identically via plain `nmcli`,
  with no Kohiko process involved at all, and Kohiko's own
  process was independently confirmed (by PID, in the provided
  `journalctl` output) not to be the one making any of the specific
  failed activation attempts logged there - so that specific failure
  is not attributed to Kohiko, and no attempt was made to explain it
  away as one. Tracing `NetworkManagerClient.cpp`'s complete profile
  lookup/secret/activation path anyway (as asked, independent of
  whether the specific logged failures were Kohiko's doing) turned up
  a real, separate, verified bug in exactly that path:
  `ConnectToAccessPoint()`'s "reuse an existing saved connection for
  this SSID" check reads a saved connection's stored SSID via
  `BytesToString(wifiSection.Get("ssid"))` - and `BytesToString()`
  called `.Items()` on that value without unwrapping it first. At the
  nesting depth that value actually comes from (inside a
  `GetSettings()`-shaped nested dict), every property value arrives
  wrapped in a D-Bus Variant; reading `.Items()` off an un-unwrapped
  Variant doesn't error, it silently returns zero items. Confirmed by
  direct execution, not just by reading: a small standalone
  reproduction, and a before/after run of the new regression test
  against the pre-fix code, both show `BytesToString()` returning `""`
  for every real SSID, unconditionally.
  The practical effect: `savedSsid == ssid` never matched, for any
  network, ever - so every "Connect" click on an already-known
  network fell through to creating a brand-new connection profile via
  `AddAndActivateConnection()` instead of reusing (or correctly
  updating) the existing one, the same "duplicates/re-asks for a
  known network" anti-pattern reported against other NetworkManager
  frontends with this exact bug shape. `BytesToString()` is fixed at
  the root (an `.Unwrap()` call, safe as a no-op for any caller that
  already had an unwrapped value, so every existing call site benefits
  without behavior changing for ones that weren't broken) and moved
  out of this file's anonymous namespace into `Kohiko::` scope,
  declared in `NetworkManagerClient.h`, specifically so it's directly
  unit-testable rather than only reachable through the much larger
  surface `ConnectToAccessPoint()` exposes.
  A second, dependent fix was necessary alongside the first, not
  optional: once SSID matching actually works, the existing code
  would have found the matching saved connection and just reactivated
  it as-is - silently discarding whatever password the user had just
  been prompted for and just typed, and reproducing the exact
  "nothing I type ever seems to take effect" experience the original
  report described, just deterministically instead of intermittently.
  `ConnectToAccessPoint()` now calls `Settings.Connection.Update()`
  with a freshly-typed, non-empty password before activating a
  matched existing connection - built from the connection's own,
  just-fetched full settings (`Update()` replaces the *entire*
  connection per NetworkManager's own D-Bus reference, not a
  merge/patch, so every other section - `ipv4`, `connection`, etc. -
  is round-tripped through unchanged) with only that one section's
  `psk` (and `key-mgmt`, if not already set) added or overwritten.
- Neither of the above two, related bugs could ever have been
  observed independently of each other in practice - the second was
  unreachable until the first was fixed, and fixing only the first
  without the second would have made the user-visible symptom *more*
  consistent, not fixed, since the stale secret would then always win
  instead of sometimes.

### Added
- `tests/test_networkmanagerclient.cpp` (6 checks, part of `make
  test`/`ctest` when `libdbus-1-dev` is available, gated the same way
  `kohiko-network` itself is): constructs the exact D-Bus wire shape
  `GetSettings()` returns for a saved connection's SSID by hand
  (Variant-wrapping an array of bytes) and confirms `BytesToString()`
  decodes it correctly - and, run against the pre-fix code during
  development, confirmed the test actually fails there rather than
  passing vacuously.

### Investigated, not changed
- A real Wi-Fi association failure separately reported and reproduced
  directly on hardware (RTL8822BU / `rtw88_8822bu`, TP-Link 2357:0138,
  `linux-lts 6.18.44-1-lts`, firmware 30.20.0): 5GHz association to
  `Linux for best_5G` consistently fails
  `authenticated → associated → failed to get tx report from firmware → deauthenticated (reason 2)`,
  2.4GHz on the same adapter works normally. Matched against a
  community report describing the identical chip, identical firmware
  version, and the same failure, explicitly as a regression starting
  at kernel 6.18.4 (6.12.58 LTS and 6.17 both confirmed still
  working) - not a chronic, always-been-there issue. No accepted
  upstream fix or kernel-bugzilla tracking entry was found for this
  specific regression. This is a kernel/driver-level issue, entirely
  outside NetworkManager's or Kohiko's own code, and neither was
  modified in relation to it, per the explicit instruction not to.
  See the conversation this release's work came from for the full
  investigation and the options that were considered.

## Version 0.20.2

Release date: 2026-08-15

Both fixes below were found on a real Kohiko 0.20.1 session, not in
this project's own sandbox - the sandbox testing behind 0.20.1 wasn't
wrong about what it verified, but it verified less than it needed to:
the tray icons genuinely docked and rendered correctly there, but
nothing in that testing happened to exercise how the *window manager
itself* first classifies a brand-new tray-icon window (0.20.1's
testing never had a second, real application window open at the same
time to notice the difference against), and nothing in it ran the
exact repeated-icon-load sequence needed to expose the Imlib2 issue
addressed below reliably. Both are fixed properly here, not documented
as accepted risk.

### Fixed
- **Tray icon windows were being managed as normal application
  windows** - tiled into the BSP layout, given a taskbar entry
  (`_NET_CLIENT_LIST`), eligible for normal focus - rather than
  treated as the non-interactive infrastructure they are, alongside
  the bar itself. Root cause: `TrayIconClient::TryDock()` maps its own
  window immediately after requesting a dock, without waiting for
  `SystemTray` to actually reparent it first (see that function's own
  comment for why - briefly, so a tray icon still gets *something* on
  screen even if the dock request goes unanswered for a moment), which
  races `WindowManager::Manage()`'s ordinary `MapRequest` handling: a
  tray icon window could be fully tiled and tracked before
  `SystemTray` ever got a chance to reparent it away, leaving a
  phantom BSP tile and taskbar entry rendering nothing once the
  window's actual content moved into the tray a moment later - this
  also explains the tray icons' own visibly wrong, oversized geometry
  under 0.20.1 (BSP was resizing them to fill a tile). This is why it
  surfaced now rather than earlier: before 0.20.1's autostart fix, the
  tray icon windows this race depends on never existed on a real
  session at all. Fixed generically, not by recognizing
  `kohiko-*-tray` by name: `WindowManager::Manage()` now skips any
  window carrying an `_XEMBED_INFO` property (new:
  `XConnection::IsXEmbedWindow()`) - the freedesktop XEmbed
  specification's own required marker for any window that intends to
  be embedded into someone else's window rather than managed as a
  normal one, which `TrayIconClient::Create()` already set correctly,
  just to no effect before now. Any future tray widget built the same
  way is classified correctly automatically, with no code changes
  needed here; the actual Audio/Network/Bluetooth *app* windows
  (opened directly, not their tray icons) are unaffected and remain
  ordinary, normal, tiled windows, since they never set this property.
  Session Restore, which only ever persists currently-managed windows,
  needed no separate change for tray icons to stay out of it either -
  same root cause, same fix.
- **A real, reproducible Imlib2 SVG-loading reliability problem**,
  found via ASan/UBSan-instrumented and Valgrind-style (glibc
  `MALLOC_CHECK_`) reproduction attempts this release, is now avoided
  entirely rather than merely documented. 0.20.1 recorded this crash
  against "libimlib2-1.12.4" - that was a transcription error; the
  only Imlib2 version ever actually available in this project's own
  sandbox is 1.12.1-1.1build2, and that correction matters, because
  the *original* specific crash (a heap corruption after loading a
  particular 9-icon sequence) could not be reproduced again this
  release on that same, correct version, despite substantial
  instrumented effort (hundreds of stress cycles, ASan/UBSan, glibc's
  `MALLOC_CHECK_=3`) - it may be heap-layout/ASLR-sensitive rather
  than strictly deterministic. A *different*, highly and reliably
  reproducible failure was found this release instead: rapidly
  creating and destroying many different SVG-sourced Imlib2-rendered
  X11 Pixmaps in one process (not `UiIconCache`'s own actual usage
  pattern, which loads each unique icon once and holds it for the
  process's lifetime - that specific pattern produced zero errors
  across all 206 real Adwaita status icons tested) reliably produced
  X11 `BadPixmap` errors on ~95% of `XFreePixmap` calls. Both findings
  - one hard to reproduce, one very easy to, under different
  conditions - point the same direction: real unreliability
  specifically in Imlib2's own SVG-to-Pixmap bridge, not in the SVG
  files themselves (librsvg's own `rsvg-convert` CLI renders the exact
  same files with no issue, standalone) and not in `UiIconCache`'s
  code (a minimal, direct, raw-Imlib2-API-only reproduction with no
  Kohiko code involved hits the same `BadPixmap` pattern). Rather than
  depend on a workaround for a bridge with a demonstrated reliability
  problem, `.svg`/`.svgz` icons are now rendered through a new,
  self-contained module, `SvgRenderer` (`include/SvgRenderer.h`,
  `src/SvgRenderer.cpp`), using librsvg and Cairo directly - the same
  underlying libraries Imlib2's own SVG loader plugin uses internally,
  and genuinely the standard way most of the Linux desktop ecosystem
  (GTK included) renders SVG content, just without Imlib2's bridging
  code in between. Verified clean (zero X errors, including under
  ASan/UBSan) across the exact adversarial pattern
  (create-many-different-SVG-pixmaps-in-a-tight-loop) that reliably
  broke the old path, and across `UiIconCache`'s own real usage
  pattern. Visually, icons now render as correct, recognizable,
  anti-aliased shapes - confirmed on a real session, and actually a
  clear improvement over 0.20.1's own rendering, which (now
  understood, not just observed) was quietly producing solid,
  detail-less blocks for at least some icons rather than true
  failures. `.png`/`.xpm`/other raster icon formats are completely
  unaffected - they never went through Imlib2's SVG loader in the
  first place, and still load exactly as before.
- Per the requirement not to depend on it: `imlib_set_cache_size(0)`,
  added in 0.20.1 as a defensive (and, per that entry's own honest
  wording, unproven) mitigation for the issue above, has been removed
  from `UiIconCache`'s constructor - the real fix above makes it
  irrelevant to the SVG path it was aimed at, and it was never
  confirmed to do anything for the raster path either.

### Added
- `librsvg2-dev` (pacman: `librsvg`) joins `libdbus-1-dev`/
  `libpipewire-0.3-dev` as a build requirement for kohiko-audio,
  kohiko-network, kohiko-bluetooth, and their three tray widgets -
  `SvgRenderer.cpp` `#include`s it unconditionally, the same "no
  fallback at compile time" treatment the other two already had. Also
  added `pipewire` to `scripts/install-arch.sh`'s own package list
  while already touching it for `librsvg` - a real, pre-existing gap
  from before this release (pipewire was already a hard build
  requirement for kohiko-audio specifically; a fresh install-arch.sh
  run without it already-installed some other way silently skipped
  building all six of these binaries rather than failing loudly).
- Two new regression tests, both following the existing
  `test_monitormanager` convention of gracefully skipping (exit 0,
  not a failure) when no real `$DISPLAY` is available, so both are
  kept out of `make test`/available separately (`make
  test-windowclassification`, `make test-svgrenderer`) the same way
  `make test-monitors` already was, but are registered with `ctest`
  the same way `test_monitormanager` already is too:
  `test_windowclassification` (3 checks: `IsXEmbedWindow()`'s three
  cases above) and `test_svgrenderer` (14 checks: `CanHandle()`'s pure
  logic always runs; `RenderToPixmaps()`'s checks include the same
  adversarial many-different-SVGs stress pattern that reproduced the
  `BadPixmap` issue above, now asserting zero X errors).

## Version 0.20.1

Release date: 2026-08-13

### Fixed
- **Audio/network/Bluetooth tray icons now actually appear.** Root
  cause: `desktop/kohiko-*-tray.desktop` were correctly installed to
  `/etc/xdg/autostart` (as the 0.19.0 entry below describes), but
  nothing in Kohiko ever read that directory - `RunAutostart()` only
  ever handled `auto_start_programs=`/`workspace<N>=`, a separate,
  config-string-only mechanism, and Kohiko has no session manager of
  its own to fall back on. The three tray processes were never
  started at all, on a display-manager-launched session or a manual
  `startx` one alike - confirmed by reproducing it against a pristine
  pre-fix build first, not just by reading the code.
  `WindowManager::RunAutostart()` now also runs every `.desktop` entry
  under `Xdg::AutostartDirs()` (new: `$XDG_CONFIG_HOME/autostart` then
  `$XDG_CONFIG_DIRS/autostart`), via a new `ShouldAutostart()`
  predicate (`DesktopEntry.h`/`.cpp`) that respects `Hidden=`,
  `X-GNOME-Autostart-enabled=false`, `TryExec=`, and
  `OnlyShowIn=`/`NotShowIn=`. `SystemTray.cpp`, `TrayIconClient.cpp`,
  and all three `tools/kohiko-*-tray.cpp` were already correct and are
  unmodified by this fix - confirmed by diff against the pre-fix
  source, not just by assumption. See
  `docs/AUDIO_NETWORK_BLUETOOTH.md`'s new "Tray widget autostart"
  section for the full investigation and exactly what was tested
  (a real Xvfb X server, a real D-Bus session bus, and a real
  PipeWire/WirePlumber stack, not just a code read).
- `IconResolver::Resolve()` now retries a bare icon name with a
  `-symbolic` suffix if the exact name isn't found anywhere in the
  theme chain. Found while verifying the fix above: with autostart
  fixed and a real, currently-installed Adwaita theme configured, all
  three tray icons docked successfully but rendered as entirely blank
  squares, because Adwaita (like most actively-maintained themes)
  only ships modern status-icon names like
  `network-wireless-signal-excellent` under a `-symbolic` suffix.
  Affects the launcher's own icon lookups too (same `IconResolver`),
  though only as an additional fallback attempted after every existing
  lookup already failed, so it cannot change the result of a lookup
  that previously succeeded.
- `make test` was silently incomplete on any system with XRandr
  development headers present (i.e. most real installs, including a
  default Arch one, since `libxrandr` is already a listed
  dependency) - `build/test_sessionstore` and
  `build/test_wallpapermanager` didn't link `-lXrandr` despite pulling
  in `MonitorManager.cpp`, so `make test` silently stopped partway
  through instead of running the remaining test binaries. Unrelated to
  the tray fix above; found only because a full, honest `make test`
  run was needed to verify it.

### Changed
- Kohiko now sets `$XDG_CURRENT_DESKTOP=Kohiko` at startup
  (`Application::Run()`) if nothing has already set it, without
  overwriting an existing value. A display manager that honours
  `desktop/kohiko.desktop`'s `DesktopNames=Kohiko` typically sets this
  already; a manual `startx`/`~/.xinitrc` launch never did, since
  there's no display manager involved to make that translation -
  meaning `OnlyShowIn=`/`NotShowIn=` on any autostart entry could
  silently behave differently between the two launch paths. Found
  while investigating the tray fix above, though it doesn't affect
  Kohiko's own three tray `.desktop` files, which don't set either key.
- `UiIconCache`'s constructor now calls `imlib_set_cache_size(0)`,
  disabling Imlib2's own internal image cache (distinct from, and not
  a replacement for, `UiIconCache`'s own separate pixmap-level cache,
  which is unchanged) - a standard, zero-downside mitigation for a
  class of Imlib2 correctness bug when loading many small images in
  one process. Added defensively; see "Known issues" below - it did
  not conclusively fix the specific crash found this release.

### Added
- Two new unit test binaries, wired into `make test` and `ctest`
  alike: `test_desktopentry` (27 checks: `ShouldAutostart()`'s
  handling of `Hidden=`, `X-GNOME-Autostart-enabled=`, `TryExec=`,
  `OnlyShowIn=`/`NotShowIn=`, and specifically that `NoDisplay=true`
  does *not* block autostart the way it blocks menu display; plus
  `Xdg::AutostartDirs()`'s priority order) and `test_iconresolver` (7
  checks: the `-symbolic` fallback above, including that it can't
  double up on a name that already ends in `-symbolic`).

### Known issues
- **An unresolved Imlib2/librsvg crash risk**, found (not caused) by
  the icon-resolver fix above: once real status-icon SVGs were
  actually being loaded, loading a specific sequence of several
  different ones in a row - the pattern a tray widget's icon naturally
  follows as its status changes over time - reproducibly corrupted the
  heap in this project's own Ubuntu 24.04 sandbox (`libimlib2-1.12.4`/
  `librsvg2-2.58.0`). Isolated to Imlib2's own SVG-loading path
  specifically (`rsvg-convert`, librsvg's own CLI, renders the exact
  same files fine standalone; a minimal reproduction using raw Imlib2
  calls only, no Kohiko code at all, hits the same crash) - not a bug
  in `UiIconCache`, `IconResolver`, or anything else in this release.
  Deterministic given a fixed load sequence, but sequence- and file-
  specific (11 of the 13 real icon names the three tray widgets can
  request were individually confirmed not to trigger it). Disclosed,
  not fixed - root-causing a third-party rendering library's own
  internals is well outside this release's scope, and unconfirmed
  against Arch Linux's actual `imlib2`/`librsvg` package versions,
  which likely differ from this sandbox's. See
  `docs/AUDIO_NETWORK_BLUETOOTH.md`'s "Icon loading" section for the
  full isolation work and a practical workaround.

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
