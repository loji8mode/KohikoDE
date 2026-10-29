#!/bin/sh
# Live regression test for tray-icon redraw latency (CHANGELOG.md 0.22.0).
#
# Runs the REAL kohiko as the tray host under Xvfb, and
# tests/live/tray_redraw_probe - which links the real TrayIconClient,
# docks into that tray, delivers a state change the way every Kohiko
# tray tool gets one (a WatchFd() file descriptor becoming readable:
# PipeWire's fd, DBus fds) and measures, from a second X connection,
# how long until the new pixels are actually on the server.
#
# The bug: TrayIconClient::Redraw() queued its drawing in Xlib's output
# buffer and Run() then blocked in poll() on the raw socket, which does
# not flush Xlib. The redraw only left the process at the next unrelated
# wakeup - for kohiko-audio-tray the 2 s PipeWire-reconnect timer,
# measured 31-1851 ms (median ~590) before the fix; with no timer at
# all, never.
#
# Two configurations are checked, because they pin different things:
#   - WITH a 2 s timer registered (what kohiko-audio-tray really has):
#     latency must not depend on where in the timer period the event
#     lands. Fixed value ~1 ms; regression value up to ~2000 ms.
#   - with NO timer: the redraw must arrive anyway - proving it's
#     event-driven rather than riding on someone else's wakeup.
#
# The limit is deliberately loose (250 ms vs ~1 ms fixed): scheduler
# noise must not fail this, only a real regression.
#
# Gracefully skipped (exit 0) if Xvfb/the binaries aren't available.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT="$SCRIPT_DIR/.."
KOHIKO_BIN="${1:-$REPO_ROOT/kohiko}"
PROBE="${2:-$REPO_ROOT/build/tray_redraw_probe}"
LIMIT_MS=250

pass=0
fail=0

check_lt()
{
    v=${1%%.*}
    if [ -n "$v" ] && [ "$v" -lt "$2" ] 2>/dev/null; then
        pass=$((pass + 1)); echo "  PASS: $3 (max ${1} ms < $2 ms)"
    else
        fail=$((fail + 1)); echo "  FAIL: $3 (max [$1] ms, limit $2 ms)"
    fi
}

command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: Xvfb not available."; exit 0; }
[ -x "$KOHIKO_BIN" ] || { echo "SKIP: $KOHIKO_BIN not built."; exit 0; }
[ -x "$PROBE" ]      || { echo "SKIP: $PROBE not built.";      exit 0; }

WORKDIR=$(mktemp -d)
XVFB_PID=""
WM_PID=""
DISPLAY_NUM=$((90 + ($$ % 100)))
export DISPLAY=":$DISPLAY_NUM"

cleanup()
{
    [ -n "$WM_PID" ] && kill "$WM_PID" 2>/dev/null
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT INT TERM

rm -f "/tmp/.X${DISPLAY_NUM}-lock"
Xvfb "$DISPLAY" -screen 0 1920x1080x24 > "$WORKDIR/xvfb.log" 2>&1 &
XVFB_PID=$!
waited=0
while [ ! -e "/tmp/.X11-unix/X${DISPLAY_NUM}" ] && [ "$waited" -lt 50 ]; do sleep 0.1; waited=$((waited + 1)); done

export XDG_CONFIG_HOME="$WORKDIR/config" XDG_RUNTIME_DIR="$WORKDIR/runtime" XDG_DATA_HOME="$WORKDIR/data"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_RUNTIME_DIR" "$XDG_DATA_HOME"
chmod 700 "$XDG_RUNTIME_DIR"

"$KOHIKO_BIN" > "$WORKDIR/kohiko.log" 2>&1 &
WM_PID=$!
waited=0
while ! grep -q '\[INFO\] ready' "$WORKDIR/kohiko.log" 2>/dev/null && [ "$waited" -lt 50 ]; do
    sleep 0.1; waited=$((waited + 1)); kill -0 "$WM_PID" 2>/dev/null || break
done
kill -0 "$WM_PID" 2>/dev/null || { echo "FAIL: kohiko did not start"; cat "$WORKDIR/kohiko.log"; exit 1; }

max_of() { grep '^summary' "$1" | sed 's/.*max_ms=\([0-9.]*\).*/\1/'; }

echo "-- tray redraw is event-driven --"

timeout 60 "$PROBE" --trials 6 --reconnect-timer > "$WORKDIR/with_timer.log" 2>&1
check_lt "$(max_of "$WORKDIR/with_timer.log")" "$LIMIT_MS" \
    "state change -> pixels on screen with a 2 s timer registered (latency must not track the timer phase)"

timeout 60 "$PROBE" --trials 4 > "$WORKDIR/no_timer.log" 2>&1
check_lt "$(max_of "$WORKDIR/no_timer.log")" "$LIMIT_MS" \
    "state change -> pixels on screen with NO timer at all (must not need any other wakeup)"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
