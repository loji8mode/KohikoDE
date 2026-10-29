#!/bin/sh
# Live test: the native control apps stay usable while a slow operation runs.
#
# The REAL kohiko-bluetooth application code - BluetoothWindow, BluezClient,
# the worker queue, UiWindow and its event loop - built against a scripted
# FAKE D-Bus (tests/fakes/FakeDBusClient.cpp) so that Connect can be made to
# take 3 seconds or to fail, with no BlueZ on the machine. Real XTEST input
# on a real X server. What this proves is the application's behaviour; what
# it cannot prove is how the real BlueZ/NetworkManager/PipeWire daemons
# behave - that needs the real PC (see the final report).
#
# For every check it asks the question a user would: with Connect running,
# can I still use the window, can I close it, was I told how it went?
#
# Usage: test_native_responsive_live.sh [kohiko-bluetooth-fakebus] [x11_probe] [x11_input]
# Skipped (exit 0) when Xvfb or a binary isn't available.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT="$SCRIPT_DIR/.."
APP="${1:-$REPO_ROOT/build/kohiko-bluetooth-fakebus}"
PROBE="${2:-$REPO_ROOT/build/x11_probe}"
INPUT="${3:-$REPO_ROOT/build/x11_input}"
BASELINE_NOTE="${NATIVE_TEST_BLOCKING_BASELINE:-0}"

command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: Xvfb not available."; exit 0; }
for b in "$APP" "$PROBE" "$INPUT"; do [ -x "$b" ] || { echo "SKIP: $b not built."; exit 0; }; done

PASS=0; FAIL=0
check() { if [ "$1" = "$2" ]; then PASS=$((PASS+1)); echo "  PASS: $3"; else FAIL=$((FAIL+1)); echo "  FAIL: $3 (got '$1', want '$2')"; fi; }
check_true() { desc=$1; shift; if "$@"; then PASS=$((PASS+1)); echo "  PASS: $desc"; else FAIL=$((FAIL+1)); echo "  FAIL: $desc"; fi; }

WORK=$(mktemp -d); DN=$((330 + $$ % 40)); D=":$DN"
export DISPLAY="$D" HOME="$WORK/home" XDG_RUNTIME_DIR="$WORK/run" XDG_CONFIG_HOME="$WORK/home/.config" XDG_DATA_HOME="$WORK/data"
export FAKEBUS_LOG="$WORK/bus.log" FAKEBUS_SCRIPT="$WORK/bus.script"
mkdir -p "$HOME" "$XDG_RUNTIME_DIR" "$XDG_DATA_HOME"; chmod 700 "$XDG_RUNTIME_DIR"
rm -f "/tmp/.X${DN}-lock" "/tmp/.X11-unix/X${DN}"
Xvfb "$D" -screen 0 1100x700x24 >/dev/null 2>&1 & XVFB=$!
trap 'kill $APPPID $XVFB 2>/dev/null; rm -rf "$WORK"' EXIT
i=0; while [ ! -e "/tmp/.X11-unix/X$DN" ] && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i+1)); done
# The socket file appears slightly before the server accepts connections: wait until a client really can.
i=0; while ! DISPLAY="$D" "$PROBE" focus >/dev/null 2>&1 && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i+1)); done

now_ms() { date +%s%3N; }
logcount() { grep -c "$1" "$FAKEBUS_LOG" 2>/dev/null || true; }
win_id() { "$PROBE" tree | grep ' state=mapped or=0 ' | grep 'Kohiko Bluetooth' | sed -n 's/^id=\(0x[0-9a-f]*\).*/\1/p' | head -1; }
app_alive() { kill -0 "$APPPID" 2>/dev/null; }

# Window is at 0,0 (no window manager). Controls, from a screenshot of the real window:
CONNECT_X=629; CONNECT_Y=278        # first action button of the selected, paired, not-connected device
DISCOVERABLE_X=305; DISCOVERABLE_Y=44   # the "Discoverable" toggle

start_app()
{
    : > "$FAKEBUS_LOG"
    "$APP" > "$WORK/app.out" 2>&1 & APPPID=$!
    i=0; while [ -z "$(win_id)" ] && [ "$i" -lt 60 ]; do sleep 0.1; i=$((i+1)); done
    WIN=$(win_id)
    sleep 0.5
}
stop_app() { kill "$APPPID" 2>/dev/null; wait "$APPPID" 2>/dev/null; }

