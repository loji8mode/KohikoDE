#!/bin/sh
# Live regression test: a floating window must not disappear under a
# tiled window that was merely moved across it (CHANGELOG.md, Unreleased).
#
# The REAL kohiko under Xvfb, real clients that paint their own pixels,
# real (XTEST) Super+drag input, and an observer that asks the X server
# - not Kohiko - for the stacking order and the pixels actually on
# screen. Nothing about this test trusts geometry alone: in the original
# bug the floating window's geometry never changed, only what was on top.
#
# Root cause pinned here: WindowManager::BeginSwapDrag() raises the
# dragged tile above everything so it can be carried over other windows;
# EndSwapDrag()'s "no valid drop target" branch snapped it back WITHOUT
# restacking, so the tile stayed above every floating window it had been
# carried across (or that overlapped its own slot) until some unrelated
# Arrange() happened to re-raise the floating tier.
#
# Also pinned: the floating windows keep their relative order across
# layout changes (ArrangeMonitor() used to re-raise them in repository
# order, so a workspace round trip swapped two overlapping floats).
#
# Usage: test_floating_stacking_live.sh [kohiko] [kohikoctl] [x11_test_client] [x11_probe] [x11_input]
# Skipped (exit 0) when Xvfb/XTEST or a binary isn't available.

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

live_init 160

RED=$(live_hex 220,30,30); BLUE=$(live_hex 30,30,220)

live_write_config 'workspace.count=4
auto_start_programs=
windowrule=app:flA mode:floating
windowrule=app:flB mode:floating'
live_start_wm

geom() { clients_lines | grep "\"class\":\"$1\"" | sed -n 's/.*"x":\([-0-9]*\),"y":\([-0-9]*\),"width":\([0-9]*\),"height":\([0-9]*\).*/\1,\2,\3,\4/p'; }

echo "== 1. Tile carried across a floating window and dropped with no target =="

live_open TileL --color 30,30,220
live_open TileR --color 30,200,30
live_open flA --color 220,30,30

A_GEOM=$(geom flA)
# 800,540 is inside flA (it is centred) AND inside TileL's slot.
check "$(live_pixel 800 540)" "$RED" "before: the floating window is what is on screen in the overlap"
check_true "before: flA is above TileL in the real X stacking order" live_is_above flA TileL

"$INPUT" move 300 500
"$INPUT" keydown Super_L
"$INPUT" press 1
"$INPUT" move 600 520
"$INPUT" move 900 540
check "$(live_pixel 800 540)" "$BLUE" "mid-drag: the carried tile is deliberately on top"
"$INPUT" move 400 700            # back over its own slot: no swap target
"$INPUT" release 1
"$INPUT" keyup Super_L
sleep 0.5                        # the 160 ms snap-back animation

check_true "after the drop: flA is above TileL again (X stacking order)" live_is_above flA TileL
check "$(live_pixel 800 540)" "$RED" "after the drop: the floating window's pixels are visible without clicking it"
check "$(geom flA)" "$A_GEOM" "the floating window's geometry never changed"

echo "== 2. Same, dropped on the bare gap between tiles =="
"$INPUT" move 300 500
"$INPUT" keydown Super_L
"$INPUT" press 1
"$INPUT" move 700 520
"$INPUT" move 958 520            # the 10 px gap between the two tiles
"$INPUT" release 1
"$INPUT" keyup Super_L
sleep 0.5
check_true "gap drop: flA above TileL" live_is_above flA TileL
check_true "gap drop: flA above TileR" live_is_above flA TileR
check "$(live_pixel 800 540)" "$RED" "gap drop: flA visible in the overlap"

echo "== 3. A real swap still works and the floating window stays on top =="
"$INPUT" move 300 500
"$INPUT" keydown Super_L
"$INPUT" press 1
"$INPUT" move 700 520
"$INPUT" move 1400 500           # over TileR
"$INPUT" release 1
"$INPUT" keyup Super_L
sleep 0.5
check "$(live_field TileL x)" "963" "swap: TileL moved to the right slot"
check_true "swap: flA above TileL" live_is_above flA TileL
check_true "swap: flA above TileR" live_is_above flA TileR
check "$(live_pixel 960 540)" "$RED" "swap: flA visible"

echo "== 4. Several floating windows keep their order across layout changes =="
live_open flB --color 220,200,30
check_true "flB opened on top of flA" live_is_above flB flA
"$KOHIKOCTL" dispatch workspace 2 >/dev/null; "$KOHIKOCTL" dispatch workspace 1 >/dev/null
sleep 0.3
check_true "after a workspace round trip flB is still above flA" live_is_above flB flA
check_true "...and both floating windows are above the tiles" live_is_above flA TileR
"$KOHIKOCTL" dispatch rotate >/dev/null; sleep 0.2
check_true "after rotate flB is still above flA" live_is_above flB flA

echo "== 5. Focusing a lower floating window still raises it (normal focus behaviour) =="
FA=$(live_field flA id)
# Click flA at a point flB does not cover (flB is centred too, so use
# an edge of flA that sticks out): flA 760..1160 vs flB same size and
# position - they coincide, so use super-drag to offset flB first.
"$INPUT" move 960 540
"$INPUT" keydown Super_L
"$INPUT" press 1
"$INPUT" move 1000 600
"$INPUT" move 1200 700
"$INPUT" release 1
"$INPUT" keyup Super_L
sleep 0.4
check_true "dragging flB leaves it on top of flA" live_is_above flB flA
# flA is now partly uncovered at the left; click it.
"$INPUT" click 780 560
sleep 0.2
"$KOHIKOCTL" dispatch workspace 2 >/dev/null; "$KOHIKOCTL" dispatch workspace 1 >/dev/null
sleep 0.3
check_true "clicking flA focused it, so it is raised at the next restack" live_is_above flA flB
check "$(live_field flA focused)" "true" "flA is the focused window"

echo "== 6. Nothing here touched the window rules =="
check "$(grep -c '^windowrule=' "$CONF")" "2" "still exactly the two rules the test wrote"

live_finish
