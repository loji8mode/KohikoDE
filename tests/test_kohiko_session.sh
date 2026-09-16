#!/bin/sh
# Regression test for scripts/kohiko-session - runs the real wrapper
# script against small fake "kohiko" stand-ins to exercise every
# branch: a clean exit, a fast crash loop hitting the burst limit, a
# crash after a stable run resetting the failure count, SIGTERM being
# forwarded to the child, and the SIGKILL fallback for a child that
# ignores it. No X11/network needed - this is pure process control.
#
# Build & run: see the "test-kohiko-session" target in the Makefile.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WRAPPER="$SCRIPT_DIR/../scripts/kohiko-session"

WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

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

# --- fake kohiko binaries ---------------------------------------------------

cat > "$WORKDIR/clean" <<'EOF'
#!/bin/sh
exit 0
EOF

cat > "$WORKDIR/crash-fast" <<'EOF'
#!/bin/sh
exit 1
EOF

cat > "$WORKDIR/crash-after-stable" <<'EOF'
#!/bin/sh
sleep 1.5
exit 1
EOF

cat > "$WORKDIR/ignores-sigterm" <<'EOF'
#!/bin/sh
trap '' TERM
while true; do sleep 0.1; done
EOF

chmod +x "$WORKDIR"/clean "$WORKDIR"/crash-fast "$WORKDIR"/crash-after-stable "$WORKDIR"/ignores-sigterm

# A copy of the real wrapper with STABLE_RUN_SECONDS and the SIGKILL
# grace period both cut down, so this test runs in seconds rather than
# minutes - every other constant (backoff, burst limit) is untouched,
# so those are still tested against the real, shipped values.
sed -e 's/STABLE_RUN_SECONDS=30/STABLE_RUN_SECONDS=1/' \
    -e 's/waited" -lt 10/waited" -lt 1/' \
    "$WRAPPER" > "$WORKDIR/wrapper-fast"
chmod +x "$WORKDIR/wrapper-fast"

echo "-- Clean exit: no restart, wrapper propagates exit 0 --"
KOHIKO_BIN="$WORKDIR/clean" "$WRAPPER" > "$WORKDIR/out1" 2>&1
check "$?" "0" "wrapper exits 0 after a clean kohiko exit"
check "$(grep -c '^\[INFO\] kohiko-session: starting' "$WORKDIR/out1")" "1" "kohiko was only launched once - no restart after a clean exit"

echo
echo "-- Fast crash loop: exponential backoff, gives up after the burst limit --"
timeout 60 env KOHIKO_BIN="$WORKDIR/crash-fast" "$WRAPPER" > "$WORKDIR/out2" 2>&1
status=$?
check "$status" "1" "wrapper exits non-zero after exhausting the restart budget"
check "$(grep -c '^\[INFO\] kohiko-session: starting' "$WORKDIR/out2")" "6" "exactly 6 attempts (1 initial + 5 allowed retries)"
check "$(grep -c 'giving up' "$WORKDIR/out2")" "1" "logs exactly one give-up message"

echo
echo "-- Crash after a stable run: failure count never accumulates --"
timeout 15 env KOHIKO_BIN="$WORKDIR/crash-after-stable" "$WORKDIR/wrapper-fast" > "$WORKDIR/out3" 2>&1 &
runner_pid=$!
sleep 8
kill -9 "$runner_pid" 2>/dev/null
wait "$runner_pid" 2>/dev/null
check "$(grep -Ec 'failure [2-9]/5' "$WORKDIR/out3")" "0" \
    "the failure count never actually reaches 2, however many cycles ran"
check "$(grep -c 'giving up' "$WORKDIR/out3")" "0" "never gives up, since no crash counts as consecutive"

started=$(grep -c '^\[INFO\] kohiko-session: starting' "$WORKDIR/out3")
if [ "$started" -ge 3 ]; then
    pass=$((pass + 1))
    echo "  PASS: at least 3 restart cycles actually ran in the test window ($started did) - not a vacuous pass"
else
    fail=$((fail + 1))
    echo "  FAIL: only $started restart cycle(s) ran - too few for the above checks to mean anything"
fi

echo
echo "-- SIGTERM is forwarded to the child, wrapper exits cleanly, no orphans --"
rm -f "$WORKDIR/out4"
env KOHIKO_BIN="$WORKDIR/ignores-sigterm" "$WORKDIR/wrapper-fast" > "$WORKDIR/out4" 2>&1 &
wrapper_pid=$!
sleep 1
kill -TERM "$wrapper_pid"
waited=0
while kill -0 "$wrapper_pid" 2>/dev/null && [ "$waited" -lt 10 ]; do
    sleep 1
    waited=$((waited + 1))
done
check "$(kill -0 "$wrapper_pid" 2>/dev/null && echo alive || echo gone)" "gone" \
    "wrapper itself has exited within ${waited}s of receiving SIGTERM"
check "$(grep -c 'sending SIGKILL' "$WORKDIR/out4")" "1" \
    "fell back to SIGKILL against a child that ignores SIGTERM"
sleep 1
check "$(pgrep -f "$WORKDIR/ignores-sigterm" >/dev/null && echo found || echo none)" "none" \
    "no orphaned child process remains"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
