#!/bin/sh
# Live regression test for the 0.20.4 XEmbed dock-timeout fallback
# fix (see CHANGELOG.md) - runs the *real* kohiko binary as an actual
# window manager under Xvfb, against tests/live/x11_test_client (a
# small, configurable, real X11 client - not a mock of anything),
# covering the two cases Task 9 asks for directly:
#
#   - an ordinary window that happens to carry _XEMBED_INFO but is
#     never actually claimed via a real SYSTEM_TRAY_REQUEST_DOCK
#     (an application using XEmbed for something unrelated to a
#     system tray, or a genuine tray icon whose host is absent/slow/
#     buggy) must still end up normally managed - not permanently
#     invisible - once a bounded timeout passes;
#   - a *genuine* tray icon (the same _XEMBED_INFO property, but this
#     time followed by a real SYSTEM_TRAY_REQUEST_DOCK ClientMessage,
#     exactly like a real system-tray-icon application sends) must
#     still be docked correctly, with no window ever briefly appearing
#     tiled at the root level first (the cosmetic "flash" the
#     original, unconditional check existed to prevent).
#
# Gracefully skipped (exit 0) if Xvfb/xwininfo/the kohiko binary/the
# test client aren't available, rather than failing the whole suite
# over missing optional live-session infrastructure.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT="$SCRIPT_DIR/.."
KOHIKO_BIN="${1:-$REPO_ROOT/kohiko}"
TEST_CLIENT="${2:-$REPO_ROOT/build/x11_test_client}"

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

for tool in Xvfb xwininfo; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "SKIP: $tool not available."
        exit 0
    fi
done

if [ ! -x "$KOHIKO_BIN" ]; then
    echo "SKIP: $KOHIKO_BIN not built."
    exit 0
fi

if [ ! -x "$TEST_CLIENT" ]; then
    echo "SKIP: $TEST_CLIENT not built."
    exit 0
fi

WORKDIR=$(mktemp -d)
XVFB_PID=""
WM_PID=""
DISPLAY_NUM=$((90 + ($$ % 100)))  # a bit of spread so parallel runs don't collide on :99
export DISPLAY=":$DISPLAY_NUM"

