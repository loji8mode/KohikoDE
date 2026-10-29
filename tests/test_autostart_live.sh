#!/bin/sh
# Live regression tests for startup/autostart (CHANGELOG.md 0.22.0): the
# real kohiko under Xvfb launching real (shim) programs whose launches
# are counted. Pins the responsibilities of the three things that can
# start a program:
#
#     workspace<N>=         launch + put its window on workspace N
#     auto_start_programs   launch
#     XDG autostart         launch (entries apps install themselves)
#
# Bugs reproduced here (each fails on the 0.21.4 baseline):
#   - a program named by two mechanisms was launched twice. Real,
#     reachable reproducer: the shipped config puts zen-browser /
#     discord Telegram / steam on workspace<N>= lines, while the config
#     migration appended the SCHEMA default for a missing
#     auto_start_programs - "Telegram discord zen-browser flameshot" -
#     so Telegram, discord and zen-browser each started twice;
#   - restarting kohiko inside the same login session (kohiko-session
#     does this after a crash) launched every startup program again on
#     top of the ones still running;
#   - `kohikoctl dispatch exec ...` did not return until the program it
#     launched exited (every spawned program inherited the IPC socket).
#
# And the opposite guards: listing a program several times WITHIN one
# mechanism is an explicit request for several instances and is honoured
# (no global "deduplicate by executable name"), and a workspace<N>= line
# still launches its program AND still lands its window on workspace N.
#
# Usage: test_autostart_live.sh [kohiko] [kohikoctl] [x11_test_client]
# Skipped (exit 0) when Xvfb or a binary isn't available.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT="$SCRIPT_DIR/.."
KOHIKO_BIN="${1:-$REPO_ROOT/kohiko}"
KOHIKOCTL="${2:-$REPO_ROOT/kohikoctl}"
TEST_CLIENT="${3:-$REPO_ROOT/build/x11_test_client}"

. "$SCRIPT_DIR/live/common.sh"

live_require Xvfb
live_init 120

# Programs that record that they were launched (one line each, named
# after themselves) and then stay alive - exactly what a long-running
# application does.
BIN="$WORKDIR/bin"
LAUNCHES="$WORKDIR/launches"
mkdir -p "$BIN"
: > "$LAUNCHES"

# Each shim also records its own pid, so cleanup kills exactly what
# this test started (never anything matched by name).
SHIM_PIDS="$WORKDIR/shim.pids"
: > "$SHIM_PIDS"

shim()
{
    printf '#!/bin/sh\necho %s >> "%s"\necho $$ >> "%s"\nexec sleep 7351\n' "$1" "$LAUNCHES" "$SHIM_PIDS" > "$BIN/$1"
    chmod +x "$BIN/$1"
}

kill_shims()
{
    while read -r pid; do kill "$pid" 2>/dev/null; done < "$SHIM_PIDS"
    : > "$SHIM_PIDS"
}

for name in Telegram discord zen-browser steam flameshot xdgapp onlyxdg multi dup solo; do
    shim "$name"
done

# A shim that also opens a real window, to check workspace placement.
printf '#!/bin/sh\necho pinned-app >> "%s"\necho $$ >> "%s"\nexec "%s" --class pinned --title pinned --width 300 --height 200 --set-pid\n' \
    "$LAUNCHES" "$SHIM_PIDS" "$TEST_CLIENT" > "$BIN/pinned-app"
chmod +x "$BIN/pinned-app"

PATH="$BIN:$PATH"
export PATH

cleanup_all()
{
    kill_shims
    live_cleanup
}
trap cleanup_all EXIT INT TERM

launched() { n=$(grep -c "^$1\$" "$LAUNCHES" 2>/dev/null); echo "${n:-0}"; }
settle()   { sleep 1.5; }

# ---------------------------------------------------------------------
echo "== 1. The real conflict: shipped workspace<N>= lines + a migrated auto_start_programs =="

live_write_config 'workspace.count=4
workspace1=zen-browser
workspace2=discord Telegram
workspace3=steam
auto_start_programs=Telegram discord zen-browser flameshot'
live_start_wm
settle

check "$(launched Telegram)"    "1" "Telegram (workspace2= AND auto_start_programs) starts once"
check "$(launched discord)"     "1" "discord (workspace2= AND auto_start_programs) starts once"
check "$(launched zen-browser)" "1" "zen-browser (workspace1= AND auto_start_programs) starts once"
check "$(launched steam)"       "1" "steam (workspace3= only) starts once"
check "$(launched flameshot)"   "1" "flameshot (auto_start_programs only) still starts - an unrelated program is unaffected"
check "$(grep -c 'not launching auto_start_programs entry' "$WORKDIR/kohiko.log")" "3" \
    "each skipped duplicate is logged, with the reason"

echo
echo "== 2. A migrated config can no longer invent a conflicting launch list =="

