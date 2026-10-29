#!/bin/sh
# Live regression tests for Session Restore (CHANGELOG.md 0.22.0): the
# real kohiko under Xvfb, shut down and restarted with windows still open.
#
# Session Restore's one job: windows that were already open when Kohiko
# (re)started go back to where they were. It must do nothing else.
#
# The bug (fails on 0.21.4): saved records are matched by the bare X11
# window id, and X11 RECYCLES ids - a client that reconnects gets its old
# resource-id range back (verified: the same program launched three
# times in a row got window 0x200001 every time). The records were never
# expired, so after one restart ANY brand-new window that happened to
# land on a recycled id silently inherited a dead window's workspace /
# floating / fullscreen state - and with session.restore_priority=session
# even overrode its window rules.
#
# Also pinned: which wins when a window rule and the saved session
# disagree is decided by session.restore_priority (default `config`, the
# rule), and only for windows that were open at startup.
#
# Usage: test_session_restore_live.sh [kohiko] [kohikoctl] [x11_test_client]
# Skipped (exit 0) when Xvfb or a binary isn't available.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT="$SCRIPT_DIR/.."
KOHIKO_BIN="${1:-$REPO_ROOT/kohiko}"
KOHIKOCTL="${2:-$REPO_ROOT/kohikoctl}"
TEST_CLIENT="${3:-$REPO_ROOT/build/x11_test_client}"

. "$SCRIPT_DIR/live/common.sh"

live_require Xvfb
live_init 130

# ---------------------------------------------------------------------
echo "== 1. A window open across a restart is restored; a LATER window reusing its id is not =="

live_write_config 'workspace.count=4
auto_start_programs='
live_start_wm

live_open sessA
a_pid=$LAST_PID
a_id=$(live_field sessA id)
"$KOHIKOCTL" dispatch movetoworkspace 3 >/dev/null
sleep 0.3
check "$(live_field sessA workspace)" "3" "(setup) the window was moved to workspace 3 by hand"

live_quit_wm            # clean shutdown: saves the session
live_start_wm           # restart with sessA still open
live_wait_for 50 class_listed sessA

check "$(live_field sessA id)" "$a_id" "(setup) the window kept its X11 id across the restart"
check "$(live_field sessA workspace)" "3" \
    "Session Restore still does its job: the window open across the restart is back on workspace 3"

# Now a different application opens - on a recycled id, if the server hands it out.
live_close "$a_pid" sessA
live_open sessB
b_id=$(live_field sessB id)

if [ "$b_id" = "$a_id" ]; then
    echo "  (the new window DID recycle the old window's X11 id $a_id - this is the case that matters)"
    check "$(live_field sessB workspace)" "1" \
        "a brand-new window that reuses a dead window's X11 id opens on the CURRENT workspace, not the dead window's workspace 3"
else
    echo "  (this X server didn't recycle the id [$a_id vs $b_id]; the reuse case can't be exercised here)"
    check "$(live_field sessB workspace)" "1" "a new window opens on the current workspace"
fi

# ---------------------------------------------------------------------
echo
echo "== 2. Rule vs saved session: session.restore_priority decides, and only at startup =="

live_quit_wm
for pid in $LIVE_PIDS; do kill "$pid" 2>/dev/null; done
LIVE_PIDS=""; sleep 0.5

run_cycle()
{
    # run_cycle <priority> -> prints the workspace a window that sat on
    # workspace 3 at shutdown comes back on, with a rule saying workspace 2.
    live_write_config "workspace.count=4
auto_start_programs=
session.restore_priority=$1
windowrule=app:sessC workspace:2"
    live_start_wm
    live_open sessC
    # It opened by its rule on workspace 2 (unfocused). Go there, move
    # it to 3 by hand (so the saved session says 3), then restart.
    "$KOHIKOCTL" dispatch workspace 2 >/dev/null; sleep 0.3
    "$KOHIKOCTL" dispatch movetoworkspace 3 >/dev/null; sleep 0.3
    "$KOHIKOCTL" dispatch workspace 1 >/dev/null; sleep 0.3
    live_quit_wm
    live_start_wm
    live_wait_for 50 class_listed sessC
    result=$(live_field sessC workspace)
    live_quit_wm
    for pid in $LIVE_PIDS; do kill "$pid" 2>/dev/null; done
    LIVE_PIDS=""; sleep 0.5
    echo "$result"
}

check "$(run_cycle config)" "2" \
    "restore_priority=config (the default): the explicit rule wins over where the window was - workspace 2"
check "$(run_cycle session)" "3" \
    "restore_priority=session: the window goes back exactly where it was - workspace 3"

live_finish
