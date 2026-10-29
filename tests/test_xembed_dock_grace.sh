#!/bin/sh
# Live regression test for how Manage() treats windows carrying
# _XEMBED_INFO (CHANGELOG.md 0.20.4 introduced the hold, 0.22.0 fixed
# its cost) - runs the *real* kohiko binary as an actual window manager
# under Xvfb, against tests/live/x11_test_client (a small, configurable,
# real X11 client - not a mock of anything).
#
# The behaviour being pinned has two halves that must BOTH hold:
#
#   1. Responsiveness. Every Qt top-level window carries _XEMBED_INFO
#      (a real FeatherPad main window does - verified on a real Qt 5.15
#      xcb build) even though it will never dock anywhere. Such a window
#      must be managed promptly. Before 0.22.0 it was held for a fixed
#      2 s deadline that was only evaluated at the ~1 Hz Tick(), i.e.
#      2-3 s - measured 2273-2378 ms create-to-map on real FeatherPad.
#
#   2. Correctness. A *genuine* tray icon - same property, followed by a
#      real SYSTEM_TRAY_REQUEST_DOCK - must still be docked with no
#      window ever briefly appearing mapped at root level first (the
#      cosmetic "flash" the hold exists to prevent), for a dock request
#      arriving right after the map (one X round trip) and inside the
#      grace window. And even one that arrives long AFTER the grace
#      window must still end up docked and viewable - the icon is never
#      lost, at worst it flashes once (the reverse-race path in
#      HandleClientMessage()).
#
# Thresholds are deliberately loose (the regression being guarded
# against is ~2300 ms; the fixed value is ~100 ms): this must not flake
# on a loaded machine, only fail on a real regression.
#
# Gracefully skipped (exit 0) if Xvfb/xwininfo/the kohiko binary/the
# test client aren't available.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT="$SCRIPT_DIR/.."
KOHIKO_BIN="${1:-$REPO_ROOT/kohiko}"
TEST_CLIENT="${2:-$REPO_ROOT/build/x11_test_client}"

# A window must be managed within this many ms. Real fixed value: ~100.
# Regression value: >= 2000.
MAX_PROMPT_MS=900

pass=0
fail=0

check()
{
    if [ "$1" = "$2" ]; then
        pass=$((pass + 1))
        echo "  PASS: $3"
    else
        fail=$((fail + 1))
        echo "  FAIL: $3 (expected [$2], got [$1])"
    fi
}

# check_lt <value> <limit> <message>: integer-compares after truncating decimals
check_lt()
{
    v=${1%%.*}
    if [ -n "$v" ] && [ "$v" -lt "$2" ] 2>/dev/null; then
        pass=$((pass + 1))
        echo "  PASS: $3 ($1 ms < $2 ms)"
    else
        fail=$((fail + 1))
        echo "  FAIL: $3 (got [$1], limit $2)"
    fi
}

for tool in Xvfb xwininfo; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "SKIP: $tool not available."
        exit 0
    fi
done

[ -x "$KOHIKO_BIN" ]  || { echo "SKIP: $KOHIKO_BIN not built.";  exit 0; }
[ -x "$TEST_CLIENT" ] || { echo "SKIP: $TEST_CLIENT not built."; exit 0; }

WORKDIR=$(mktemp -d)
XVFB_PID=""
WM_PID=""
CLIENT_PIDS=""
DISPLAY_NUM=$((90 + ($$ % 100)))
export DISPLAY=":$DISPLAY_NUM"

