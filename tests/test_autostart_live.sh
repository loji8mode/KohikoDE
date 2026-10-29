#!/bin/sh
# Live regression test for the startup plan (StartupPlan.h) and the
# once-per-X-session guard - runs the *real* kohiko binary as an
# actual window manager under Xvfb. Every "program" launched is a tiny
# script that appends one line to a launch log, so the number of
# launches is counted directly rather than inferred.
#
#   ACCIDENTAL duplicates across configuration sources
#     - the real 0.21.4 scenario: ConfigMigration appended
#       auto_start_programs=<programs>, and the same programs were
#       already in workspace<N>= lines -> every one launched twice and
#       only the workspace copy was placed. Now: once each, on the
#       workspace.
#     - an XDG autostart entry naming a program that is already
#       configured above it.
#
#   INTENTIONAL multiples (must NOT be merged)
#     - the same program repeated within one source
#     - the same executable with different arguments (XDG entries)
#
#   ONCE PER X SESSION
#     - one fresh X session: autostart launches once
#     - restarting Kohiko inside the same X session (clean exit AND
#       kill -9): does not relaunch
#     - a genuinely new X session: launches again
#
# Gracefully skipped (exit 0) if Xvfb/kohiko/kohikoctl/the test client
# aren't available.

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
        pass=$((pass + 1))
        echo "  PASS: $3"
    else
        fail=$((fail + 1))
        echo "  FAIL: $3 (expected [$2], got [$1])"
    fi
}

if ! command -v Xvfb >/dev/null 2>&1; then
    echo "SKIP: Xvfb not available."
    exit 0
fi

for bin in "$KOHIKO_BIN" "$KOHIKOCTL" "$TEST_CLIENT"; do
    if [ ! -x "$bin" ]; then
        echo "SKIP: $bin not built."
        exit 0
    fi
done

WORK=$(mktemp -d)
DISPNUM=${KOHIKO_TEST_DISPLAY_BASE:-110}
XVFB_PID=""
WM_PID=""
WM_GROUPS=""
SESSION_DIR=""
LOG=""

stop_wm()
{
    [ -n "$WM_PID" ] && kill "$WM_PID" 2>/dev/null
    wait "$WM_PID" 2>/dev/null
    WM_PID=""
}

stop_x()
{
    # Every Kohiko runs in its own session/process group (see
    # start_kohiko), so the programs it launched die with the group.
    # (Deliberately NOT `pkill -f <path>`: that matches any process
    # whose command line merely mentions the path - including the
    # shell running this very test.)
    for g in $WM_GROUPS; do kill -- "-$g" 2>/dev/null; done
    WM_GROUPS=""
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null
    wait "$XVFB_PID" 2>/dev/null
    XVFB_PID=""
}

cleanup()
{
    stop_wm
    stop_x
    rm -rf "$WORK"
}
trap cleanup EXIT

# new_session <name>: fresh XDG dirs, launch log, and Xvfb.
# -noreset: a real session always has other X clients alive across a
# Kohiko restart (the apps themselves), so the server - and the root
# property that is the once-per-session marker - survives it. Without
# any other client a bare Xvfb would reset and forget when Kohiko exits.
new_session()
{
    stop_wm
    stop_x
    DISPNUM=$((DISPNUM + 1))
    SESSION_DIR="$WORK/$1"
    mkdir -p "$SESSION_DIR/config/kohiko" "$SESSION_DIR/config/autostart" "$SESSION_DIR/data" "$SESSION_DIR/run" "$SESSION_DIR/home"
    chmod 700 "$SESSION_DIR/run"
    LOG="$SESSION_DIR/launches.log"
    : > "$LOG"

    # No system-wide autostart entries may leak into the test.
    export XDG_CONFIG_DIRS="$SESSION_DIR/no-system-xdg"
    export DISPLAY=":$DISPNUM" XDG_CONFIG_HOME="$SESSION_DIR/config" XDG_DATA_HOME="$SESSION_DIR/data" \
           XDG_RUNTIME_DIR="$SESSION_DIR/run" HOME="$SESSION_DIR/home"

    start_x
}

