#!/bin/sh
# Live regression test for the Launcher's interaction with workspaces,
# global shortcuts and Enter - runs the *real* kohiko under Xvfb and
# drives it with REAL key events (xdotool / XTEST), observing real
# state through `kohikoctl launcher|monitors|clients|activewindow`.
#
#   WORKSPACE CONTEXT
#     - the Launcher opens on workspace 3 and follows the user to
#       workspace 5 when they switch while it is open, with its query
#       and selection intact, and stays usable;
#     - after switching with the Launcher open, Enter must not leave a
#       window on the OLD workspace "focused" (the stale context the
#       Launcher captured when it opened);
#     - a slowly-starting application's window appears on the
#       workspace the user is on when it appears (latest context),
#       including when the user switches several times while it
#       starts, and including after the Launcher has already closed;
#     - switching workspaces never launches anything twice.
#
#   GLOBAL SHORTCUTS vs TEXT
#     - Super+<N> switches workspace from inside the Launcher and does
#       NOT insert the digit; an unmodified digit is ordinary input;
#       the toggle shortcut closes the Launcher.
#
#   ENTER / SELECTION RACE
#     - what Enter submitted is what launches, exactly once, however
#       fast further input follows.
#
# Gracefully skipped (exit 0) if Xvfb/xdotool/the binaries are missing.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT="$SCRIPT_DIR/.."
KOHIKO_BIN="${1:-$REPO_ROOT/kohiko}"
KOHIKOCTL="${2:-$REPO_ROOT/kohikoctl}"
TEST_CLIENT="${3:-$REPO_ROOT/build/x11_test_client}"

pass=0
fail=0
check()
{
    if [ "$1" = "$2" ]; then
        pass=$((pass + 1)); echo "  PASS: $3"
    else
        fail=$((fail + 1)); echo "  FAIL: $3 (expected [$2], got [$1])"
    fi
}

for tool in Xvfb xdotool; do
    command -v "$tool" >/dev/null 2>&1 || { echo "SKIP: $tool not available."; exit 0; }
done
for bin in "$KOHIKO_BIN" "$KOHIKOCTL" "$TEST_CLIENT"; do
    [ -x "$bin" ] || { echo "SKIP: $bin not built."; exit 0; }
done

WORK=$(mktemp -d)
DISPNUM=${KOHIKO_TEST_DISPLAY_BASE:-130}
XVFB_PID=""; WM_PID=""; SESSION_DIR=""

stop_session()
{
    [ -n "$WM_PID" ] && kill -- "-$WM_PID" 2>/dev/null
    [ -n "$WM_PID" ] && kill "$WM_PID" 2>/dev/null
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null
    wait 2>/dev/null
    WM_PID=""; XVFB_PID=""
}
trap 'stop_session; rm -rf "$WORK"' EXIT

