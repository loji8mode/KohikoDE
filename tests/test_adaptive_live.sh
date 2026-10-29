#!/bin/sh
# Live regression test for adaptive placement's WindowManager wiring -
# runs the *real* kohiko binary as an actual window manager under
# Xvfb, against tests/live/x11_test_client (a small real X11 client),
# and asserts through the real on-disk habit file. The pure
# learning-engine arithmetic lives in tests/test_placementhabits.cpp;
# this file covers what only a real WindowManager can get wrong:
#
#   LEARNING EVENTS (what DOES count)
#     - a window opening with nothing deciding its workspace is one
#       workspace opening; a second tile next to it is also a
#       position opening on both axes;
#     - `kohikoctl dispatch movetoworkspace N` is one user vote;
#     - a user swap (Super+Shift+h/j/k/l path) votes for the window
#       the USER moved ONLY - the passive neighbour gets nothing
#       (0.21.4 voted for both), and a window spanning the vertical
#       axis casts no top/bottom vote.
#
#   NOT EVIDENCE (what must NOT count)
#     - a window placed by an existing habit: no opening, no vote
#       (Kohiko never learns from its own placement; a working habit
#       does not erode by being applied);
#     - a window opened by a `windowrule=workspace:N`;
#     - a transient/dialog;
#     - a window already open when Kohiko started (adoption);
#     - a window launched by Kohiko's own autostart.
#
#   TEMPORARY ACTIONS
#     - sending a habit window away and straight back leaves exactly
#       the original habit (one vote per window, last placement wins).
#
#   MIGRATION
#     - a 0.21.4 (headerless) habit file is upgraded in place, keeps
#       predicting, and the original is kept as .v1.bak.
#
# Gracefully skipped (exit 0) if Xvfb/kohiko/kohikoctl/the test client
# aren't available, rather than failing the whole suite over missing
# optional live-session infrastructure.

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
DISPNUM=${KOHIKO_TEST_DISPLAY_BASE:-90}
XVFB_PID=""
WM_PID=""
CLIENT_PIDS=""
CONFIG=""
HABITS=""
SESSION_DIR=""

stop_session()
{
    for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
    CLIENT_PIDS=""
    [ -n "$WM_PID" ] && kill "$WM_PID" 2>/dev/null
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null
    wait 2>/dev/null
    WM_PID=""
    XVFB_PID=""
}

cleanup()
{
    stop_session
    rm -rf "$WORK"
}
trap cleanup EXIT

# start_session <name> [config-file-contents]
# Fresh Xvfb + fresh XDG dirs. With no config text there is NO config
# file, i.e. Kohiko runs on its built-in defaults (what most scenarios
# want); a scenario that needs config passes its text.
start_session()
{
    stop_session
    DISPNUM=$((DISPNUM + 1))
    SESSION_DIR="$WORK/$1"
    mkdir -p "$SESSION_DIR/config/kohiko" "$SESSION_DIR/data" "$SESSION_DIR/run" "$SESSION_DIR/home"
    chmod 700 "$SESSION_DIR/run"
    CONFIG="$SESSION_DIR/config/kohiko/kohiko.conf"
    rm -f "$CONFIG"
    [ -n "${2:-}" ] && printf '%s\n' "$2" > "$CONFIG"
    HABITS="$SESSION_DIR/data/kohiko/placement_habits"

    rm -f "/tmp/.X${DISPNUM}-lock"
    Xvfb ":$DISPNUM" -screen 0 1920x1080x24 >"$SESSION_DIR/xvfb.log" 2>&1 &
    XVFB_PID=$!
    n=0
    while [ ! -e "/tmp/.X11-unix/X$DISPNUM" ] && [ "$n" -lt 50 ]; do sleep 0.1; n=$((n + 1)); done

    export DISPLAY=":$DISPNUM" XDG_CONFIG_HOME="$SESSION_DIR/config" XDG_DATA_HOME="$SESSION_DIR/data" \
           XDG_RUNTIME_DIR="$SESSION_DIR/run" HOME="$SESSION_DIR/home"
}

start_kohiko()
{
    # Kohiko reads $HOME/.config/kohiko/kohiko.conf, or argv[1]: pass
    # the scenario's own file explicitly if it wrote one.
    if [ -f "$CONFIG" ]; then
        "$KOHIKO_BIN" "$CONFIG" >"$SESSION_DIR/kohiko.log" 2>&1 &
    else
        "$KOHIKO_BIN" >"$SESSION_DIR/kohiko.log" 2>&1 &
    fi
    WM_PID=$!
    n=0
    while ! grep -q 'ready' "$SESSION_DIR/kohiko.log" 2>/dev/null && [ "$n" -lt 80 ]; do sleep 0.1; n=$((n + 1)); done
}

