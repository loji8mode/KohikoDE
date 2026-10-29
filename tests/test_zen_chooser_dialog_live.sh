#!/bin/sh
# Live test: the REAL kohiko-choose-window dialog, driven by REAL keys
# (XTEST), for the two Zen window kinds - the user-facing workflow:
#
#   focus a window -> press the choose_window key (SUPER+SHIFT+W) ->
#   pick "Applies to" / workspace / position / mode -> Enter
#
# The window manager (the real `kohiko`) spawns the real dialog with the
# focused window's identity read from X11; the test only presses keys.
# Windows are x11_test_client windows carrying exactly the properties
# collected from a real Zen Browser on Arch/KohikoDE (see
# test_zen_window_rules_live.sh). This is NOT a run against a real Zen
# Browser, and the Xft text is a no-op stub in sandbox builds, so what
# the dialog LOOKS like is not checked here - only what it does.
#
# Also a smoke test of kohiko-settings with role/type rules (it opens,
# lays out the three-line rows and stays alive; it is not driven).
#
# Usage: test_zen_chooser_dialog_live.sh [kohiko] [kohikoctl] [kohiko-choose-window] [x11_test_client] [x11_input] [kohiko-settings]
# Skipped (exit 0) when Xvfb or a binary isn't available.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT="$SCRIPT_DIR/.."
KOHIKO_BIN="${1:-$REPO_ROOT/kohiko}"
KOHIKOCTL="${2:-$REPO_ROOT/kohikoctl}"
CHOOSER="${3:-$REPO_ROOT/kohiko-choose-window}"
TEST_CLIENT="${4:-$REPO_ROOT/build/x11_test_client}"
INPUT="${5:-$REPO_ROOT/build/x11_input}"
SETTINGS="${6:-$REPO_ROOT/kohiko-settings}"

. "$SCRIPT_DIR/live/common.sh"

live_require Xvfb
[ -x "$CHOOSER" ] && [ -x "$INPUT" ] || { echo "SKIP: kohiko-choose-window / x11_input not built."; exit 0; }

live_init 240

win_field()
{
    clients_lines | grep "\"instance\":\"$1\"" | head -1 \
        | sed -n "s/.*\"$2\":\([^,}]*\).*/\1/p" | tr -d '"'
}
inst_listed() { [ -n "$(win_field "$1" id)" ]; }
dialog_listed() { clients_lines | grep -q '"class":"Kohiko-choose-window"'; }
dialog_gone() { ! dialog_listed; }

open_pip()
{
    "$TEST_CLIENT" --title "Picture-in-Picture" --class zen --instance Toolkit --role PictureInPicture \
        --window-type _NET_WM_WINDOW_TYPE_UTILITY --set-pid --width 320 --height 180 \
        > "$WORKDIR/zen_pip.log" 2>&1 &
    LIVE_PIDS="$LIVE_PIDS $!"; PIP_PID=$!
    live_wait_for 60 inst_listed Toolkit || echo "  (note: PiP window never appeared)"
    sleep 0.2
}

open_browser()
{
    "$TEST_CLIENT" --title 'KohikoDE Session Launch - Zen Browser' --class zen --instance Navigator --role browser \
        --window-type _NET_WM_WINDOW_TYPE_NORMAL --set-pid --width 500 --height 400 \
        > "$WORKDIR/zen_browser.log" 2>&1 &
    LIVE_PIDS="$LIVE_PIDS $!"; BROWSER_PID=$!
    live_wait_for 60 inst_listed Navigator || echo "  (note: browser window never appeared)"
    sleep 0.2
}

close_windows()
{
    kill "${BROWSER_PID:-0}" "${PIP_PID:-0}" 2>/dev/null
    wait "${BROWSER_PID:-0}" "${PIP_PID:-0}" 2>/dev/null
    i=0
    while { inst_listed Navigator || inst_listed Toolkit; } && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
}

# SUPER+SHIFT+W: the choose_window binding, pressed for real.
open_chooser()
{
    "$INPUT" key w super shift
    live_wait_for 80 dialog_listed || echo "  (note: the dialog never appeared)"
    sleep 0.4
}

key() { for k in "$@"; do "$INPUT" key "$k"; sleep 0.08; done; }

# Enter/Escape/Delete: the dialog saves/cancels/resets and exits; the
# chooser then runs `kohikoctl reload` itself.
finish_dialog() { live_wait_for 80 dialog_gone || echo "  (note: the dialog never closed)"; sleep 0.6; }

rules() { grep '^windowrule=' "$CONF" | sed 's/^windowrule=//'; }
rule_count() { grep -c '^windowrule=' "$CONF"; }

# ---------------------------------------------------------------------
echo "== 1. Picture-in-Picture: create ITS rule through the dialog (a generic zen rule already exists) =="

live_write_config 'workspace.count=4
auto_start_programs=
bind=SUPER+SHIFT+W choose_window
windowrule=app:zen horizontal:left'
live_start_wm

open_browser
open_pip                      # newest, so it holds focus
check "$(win_field Toolkit role)" "PictureInPicture" "kohikoctl clients reports the PiP window's role as Kohiko read it from X11"
check "$(win_field Toolkit type)" "utility" "...and its window type"
check "$(win_field Navigator role)/$(win_field Navigator type)" "browser/normal" "the normal window: role browser, type normal"

open_chooser
check_true "the dialog opened" dialog_listed

# The dialog opens on 'Applies to' (this app has only a generic rule, so
# the choice starts on All windows). Right -> the window's own role;
# 4 -> workspace 4; Down, Right, Right -> Horizontal: Right;
# Down, Down, Right, Right -> Mode: Floating; Enter -> save.
key Right 4 Down Right Right Down Down Right Right Return
finish_dialog