cleanup()
{
    [ -n "${A_PID:-}" ] && kill "$A_PID" 2>/dev/null
    [ -n "${B_PID:-}" ] && kill "$B_PID" 2>/dev/null
    [ -n "${C_PID:-}" ] && kill "$C_PID" 2>/dev/null
    [ -n "$WM_PID" ] && kill "$WM_PID" 2>/dev/null
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT INT TERM

rm -f "/tmp/.X${DISPLAY_NUM}-lock"
Xvfb "$DISPLAY" -screen 0 1920x1080x24 > "$WORKDIR/xvfb.log" 2>&1 &
XVFB_PID=$!

# Poll for the socket rather than a fixed sleep - see
# test_networkmanager_live.sh's own identical fix/comment for why:
# several heavy Xvfb-based tests now run back-to-back in the same
# `make test`, and a fixed sleep long enough standalone isn't
# necessarily long enough under that contention.
waited=0
while [ ! -e "/tmp/.X11-unix/X${DISPLAY_NUM}" ] && [ "$waited" -lt 50 ]; do
    sleep 0.1
    waited=$((waited + 1))
done

export XDG_CONFIG_HOME="$WORKDIR/config"
export XDG_RUNTIME_DIR="$WORKDIR/runtime"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

"$KOHIKO_BIN" > "$WORKDIR/kohiko.log" 2>&1 &
WM_PID=$!

# Poll for kohiko's own "ready" log line rather than a fixed sleep.
waited=0
while ! grep -q '\[INFO\] ready' "$WORKDIR/kohiko.log" 2>/dev/null && [ "$waited" -lt 50 ]; do
    sleep 0.1
    waited=$((waited + 1))
    if ! kill -0 "$WM_PID" 2>/dev/null; then
        break
    fi
done

if ! kill -0 "$WM_PID" 2>/dev/null; then
    echo "FAIL: kohiko did not start"
    cat "$WORKDIR/kohiko.log"
    exit 1
fi

# --- Client A: plain window, no _XEMBED_INFO - the control ------------------

"$TEST_CLIENT" --title "Control" --class "TestApp" --width 400 --height 300 \
    > "$WORKDIR/A.log" 2>&1 &
A_PID=$!
sleep 0.5
A_WIN=$(grep -oE '0x[0-9a-f]+' "$WORKDIR/A.log" | head -1)

# --- Client B: _XEMBED_INFO set, but no dock request ever sent --------------
# (an ordinary app using XEmbed for something else, or an orphaned/
# never-claimed tray icon)

"$TEST_CLIENT" --title "OrphanedXEmbed" --class "TestApp" --width 400 --height 300 --xembed \
    > "$WORKDIR/B.log" 2>&1 &
B_PID=$!
sleep 0.5
B_WIN=$(grep -oE '0x[0-9a-f]+' "$WORKDIR/B.log" | head -1)

# --- Client C: _XEMBED_INFO set AND a real SYSTEM_TRAY_REQUEST_DOCK ---------
# sent - a genuine tray icon.

"$TEST_CLIENT" --title "GenuineDock" --class "TestApp" --width 24 --height 24 \
    --xembed --send-dock-request > "$WORKDIR/C.log" 2>&1 &
C_PID=$!
sleep 0.5
C_WIN=$(grep -oE '0x[0-9a-f]+' "$WORKDIR/C.log" | head -1)

echo "-- Immediately after launch (before the dock timeout can have fired) --"

check "$(xwininfo -id "$A_WIN" | grep -c 'Width: 400')" "0" \
    "control window (no _XEMBED_INFO) was resized/tiled promptly, not left at its 400px creation width"

check "$(xwininfo -id "$B_WIN" | grep -c 'Width: 400')" "1" \
    "orphaned-XEmbed window is still untouched at its original creation size - the fast path correctly "\
"holds off immediately, exactly like a genuine tray icon would need"

check "$(xwininfo -root -tree | grep -c "$C_WIN\b.*400x300\|$C_WIN\b.*Width: 24")" "0" \
    "genuine-dock window never appears as a plain root-level child at its raw creation size - no visible "\
"flash before DockIcon() reparents it"

echo
echo "-- After the 2-second dock timeout has had time to elapse --"
sleep 3.5

check "$(xwininfo -id "$B_WIN" | grep -c 'Width: 400')" "0" \
    "orphaned-XEmbed window (never actually claimed by a dock request) was NOT left permanently invisible - "\
"it was resized/tiled by the timeout fallback, exactly the bug this fix closes"

check "$(xwininfo -id "$C_WIN" 2>&1 | grep -c 'IsViewable')" "1" \
    "genuine tray icon is still correctly docked and viewable after the same timeout window - the fallback "\
"doesn't second-guess a dock that already succeeded"

check "$(xwininfo -id "$C_WIN" 2>&1 | grep -c 'Width: 24')" "0" \
    "...and it's still sized to fit the tray (resized by ReflowIcons()), not back at its raw 24px request "\
"as it would be if it had been forced through ordinary tiling instead"

# --- Client D: a Qt-like application window ----------------------------------
# Qt sets _XEMBED_INFO on EVERY top-level window (main windows, dialogs,
# tool windows, splash screens - not just tray icons) and ALSO declares
# an EWMH _NET_WM_WINDOW_TYPE on all of them. Measured against real
# FeatherPad and a real Qt app, the hold above used to keep every such
# window invisible for the full 2 s deadline (2.5 s on screen, because
# the sweep only runs once per Tick()). A window that declares a type is
# an ordinary managed window, not a possible tray icon, and must be
# tiled immediately - while B above (no declared type) is still held
# for the deadline and C still docks.
echo
echo "-- A Qt-like window (_XEMBED_INFO + declared window type) is NOT held --"

now_ms() { date +%s%3N; }

D_START=$(now_ms)
"$TEST_CLIENT" --title "QtLike" --class "TestApp" --width 400 --height 300 --xembed \
    --window-type _NET_WM_WINDOW_TYPE_NORMAL > "$WORKDIR/D.log" 2>&1 &
D_PID=$!

D_WIN=""
while [ -z "$D_WIN" ]; do
    D_WIN=$(grep -oE '0x[0-9a-f]+' "$WORKDIR/D.log" 2>/dev/null | head -1)
    [ -n "$D_WIN" ] || sleep 0.02
    [ $(( $(now_ms) - D_START )) -lt 3000 ] || break
done

# Poll until it has been resized by the tiling layout (no longer its raw
# 400px creation width), up to 3 s - the old hold would take >= 2 s.
D_TILED=0
while [ $(( $(now_ms) - D_START )) -lt 3000 ]; do
    if [ -n "$D_WIN" ] && [ "$(xwininfo -id "$D_WIN" 2>/dev/null | grep -c 'Width: 400')" = "0" ] && \
       xwininfo -id "$D_WIN" 2>/dev/null | grep -q 'IsViewable'; then
        D_TILED=1
        break
    fi
    sleep 0.02
done
D_MS=$(( $(now_ms) - D_START ))

echo "  (Qt-like window tiled and viewable after ${D_MS} ms)"
check "$D_TILED" "1" \
    "a window with _XEMBED_INFO that declares a window type is managed and visible, not held as a possible tray icon"
check "$([ "$D_MS" -lt 1000 ] && echo fast || echo slow)" "fast" \
    "...and promptly: within 1 s of launch, versus the >= 2 s the XEmbed hold imposed on every Qt window before"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
