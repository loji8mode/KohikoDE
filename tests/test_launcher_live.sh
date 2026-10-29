#!/bin/sh
# Live regression test: the Launcher, workspaces, global shortcuts and
# launch requests. The REAL kohiko under Xvfb, real XTEST key input (the
# same input path as a physical keyboard - passive key grabs fire), real
# application launches through the Launcher, observed through the X
# server and Kohiko's own IPC.
#
# What this pins (see CHANGELOG.md, Unreleased):
#
#  1. Global shortcuts (SUPER+<n> -> workspace n) work while the Launcher
#     has keyboard focus. Root cause: EventDispatcher offered every
#     KeyPress to the Launcher first, including the grabbed shortcut,
#     which Xutf8LookupString turned into the character "3".
#  2. The Launcher follows a workspace switch with its query and
#     selection intact; plain digits stay text.
#  3. An application started from the Launcher opens on the workspace the
#     user is on WHEN ITS WINDOW MAPS - however slow it was to start and
#     even though the Launcher closed right after Enter. (The window's
#     workspace was never captured at Launcher open; this guards that no
#     one adds such a capture.)
#  4. An explicit workspace Window Rule still beats that.
#  5. Enter freezes WHAT is launched (an immutable LaunchRequest) and
#     launches it exactly once, however fast further keys arrive.
#  6. A failing launch cleans up; a second launch of the same
#     application never moves its existing window.
#  7. The focus the Launcher gives back is a window on the workspace the
#     user is looking at, not the one from where it was opened.
#
# LAUNCHER_TEST_SWITCH=ipc uses `kohikoctl dispatch workspace N` instead
# of the SUPER+N key, to run the delayed-launch checks against a build
# where the shortcut bug is still present.
#
# Usage: test_launcher_live.sh [kohiko] [kohikoctl] [x11_test_client] [x11_probe] [x11_input]

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT="$SCRIPT_DIR/.."
KOHIKO_BIN="${1:-$REPO_ROOT/kohiko}"
KOHIKOCTL="${2:-$REPO_ROOT/kohikoctl}"
TEST_CLIENT="${3:-$REPO_ROOT/build/x11_test_client}"
PROBE="${4:-$REPO_ROOT/build/x11_probe}"
INPUT="${5:-$REPO_ROOT/build/x11_input}"

. "$SCRIPT_DIR/live/common.sh"

live_require Xvfb
[ -x "$PROBE" ] && [ -x "$INPUT" ] || { echo "SKIP: x11_probe/x11_input not built."; exit 0; }

live_init 240

LAUNCH_LOG="$WORKDIR/launch.log"
: > "$LAUNCH_LOG"
mkdir -p "$XDG_DATA_HOME/applications" "$WORKDIR/bin"

# Two "applications": each records that it was started, then creates its
# window only after a delay - a slow-starting program.
make_app()   # make_app <id> <Name> <class> <delay ms>
{
    cat > "$WORKDIR/bin/$1" <<APP
#!/bin/sh
echo "$1" >> "$LAUNCH_LOG"
exec "$TEST_CLIENT" --title "$2" --class "$3" --map-delay-ms "$4" --color 200,60,60 --width 400 --height 300
APP
    chmod +x "$WORKDIR/bin/$1"
    cat > "$XDG_DATA_HOME/applications/$1.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=$2
Exec=$WORKDIR/bin/$1
DESK
}

make_app alpha "Alphaprogram" alphaapp 1500
make_app beta  "Betaprogram"  betaapp  1500
cat > "$WORKDIR/bin/broken" <<APP
#!/bin/sh
echo "broken" >> "$LAUNCH_LOG"
exit 3
APP
chmod +x "$WORKDIR/bin/broken"
cat > "$XDG_DATA_HOME/applications/broken.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=Brokenprogram
Exec=$WORKDIR/bin/broken
DESK

live_write_config 'workspace.count=4
auto_start_programs=
bind=SUPER+D launcher_toggle
bind=SUPER+Q close
bind=SUPER+1 workspace 1
bind=SUPER+2 workspace 2
bind=SUPER+3 workspace 3
bind=SUPER+4 workspace 4'
live_start_wm