cleanup()
{
    for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
    [ -n "$WM_PID" ] && kill "$WM_PID" 2>/dev/null
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT INT TERM

rm -f "/tmp/.X${DISPLAY_NUM}-lock"
Xvfb "$DISPLAY" -screen 0 1920x1080x24 > "$WORKDIR/xvfb.log" 2>&1 &
XVFB_PID=$!

waited=0
while [ ! -e "/tmp/.X11-unix/X${DISPLAY_NUM}" ] && [ "$waited" -lt 50 ]; do
    sleep 0.1
    waited=$((waited + 1))
done

export XDG_CONFIG_HOME="$WORKDIR/config"
export XDG_RUNTIME_DIR="$WORKDIR/runtime"
export XDG_DATA_HOME="$WORKDIR/data"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_RUNTIME_DIR" "$XDG_DATA_HOME"
chmod 700 "$XDG_RUNTIME_DIR"

"$KOHIKO_BIN" > "$WORKDIR/kohiko.log" 2>&1 &
WM_PID=$!

waited=0
while ! grep -q '\[INFO\] ready' "$WORKDIR/kohiko.log" 2>/dev/null && [ "$waited" -lt 50 ]; do
    sleep 0.1
    waited=$((waited + 1))
    kill -0 "$WM_PID" 2>/dev/null || break
done

if ! kill -0 "$WM_PID" 2>/dev/null; then
    echo "FAIL: kohiko did not start"
    cat "$WORKDIR/kohiko.log"
    exit 1
fi

# launch <logname> <client args...>  - starts a client, remembers its pid
launch()
{
    name=$1; shift
    "$TEST_CLIENT" "$@" > "$WORKDIR/$name.log" 2>&1 &
    CLIENT_PIDS="$CLIENT_PIDS $!"
}

# wait_for <logname> <regex> <max tenths of a second> -> 0 if seen
wait_for()
{
    i=0
    while [ "$i" -lt "$3" ]; do
        grep -q "$2" "$WORKDIR/$1.log" 2>/dev/null && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

latency_of()  { grep -o 'MAP_LATENCY_MS=[0-9.]*' "$WORKDIR/$1.log" | head -1 | cut -d= -f2; }
win_of()      { grep -o 'WINDOW=0x[0-9a-f]*' "$WORKDIR/$1.log" | head -1 | cut -d= -f2; }
# First EVENT= line a window ever saw, e.g. MapNotify / ReparentNotify.
first_event() { grep -o '^EVENT=[A-Za-z]*' "$WORKDIR/$1.log" | head -1 | cut -d= -f2; }

echo "-- 1. Responsiveness: non-docking windows must be managed promptly --"

launch A --title Control --class TestApp --width 400 --height 300 --report-map-latency
wait_for A 'MAP_LATENCY_MS' 60
check_lt "$(latency_of A)" "$MAX_PROMPT_MS" \
    "plain window (no _XEMBED_INFO) is managed promptly"

# The Qt case: _XEMBED_INFO present, never docks.
launch B --title QtLike --class TestApp --width 400 --height 300 --xembed --report-map-latency
wait_for B 'MAP_LATENCY_MS' 60
check_lt "$(latency_of B)" "$MAX_PROMPT_MS" \
    "window carrying _XEMBED_INFO that never docks (every Qt top-level) is managed promptly, not after ~2-3 s"
check "$(xwininfo -id "$(win_of B)" | grep -c 'Width: 400')" "0" \
    "...and it was actually tiled (resized from its 400px creation width), not left untouched or invisible"

echo
echo "-- 2. Correctness: genuine tray icons are still docked, never flashed --"

# A: map, one X round trip, dock - the existing synthetic ordering.
launch C --title DockNow --class TestApp --width 24 --height 24 --xembed --send-dock-request --report-events
wait_for C 'EVENT=' 30
sleep 0.4
check "$(first_event C)" "ReparentNotify" \
    "dock right after map: the window is reparented BEFORE it is ever mapped at root level (no flash)"
check "$(xwininfo -id "$(win_of C)" 2>&1 | grep -c 'IsViewable')" "1" \
    "...and it is docked and viewable"
check "$(xwininfo -id "$(win_of C)" 2>&1 | grep -c 'Width: 24')" "0" \
    "...and sized to fit the tray (ReflowIcons), not left at its raw 24px request nor tiled to 400x300"

# B: same, but the dock request trails the map by 40 ms - inside the grace.
launch D --title DockSoon --class TestApp --width 24 --height 24 --xembed \
    --send-dock-request --dock-delay-ms 40 --report-events
wait_for D 'EVENT=' 30
sleep 0.5
check "$(first_event D)" "ReparentNotify" \
    "dock 40 ms after map (inside the grace window): still no flash"
check "$(xwininfo -id "$(win_of D)" 2>&1 | grep -c 'IsViewable')" "1" \
    "...and docked and viewable"

# C: dock request far outside the grace (400 ms). Documented worst case:
# the window may have been tiled first (a brief flash), but the icon must
# NEVER be lost - HandleClientMessage() must unmanage and dock it.
launch E --title DockLate --class TestApp --width 24 --height 24 --xembed \
    --send-dock-request --dock-delay-ms 400 --report-events
wait_for E 'EVENT=' 30
sleep 1.2
check "$(xwininfo -id "$(win_of E)" 2>&1 | grep -c 'IsViewable')" "1" \
    "dock arriving long after the grace window: the icon is still docked and viewable (never lost)"
check "$(xwininfo -id "$(win_of E)" 2>&1 | grep -c 'Width: 24')" "0" \
    "...and re-sized for the tray rather than left as a tiled window"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
