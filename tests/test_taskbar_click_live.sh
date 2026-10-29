#!/bin/sh
# Live regression test: clicking a workspace number on the taskbar
# switches to that workspace. The REAL kohiko under Xvfb, real (XTEST)
# pointer clicks on the real bar window, observed through Kohiko's IPC
# AND through the pixels the bar actually painted.
#
# Pixel inspection: the sandbox Xft shim paints each text run as a solid
# block in its text colour (see /home/claude/shims - NOT part of Kohiko),
# which is what makes "which number is drawn in the active colour"
# observable without real fonts. With real Xft the same pixels are glyph
# anti-aliasing, so this check is meaningful only against that shim.
# Set TASKBAR_TEST_PIXELS=0 to skip the pixel checks on a real-font build.
#
# Usage: test_taskbar_click_live.sh [kohiko] [kohikoctl] [x11_test_client] [x11_probe] [x11_input]

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT="$SCRIPT_DIR/.."
KOHIKO_BIN="${1:-$REPO_ROOT/kohiko}"
KOHIKOCTL="${2:-$REPO_ROOT/kohikoctl}"
TEST_CLIENT="${3:-$REPO_ROOT/build/x11_test_client}"
PROBE="${4:-$REPO_ROOT/build/x11_probe}"
INPUT="${5:-$REPO_ROOT/build/x11_input}"

. "$SCRIPT_DIR/live/common.sh"

live_require Xvfb
[ -x "$PROBE" ] && [ -x "$INPUT" ] || { echo "SKIP: x11_probe/x11_input not built."; exit 0; }

live_init 280

ACTIVE=89b4fa; NORMAL=cdd6f4   # bar.active / bar.foreground defaults

active_ws() { "$KOHIKOCTL" monitors | sed -n 's/.*"workspace":\([0-9]*\).*/\1/p' | head -1; }

# Bar geometry (include/BarLayout.h): label n at x = 10 + 22*(n-1); its
# clickable cell is [label-4, label+18). Bar is 26 px tall; click mid-height.
label_x()  { echo $((10 + 22 * ($1 - 1))); }
cell_x0()  { echo $((10 + 22 * ($1 - 1) - 4)); }
click_ws() { "$INPUT" click "$(( $(cell_x0 "$1") + 11 ))" 13; sleep 0.15; }

# The colour painted for label n (a pixel inside its drawn run).
label_colour() { live_pixel "$(( $(label_x "$1") + 3 ))" 13; }

pixels_ok=1
[ "${TASKBAR_TEST_PIXELS:-1}" = "0" ] && pixels_ok=0

active_indicators()   # which workspace numbers are drawn in the active colour
{
    out=""
    for n in $(seq 1 "$1"); do [ "$(label_colour "$n")" = "$ACTIVE" ] && out="$out$n,"; done
    echo "$out"
}

live_write_config 'workspace.count=4
auto_start_programs=
bind=SUPER+D launcher_toggle
bind=SUPER+2 workspace 2'
live_start_wm
sleep 0.4

echo "== 1. Left-clicking workspace number N switches to workspace N =="
check "$(active_ws)" "1" "starts on workspace 1"
[ "$pixels_ok" = 1 ] && check "$(active_indicators 4)" "1," "the bar draws exactly workspace 1 in the active colour"

for n in 3 1 4 2; do
    click_ws "$n"
    check "$(active_ws)" "$n" "clicking number $n switches to workspace $n"
    [ "$pixels_ok" = 1 ] && check "$(active_indicators 4)" "$n," "the active indicator moved to $n immediately - and only $n is highlighted"
done

echo "== 2. Hitboxes: no neighbour is activated by a click near an edge =="
click_ws 1
# Last pixel of cell 2 -> 2, first pixel of cell 3 -> 3, last of 3 -> 3.
"$INPUT" click "$(( $(cell_x0 3) - 1 ))" 13; sleep 0.15
check "$(active_ws)" "2" "the last pixel of cell 2 selects 2"
"$INPUT" click "$(cell_x0 3)" 13; sleep 0.15
check "$(active_ws)" "3" "the first pixel of cell 3 selects 3 (no gap, no overlap)"
"$INPUT" click "$(( $(cell_x0 4) - 1 ))" 1; sleep 0.15
check "$(active_ws)" "3" "the top row of cell 3 still selects 3"
"$INPUT" click "$(( $(cell_x0 3) + 21 ))" 25; sleep 0.15
check "$(active_ws)" "3" "the bottom-right pixel of cell 3 selects 3, not 4"