launcher_field() { "$KOHIKOCTL" launcher | sed -n "s/.*\"$1\":\([^,}]*\).*/\1/p" | tr -d '"'; }
active_ws()      { "$KOHIKOCTL" monitors | sed -n 's/.*"workspace":\([0-9]*\).*/\1/p' | head -1; }
app_ws()         { clients_lines | grep "\"class\":\"$1\"" | head -1 | sed -n 's/.*"workspace":\([0-9]*\).*/\1/p'; }
app_listed()     { [ -n "$(app_ws "$1")" ]; }
launches()       { grep -c "^$1\$" "$LAUNCH_LOG"; }

switch_ws()
{
    if [ "${LAUNCHER_TEST_SWITCH:-key}" = "ipc" ]; then
        "$KOHIKOCTL" dispatch workspace "$1" >/dev/null
    else
        "$INPUT" key "$1" super
    fi
}

wait_launcher_matches()   # wait until the (async-built) index produced a match
{
    i=0
    while [ "$i" -lt 60 ]; do
        [ "$(launcher_field matches)" -ge 1 ] 2>/dev/null && return 0
        sleep 0.2; i=$((i + 1))
    done
    return 1
}

open_launcher() { "$INPUT" key d super; sleep 0.15; }

close_all_apps()
{
    for c in alphaapp betaapp; do
        while app_listed "$c"; do "$KOHIKOCTL" dispatch close >/dev/null; sleep 0.2; break; done
    done
}

sleep 1.2   # the application index is built on a background thread at startup

echo "== 1. Global shortcuts work while the Launcher has keyboard focus =="

open_launcher
check "$(launcher_field open)" "true" "SUPER+D opens the Launcher"
"$INPUT" type alpha
check "$(launcher_field query)" "alpha" "typing fills the query"
wait_launcher_matches
SEL_BEFORE=$(launcher_field selected)

switch_ws 3
sleep 0.2
check "$(active_ws)" "3" "SUPER+3 from the focused Launcher switches to workspace 3"
check "$(launcher_field open)" "true" "the Launcher is still open after the switch"
check "$(launcher_field query)" "alpha" "its query is intact (and no '3' was typed into it)"
check "$(launcher_field selected)" "$SEL_BEFORE" "its selection is intact"
check "$("$PROBE" focus)" "$(printf '0x%x' "$(launcher_field window)")" "the Launcher still has X keyboard focus (not the application window behind it)"

"$INPUT" key 3
check "$(launcher_field query)" "alpha3" "an UNMODIFIED digit is still ordinary Launcher text"
"$INPUT" key BackSpace
check "$(launcher_field query)" "alpha" "BackSpace still edits"

switch_ws 1
sleep 0.2
check "$(active_ws)" "1" "SUPER+1 switches back"
check "$(launcher_field query)" "alpha" "query still intact after a round trip"
"$INPUT" key d super
sleep 0.15
check "$(launcher_field open)" "false" "SUPER+D (launcher_toggle) closes the Launcher from its own focus"

echo "== 2. Escape and ordinary navigation are unchanged =="
open_launcher
"$INPUT" type al
"$INPUT" key Escape
sleep 0.1
check "$(launcher_field open)" "false" "Escape closes it"
open_launcher
check "$(launcher_field query)" "" "reopened empty"
"$INPUT" key Escape
sleep 0.1

echo "== 3. A slow application follows the workspace the user is on when its window maps =="

: > "$LAUNCH_LOG"
open_launcher
"$INPUT" type alpha
wait_launcher_matches
"$INPUT" key Return          # the Launcher closes immediately; alpha takes 1.5 s to create its window
sleep 0.2
check "$(launcher_field open)" "false" "the Launcher closed right after Enter"
switch_ws 3                   # the user moves on while alpha is still starting
sleep 0.2
check "$(active_ws)" "3" "user is now on workspace 3"
live_wait_for 80 app_listed alphaapp
check "$(app_ws alphaapp)" "3" "the window opens on workspace 3 (latest), not workspace 1 (where the Launcher opened)"
check "$(launches alpha)" "1" "alpha was started exactly once"

