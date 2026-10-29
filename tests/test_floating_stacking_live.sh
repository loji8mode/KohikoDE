#!/bin/sh
# Live regression test: a floating window must stay visible (stacked above
# tiled windows, in its own intended order among other floats) when tiled
# windows are moved across it. Real kohiko under Xvfb, real pointer input,
# and - because the bug was purely about what is VISIBLE while the floating
# window's own geometry never changed - real pixels read back from the X
# server with x11_pixel_probe, plus the real X stacking order (xwininfo).
#
# The bug (0.21.5 and earlier): dragging a TILED window with Super+LMB
# raises it above everything (so it follows the cursor visibly); releasing
# without swapping with another tile snaps it back to its slot but never
# restored the stacking policy (floating above tiled) - the swap branch got
# that for free from Arrange(), the no-swap branch did not. The tiled window
# stayed on top of every floating window it had crossed, hiding them until
# something else raised them.
#
# Gracefully skipped (exit 0) without Xvfb/xdotool/xwininfo/binaries.

set -u
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT="$SCRIPT_DIR/.."
KOHIKO_BIN="${1:-$REPO_ROOT/kohiko}"
KOHIKOCTL="${2:-$REPO_ROOT/kohikoctl}"
CLIENT="${3:-$REPO_ROOT/build/x11_test_client}"
PROBE="${4:-$REPO_ROOT/build/x11_pixel_probe}"

pass=0; fail=0
check() { if [ "$1" = "$2" ]; then pass=$((pass+1)); echo "  PASS: $3"; else fail=$((fail+1)); echo "  FAIL: $3 (expected [$2], got [$1])"; fi; }

for tool in Xvfb xdotool xwininfo; do command -v "$tool" >/dev/null 2>&1 || { echo "SKIP: $tool not available."; exit 0; }; done
for bin in "$KOHIKO_BIN" "$KOHIKOCTL" "$CLIENT" "$PROBE"; do [ -x "$bin" ] || { echo "SKIP: $bin not built."; exit 0; }; done