# start_session <name>  - Xvfb + kohiko with the shortcuts under test + two
# desktop entries whose launches are logged (and optionally slow).
start_session()
{
    stop_session
    DISPNUM=$((DISPNUM + 1))
    SESSION_DIR="$WORK/$1"
    mkdir -p "$SESSION_DIR/config" "$SESSION_DIR/data/applications" "$SESSION_DIR/run" "$SESSION_DIR/home"
    chmod 700 "$SESSION_DIR/run"
    LOG="$SESSION_DIR/launches.log"; : > "$LOG"

    # A launchable app: logs one line per launch, waits SLOW seconds
    # (a slow starter), then becomes a real window of the given class.
    for name in Zzalpha Zzbeta Zzslow; do
        lower=$(echo "$name" | tr 'A-Z' 'a-z')
        cat > "$SESSION_DIR/$lower.sh" <<EOF
#!/bin/sh
echo "$name \$\$" >> "$LOG"
sleep \$(cat "$SESSION_DIR/slow-seconds" 2>/dev/null || echo 0)
exec "$TEST_CLIENT" --class "$name" --title "$name" --net-wm-pid
EOF
        chmod +x "$SESSION_DIR/$lower.sh"
        printf '[Desktop Entry]\nType=Application\nName=%s\nExec=%s\nTerminal=false\n' \
            "$name" "$SESSION_DIR/$lower.sh" > "$SESSION_DIR/data/applications/$lower.desktop"
    done
    echo 0 > "$SESSION_DIR/slow-seconds"

    # (A config file triggers ConfigMigration, which would otherwise
    # write the schema default workspace.count=4 - too few for this test.)
    cat > "$SESSION_DIR/kohiko.conf" <<EOF
workspace.count=9
bind=SUPER+D launcher_toggle
bind=SUPER+3 workspace 3
bind=SUPER+4 workspace 4
bind=SUPER+5 workspace 5
bind=SUPER+7 workspace 7
EOF

    rm -f "/tmp/.X${DISPNUM}-lock"
    Xvfb ":$DISPNUM" -screen 0 1920x1080x24 >"$SESSION_DIR/xvfb.log" 2>&1 &
    XVFB_PID=$!
    n=0; while [ ! -e "/tmp/.X11-unix/X$DISPNUM" ] && [ "$n" -lt 50 ]; do sleep 0.1; n=$((n + 1)); done

    export DISPLAY=":$DISPNUM" XDG_CONFIG_HOME="$SESSION_DIR/config" XDG_DATA_HOME="$SESSION_DIR/data" \
           XDG_DATA_DIRS="$SESSION_DIR/data" XDG_RUNTIME_DIR="$SESSION_DIR/run" HOME="$SESSION_DIR/home"

    setsid "$KOHIKO_BIN" "$SESSION_DIR/kohiko.conf" >"$SESSION_DIR/kohiko.log" 2>&1 &
    WM_PID=$!
    n=0; while ! grep -q 'ready' "$SESSION_DIR/kohiko.log" 2>/dev/null && [ "$n" -lt 80 ]; do sleep 0.1; n=$((n + 1)); done
    "$KOHIKOCTL" reloadlauncher >/dev/null 2>&1   # make sure the entries above are indexed
    sleep 0.6
}

ctl() { "$KOHIKOCTL" "$@" 2>/dev/null; }
cur_ws() { ctl monitors | grep -o '"workspace":[0-9]*' | head -1 | cut -d: -f2; }
lfield() { ctl launcher | sed -n "s/.*\"$1\":\\(\"[^\"]*\"\\|[a-z0-9-]*\\).*/\\1/p" | tr -d '"'; }
ws_of() { ctl clients | tr '{' '\n' | grep "\"class\":\"$1\"" | sed 's/.*"workspace":\([0-9]*\).*/\1/' | head -1; }
launches() { awk -v a="$1" '$1 == a { c++ } END { print c + 0 }' "$LOG"; }
active_ws() { ctl activewindow | sed -n 's/.*"workspace":\([0-9]*\).*/\1/p'; }
open_launcher() { ctl dispatch launcher_toggle >/dev/null; sleep 0.4; }
wait_class() { n=0; while [ -z "$(ws_of "$1")" ] && [ "$n" -lt "${2:-60}" ]; do sleep 0.1; n=$((n + 1)); done; sleep 0.2; }
anchor() # an ordinary window so a workspace has something on it
{
    "$TEST_CLIENT" --class "$1" --title "$1" >/dev/null 2>&1 &
    wait_class "$1" 30
}

# ---------------------------------------------------------------------------
echo "-- Launcher follows the user across workspaces, keeping its state --"
start_session follow
ctl dispatch workspace 3 >/dev/null; sleep 0.3
anchor OnThree
open_launcher
check "$(lfield open)" "true" "the Launcher opened (on workspace 3)"
check "$(cur_ws)" "3" "the user is on workspace 3"
xdotool type --delay 25 abc; sleep 0.3
check "$(lfield query)" "abc" "typed text reaches the Launcher"

