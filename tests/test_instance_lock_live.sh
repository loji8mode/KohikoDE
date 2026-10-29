#!/bin/sh
# Single-instance guarantees of AppInstanceLock (the thing every native
# control window and its tray icon rely on to avoid a second copy):
#
#   1. Many copies started at the same instant: exactly ONE becomes the
#      instance. (Before: "connect; if nobody answers unlink+bind" - copies
#      that all saw an empty socket each unlinked and bound their own, so
#      several could run.)
#   2. A copy that loses tells the running instance, which raises.
#   3. A stale lock/socket left by a crashed instance never blocks the
#      next one (flock is released by the kernel; the socket is replaced).
#   4. LaunchOrRaise (what a tray left-click does): raises a running
#      instance, and does NOT start a second process while the first is
#      still starting up.
#
# No X server needed. Usage: test_instance_lock_live.sh [lock_probe_app]

set -u
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT="$SCRIPT_DIR/.."
PROBE_APP="${1:-$REPO_ROOT/build/lock_probe_app}"
LAUNCHER="${2:-$REPO_ROOT/build/launch_or_raise_probe}"
ROUNDS="${LOCK_RACE_ROUNDS:-40}"

[ -x "$PROBE_APP" ] || { echo "SKIP: $PROBE_APP not built."; exit 0; }

PASS=0; FAIL=0
check() { if [ "$1" = "$2" ]; then PASS=$((PASS+1)); echo "  PASS: $3"; else FAIL=$((FAIL+1)); echo "  FAIL: $3 (got '$1', want '$2')"; fi; }

WORK=$(mktemp -d); export XDG_RUNTIME_DIR="$WORK"; chmod 700 "$WORK"
trap 'rm -rf "$WORK"; kill $(jobs -p) 2>/dev/null' EXIT
count() { grep -c "^$1 " "$2" 2>/dev/null || true; }

echo "== 1. Simultaneous starts: exactly one instance =="
WORST=0; ROUNDS_WITH_DUP=0
r=0
while [ "$r" -lt "$ROUNDS" ]; do
    LOG="$WORK/race$r.log"; : > "$LOG"
    for i in 1 2 3 4 5 6 7 8; do "$PROBE_APP" --id race --log "$LOG" --stay-ms 150 & done
    wait
    locked=$(count LOCKED "$LOG")
    [ "$locked" -gt 1 ] && ROUNDS_WITH_DUP=$((ROUNDS_WITH_DUP+1))
    [ "$locked" -gt "$WORST" ] && WORST=$locked
    r=$((r+1))
done
check "$ROUNDS_WITH_DUP" "0" "in $ROUNDS rounds of 8 simultaneous starts, never more than one instance (worst round: $WORST)"

echo "== 2. A losing copy asks the running instance to raise =="
LOG="$WORK/raise.log"; : > "$LOG"
"$PROBE_APP" --id raise --log "$LOG" --stay-ms 1500 & FIRST=$!
sleep 0.4
"$PROBE_APP" --id raise --log "$LOG" --stay-ms 100
sleep 0.2
check "$(count LOCKED "$LOG")" "1" "the first copy holds the lock"
check "$(count LOST "$LOG")" "1" "the second copy lost and exited"
check "$(count RAISED "$LOG")" "1" "...and asked the first to raise its window"
wait "$FIRST"

echo "== 3. A crashed instance never blocks the next one =="
LOG="$WORK/crash.log"; : > "$LOG"
"$PROBE_APP" --id crash --log "$LOG" --stay-ms 30000 & VICTIM=$!
sleep 0.4
kill -9 "$VICTIM"; wait "$VICTIM" 2>/dev/null
"$PROBE_APP" --id crash --log "$LOG" --stay-ms 200
check "$(count LOCKED "$LOG")" "2" "after a SIGKILL the next start becomes the instance (stale lock + socket are replaced)"

if [ -x "$LAUNCHER" ]; then
echo "== 4. LaunchOrRaise (tray left-click): raise if running, never start twice while starting =="
LOG="$WORK/tray.log"; : > "$LOG"
# Two rapid 'clicks' from ONE process (a tray widget is one long-lived process).
"$LAUNCHER" --id traycase --exe "$PROBE_APP" --log "$LOG" --startup-ms 600 --stay-ms 2500 --clicks "0 120 250"
sleep 1.0
check "$(count STARTED "$LOG")" "1" "three clicks within 250 ms of starting: the application was started exactly once"
sleep 0.2
"$LAUNCHER" --id traycase --exe "$PROBE_APP" --log "$LOG" --clicks "0"
sleep 0.3
check "$(count STARTED "$LOG")" "1" "a click after it is up does not start another process"
check "$(count LOCKED "$LOG")" "1" "exactly one instance holds the lock"
check "$(count RAISED "$LOG")" "1" "...and the click raised it"
fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