wait_for_class() # <class> [max tenths of a second]
{
    n=0
    while ! "$KOHIKOCTL" clients 2>/dev/null | grep -q "\"class\":\"$1\"" && [ "$n" -lt "${2:-50}" ]; do sleep 0.1; n=$((n + 1)); done
    sleep 0.2
}

# open_window <class> [extra client args...]  -> waits until managed
open_window()
{
    cls=$1; shift
    "$TEST_CLIENT" --class "$cls" --title "$cls" "$@" >"$SESSION_DIR/client-$cls.out" 2>&1 &
    CLIENT_PIDS="$CLIENT_PIDS $!"
    wait_for_class "$cls"
}

dispatch() { "$KOHIKOCTL" dispatch "$@" >/dev/null 2>&1; sleep 0.3; }

# Habit file field accessors.
# v2 line:  class leftV rightV topV bottomV hOpen vOpen wsOpen n [ws votes]...
habit_field() # <class> <1-based field number after the class>
{
    awk -v c="$1" -v f="$(($2 + 1))" '$1 == c { print $f }' "$HABITS" 2>/dev/null
}
ws_votes() # <class> <workspace>
{
    awk -v c="$1" -v w="$2" '$1 == c { for (i = 10; i < NF; i += 2) if ($i == w) print $(i + 1) }' "$HABITS" 2>/dev/null
}
workspace_of() # <class>
{
    "$KOHIKOCTL" clients 2>/dev/null | tr '{' '\n' | grep "\"class\":\"$1\"" | sed 's/.*"workspace":\([0-9]*\).*/\1/' | head -1
}

L=1 R=2 T=3 B=4 HO=5 VO=6 WO=7

# ---------------------------------------------------------------------------
echo "-- A window opening with nothing deciding its placement is an opening --"
start_session opening
start_kohiko
open_window Alpha
check "$(habit_field Alpha $WO)" "1" "workspace opening recorded for a plain new window"
check "$(habit_field Alpha $HO)" "0" "no position opening: a lone window has nothing to swap with"
open_window Beta
check "$(habit_field Beta $WO)" "1" "second window: workspace opening recorded"
check "$(habit_field Beta $HO)$(habit_field Beta $VO)" "11" "second tile: position opening recorded on both axes"

echo "-- A user move to another workspace is a vote --"
dispatch movetoworkspace 3
check "$(ws_votes Beta 3)" "1" "'movetoworkspace 3' recorded exactly one vote for workspace 3"
check "$(workspace_of Beta)" "3" "and the window really is on workspace 3"
stop_session