echo "== 4. A second launch never moves the existing window =="
switch_ws 2
sleep 0.2
open_launcher
"$INPUT" type alpha
wait_launcher_matches
"$INPUT" key Return
live_wait_for 80 sh -c "[ \"\$(\"$KOHIKOCTL\" clients | grep -c '\"class\":\"alphaapp\"')\" -ge 2 ]"
sleep 0.2
check "$(clients_lines | grep '"class":"alphaapp"' | sed -n 's/.*"workspace":\([0-9]*\).*/\1/p' | sort | tr '\n' ' ')" "2 3 " \
      "the first alpha window stayed on 3; the new one opened on 2"
check "$(launches alpha)" "2" "alpha was started twice - once per submission"

echo "== 5. An explicit workspace Window Rule beats the Launcher context =="
live_write_config 'workspace.count=4
auto_start_programs=
bind=SUPER+D launcher_toggle
bind=SUPER+Q close
bind=SUPER+1 workspace 1
bind=SUPER+2 workspace 2
bind=SUPER+3 workspace 3
bind=SUPER+4 workspace 4
windowrule=app:betaapp workspace:1'
"$KOHIKOCTL" reload >/dev/null; sleep 0.4
BEFORE_RULES=$(grep '^windowrule=' "$CONF" | cksum)
switch_ws 4
sleep 0.2
open_launcher
"$INPUT" type beta
wait_launcher_matches
"$INPUT" key Return
live_wait_for 80 app_listed betaapp
check "$(app_ws betaapp)" "1" "betaapp's explicit rule (workspace 1) wins over the workspace the user is on (4)"
check "$(grep '^windowrule=' "$CONF" | cksum)" "$BEFORE_RULES" "no rule was created or changed by launching or switching"

echo "== 6. Enter freezes the selection: rapid input afterwards cannot change what is launched =="
: > "$LAUNCH_LOG"
switch_ws 4; sleep 0.2
open_launcher
"$INPUT" type alpha
wait_launcher_matches
# Enter, then - already queued behind it - a different query, selection
# keys and a SECOND Enter. None of it may reach the Launcher.
"$INPUT" burst Return b e t a Down Down Up Return
sleep 0.4
check "$(launches alpha)" "1" "the submitted application (alpha) was launched exactly once"
check "$(launches beta)" "0" "the later 'beta' typing launched nothing"
check "$(launcher_field open)" "false" "the Launcher stayed closed"

echo "== 7. A failing launch cleans up and does not wedge the Launcher =="
: > "$LAUNCH_LOG"
open_launcher
"$INPUT" type broken
wait_launcher_matches
"$INPUT" key Return
sleep 0.6
check "$(launches broken)" "1" "the failing program was started once"
check "$(launcher_field open)" "false" "the Launcher closed"
check_true "the window manager is still running" kill -0 "$WM_PID"
open_launcher
check "$(launcher_field open)" "true" "the Launcher opens again normally"
check "$(launcher_field query)" "" "...empty, with no leftover state from the failed launch"
"$INPUT" key Escape
sleep 0.1

echo "== 8. Focus handed back is a window on the workspace the user is looking at =="
# A tiled window on workspace 1, another on workspace 2; open the Launcher on 1, switch to 2, close it.
switch_ws 1; sleep 0.2
"$TEST_CLIENT" --title fw1 --class focusone --color 30,30,220 > "$WORKDIR/fw1.log" 2>&1 & LIVE_PIDS="$LIVE_PIDS $!"
live_wait_for 60 app_listed focusone
switch_ws 2; sleep 0.2
"$TEST_CLIENT" --title fw2 --class focustwo --color 30,200,30 > "$WORKDIR/fw2.log" 2>&1 & LIVE_PIDS="$LIVE_PIDS $!"
live_wait_for 60 app_listed focustwo
switch_ws 1; sleep 0.2
open_launcher
switch_ws 2; sleep 0.2
"$INPUT" key Escape
sleep 0.2
check "$("$KOHIKOCTL" activewindow | sed -n 's/.*"class":"\([^"]*\)".*/\1/p')" "focustwo" \
      "after closing, the focused window is the one on workspace 2 (where the user is), not the one from workspace 1"

live_finish