start_x()
{
    rm -f "/tmp/.X${DISPNUM}-lock"
    Xvfb ":$DISPNUM" -noreset -screen 0 1920x1080x24 >"$SESSION_DIR/xvfb.log" 2>&1 &
    XVFB_PID=$!
    n=0
    while [ ! -e "/tmp/.X11-unix/X$DISPNUM" ] && [ "$n" -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
}

# make_app <name>: a launchable "program" that logs one line per
# launch (with its arguments) and then becomes a real window of class
# <name> so placement can be checked. Prints its path.
make_app()
{
    path="$SESSION_DIR/$1"
    cat > "$path" <<EOF
#!/bin/sh
echo "$1 \$*" >> "$LOG"
exec "$TEST_CLIENT" --class "$1" --title "$1" --net-wm-pid
EOF
    chmod +x "$path"
    echo "$path"
}

start_kohiko()
{
    setsid "$KOHIKO_BIN" "$SESSION_DIR/config/kohiko/kohiko.conf" >>"$SESSION_DIR/kohiko.log" 2>&1 &
    WM_PID=$!
    WM_GROUPS="$WM_GROUPS $WM_PID"
    n=0
    while ! grep -q 'ready' "$SESSION_DIR/kohiko.log" 2>/dev/null && [ "$n" -lt 80 ]; do sleep 0.1; n=$((n + 1)); done
    sleep 1.2 # let the launched programs start and their windows map
}

launches_of() # <app name>  (matches whole first word)
{
    awk -v a="$1" '$1 == a { c++ } END { print c + 0 }' "$LOG"
}

workspace_of() # <class>  (first match)
{
    "$KOHIKOCTL" clients 2>/dev/null | tr '{' '\n' | grep "\"class\":\"$1\"" | sed 's/.*"workspace":\([0-9]*\).*/\1/' | head -1
}

windows_of() # <class>
{
    "$KOHIKOCTL" clients 2>/dev/null | grep -o "\"class\":\"$1\"" | wc -l | tr -d ' '
}

# ---------------------------------------------------------------------------
echo "-- ACCIDENTAL duplicate: the real 0.21.4 scenario (auto_start_programs + workspace<N>) --"
new_session dup
A=$(make_app Telegramish)
B=$(make_app Discordish)
C=$(make_app Flameshotish)
# What ConfigMigration used to leave behind for a config that already had
# workspace lines: every program in BOTH places.
printf 'auto_start_programs=%s %s %s\nworkspace2=%s %s\n' "$A" "$B" "$C" "$B" "$A" > "$SESSION_DIR/config/kohiko/kohiko.conf"
start_kohiko
check "$(launches_of Telegramish)" "1" "Telegramish launched exactly once (it used to launch twice)"
check "$(launches_of Discordish)" "1" "Discordish launched exactly once"
check "$(launches_of Flameshotish)" "1" "Flameshotish (in auto_start_programs only) still launched once"
check "$(windows_of Telegramish)" "1" "only ONE Telegramish window exists (no second instance fighting for placement)"
check "$(workspace_of Telegramish)" "2" "and it landed on workspace 2 as the workspace line asked"
check "$(workspace_of Discordish)" "2" "Discordish also landed on workspace 2"
check "$(workspace_of Flameshotish)" "1" "the unplaced program stayed on the focused workspace"
stop_wm

# ---------------------------------------------------------------------------
echo "-- ACCIDENTAL duplicate: an XDG autostart entry naming an already-configured program --"
new_session xdg
A=$(make_app Xdgdup)
printf 'workspace3=%s\n' "$A" > "$SESSION_DIR/config/kohiko/kohiko.conf"
printf '[Desktop Entry]\nType=Application\nName=Xdgdup\nExec=%s\n' "$A" > "$SESSION_DIR/config/autostart/xdgdup.desktop"
start_kohiko
check "$(launches_of Xdgdup)" "1" "launched once, not once by the workspace line and again by the XDG entry"
check "$(workspace_of Xdgdup)" "3" "and placed on workspace 3"
stop_wm

# ---------------------------------------------------------------------------
echo "-- INTENTIONAL multiples are never merged --"
new_session intent
A=$(make_app Twice)
printf 'workspace1=%s %s\n' "$A" "$A" > "$SESSION_DIR/config/kohiko/kohiko.conf"
start_kohiko
check "$(launches_of Twice)" "2" "the same program listed twice in ONE source launches twice"
check "$(windows_of Twice)" "2" "and both windows exist"
stop_wm

new_session profiles
P=$(make_app Browserish)
printf 'workspace1=%s\n' "$P" > "$SESSION_DIR/config/kohiko/kohiko.conf"
printf '[Desktop Entry]\nType=Application\nName=Work\nExec=%s --profile work\n' "$P" > "$SESSION_DIR/config/autostart/browser-work.desktop"
printf '[Desktop Entry]\nType=Application\nName=Home\nExec=%s --profile home\n' "$P" > "$SESSION_DIR/config/autostart/browser-home.desktop"
start_kohiko
check "$(launches_of Browserish)" "3" "a plain launch plus two different-profile launches of the same executable all run"
check "$(grep -c -- '--profile work' "$LOG")$(grep -c -- '--profile home' "$LOG")" "11" "each profile launched exactly once"
stop_wm

# ---------------------------------------------------------------------------
echo "-- ONCE PER X SESSION: a fresh session launches autostart once --"
new_session session
A=$(make_app Sessionapp)
printf 'auto_start_programs=%s\n' "$A" > "$SESSION_DIR/config/kohiko/kohiko.conf"
start_kohiko
check "$(launches_of Sessionapp)" "1" "fresh X session: launched once"

echo "-- ...restarting Kohiko (clean exit) inside the SAME X session does not relaunch --"
stop_wm
start_kohiko
check "$(launches_of Sessionapp)" "1" "after a clean restart: still exactly one launch"
check "$(grep -c 'already ran in this X session' "$SESSION_DIR/kohiko.log")" "1" "and Kohiko said why it skipped"

echo "-- ...nor after a crash (kill -9) restart --"
kill -9 "$WM_PID" 2>/dev/null
wait "$WM_PID" 2>/dev/null
WM_PID=""
sleep 0.3
start_kohiko
check "$(launches_of Sessionapp)" "1" "after a crash restart: still exactly one launch"
check "$(windows_of Sessionapp)" "1" "and still exactly one window (the one launched originally was adopted)"
stop_wm

echo "-- ...but a genuinely NEW X session launches again --"
stop_x
start_x
: > "$SESSION_DIR/kohiko.log"
start_kohiko
check "$(launches_of Sessionapp)" "2" "new X server (new login): autostart ran again (2 launches in total)"
stop_wm

echo
echo "Passed: $pass   Failed: $fail"
[ "$fail" -eq 0 ]
