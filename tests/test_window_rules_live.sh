#!/bin/sh
# End-to-end tests for explicit window rules (CHANGELOG.md 0.22.0): the
# real kohiko window manager under Xvfb, real X11 clients, the real
# kohiko-choose-window tool (headless --set/--delete/--list AND, when
# xdotool is installed, the actual dialog driven with real key events).
#
# What this pins down, in the terms of the redesign's acceptance
# criteria:
#   - DETERMINISTIC placement: a saved rule always produces the same
#     workspace / side / mode, each field independently of the others;
#   - NO IMPLICIT LEARNING: moving windows by hand any number of times
#     never creates a rule, never changes one, never writes anything
#     to the config or data directory;
#   - EXPLICIT CONTROL: the chooser (CLI and dialog) creates, changes
#     and deletes a rule, and nothing else in the config is disturbed;
#   - the old adaptive-placement state is retired, never reinterpreted.
#
# Usage: test_window_rules_live.sh [kohiko] [kohikoctl] [kohiko-choose-window] [x11_test_client]
# Skipped (exit 0) when Xvfb or a binary isn't available.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT="$SCRIPT_DIR/.."
KOHIKO_BIN="${1:-$REPO_ROOT/kohiko}"
KOHIKOCTL="${2:-$REPO_ROOT/kohikoctl}"
CHOOSER="${3:-$REPO_ROOT/kohiko-choose-window}"
TEST_CLIENT="${4:-$REPO_ROOT/build/x11_test_client}"

. "$SCRIPT_DIR/live/common.sh"

live_require Xvfb
[ -x "$CHOOSER" ] || { echo "SKIP: $CHOOSER not built."; exit 0; }

live_init 110

# Everything except the windowrule= lines - what must never be disturbed.
non_rule_lines() { grep -v '^windowrule=' "$CONF"; }
rule_lines()     { grep '^windowrule=' "$CONF"; }

# ---------------------------------------------------------------------
echo "== 1. Placement from explicit rules =="

live_write_config 'workspace.count=4
auto_start_programs=
windowrule=app:wsonly workspace:3
windowrule=app:leftapp horizontal:left
windowrule=app:rightapp horizontal:right
windowrule=app:topapp vertical:top
windowrule=app:floatbr mode:floating horizontal:right vertical:bottom
windowrule=app:floater mode:floating
windowrule=app:fsapp mode:fullscreen
windowrule=app:forcetile mode:tiled
windowrule=app:combo workspace:2 horizontal:right vertical:top mode:tiled
windowrule=app:specific workspace:2
windowrule=class:specific title:special workspace:4'
live_start_wm

live_open NoRule
check "$(live_field NoRule workspace)/$(live_field NoRule floating)/$(live_field NoRule fullscreen)" "1/false/false" \
    "no rule: normal placement (current workspace, tiled, not fullscreen)"
live_close "$LAST_PID" NoRule

live_open wsonly
check "$(live_field wsonly workspace)/$(live_field wsonly floating)" "3/false" \
    "workspace-only rule: opens on workspace 3 and is still tiled (mode untouched)"

live_open floater
check "$(live_field floater workspace)/$(live_field floater floating)" "1/true" \
    "mode-only rule (floating): floats, stays on the current workspace"

live_open fsapp
check "$(live_field fsapp fullscreen)/$(live_field fsapp workspace)" "true/1" \
    "mode-only rule (fullscreen): opens fullscreen on the current workspace"

# A dialog-type window floats by itself; a mode:tiled rule overrides that.
live_open plaindialog --window-type _NET_WM_WINDOW_TYPE_DIALOG
check "$(live_field plaindialog floating)" "true" "(control) a dialog-type window with no rule floats"
live_open forcetile --window-type _NET_WM_WINDOW_TYPE_DIALOG
check "$(live_field forcetile floating)" "false" "mode:tiled rule overrides the dialog's own tendency to float"

# Floating window: horizontal/vertical put it flush against those screen edges.
live_open floatbr
fx=$(live_field floatbr x); fw=$(live_field floatbr width); fy=$(live_field floatbr y); fh=$(live_field floatbr height)
check "$(live_field floatbr floating)" "true" "floating + position rule: floats"
check_true "...and sits at the RIGHT edge (x+width within 16px of the 1920px screen edge)" \
    test "$((fx + fw))" -ge 1904
check_true "...and at the BOTTOM edge (y+height within 16px of the 1080px screen edge)" \
    test "$((fy + fh))" -ge 1064