WORK=$(mktemp -d); DISPNUM=${KOHIKO_TEST_DISPLAY_BASE:-170}; XVFB_PID=""; WM_PID=""
cleanup() { [ -n "$WM_PID" ] && kill -- "-$WM_PID" 2>/dev/null; [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null; wait 2>/dev/null; WM_PID=""; XVFB_PID=""; }
trap 'cleanup; rm -rf "$WORK"' EXIT

RED=ff0000; BLUE=0000ff; GREEN=00ff00; YELLOW=ffff00

start_session()
{
    cleanup
    DISPNUM=$((DISPNUM + 1)); S="$WORK/$1"
    mkdir -p "$S/config" "$S/data" "$S/run" "$S/home"; chmod 700 "$S/run"
    printf 'windowrule=float class:FloatA\nwindowrule=float class:FloatC\n' > "$S/kohiko.conf"
    rm -f "/tmp/.X${DISPNUM}-lock"
    Xvfb ":$DISPNUM" -screen 0 1920x1080x24 >/dev/null 2>&1 & XVFB_PID=$!
    n=0; while [ ! -e "/tmp/.X11-unix/X$DISPNUM" ] && [ "$n" -lt 50 ]; do sleep 0.1; n=$((n+1)); done
    export DISPLAY=":$DISPNUM" XDG_CONFIG_HOME="$S/config" XDG_DATA_HOME="$S/data" XDG_RUNTIME_DIR="$S/run" HOME="$S/home"
    setsid "$KOHIKO_BIN" "$S/kohiko.conf" >"$S/kohiko.log" 2>&1 & WM_PID=$!
    n=0; while ! grep -q ready "$S/kohiko.log" 2>/dev/null && [ "$n" -lt 80 ]; do sleep 0.1; n=$((n+1)); done
}

openw() # <class> <r,g,b> [extra args]  - a real client window of a solid color
{
    cls=$1; col=$2; shift 2
    "$CLIENT" --class "$cls" --title "$cls" --color "$col" "$@" >/dev/null 2>&1 &
    n=0; while ! "$KOHIKOCTL" clients 2>/dev/null | grep -q "\"class\":\"$cls\"" && [ "$n" -lt 40 ]; do sleep 0.1; n=$((n+1)); done
    sleep 0.35
}
rect() { "$KOHIKOCTL" clients | tr '{' '\n' | grep "\"class\":\"$1\"" | sed 's/.*"x":\([-0-9]*\),"y":\([-0-9]*\),"width":\([0-9]*\),"height":\([0-9]*\).*/\1 \2 \3 \4/'; }
pix() { "$PROBE" pixel "$1" "$2"; }
order() # top -> bottom among the named classes (xwininfo lists top-most first)
{
    xwininfo -root -children | grep -E "\"($1)\"" | sed 's/^ *0x[0-9a-f]* "\([A-Za-z]*\)".*/\1/' | tr '\n' ' ' | sed 's/ $//'
}
centre() { set -- $(rect "$1"); echo "$(( $1 + $3 / 2 )) $(( $2 + $4 / 2 ))"; }
overlap_centre() # centre of the intersection of two windows' rects
{
    set -- $(rect "$1") $(rect "$2")
    ax=$1; ay=$2; aw=$3; ah=$4; bx=$5; by=$6; bw=$7; bh=$8
    l=$(( ax > bx ? ax : bx )); r=$(( ax+aw < bx+bw ? ax+aw : bx+bw ))
    t=$(( ay > by ? ay : by )); b=$(( ay+ah < by+bh ? ay+ah : by+bh ))
    echo "$(( (l + r) / 2 )) $(( (t + b) / 2 ))"
}
super_drag() # <x1> <y1> <x2> <y2> [x3 y3]  press at 1, move via 2 (and 3), release at the last point
{
    x1=$1; y1=$2; x2=$3; y2=$4; x3=${5:-$3}; y3=${6:-$4}
    xdotool mousemove "$x1" "$y1" keydown super mousedown 1
    sleep 0.15; xdotool mousemove "$x2" "$y2"; sleep 0.12
    xdotool mousemove "$x3" "$y3"; sleep 0.12
    xdotool mouseup 1 keyup super; sleep 0.6
}

# ---------------------------------------------------------------------------
echo "-- One floating window, one tiled window dragged across it and dropped back --"
start_session single
openw TiledB 0,0,255
openw FloatA 255,0,0 --width 500 --height 400
set -- $(centre FloatA); AX=$1; AY=$2; A_RECT=$(rect FloatA)
check "$(pix $AX $AY)" "$RED" "before: the floating window is visible (its own colour at its centre)"
check "$(order 'FloatA|TiledB')" "FloatA TiledB" "before: stacking is floating above tiled"
super_drag 200 600 $AX $AY 200 600     # pick up B, carry it across A, put it back on its own slot
check "$(rect FloatA)" "$A_RECT" "after: the floating window's geometry is unchanged"
check "$(order 'FloatA|TiledB')" "FloatA TiledB" "after: the floating window is still stacked ABOVE the tiled one"
check "$(pix $AX $AY)" "$RED" "after: the floating window is actually visible on screen - no click needed"
check "$(pix 200 600)" "$BLUE" "and the tiled window is visible where it belongs"

echo "-- Partial coverage: a float straddling two tiled windows --"
start_session partial
openw TiledB 0,0,255
openw TiledD 0,255,0
openw FloatA 255,0,0 --width 500 --height 400
set -- $(centre FloatA); AX=$1; AY=$2; A_RECT=$(rect FloatA)
super_drag 200 700 $AX $AY 300 700
check "$(rect FloatA)" "$A_RECT" "float geometry unchanged"
check "$(pix $AX $AY)" "$RED" "the straddling floating window is still visible after a tiled window crossed it"
check "$(order 'FloatA|TiledB|TiledD' | awk '{print $1}')" "FloatA" "and it is still the top-most of the three"

echo "-- Dropping on another tile (a swap) keeps floats on top too --"
super_drag 200 700 $AX $AY 1500 700    # B dragged over A and dropped onto D: swaps
check "$(rect FloatA)" "$A_RECT" "float geometry unchanged after a swap"
check "$(pix $AX $AY)" "$RED" "the floating window is still visible after the swap"
check "$(order 'FloatA|TiledB|TiledD' | awk '{print $1}')" "FloatA" "and still top-most"

echo "-- Several floats, in both stacking orders --"
start_session multi
openw TiledB 0,0,255
openw FloatA 255,0,0 --width 500 --height 400
openw FloatC 255,255,0 --width 500 --height 400
set -- $(centre FloatA); X=$1; Y=$2
check "$(pix $X $Y)" "$YELLOW" "a newly opened floating window (C) appears on top of the existing one (A)"

# Spread them so they overlap only partly: drag C (top at the shared centre) down-right,
# then A (its top-left corner is now uncovered) up-left. A was dragged LAST => A on top.
super_drag $X $Y $((X + 130)) $((Y + 90))
set -- $(rect FloatA); super_drag $(( $1 + 20 )) $(( $2 + 20 )) $(( $1 + 20 - 130 )) $(( $2 + 20 - 90 ))
set -- $(overlap_centre FloatA FloatC); OX=$1; OY=$2
check "$(pix $OX $OY)" "$RED" "[A on top] the float dragged last (A) is on top where they overlap"
ra=$(rect FloatA); rc=$(rect FloatC)

super_drag 150 800 $OX $OY 150 800                    # carry the TILED window across both floats, put it back
check "$(rect FloatA)|$(rect FloatC)" "$ra|$rc" "[A on top] both floats keep their geometry"
check "$(pix $OX $OY)" "$RED" "[A on top] after a tiled window crossed them, A is still on top of C (relative order preserved)"
check "$(order 'FloatA|FloatC|TiledB' | awk '{print $1" "$2" "$3}')" "FloatA FloatC TiledB" "[A on top] real stacking: A, then C, then the tiled window below both"

# Now put C on top: grab it by a corner A does not cover and nudge it.
set -- $(rect FloatC); super_drag $(( $1 + $3 - 20 )) $(( $2 + $4 - 20 )) $(( $1 + $3 - 30 )) $(( $2 + $4 - 30 ))
set -- $(overlap_centre FloatA FloatC); OX=$1; OY=$2
check "$(pix $OX $OY)" "$YELLOW" "[C on top] raising C (a drag) puts it above A where they overlap"

super_drag 150 800 $OX $OY 150 800
check "$(pix $OX $OY)" "$YELLOW" "[C on top] after another tiled drag across them, C is still on top of A"
check "$(order 'FloatA|FloatC|TiledB' | awk '{print $1" "$2" "$3}')" "FloatC FloatA TiledB" "[C on top] real stacking: C, then A, then the tiled window"

openw TiledD 0,255,0                                    # a new window = another full Arrange() pass
check "$(pix $OX $OY)" "$YELLOW" "[C on top] a layout pass caused by a new window does not reshuffle the floats (0.21.5 put A back on top)"
check "$(order 'FloatA|FloatC' )" "FloatC FloatA" "[C on top] and the real stacking order of the floats is the one the user left them in"

echo "-- Focus and click behaviour are unchanged --"
start_session focus
openw TiledB 0,0,255
openw FloatA 255,0,0 --width 500 --height 400
set -- $(centre FloatA)
super_drag 200 600 $1 $2 200 600
xdotool mousemove 200 600 click 1; sleep 0.3
check "$("$KOHIKOCTL" activewindow | sed -n 's/.*"class":"\([^"]*\)".*/\1/p')" "TiledB" "clicking the tiled window still focuses it"
xdotool mousemove $1 $2 click 1; sleep 0.3
check "$("$KOHIKOCTL" activewindow | sed -n 's/.*"class":"\([^"]*\)".*/\1/p')" "FloatA" "clicking the floating window still focuses it"
check "$(order 'FloatA|TiledB')" "FloatA TiledB" "and focusing neither changed the stacking"

echo
echo "Passed: $pass   Failed: $fail"
[ "$fail" -eq 0 ]