live_quit_wm
kill_shims; sleep 0.3
: > "$LAUNCHES"
# A user config that lacks auto_start_programs entirely; the migration
# fills in the schema default.
live_write_config 'workspace.count=4
workspace1=zen-browser
workspace2=discord Telegram
workspace3=steam'
# New login session (token differs) so autostart is allowed to run again.
live_start_wm XDG_SESSION_ID=migrated
settle
check "$(grep -c '^auto_start_programs=' "$CONF")" "1" "(the migration did add the key)"
check "$(launched Telegram)/$(launched discord)/$(launched zen-browser)" "1/1/1" \
    "...and what it added overlaps nothing the shipped workspace lines launch"

# ---------------------------------------------------------------------
echo
echo "== 3. XDG autostart entries vs explicit configuration =="

live_quit_wm
kill_shims; sleep 0.3
: > "$LAUNCHES"
mkdir -p "$XDG_CONFIG_HOME/autostart"
for name in xdgapp onlyxdg; do
    printf '[Desktop Entry]\nType=Application\nName=%s\nExec=%s/%s\n' "$name" "$BIN" "$name" \
        > "$XDG_CONFIG_HOME/autostart/$name.desktop"
done
live_write_config 'workspace.count=4
auto_start_programs=xdgapp'
live_start_wm XDG_SESSION_ID=xdg
settle
check "$(launched xdgapp)" "1" "a program in BOTH auto_start_programs and an XDG entry starts once"
check "$(launched onlyxdg)" "1" "an XDG entry nothing else mentions still starts"
check "$(grep -c 'not launching XDG autostart entry xdgapp' "$WORKDIR/kohiko.log")" "1" "the skipped XDG duplicate is logged"

# ---------------------------------------------------------------------
echo
echo "== 4. Intentional multiple instances are NOT collapsed =="

live_quit_wm
kill_shims; sleep 0.3
: > "$LAUNCHES"
live_write_config 'workspace.count=4
auto_start_programs=multi multi
workspace1=dup
workspace2=dup'
live_start_wm XDG_SESSION_ID=multi
settle
check "$(launched multi)" "2" "the same program listed twice in auto_start_programs starts twice"
check "$(launched dup)" "2" "the same program on two workspace lines starts twice"

# ---------------------------------------------------------------------
echo
echo "== 5. workspace<N>= still launches AND still places the window =="

live_quit_wm
kill_shims; sleep 0.3
: > "$LAUNCHES"
live_write_config 'workspace.count=4
workspace3=pinned-app'
live_start_wm XDG_SESSION_ID=pin
live_wait_for 80 class_listed pinned
check "$(launched pinned-app)" "1" "the program was launched"
check "$(live_field pinned workspace)" "3" "...and its window opened on workspace 3"

# ---------------------------------------------------------------------
echo
echo "== 6. Restarting kohiko within one login session does not relaunch anything =="

live_write_config 'workspace.count=4
auto_start_programs=solo'
live_crash_wm
kill_shims; sleep 0.3
: > "$LAUNCHES"

live_start_wm XDG_SESSION_ID=sessionone
settle
check "$(launched solo)" "1" "first start in a session launches the program"

# A real session always has other X clients (the apps themselves, tray
# widgets) still connected when the window manager dies. That matters
# here: an X server RESETS when its last client disconnects, wiping root
# window properties - so keep one ordinary client alive across the
# restarts below, exactly as a real session would.
live_open session_client

live_crash_wm       # kohiko-session restarts it like this after a crash
live_start_wm XDG_SESSION_ID=sessionone
settle
check "$(launched solo)" "1" "a restart in the same session does NOT launch it again (it is still running)"
check "$(grep -c 'autostart already ran in this login session' "$WORKDIR/kohiko.log")" "1" "...and says so in the log"

"$KOHIKOCTL" reload >/dev/null 2>&1
settle
check "$(launched solo)" "1" "kohikoctl reload doesn't relaunch it either"

live_crash_wm
live_start_wm XDG_SESSION_ID=sessiontwo
settle
check "$(launched solo)" "2" "a NEW login session (e.g. a display manager reusing the X server) launches autostart again"

# ---------------------------------------------------------------------
echo
echo "== 7. A spawned program must not hold kohikoctl's connection open =="

live_quit_wm
kill_shims; sleep 0.3
live_write_config 'workspace.count=4
auto_start_programs=
exec.sleeper=sleep 6'
live_start_wm XDG_SESSION_ID=ipc
t0=$(date +%s%N)
"$KOHIKOCTL" dispatch exec sleeper >/dev/null 2>&1
t1=$(date +%s%N)
check_lt "$(( (t1 - t0) / 1000000 ))" 1500 \
    "kohikoctl dispatch exec returns at once, not when the 6 s program exits (0.21.4: 6013 ms)"

live_finish