check_true "...and is not simply centred (x is far right of centre)" test "$fx" -gt 1000

live_open combo
check "$(live_field combo workspace)/$(live_field combo floating)" "2/false" \
    "combined rule: workspace 2, tiled (position has nothing to move for a lone window)"

# Specificity: the 2-selector rule beats the app rule, for its window only.
live_open specific --title ordinary
live_open specific --title special
check "$(live_title_field ordinary workspace)" "2" "app rule applies to an ordinary window of that application"
check "$(live_title_field special workspace)" "4" "...while the more specific class+title rule wins for the matching window"

# --- tiled position rules, on workspace 1 with two plain windows already open ---
for pid in $LIVE_PIDS; do kill "$pid" 2>/dev/null; done
LIVE_PIDS=""
i=0; while [ -n "$(clients_lines | grep -c '"class"' | grep -v '^0$')" ] && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i + 1)); done

live_open plainA
live_open plainB
live_open leftapp
lx=$(live_field leftapp x)
check "$lx" "$(live_extreme_x min 1)" "horizontal:left: the new window ends up in the leftmost column of workspace 1"
check_true "...and it really moved - it is left of at least one other window" \
    test "$lx" -lt "$(live_extreme_x max 1)"

for pid in $LIVE_PIDS; do kill "$pid" 2>/dev/null; done
LIVE_PIDS=""; sleep 0.8
live_open plainA
live_open plainB
live_open rightapp
rx=$(live_field rightapp x)
check "$rx" "$(live_extreme_x max 1)" "horizontal:right: the new window ends up in the rightmost column"
check_true "...and it is right of at least one other window" test "$rx" -gt "$(live_extreme_x min 1)"

for pid in $LIVE_PIDS; do kill "$pid" 2>/dev/null; done
LIVE_PIDS=""; sleep 0.8
live_open plainA
live_open plainB
live_open topapp
ty=$(live_field topapp y)
check "$ty" "$(live_extreme_y min 1)" "vertical:top: the new window ends up in the top row"
check_true "...and it is above at least one other window" test "$ty" -lt "$(live_extreme_y max 1)"

# ---------------------------------------------------------------------
echo
echo "== 2. Moving windows by hand never learns, never changes a rule =="

live_quit_wm
for pid in $LIVE_PIDS; do kill "$pid" 2>/dev/null; done
LIVE_PIDS=""; sleep 0.5

live_write_config 'workspace.count=4
auto_start_programs=
windowrule=app:ruled workspace:2'
live_start_wm
before_sum=$(cksum < "$CONF")

# 5 launches (the old system learned after 3), each time moved by hand.
for n in 1 2 3 4; do
    live_open wanderer
    "$KOHIKOCTL" dispatch movetoworkspace 3 >/dev/null
    sleep 0.2
    live_close "$LAST_PID" wanderer
done
live_open wanderer
check "$(live_field wanderer workspace)" "1" \
    "a window moved to workspace 3 by hand on 4 earlier launches still opens on the CURRENT workspace (nothing was learned)"
live_close "$LAST_PID" wanderer

# Swaps and floating toggles are manual too.
live_open wanderer
live_open wanderer2
for n in 1 2 3 4; do "$KOHIKOCTL" dispatch move left >/dev/null; "$KOHIKOCTL" dispatch move right >/dev/null; done
"$KOHIKOCTL" dispatch floating >/dev/null
"$KOHIKOCTL" dispatch floating >/dev/null
sleep 0.2

# Moving the RULED window by hand doesn't change its rule either.
live_open ruled
check "$(live_field ruled workspace)" "2" "(control) the ruled window opens where its rule says"
# It opened quietly on workspace 2 (unfocused, in the background): go
# there so it is the focused window, then move it by hand.
"$KOHIKOCTL" dispatch workspace 2 >/dev/null
sleep 0.3
"$KOHIKOCTL" dispatch movetoworkspace 4 >/dev/null
sleep 0.3
check "$(live_field ruled workspace)" "4" "a manual move of the ruled window works (moves are still free to happen)"
live_close "$LAST_PID" ruled
# Back to workspace 1 first, so "opens on 2" can only be the rule talking.
"$KOHIKOCTL" dispatch workspace 1 >/dev/null
sleep 0.3
live_open ruled
check "$(live_field ruled workspace)" "2" "...but the next ruled window still opens on workspace 2: the manual move did not change the rule"