echo "== 1. Connect takes 3 s: the UI stays usable, the call runs off the UI thread =="
printf 'Connect delay=3000\n' > "$FAKEBUS_SCRIPT"
start_app
check_true "the window is up" test -n "$WIN"
[ -n "$WIN" ] || { echo "--- app output:"; head -c 600 "$WORK/app.out"; echo "--- tree:"; "$PROBE" tree | head -5; }
BEFORE_PIXEL=$("$PROBE" pixel 629 275)
T0=$(now_ms)
"$INPUT" click $CONNECT_X $CONNECT_Y 1
sleep 0.5
check "$(logcount 'worker begin Connect')" "1" "Connect started on the WORKER thread"
check "$(logcount 'ui begin Connect')" "0" "...and never on the UI thread"
sleep 0.4
check_true "while Connect is running the button is disabled/relabelled (its colour changed)" test "$("$PROBE" pixel 629 275)" != "$BEFORE_PIXEL"

# An independent control, used while Connect is still running (it has ~2 s left).
"$INPUT" click $DISCOVERABLE_X $DISCOVERABLE_Y 1
T1=$(now_ms)
i=0; while [ "$(logcount 'begin Set:Discoverable')" = 0 ] && [ "$i" -lt 40 ]; do sleep 0.05; i=$((i+1)); done
ELAPSED=$(( $(now_ms) - T1 ))
check "$(logcount 'begin Set:Discoverable')" "1" "the Discoverable toggle was handled while Connect was still running"
check_true "...within 600 ms of the click ($ELAPSED ms) - the UI was not frozen behind Connect" test "$ELAPSED" -lt 600
check "$(logcount 'worker begin Set:Discoverable')" "1" "...also off the UI thread"
check "$(logcount 'end Connect')" "0" "(Connect has still not finished at that moment)"

# A second click on the busy Connect button must not start a second connect.
"$INPUT" click $CONNECT_X $CONNECT_Y 1
sleep 0.3
check "$(logcount 'begin Connect')" "1" "clicking the busy Connect button again starts nothing"

# The window can still be moved.
"$INPUT" movewin "$WIN" 40 30
sleep 0.2
check_true "the window can be moved while the operation runs" "$PROBE" tree | grep -q "id=$WIN .*x=40 y=30"

sleep 2.6
check "$(logcount 'end Connect')" "1" "Connect completed - exactly once"
check "$(logcount 'fail')" "0" "no failure was reported for a successful connect"
check "$(logcount 'begin Connect')" "1" "...and was started exactly once"
stop_app

echo "== 2. A failure is reported, not swallowed =="
printf 'Connect delay=300 error=Page Timeout\n' > "$FAKEBUS_SCRIPT"
start_app
"$INPUT" click $CONNECT_X $CONNECT_Y 1
sleep 1.2
check "$(logcount 'fail Connect')" "1" "BlueZ's error reached the app"
check_true "the device's status line shows the failure in the error colour (pixel e0554f)" test "$("$PROBE" pixel 540 127)" = "e0554f"
check_true "a desktop notification with the error was sent" test "$(logcount 'begin Notify')" -ge 1
printf '' > "$FAKEBUS_SCRIPT"            # BlueZ recovers
"$INPUT" click $CONNECT_X $CONNECT_Y 1
sleep 0.8
check "$(logcount 'end Connect')" "1" "a retry after the failure works"
stop_app

echo "== 3. The window closes at once even though Connect is still running =="
printf 'Connect delay=4000\n' > "$FAKEBUS_SCRIPT"
start_app
"$INPUT" click $CONNECT_X $CONNECT_Y 1
sleep 0.5
T0=$(now_ms)
"$INPUT" close "$WIN"
i=0; while app_alive && [ "$i" -lt 100 ]; do sleep 0.05; i=$((i+1)); done
CLOSE_MS=$(( $(now_ms) - T0 ))
check_true "the app exited ($CLOSE_MS ms after the close request, 3.5 s before Connect would have finished)" test "$CLOSE_MS" -lt 1000
app_alive && check "still running" "exited" "the window closed" || true

echo "== 4. No timer polling while idle =="
printf '' > "$FAKEBUS_SCRIPT"
start_app
sleep 0.3
PID=$APPPID
T_A=$(awk '{print $14+$15}' /proc/$PID/stat); sleep 3; T_B=$(awk '{print $14+$15}' /proc/$PID/stat)
check_true "an idle app uses ~no CPU ($(( T_B - T_A )) clock ticks in 3 s)" test "$(( T_B - T_A ))" -le 3
stop_app

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
