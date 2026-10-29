#!/bin/sh
# Live regression test: every Kohiko native window (audio, network,
# Bluetooth, settings...) runs on UiWindow::Run(). A close request - the
# window's [x] - must be honoured even when it arrives while the process
# is busy in a timer/fd callback that makes an X round trip.
#
# Root cause pinned here: Xlib keeps its own event queue in front of the
# socket, and any call that waits for a server reply (XSync, XGetImage,
# XQueryPointer, ...) parks every event that arrived meanwhile in that
# queue. UiWindow::Run() only looked at XPending() after poll() reported
# NEW data on the X socket, so a parked WM_DELETE_WINDOW sat there until
# an unrelated event happened to wake poll() - a window that "cannot be
# closed". TrayIconClient fixed the same thing earlier; UiWindow never
# got it.
#
# Real X server (Xvfb), real XTEST input and a real WM_DELETE_WINDOW
# ClientMessage, the real UiWindow event loop (tests/live/ui_probe.cpp).
# No window manager is involved.
#
# Usage: test_ui_event_queue_live.sh [ui_probe] [x11_input]

set -u
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT="$SCRIPT_DIR/.."
UI_PROBE="${1:-$REPO_ROOT/build/ui_probe}"
INPUT="${2:-$REPO_ROOT/build/x11_input}"
X11_PROBE="${3:-$REPO_ROOT/build/x11_probe}"

command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: Xvfb not available."; exit 0; }
for b in "$UI_PROBE" "$INPUT"; do [ -x "$b" ] || { echo "SKIP: $b not built."; exit 0; }; done

PASS=0; FAIL=0
check() { if [ "$1" = "$2" ]; then PASS=$((PASS+1)); echo "  PASS: $3"; else FAIL=$((FAIL+1)); echo "  FAIL: $3 (got '$1', want '$2')"; fi; }

WORK=$(mktemp -d); DN=$((380 + $$ % 40)); export DISPLAY=":$DN"
export HOME="$WORK/home" XDG_RUNTIME_DIR="$WORK/run" XDG_CONFIG_HOME="$WORK/home/.config"
mkdir -p "$HOME" "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
rm -f "/tmp/.X${DN}-lock" "/tmp/.X11-unix/X${DN}"
Xvfb "$DISPLAY" -screen 0 800x600x24 >/dev/null 2>&1 & XVFB=$!
trap 'kill $XVFB $PROBE_PID 2>/dev/null; rm -rf "$WORK"' EXIT
i=0; while [ ! -e "/tmp/.X11-unix/X$DN" ] && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i+1)); done
# The socket file appears slightly before the server accepts connections: wait until a client really can.
i=0; while ! "$X11_PROBE" focus >/dev/null 2>&1 && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i+1)); done
now_ms() { date +%s%3N; }

run_case()   # run_case <label> <probe args...>   -> sets CLOSE_MS or -1
{
    label=$1; shift
    : > "$WORK/out"
    "$UI_PROBE" "$@" > "$WORK/out" 2>&1 & PROBE_PID=$!
    i=0; while ! grep -q READY "$WORK/out" && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i+1)); done
    WIN=$(sed -n 's/^READY //p' "$WORK/out")
    sleep 0.3
    case "$label" in
        click) ( "$INPUT" click 40 40 >/dev/null 2>&1 ) & sleep 0.25 ;;
        timer) i=0; while ! grep -q TIMER_STALL "$WORK/out" && [ "$i" -lt 60 ]; do sleep 0.05; i=$((i+1)); done; sleep 0.2 ;;
    esac
    T0=$(now_ms)
    "$INPUT" close "$WIN"
    i=0; while kill -0 "$PROBE_PID" 2>/dev/null && [ "$i" -lt 60 ]; do sleep 0.05; i=$((i+1)); done
    if kill -0 "$PROBE_PID" 2>/dev/null; then CLOSE_MS=-1; kill "$PROBE_PID" 2>/dev/null; else CLOSE_MS=$(( $(now_ms) - T0 )); fi
    wait "$PROBE_PID" 2>/dev/null
}

echo "== 1. Close request while a TIMER callback is stalled in an X round trip =="
run_case timer --timer-stall-ms 800
check "$([ "$CLOSE_MS" -ge 0 ] && [ "$CLOSE_MS" -lt 1500 ] && echo closed || echo "still-open($CLOSE_MS)")" "closed" "the window closes (took ${CLOSE_MS} ms; the stalled callback itself lasts ~800 ms)"
check "$(grep -c EXIT "$WORK/out")" "1" "the event loop ended cleanly"

echo "== 2. Close request while a click handler is busy =="
run_case click --stall-ms 600
check "$([ "$CLOSE_MS" -ge 0 ] && [ "$CLOSE_MS" -lt 1500 ] && echo closed || echo "still-open($CLOSE_MS)")" "closed" "the window closes (took ${CLOSE_MS} ms)"

echo "== 3. An idle window closes at once and uses no CPU while idle =="
run_case idle
check "$([ "$CLOSE_MS" -ge 0 ] && [ "$CLOSE_MS" -lt 300 ] && echo closed || echo "slow($CLOSE_MS)")" "closed" "an idle window closes within 300 ms (took ${CLOSE_MS} ms)"
"$UI_PROBE" > "$WORK/out" 2>&1 & PROBE_PID=$!
sleep 0.6; A=$(awk '{print $14+$15}' /proc/$PROBE_PID/stat); sleep 3; B=$(awk '{print $14+$15}' /proc/$PROBE_PID/stat)
check "$([ $((B - A)) -le 2 ] && echo idle || echo "busy($((B - A)) ticks)")" "idle" "an idle UiWindow does not spin or poll (CPU ticks in 3 s: $((B - A)))"
kill "$PROBE_PID" 2>/dev/null

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