check "$(cksum < "$CONF")" "$before_sum" "the config file is byte-for-byte identical after all of that"
check "$(ls "$DATA_DIR" 2>/dev/null | grep -c placement_habits)" "0" "no placement_habits file was ever written"
check "$(grep -c 'app:wanderer' "$CONF")" "0" "no rule was created for the application that was moved around"

# ---------------------------------------------------------------------
echo
echo "== 3. kohiko-choose-window (headless): create / change / delete / persist =="

before_other=$(non_rule_lines | cksum)

"$CHOOSER" --config "$CONF" --app persistapp --set "workspace:3 mode:floating"
check "$?" "0" "--set succeeds"
check "$("$CHOOSER" --config "$CONF" --app persistapp --show)" "app:persistapp workspace:3 mode:floating" \
    "--show prints the saved rule in canonical form"
check "$(grep -c '^windowrule=app:persistapp ' "$CONF")" "1" "exactly one line was written to kohiko.conf"
check "$(non_rule_lines | cksum)" "$before_other" "every other line in the config is untouched"
check "$(rule_lines | grep -c 'app:ruled workspace:2')" "1" "...including the existing hand-written rule"

live_open persistapp
check "$(live_field persistapp workspace)/$(live_field persistapp floating)" "3/true" \
    "a window opened after saving obeys the rule (workspace 3, floating) - the rule was reloaded"
existing_pid=$LAST_PID

"$CHOOSER" --config "$CONF" --app persistapp --set "workspace:2"
live_open persistapp --title second
check "$(live_title_field second workspace)/$(live_title_field second floating)" "2/false" \
    "a CHANGED rule applies to the next window: workspace 2, and floating is back to default (field cleared)"
check "$(live_title_field persistapp workspace)" "3" "...and the window that was already open was NOT moved by saving"

check "$("$CHOOSER" --config "$CONF" --list | grep -c 'persistapp')" "1" "--list shows it"

"$CHOOSER" --config "$CONF" --app persistapp --delete
check "$("$CHOOSER" --config "$CONF" --app persistapp --show)" "" "after --delete the application has no rule"
live_open persistapp --title third
check "$(live_title_field third workspace)/$(live_title_field third floating)" "1/false" \
    "...and its next window is back to Kohiko's default placement"
check "$(non_rule_lines | cksum)" "$before_other" "deleting a rule leaves every other line untouched"

# Bad input is rejected without touching anything.
sum_now=$(cksum < "$CONF")
"$CHOOSER" --config "$CONF" --app x --set "workspace:0" 2>/dev/null
check_true "an invalid rule (workspace:0) is refused" test "$?" -ne 0
"$CHOOSER" --config "$CONF" --app x --set "horizontal:middle" 2>/dev/null
check_true "an invalid rule (horizontal:middle) is refused" test "$?" -ne 0
check "$(cksum < "$CONF")" "$sum_now" "...and the config is unchanged by either"

# An app with a space in its class (percent-escaped in the file).
"$CHOOSER" --config "$CONF" --app "My App" --set "workspace:4"
check "$(grep -c '^windowrule=app:My%20App workspace:4$' "$CONF")" "1" "an application key with a space is stored %-escaped"
check "$("$CHOOSER" --config "$CONF" --app "My App" --show)" "app:My%20App workspace:4" "...and found again by its real name"

# A config that doesn't exist is never fabricated (it would have no bind= lines).
"$CHOOSER" --config "$WORKDIR/does-not-exist.conf" --app x --set "workspace:1" 2>/dev/null
check_true "a missing config is refused" test "$?" -ne 0
check "$([ -e "$WORKDIR/does-not-exist.conf" ] && echo yes || echo no)" "no" "...and no config file was created"

# Persistence across a restart of the window manager.
"$CHOOSER" --config "$CONF" --app persistapp --set "workspace:3"
live_quit_wm
for pid in $LIVE_PIDS; do kill "$pid" 2>/dev/null; done
LIVE_PIDS=""; sleep 0.5
live_start_wm
live_open persistapp --title afterrestart
check "$(live_title_field afterrestart workspace)" "3" "the rule survives a restart of the window manager"

# ---------------------------------------------------------------------
echo
echo "== 4. Old adaptive-placement state is retired, never reinterpreted =="

live_quit_wm
for pid in $LIVE_PIDS; do kill "$pid" 2>/dev/null; done
LIVE_PIDS=""; sleep 0.5

