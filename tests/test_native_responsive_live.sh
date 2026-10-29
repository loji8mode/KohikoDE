#!/bin/sh
# Live regression test: a native Kohiko utility must stay responsive while
# a slow operation is in flight. Real BluetoothWindow + real BluezClient,
# real D-Bus (a private fake system bus), the mock bluetoothd in
# tests/mock_bluez.py holding each call for seconds, and REAL X input events
# delivered to the real window by tests/test_native_responsive_harness.cpp.
#
# Root cause being tested (0.21.5 and earlier): BluezClient's actions used
# DBusClient::Call() = dbus_connection_send_with_reply_and_block(), with
# timeouts of 30 s (Pair) and 15 s (Connect), called straight from button
# handlers on the window's single-threaded event loop - and the Pair button
# issued Pair AND Connect back to back (up to 45 s). For all of that time the
# window could not redraw, move, be closed, or use any other control.
#
# What is asserted, per the brief:
#   1. Start an operation (click Pair; the service holds it for ~4 s).
#   2. While it is still running, use ANOTHER control (Scan) -> honoured at
#      once, not after the Pair finishes.
#   3. Ask the window to CLOSE -> honoured at once.
#   4. The Pair button becomes Cancel, and clicking it sends a REAL
#      CancelPairing to the service (not just a UI change).
#   5. Completion paths: success (Pair then Connect, in order), failure (the
#      reason is recorded and reported, never swallowed), cancel.
#   6. No duplicate operation reaches the service, nothing is left busy.
#
# Thresholds are deliberately generous (responsive = well under the ~4 s the
# service holds the call) so a loaded machine cannot make it flaky.
#
# Gracefully skipped (exit 0) without Xvfb / dbus-daemon / python3-dbus.

set -u
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT="$SCRIPT_DIR/.."
HARNESS="${1:-$REPO_ROOT/build/test_native_responsive_harness}"

pass=0; fail=0
check() { if [ "$1" = "$2" ]; then pass=$((pass+1)); echo "  PASS: $3"; else fail=$((fail+1)); echo "  FAIL: $3 (expected [$2], got [$1])"; fi; }
below() { # <value> <limit> <what>
    if awk -v v="$1" -v l="$2" 'BEGIN { exit !(v >= 0 && v < l) }'; then pass=$((pass+1)); echo "  PASS: $3 (${1} ms < ${2} ms)"; else fail=$((fail+1)); echo "  FAIL: $3 (got ${1} ms, wanted < ${2} ms)"; fi
}

command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: Xvfb not available."; exit 0; }
command -v dbus-daemon >/dev/null 2>&1 || { echo "SKIP: dbus-daemon not available."; exit 0; }
python3 -c "import dbus, dbus.mainloop.glib, gi.repository.GLib" >/dev/null 2>&1 || { echo "SKIP: python3-dbus/PyGObject not available."; exit 0; }
[ -x "$HARNESS" ] || { echo "SKIP: $HARNESS not built."; exit 0; }

