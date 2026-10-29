#!/bin/sh
# Live regression test for clickable taskbar workspace numbers - real
# kohiko under Xvfb, real mouse clicks (xdotool/XTEST) on the real bar,
# real pixels read back with x11_pixel_probe. The pure hit-test geometry is
# covered by tests/test_barworkspaceclick.cpp.
#
#   - a left click on number N switches to workspace N, for every
#     configured workspace;
#   - boundary pixels select exactly one indicator (no click lands on a
#     neighbour), and clicks outside the row, right/middle clicks, and
#     clicking the already-active number do nothing harmful;
#   - the highlighted (active) number really changes on screen, right away;
#   - the click goes through the same transition as the keyboard: an open
#     Launcher follows it, keeping its query.
#
# Gracefully skipped (exit 0) without Xvfb/xdotool/binaries.

set -u
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT="$SCRIPT_DIR/.."
KOHIKO_BIN="${1:-$REPO_ROOT/kohiko}"
KOHIKOCTL="${2:-$REPO_ROOT/kohikoctl}"
PROBE="${3:-$REPO_ROOT/build/x11_pixel_probe}"

pass=0; fail=0
check() { if [ "$1" = "$2" ]; then pass=$((pass+1)); echo "  PASS: $3"; else fail=$((fail+1)); echo "  FAIL: $3 (expected [$2], got [$1])"; fi; }

for tool in Xvfb xdotool; do command -v "$tool" >/dev/null 2>&1 || { echo "SKIP: $tool not available."; exit 0; }; done
for bin in "$KOHIKO_BIN" "$KOHIKOCTL" "$PROBE"; do [ -x "$bin" ] || { echo "SKIP: $bin not built."; exit 0; }; done

WORK=$(mktemp -d); DISPNUM=${KOHIKO_TEST_DISPLAY_BASE:-150}; XVFB_PID=""; WM_PID=""
trap '[ -n "$WM_PID" ] && kill "$WM_PID" 2>/dev/null; [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORK"' EXIT

mkdir -p "$WORK/config" "$WORK/data" "$WORK/run" "$WORK/home"; chmod 700 "$WORK/run"
printf 'workspace.count=9\nbind=SUPER+D launcher_toggle\n' > "$WORK/kohiko.conf"
rm -f "/tmp/.X${DISPNUM}-lock"
Xvfb ":$DISPNUM" -screen 0 1920x1080x24 >/dev/null 2>&1 & XVFB_PID=$!
n=0; while [ ! -e "/tmp/.X11-unix/X$DISPNUM" ] && [ "$n" -lt 50 ]; do sleep 0.1; n=$((n+1)); done
export DISPLAY=":$DISPNUM" XDG_CONFIG_HOME="$WORK/config" XDG_DATA_HOME="$WORK/data" XDG_RUNTIME_DIR="$WORK/run" HOME="$WORK/home"
"$KOHIKO_BIN" "$WORK/kohiko.conf" >"$WORK/kohiko.log" 2>&1 & WM_PID=$!
n=0; while ! grep -q ready "$WORK/kohiko.log" 2>/dev/null && [ "$n" -lt 80 ]; do sleep 0.1; n=$((n+1)); done
sleep 0.8

cur_ws() { "$KOHIKOCTL" monitors 2>/dev/null | grep -o '"workspace":[0-9]*' | head -1 | cut -d: -f2; }
lq() { "$KOHIKOCTL" launcher 2>/dev/null | sed -n 's/.*"query":"\([^"]*\)".*/\1/p'; }
lopen() { "$KOHIKOCTL" launcher 2>/dev/null | sed -n 's/.*"open":\([a-z]*\).*/\1/p'; }
# Constants mirror Bar::WorkspaceGlyphX/SlotLeft (10 + 22(i-1), inset 5, width 22); the
# unit test pins those, this test pins the end-to-end behaviour built on them.
glyph() { echo $((10 + 22 * ($1 - 1))); }
slotleft() { echo $(($(glyph "$1") - 5)); }
BARY=13
click() { xdotool mousemove "$1" "$BARY" click "${2:-1}"; sleep 0.35; }
box() { "$PROBE" checksum "$(slotleft "$1")" 2 22 22; }   # the number's own box on the real screen

echo "-- Left-clicking number N switches to workspace N (every configured workspace) --"
check "$(cur_ws)" "1" "starts on workspace 1"
for i in 2 3 4 5 6 7 8 9 1; do
    click $(($(glyph "$i") + 4))
    check "$(cur_ws)" "$i" "clicking the '$i' on the taskbar switched to workspace $i"
done

echo "-- Boundaries: a click lands on exactly one indicator --"
click "$(slotleft 5)";                        check "$(cur_ws)" "5" "first pixel of box 5 selects 5"
click $(($(slotleft 5) + 21));                check "$(cur_ws)" "5" "last pixel of box 5 selects 5"
click $(($(slotleft 6) + 0));                 check "$(cur_ws)" "6" "the very next pixel belongs to 6, not 5"
click $(($(slotleft 4) + 21));                check "$(cur_ws)" "4" "last pixel of box 4 selects 4 (not its neighbour 5)"

echo "-- Clicks that must do nothing --"
click 4;                                      check "$(cur_ws)" "4" "left of the first number: nothing"
click $(($(slotleft 9) + 22 + 6));            check "$(cur_ws)" "4" "just right of the last number: nothing"
click $(($(glyph 7) + 4)) 3;                  check "$(cur_ws)" "4" "right-click on a number: nothing"
click $(($(glyph 7) + 4)) 2;                  check "$(cur_ws)" "4" "middle-click on a number: nothing"
click $(($(glyph 4) + 4));                    check "$(cur_ws)" "4" "clicking the already-active number: stays on it"

echo "-- The highlight really changes on screen, immediately --"
click $(($(glyph 1) + 4))
before1=$(box 1); before3=$(box 3)
click $(($(glyph 3) + 4))
check "$([ "$(box 3)" != "$before3" ] && echo changed || echo same)" "changed" "number 3's box is drawn differently once 3 is the active workspace"
check "$([ "$(box 1)" != "$before1" ] && echo changed || echo same)" "changed" "number 1's box is redrawn when it stops being active"
click $(($(glyph 1) + 4))
check "$([ "$(box 1)" = "$before1" ] && echo same || echo different)" "same" "returning to 1 restores 1's original drawing (a pure active/inactive distinction)"

echo "-- A click is the same transition as the keyboard: the Launcher follows it --"
click $(($(glyph 3) + 4))
"$KOHIKOCTL" dispatch launcher_toggle >/dev/null; sleep 0.4
xdotool type --delay 25 keep; sleep 0.3
check "$(lopen)" "true" "the Launcher is open on workspace 3"
click $(($(glyph 6) + 4))
check "$(cur_ws)" "6" "clicking '6' on the taskbar switched to workspace 6"
check "$(lopen)" "true" "the Launcher followed (still open)"
check "$(lq)" "keep" "and kept its query"
xdotool type --delay 25 ing; sleep 0.3
check "$(lq)" "keeping" "and is still usable on the new workspace"

echo
echo "Passed: $pass   Failed: $fail"
[ "$fail" -eq 0 ]