check "$(rule_count)" "2" "two rules: the generic one and the new PiP one"
check "$(grep -c '^windowrule=app:zen horizontal:left$' "$CONF")" "1" "the generic zen rule is byte-for-byte untouched"
check "$(grep '^windowrule=app:zen role:PictureInPicture' "$CONF")" \
      "windowrule=app:zen role:PictureInPicture workspace:4 horizontal:right mode:floating" \
      "saved with its role: windowrule=app:zen role:PictureInPicture workspace:4 horizontal:right mode:floating"
check "$(grep -c '^bind=SUPER+SHIFT+W choose_window$' "$CONF")" "1" "the keybinding line survived the save"
close_windows

# ---------------------------------------------------------------------
echo "== 2. Normal window: its own rule, from the normal window - the PiP rule must not move =="

open_pip                      # goes to workspace 4 by its rule: hidden, not focused
open_browser                  # stays here, holds focus
check "$(win_field Toolkit workspace)/$(win_field Toolkit floating)" "4/true" "a NEW PiP window follows the saved PiP rule (workspace 4, floating)"

BEFORE_PIP=$(grep '^windowrule=app:zen role:PictureInPicture' "$CONF")
open_chooser
# This app now has a role-scoped rule, so the dialog opens on THIS
# window's own role (no kind was picked by the test): 3, Down, Right.
key 3 Down Right Return
finish_dialog

check "$(rule_count)" "3" "three rules"
check "$(grep '^windowrule=app:zen role:browser' "$CONF")" \
      "windowrule=app:zen role:browser workspace:3 horizontal:left" \
      "saved: windowrule=app:zen role:browser workspace:3 horizontal:left"
check "$(grep '^windowrule=app:zen role:PictureInPicture' "$CONF")" "$BEFORE_PIP" "the PiP rule is byte-for-byte unchanged by saving the browser rule"
check "$(grep -c '^windowrule=app:zen horizontal:left$' "$CONF")" "1" "the generic rule is unchanged too"
close_windows

# ---------------------------------------------------------------------
echo "== 3. New windows follow their own rule =="

open_pip
open_browser
check "$(win_field Navigator workspace)/$(win_field Navigator floating)" "3/false" "normal window: workspace 3, tiled"
check "$(win_field Toolkit workspace)/$(win_field Toolkit floating)" "4/true" "Picture-in-Picture: workspace 4, floating"
close_windows

# ---------------------------------------------------------------------
echo "== 4. Reopening loads the saved rule; saving unchanged preserves role/type; Escape changes nothing =="

"$KOHIKOCTL" dispatch workspace 4 >/dev/null; sleep 0.3
open_pip                      # on workspace 4 now: visible and focused
CONF_BEFORE=$(cksum < "$CONF")
open_chooser
check_true "the dialog opened for the PiP window" dialog_listed
key Return                    # no change at all
finish_dialog
check "$(cksum < "$CONF")" "$CONF_BEFORE" "reopen + Enter without changing anything: the config is byte-identical (the PiP rule kept its role and values)"

open_chooser
key Escape
finish_dialog
check "$(cksum < "$CONF")" "$CONF_BEFORE" "Escape: nothing written"
close_windows

# ---------------------------------------------------------------------
echo "== 5. Reset acts on one kind only =="

open_pip
open_chooser
key Delete
finish_dialog
check "$(grep -c 'role:PictureInPicture' "$CONF")" "0" "Reset on the PiP dialog deletes the PiP rule"
check "$(grep -c '^windowrule=app:zen role:browser workspace:3 horizontal:left$' "$CONF")" "1" "the browser rule remains"
check "$(grep -c '^windowrule=app:zen horizontal:left$' "$CONF")" "1" "the generic rule remains"
close_windows

# ---------------------------------------------------------------------
echo "== 6. Choosing 'All windows' for a PiP window edits the generic rule, not a role rule =="

open_pip
open_chooser
# Opens on role PictureInPicture (per-kind rules exist, none for PiP now).
# Left -> All windows; 2 -> workspace 2 for THAT kind; Enter.
key Left 2 Return
finish_dialog
check "$(grep '^windowrule=app:zen workspace:2' "$CONF")" "windowrule=app:zen workspace:2 horizontal:left" "the generic rule gained workspace 2"
check "$(grep -c 'role:PictureInPicture' "$CONF")" "0" "no PiP rule was created"
close_windows

# ---------------------------------------------------------------------
echo "== 7. kohiko-settings opens with role/type rules (smoke test; not driven) =="

live_quit_wm
live_write_config 'workspace.count=4
auto_start_programs=
windowrule=app:zen workspace:2
windowrule=app:zen role:browser workspace:3 horizontal:left mode:tiled
windowrule=app:zen role:PictureInPicture type:utility workspace:5 horizontal:right mode:floating'
if [ -x "$SETTINGS" ]; then
    CONF_BEFORE_SETTINGS=$(cksum < "$CONF")
    "$SETTINGS" > "$WORKDIR/settings.log" 2>&1 &
    SETTINGS_PID=$!
    LIVE_PIDS="$LIVE_PIDS $SETTINGS_PID"
    sleep 1.5
    check_true "kohiko-settings is still running 1.5 s after start with three Zen rules" kill -0 "$SETTINGS_PID"
    kill "$SETTINGS_PID" 2>/dev/null; wait "$SETTINGS_PID" 2>/dev/null
    check "$(cksum < "$CONF")" "$CONF_BEFORE_SETTINGS" "the config is byte-identical after merely opening and closing Settings"
else
    echo "  SKIP: $SETTINGS not built"
fi

live_finish