echo "== 3. Clicks that are not on a number do nothing =="
click_ws 2
check "$(active_ws)" "2" "(set up: on workspace 2)"
"$INPUT" click "$(( $(cell_x0 4) + 22 + 10 ))" 13; sleep 0.15
check "$(active_ws)" "2" "right of the last number (only 4 configured)"
"$INPUT" click 3 13; sleep 0.15
check "$(active_ws)" "2" "left of the first number"
"$INPUT" click "$(( $(cell_x0 4) + 11 ))" 13 3; sleep 0.15
check "$(active_ws)" "2" "a RIGHT click on a number does nothing"
"$INPUT" click "$(( $(cell_x0 4) + 11 ))" 13 2; sleep 0.15
check "$(active_ws)" "2" "a MIDDLE click on a number does nothing"
click_ws 2
check "$(active_ws)" "2" "clicking the workspace already shown is harmless"
[ "$pixels_ok" = 1 ] && check "$(active_indicators 4)" "2," "...and the indicator is unchanged"

echo "== 4. Mouse and keyboard end in the same transition =="
"$INPUT" key 2 super; sleep 0.15
click_ws 3
check "$(active_ws)" "3" "click after a SUPER+2 shortcut"
"$INPUT" key 2 super; sleep 0.15
check "$(active_ws)" "2" "shortcut after a click"
[ "$pixels_ok" = 1 ] && check "$(active_indicators 4)" "2," "the indicator follows both"

echo "== 5. An open Launcher follows a click on a workspace number =="
"$INPUT" key d super; sleep 0.2
"$INPUT" type al
check "$("$KOHIKOCTL" launcher | sed -n 's/.*"query":"\([^"]*\)".*/\1/p')" "al" "Launcher open with query 'al' on workspace 2"
click_ws 4
check "$(active_ws)" "4" "clicking number 4 switches to workspace 4"
check "$("$KOHIKOCTL" launcher | sed -n 's/.*"open":\([a-z]*\).*/\1/p')" "true" "the Launcher is still open"
check "$("$KOHIKOCTL" launcher | sed -n 's/.*"query":"\([^"]*\)".*/\1/p')" "al" "its query is intact"
check "$("$PROBE" focus)" "$(printf '0x%x' "$("$KOHIKOCTL" launcher | sed -n 's/.*"window":\([0-9]*\).*/\1/p')")" "and it still has keyboard focus"
"$INPUT" key Escape; sleep 0.15

echo "== 6. Other bar interactions still work =="
BEFORE=$("$PROBE" tree | grep -c ' state=mapped or=1 ')
# [Power] sits just left of the clock at the right end of the 1920 px bar.
"$INPUT" click 1790 13; sleep 0.3
AFTER=$("$PROBE" tree | grep -c ' state=mapped or=1 ')
check "$(active_ws)" "4" "a click on the power button does not switch workspace"
check_true "...it opens the power menu (an extra override-redirect window is mapped)" [ "$AFTER" -gt "$BEFORE" ]
"$INPUT" click 300 13; sleep 0.2      # an empty part of the bar dismisses it
check "$("$PROBE" tree | grep -c ' state=mapped or=1 ')" "$BEFORE" "a click elsewhere on the bar dismisses the power menu"
check "$(active_ws)" "4" "...without switching workspace"

echo "== 7. Every configured workspace is clickable (workspace.count=9) =="
# workspace.count is read when Kohiko starts (a reload does not add or
# remove workspaces), so this restarts the window manager.
live_quit_wm
live_write_config 'workspace.count=9
auto_start_programs='
live_start_wm
sleep 0.4
for n in 9 5 1 8; do
    click_ws "$n"
    check "$(active_ws)" "$n" "workspace $n is clickable"
done
[ "$pixels_ok" = 1 ] && check "$(active_indicators 9)" "8," "and only 8 is highlighted"

live_finish