WORK=$(mktemp -d)
XVFB_PID=""; MOCK_PID=""; DBUS_PID=""
stop_mock() { [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null; wait "$MOCK_PID" 2>/dev/null; MOCK_PID=""; }
cleanup() { stop_mock; [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null; [ -n "$DBUS_PID" ] && kill "$DBUS_PID" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT INT TERM

cat > "$WORK/bus.conf" <<'CONF'
<busconfig>
  <type>system</type>
  <listen>unix:tmpdir=/tmp</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
    <allow user="*"/>
  </policy>
</busconfig>
CONF
dbus-daemon --config-file="$WORK/bus.conf" --print-address=1 --fork > "$WORK/addr.txt" 2>"$WORK/dbus.log"
n=0; while [ ! -s "$WORK/addr.txt" ] && [ "$n" -lt 50 ]; do sleep 0.1; n=$((n+1)); done
DBUS_ADDR=$(cat "$WORK/addr.txt")
[ -n "$DBUS_ADDR" ] || { echo "SKIP: could not start a private dbus-daemon."; exit 0; }
DBUS_PID=$(pgrep -f "dbus-daemon --config-file=$WORK/bus.conf" | head -1)

DISPNUM=${KOHIKO_TEST_DISPLAY_BASE:-190}
rm -f "/tmp/.X${DISPNUM}-lock"
Xvfb ":$DISPNUM" -screen 0 1280x800x24 >/dev/null 2>&1 & XVFB_PID=$!
n=0; while [ ! -e "/tmp/.X11-unix/X$DISPNUM" ] && [ "$n" -lt 50 ]; do sleep 0.1; n=$((n+1)); done

mkdir -p "$WORK/run" "$WORK/home"; chmod 700 "$WORK/run"
export DISPLAY=":$DISPNUM" DBUS_SYSTEM_BUS_ADDRESS="$DBUS_ADDR" XDG_RUNTIME_DIR="$WORK/run" HOME="$WORK/home" XDG_DATA_HOME="$WORK/home/data"

# start_mock <scenario>  -> fresh service + empty call log
start_mock() {
    stop_mock
    LOG="$WORK/calls.log"; : > "$LOG"
    python3 "$SCRIPT_DIR/mock_bluez.py" "$SCRIPT_DIR/$1" "$LOG" > "$WORK/mock.log" 2>&1 & MOCK_PID=$!
    n=0; while ! grep -q "^mock_bluez: ready" "$WORK/mock.log" 2>/dev/null && [ "$n" -lt 50 ]; do sleep 0.1; n=$((n+1)); done
    export MOCK_CALL_LOG="$LOG"
}
run() { timeout 60 "$HARNESS" "$1" 2>/dev/null; }
val() { echo "$OUT" | awk -v k="$1" '$1=="RESULT" && $2==k { $1=""; $2=""; sub(/^ +/, ""); print }'; }
calls() { grep -c "^CALL $1 " "$LOG"; }

# ---------------------------------------------------------------------------
echo "-- Window stays responsive while a 4 s Pair is in flight --"
start_mock bluetooth_slow_scenario.json
OUT=$(run pending)
check "$(val error)" "" "the harness drove the real window (no setup error)"
below "$(val pair_reached_service_ms)" 2000 "clicking Pair reached the service promptly"
check "$(val pair_still_pending_when_closing)" "1" "the Pair was STILL pending at the service when the window was asked to close"
below "$(val other_control_honoured_ms)" 1500 "another control (Scan) was honoured while the Pair was pending (0.21.5: only after it finished)"
below "$(val close_honoured_ms)" 1000 "the window honoured its close request while the Pair was pending (0.21.5: frozen for the rest of the Pair)"
check "$(calls Pair)" "1" "exactly one Pair reached the service (no duplicate operation)"
sleep 4.5; stop_mock

echo "-- Closing the window 300 ms into a 4 s Pair --"
start_mock bluetooth_slow_scenario.json
OUT=$(run close)
check "$(val pair_still_pending_when_closing)" "1" "the Pair was certainly still pending when the close was requested"
below "$(val close_honoured_ms)" 1000 "the window closed immediately (0.21.5: not until the Pair AND the Connect that follows it had both finished, ~5 s)"
stop_mock

echo "-- The Pair button becomes Cancel, and Cancel is a REAL cancel --"
start_mock bluetooth_slow_scenario.json
OUT=$(run cancel)
below "$(val cancel_reached_service_ms)" 1500 "clicking the button again sent CancelPairing to the service"
check "$(val pair_cancelled_at_service)" "1" "and the service actually aborted the pending Pair"
check "$(calls Pair)" "1" "still only one Pair was ever issued"
check "$(calls CancelPairing)" "1" "and exactly one CancelPairing"
stop_mock

echo "-- BluezClient: async completion, busy tracking, duplicates, cancel semantics --"
start_mock bluetooth_slow_scenario.json
OUT=$(run client)
below "$(val start_returned_ms)" 50 "starting a 4 s Pair returns immediately (it used to block for the whole call)"
check "$(val busy_while_pending)" "1" "the device is reported busy while the Pair is pending"
check "$(echo "$(val busy_label)" | grep -c 'Pairing')" "1" "with a user-facing in-progress label"
check "$(val duplicate_refused)" "1" "a second Pair on the same device is refused, not queued"
check "$(val cancel_accepted)" "1" "CancelPending accepts a pending Pair"
check "$(val not_busy_after_cancel)" "1" "and nothing is left busy afterwards"
check "$(val last_error_after_cancel)" "Pairing cancelled" "the outcome is reported to the user, not dropped"
check "$(val done_called_after_cancel)" "1" "the caller's completion handler ran"
check "$(val change_handler_fired)" "1" "and the window was told to redraw"
check "$(val cancel_idle_returns_false)" "1" "cancelling when nothing is in flight is a no-op that says so"
check "$(calls Pair)" "1" "the service saw one Pair"
check "$(calls CancelPairing)" "1" "and the cancel really reached it"
stop_mock

echo "-- Success: Pair, then Connect only after Pair has finished --"
start_mock bluetooth_fast_scenario.json
OUT=$(run client-ok)
check "$(val not_busy_at_end)" "1" "nothing left busy"
check "$(calls Pair)$(calls Connect)" "11" "one Pair and one Connect reached the service"
pair_done=$(grep -n '^DONE Pair' "$LOG" | head -1 | cut -d: -f1); connect_call=$(grep -n '^CALL Connect' "$LOG" | head -1 | cut -d: -f1)
check "$([ -n "$pair_done" ] && [ -n "$connect_call" ] && [ "$connect_call" -gt "$pair_done" ] && echo ordered)" "ordered" "Connect was issued only AFTER Pair completed (0.21.5 sent both back to back)"
stop_mock

echo "-- Failure: reported with its reason, never swallowed --"
start_mock bluetooth_failing_scenario.json
OUT=$(run client-fail)
check "$(val reported_failure)" "1" "the completion handler was told the Pair failed"
check "$(val last_error_recorded)" "1" "the failure is kept for the window to show ('Pair failed: ...')"
check "$(val last_error_has_reason)" "1" "and it carries the service's own reason"
check "$(val not_busy_at_end)" "1" "the device is not left stuck busy"
check "$(calls Connect)" "0" "Connect was NOT attempted after a failed Pair"
stop_mock

echo
echo "Passed: $pass   Failed: $fail"
[ "$fail" -eq 0 ]