# ---------------------------------------------------------------------------
echo "-- A swap votes for the window the USER moved, never the passive one --"
start_session swap
start_kohiko
open_window Passive
open_window Mover
check "$("$KOHIKOCTL" activewindow | sed 's/.*"class":"\([^"]*\)".*/\1/')" "Mover" "the newest window (Mover) is the focused one"
dispatch move left
check "$(habit_field Mover $L)" "1" "the mover got a left vote"
check "$(habit_field Passive $L)$(habit_field Passive $R)$(habit_field Passive $T)$(habit_field Passive $B)" "0000" "the passive neighbour got NO position vote (0.21.4 gave it a right vote)"
check "$(habit_field Mover $T)$(habit_field Mover $B)" "00" "side-by-side windows span the vertical axis: no top/bottom vote (0.21.4 cast one)"
stop_session

# ---------------------------------------------------------------------------
echo "-- Kohiko's own placement is never evidence; a working habit does not erode --"
start_session habit
mkdir -p "$SESSION_DIR/data/kohiko"
# A confident habit: 3 votes for workspace 2 out of 3 openings.
printf '# kohiko-placement-habits v2\nAuto 0 0 0 0 0 0 3 1 2 3\n' > "$HABITS"
start_kohiko
open_window Auto
check "$(workspace_of Auto)" "2" "the habit placed the new window on workspace 2"
check "$(ws_votes Auto 2)" "3" "that placement did NOT add a vote (Kohiko must not learn from itself)"
check "$(habit_field Auto $WO)" "3" "and did NOT add an opening (it would dilute the habit it just used)"
open_window Auto
open_window Auto
check "$(habit_field Auto $WO)$(ws_votes Auto 2)" "33" "after 3 habit-placed windows: still 3 openings / 3 votes, habit unchanged"
stop_session

# ---------------------------------------------------------------------------
echo "-- A brief move away and straight back leaves the habit untouched --"
start_session temp
mkdir -p "$SESSION_DIR/data/kohiko"
printf '# kohiko-placement-habits v2\nHome 0 0 0 0 0 0 3 1 3 3\n' > "$HABITS"
start_kohiko
open_window Home
dispatch workspace 3
check "$(workspace_of Home)" "3" "habit placed Home on workspace 3 (we switched to it)"
dispatch movetoworkspace 5
check "$(ws_votes Home 5)" "1" "the excursion to workspace 5 is a vote while it lasts"
dispatch workspace 5
dispatch movetoworkspace 3
check "$(ws_votes Home 5)" "" "moving it back retracts the temporary vote for workspace 5"
check "$(ws_votes Home 3)" "4" "and counts once for workspace 3"
stop_session

# ---------------------------------------------------------------------------
echo "-- Explicit configuration is not an opportunity --"
start_session rule "windowrule=workspace:2 class:Ruled"
start_kohiko
open_window Ruled
check "$(workspace_of Ruled)" "2" "the windowrule placed the window on workspace 2"
check "$(habit_field Ruled $WO)" "" "and it was NOT recorded as an opening (a rule decided, not the user)"
stop_session

echo "-- A transient dialog is not an opportunity --"
start_session transient
start_kohiko
open_window Parent
parent_id=$("$KOHIKOCTL" clients | tr '{' '\n' | grep '"class":"Parent"' | sed 's/.*"id":\([0-9]*\).*/\1/' | head -1)
open_window Child --transient-for "$parent_id"
check "$(habit_field Parent $WO)" "1" "the parent window is an opening"
check "$(habit_field Child $WO)" "" "its transient child is NOT"
stop_session

# ---------------------------------------------------------------------------
echo "-- A window already open when Kohiko starts (adoption) is not an opportunity --"
start_session adopt
"$TEST_CLIENT" --class Existing --title Existing >"$SESSION_DIR/client-Existing.out" 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 0.6
start_kohiko
wait_for_class Existing 30
check "$("$KOHIKOCTL" clients | grep -c '"class":"Existing"')" "1" "the pre-existing window was adopted"
check "$(habit_field Existing $WO)" "" "but adoption did not count as an opening"
open_window Fresh
check "$(habit_field Fresh $WO)" "1" "while a window opened afterwards does"
stop_session

# ---------------------------------------------------------------------------
echo "-- A window launched by Kohiko's own autostart is not an opportunity --"
start_session autostart
cat > "$SESSION_DIR/launch-started.sh" <<EOF
#!/bin/sh
exec "$TEST_CLIENT" --class Started --title Started --net-wm-pid
EOF
chmod +x "$SESSION_DIR/launch-started.sh"
printf 'auto_start_programs=%s\n' "$SESSION_DIR/launch-started.sh" > "$CONFIG"
start_kohiko
wait_for_class Started 50
check "$("$KOHIKOCTL" clients | grep -c '"class":"Started"')" "1" "the autostart program's window appeared"
check "$(habit_field Started $WO)" "" "and was NOT counted as an opening (Kohiko launched it, the user did not)"
open_window ByUser
check "$(habit_field ByUser $WO)" "1" "a window the user opens later still is"
stop_session

# ---------------------------------------------------------------------------
echo "-- A 0.21.4 (headerless) habit file is migrated, still works, and is backed up --"
start_session migrate
mkdir -p "$SESSION_DIR/data/kohiko"
printf 'Legacy 0 0 0 0 1 3 5\n' > "$HABITS"
cp "$HABITS" "$SESSION_DIR/original.v1"
start_kohiko
open_window Legacy
check "$(workspace_of Legacy)" "3" "the legacy habit (5 votes -> ws 3) still placed the window"
check "$(head -1 "$HABITS")" "# kohiko-placement-habits v2" "the file was rewritten as v2"
check "$(cmp -s "$HABITS.v1.bak" "$SESSION_DIR/original.v1" && echo same)" "same" "the original 0.21.4 file was kept byte-for-byte as .v1.bak"
check "$(habit_field Legacy $WO)" "5" "the migrated habit is intact and the habit-placed window added no opening"
stop_session

echo
echo "Passed: $pass   Failed: $fail"
[ "$fail" -eq 0 ]
