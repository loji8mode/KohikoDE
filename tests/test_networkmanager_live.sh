#!/bin/sh
# Live round-trip regression test for the 0.20.4 credential-reuse fix
# (see CHANGELOG.md): a real NetworkManagerClient talking, over a
# real D-Bus connection, to tests/mock_networkmanager.py standing in
# for NetworkManager - not a real system bus, a private throwaway
# dbus-daemon instance pointed to by DBUS_SYSTEM_BUS_ADDRESS, so this
# never touches anything on the actual machine.
#
# Covers exactly the four cases CHANGELOG.md's 0.20.4 entry calls out
# for this fix: an existing saved secret skips the prompt/reuses
# without a duplicate profile; a genuinely missing secret still needs
# one; a newly entered password is persisted via Update() rather than
# creating a second profile; a brand new network correctly still uses
# AddAndActivateConnection() (the one case that legitimately should).
#
# Gracefully skips (exit 0) if python3-dbus or a usable dbus-daemon
# aren't available, rather than failing the whole suite over missing
# optional test infrastructure - see the Makefile's own comment on
# this same test for why it isn't gated there instead.
#
# Build & run: see the "test-networkmanager-live" target in the
# Makefile.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
HARNESS="${1:-$SCRIPT_DIR/../build/test_networkmanager_live_harness}"

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

if ! command -v python3 >/dev/null 2>&1 || ! python3 -c "import dbus, dbus.mainloop.glib, gi.repository.GLib" >/dev/null 2>&1; then
    echo "SKIP: python3-dbus/PyGObject not available - can't run the mock NetworkManager service."
    exit 0
fi

if ! command -v dbus-daemon >/dev/null 2>&1; then
    echo "SKIP: dbus-daemon not available."
    exit 0
fi

if [ ! -x "$HARNESS" ]; then
    echo "SKIP: $HARNESS not built."
    exit 0
fi

WORKDIR=$(mktemp -d)
DBUS_PID=""
MOCK_PID=""

cleanup()
{
    [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null
    [ -n "$DBUS_PID" ] && kill "$DBUS_PID" 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT INT TERM

cat > "$WORKDIR/fake-system-bus.conf" <<'EOF'
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
EOF

dbus-daemon --config-file="$WORKDIR/fake-system-bus.conf" --print-address=1 --fork > "$WORKDIR/dbus-addr.txt" 2>"$WORKDIR/dbus.log"

# Poll for the address file rather than a fixed sleep - --fork returns
# as soon as the daemon forks, not once it's actually finished writing
# its address out, and a fixed sleep long enough on a quiet machine
# isn't necessarily long enough when several other Xvfb-based tests
# (test_kohiko_session.sh, test_xembed_dock_timeout.sh) are competing
# for CPU in the same `make test` run - exactly the flake this
# replaced after it showed up under that exact condition.
waited=0
while [ ! -s "$WORKDIR/dbus-addr.txt" ] && [ "$waited" -lt 50 ]; do
    sleep 0.1
    waited=$((waited + 1))
done

DBUS_ADDR=$(cat "$WORKDIR/dbus-addr.txt")
if [ -z "$DBUS_ADDR" ]; then
    echo "FAIL: could not start the private fake system bus"
    cat "$WORKDIR/dbus.log"
    exit 1
fi
export DBUS_SYSTEM_BUS_ADDRESS="$DBUS_ADDR"
# find the daemon's own pid to kill it later - --fork doesn't print one
DBUS_PID=$(pgrep -f "dbus-daemon --config-file=$WORKDIR/fake-system-bus.conf" | head -1)

LOG_FILE="$WORKDIR/calls.log"
python3 "$SCRIPT_DIR/mock_networkmanager.py" "$SCRIPT_DIR/network_live_scenario.json" "$LOG_FILE" \
    > "$WORKDIR/mock.log" 2>&1 &
MOCK_PID=$!

# Same reasoning as above: poll for the mock's own "ready" line rather
# than assume a fixed sleep was long enough.
waited=0
while ! grep -q "^mock_networkmanager: ready" "$WORKDIR/mock.log" 2>/dev/null && [ "$waited" -lt 50 ]; do
    sleep 0.1
    waited=$((waited + 1))
    if ! kill -0 "$MOCK_PID" 2>/dev/null; then
        break
    fi
done

if ! kill -0 "$MOCK_PID" 2>/dev/null; then
    echo "FAIL: mock_networkmanager.py did not start"
    cat "$WORKDIR/mock.log"
    exit 1
fi

echo "-- running the C++ harness against the mock --"
DBUS_SYSTEM_BUS_ADDRESS="$DBUS_ADDR" "$HARNESS"
HARNESS_EXIT=$?
check "$HARNESS_EXIT" "0" "the C++ harness's own in-process checks (HasUsableSavedSecret) all passed"

echo
echo "-- inspecting the mock's call log for what ConnectToAccessPoint() actually did --"

CONN0="/org/freedesktop/NetworkManager/Settings/0"   # HomeNet

check "$(grep -c "^CALL ActivateConnection connection=$CONN0$" "$LOG_FILE")" "2" \
    "HomeNet was activated directly (not recreated) on both the reuse call and the new-password call"

check "$(grep -c "^CALL AddAndActivateConnection ssid=HomeNet " "$LOG_FILE")" "0" \
    "HomeNet never went through AddAndActivateConnection - no duplicate profile was created for it, on either call"

check "$(grep -c "^CALL Update connection=$CONN0 psk=None$" "$LOG_FILE")" "0" \
    "the empty-password reuse call never invoked Update() at all - an empty password from a "\
"skipped prompt doesn't overwrite the real stored secret with nothing"

check "$(grep -c "^CALL Update connection=$CONN0 psk=newpass123!$" "$LOG_FILE")" "1" \
    "the new-password call updated HomeNet's saved connection with exactly the typed password"

check "$(grep -c "^CALL AddAndActivateConnection ssid=BrandNewNetwork psk=somepassword$" "$LOG_FILE")" "1" \
    "a genuinely new network (no saved connection at all) correctly still goes through "\
"AddAndActivateConnection - this fix didn't break creating a new profile when one is actually needed"

# Order matters too: Update() must happen *before* the second
# ActivateConnection() call it's paired with, not after (which would
# mean activating with the stale secret and updating too late to
# matter).
UPDATE_LINE=$(grep -n "^CALL Update connection=$CONN0 psk=newpass123!$" "$LOG_FILE" | head -1 | cut -d: -f1)
SECOND_ACTIVATE_LINE=$(grep -n "^CALL ActivateConnection connection=$CONN0$" "$LOG_FILE" | sed -n '2p' | cut -d: -f1)
if [ -n "$UPDATE_LINE" ] && [ -n "$SECOND_ACTIVATE_LINE" ] && [ "$UPDATE_LINE" -lt "$SECOND_ACTIVATE_LINE" ]; then
    check "ordered" "ordered" "Update() happened before the activation it's paired with, not after"
else
    check "ordered" "MISORDERED" "Update() happened before the activation it's paired with, not after"
fi

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