xdotool key super+5; sleep 0.5
check "$(cur_ws)" "5" "Super+5 INSIDE the Launcher switched to workspace 5 (a global shortcut works while it is open)"
check "$(lfield query)" "abc" "and did NOT insert a '5' into the query (it used to)"
check "$(lfield open)" "true" "the Launcher is still open after the switch"

xdotool type --delay 25 de; sleep 0.3
check "$(lfield query)" "abcde" "the Launcher is still usable on workspace 5: typing still lands in it"
xdotool key BackSpace BackSpace; sleep 0.3
check "$(lfield query)" "abc" "BackSpace still edits it"

echo "-- Unmodified digits are ordinary input --"
xdotool type --delay 25 5373; sleep 0.3
check "$(lfield query)" "abc5373" "bare digits (even ones that are bound with Super) are plain text"
check "$(cur_ws)" "5" "and did not switch workspace"

echo "-- A bound shortcut can close the Launcher too --"
xdotool key super+d; sleep 0.4
check "$(lfield open)" "false" "Super+D (launcher_toggle) closed the open Launcher"
stop_session

# ---------------------------------------------------------------------------
echo "-- Enter after switching workspaces does not resurrect the OLD workspace's focus --"
start_session stale
ctl dispatch workspace 3 >/dev/null; sleep 0.3
anchor OnThree
open_launcher
xdotool type --delay 25 zzalpha; sleep 0.3
xdotool key super+5; sleep 0.5
check "$(cur_ws)" "5" "switched to the (empty) workspace 5 with the Launcher open"
xdotool key Return; sleep 0.8
check "$(active_ws)" "5" "after Enter the focused window is on workspace 5 (0.21.5: a hidden workspace-3 window was 'focused')"
check "$(ws_of Zzalpha)" "5" "the launched application's window appeared on workspace 5"
check "$(launches Zzalpha)" "1" "launched exactly once"
stop_session

# ---------------------------------------------------------------------------
echo "-- A slow application follows the user's LATEST workspace --"
start_session slow
echo 2 > "$SESSION_DIR/slow-seconds"
ctl dispatch workspace 3 >/dev/null; sleep 0.3
anchor OnThree
open_launcher
xdotool type --delay 25 zzslow; sleep 0.3
xdotool key Return; sleep 0.4
check "$(lfield open)" "false" "the Launcher closed immediately after launching"
ctl dispatch workspace 5 >/dev/null; sleep 0.5        # user moves on while the app is still starting
xdotool key super+7; sleep 0.5                          # ...and again
check "$(cur_ws)" "7" "user is now on workspace 7 (two switches while the app starts)"
wait_class Zzslow 60
check "$(ws_of Zzslow)" "7" "the window appeared on workspace 7 - the latest context, not 3 (where the Launcher opened) or 5"
check "$(launches Zzslow)" "1" "and the application was launched exactly once"
check "$(ws_of OnThree)" "3" "the existing window of another application stayed on workspace 3 (nothing was moved globally)"
stop_session

# ---------------------------------------------------------------------------
echo "-- Enter submits what was selected, however fast input follows --"
start_session race
echo 1 > "$SESSION_DIR/slow-seconds"
ctl dispatch workspace 3 >/dev/null; sleep 0.3
open_launcher
xdotool type --delay 25 zzalpha; sleep 0.3
# Enter, then a burst of keys that would change the query/selection/page if
# the Launcher were still taking input, then switch workspaces.
xdotool key Return Down Down Up BackSpace z z b e t a Return Tab Page_Down Page_Up
sleep 0.2
xdotool key super+4 super+5 super+3 super+5
sleep 2.5
check "$(launches Zzalpha)" "1" "the submitted application (Zzalpha) launched exactly once"
check "$(launches Zzbeta)" "0" "the application the later keystrokes would have selected (Zzbeta) never launched"
check "$(launches Zzslow)" "0" "nothing else launched"
check "$(ws_of Zzalpha)" "5" "and its window followed the user's last workspace switch"
stop_session

echo
echo "Passed: $pass   Failed: $fail"
[ "$fail" -eq 0 ]