live_write_config 'workspace.count=4
auto_start_programs=
general.adaptive_placement=true
windowrule=app:keepme workspace:2'
mkdir -p "$DATA_DIR"
# What the removed feature left behind: 5 manual moves of "oldapp" to workspace 3.
printf 'oldapp 0 0 0 0 1 3 5\n' > "$DATA_DIR/placement_habits"
live_start_wm

check "$([ -e "$DATA_DIR/placement_habits" ] && echo yes || echo no)" "no" "the obsolete placement_habits file is deleted at startup"
check "$(grep -c 'adaptive_placement' "$CONF")" "0" "the obsolete general.adaptive_placement line is removed from the config"
check "$([ -e "$CONF.bak" ] && echo yes || echo no)" "yes" "...after the previous config was saved to kohiko.conf.bak"
check "$(grep -c 'windowrule=app:keepme workspace:2' "$CONF")" "1" "the user's own rules are kept"
check "$(grep -c 'windowrule=app:oldapp' "$CONF")" "0" "the old votes were NOT turned into a rule"
check "$(grep -c 'obsolete' "$WORKDIR/kohiko.log")" "2" "both removals are logged (setting and data file)"

live_open oldapp
check "$(live_field oldapp workspace)" "1" "an application the old system had 'learned' now opens on the current workspace"

# ---------------------------------------------------------------------
echo
if command -v xdotool >/dev/null 2>&1; then
    echo "== 5. The dialog itself (real key events via xdotool) =="

    live_quit_wm
    for pid in $LIVE_PIDS; do kill "$pid" 2>/dev/null; done
    LIVE_PIDS=""; sleep 0.5
    live_write_config 'workspace.count=4
auto_start_programs=
windowrule=float class:uxapp'
    live_start_wm

    dialog_windows() { xdotool search --classname kohiko-choose-window 2>/dev/null | wc -l; }

    live_open uxapp --title "Mozilla Firefox" --instance Navigator

    t0=$(date +%s%N)
    "$KOHIKOCTL" dispatch choose_window >/dev/null 2>&1
    t1=$(date +%s%N)
    check_lt "$(( (t1 - t0) / 1000000 ))" 1500 \
        "kohikoctl dispatch choose_window returns promptly while the dialog stays open (spawned programs must not inherit the IPC connection)"

    live_wait_for 60 test "$(dialog_windows)" -ge 1
    check "$(dialog_windows)" "1" "the dialog opened"
    check "$(live_field Kohiko-choose-window floating)" "true" "...as a floating window (it is not tiled into your layout)"

    "$KOHIKOCTL" dispatch choose_window >/dev/null 2>&1
    sleep 0.6
    check "$(dialog_windows)" "1" "pressing the key again does not stack a second dialog"

    # workspace 3, horizontal Right, vertical Bottom, mode Floating, Enter
    xdotool key 3 Down Right Right Down Right Right Down Right Right
    sleep 0.3
    xdotool key Return
    live_wait_for 40 test "$(dialog_windows)" -eq 0
    check "$(dialog_windows)" "0" "Enter closed the dialog"
    check "$(grep -c '^windowrule=app:uxapp workspace:3 horizontal:right vertical:bottom mode:floating$' "$CONF")" "1" \
        "...and saved exactly the chosen rule, keyed by the focused window's application"
    check "$(grep -c '^windowrule=float class:uxapp$' "$CONF")" "1" "...leaving the hand-written rule alone"

    sum_now=$(cksum < "$CONF")
    "$KOHIKOCTL" dispatch choose_window >/dev/null 2>&1
    live_wait_for 60 test "$(dialog_windows)" -ge 1
    xdotool key Down Right Escape
    live_wait_for 40 test "$(dialog_windows)" -eq 0
    check "$(cksum < "$CONF")" "$sum_now" "Cancel (Escape) after changing a choice leaves the config byte-for-byte unchanged"

    "$KOHIKOCTL" dispatch choose_window >/dev/null 2>&1
    live_wait_for 60 test "$(dialog_windows)" -ge 1
    xdotool key Delete
    live_wait_for 40 test "$(dialog_windows)" -eq 0
    check "$(grep -c 'app:uxapp' "$CONF")" "0" "Delete (Reset rule) removes the saved rule"
    check "$(grep -c '^windowrule=float class:uxapp$' "$CONF")" "1" "...and only that rule"
else
    echo "== 5. The dialog itself: SKIPPED (xdotool not installed) =="
fi

live_finish
