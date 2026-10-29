#!/bin/sh
# Live test: window-specific rules by WM_WINDOW_ROLE / window type, using
# the exact property combinations observed on a real Zen Browser
# (Arch Linux / Kohiko):
#
#   normal window    WM_CLASS "Navigator","zen"  role=browser
#                    _NET_WM_WINDOW_TYPE_NORMAL  no WM_TRANSIENT_FOR
#                    title "KohikoDE Session Launch - Zen Browser"
#   Picture-in-Picture WM_CLASS "Toolkit","zen"  role=PictureInPicture
#                    _NET_WM_WINDOW_TYPE_UTILITY no WM_TRANSIENT_FOR
#                    title "Picture-in-Picture"
#
# (Same PID and WM_CLIENT_LEADER for both in the real app - which is why
# neither is part of rule matching.) The REAL kohiko under Xvfb, real
# X11 clients carrying exactly those properties, the real
# kohiko-choose-window. This is NOT a run against a real Zen Browser.
#
# Usage: test_zen_window_rules_live.sh [kohiko] [kohikoctl] [kohiko-choose-window] [x11_test_client]
# Skipped (exit 0) when Xvfb or a binary isn't available.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT="$SCRIPT_DIR/.."
KOHIKO_BIN="${1:-$REPO_ROOT/kohiko}"
KOHIKOCTL="${2:-$REPO_ROOT/kohikoctl}"
CHOOSER="${3:-$REPO_ROOT/kohiko-choose-window}"
TEST_CLIENT="${4:-$REPO_ROOT/build/x11_test_client}"

. "$SCRIPT_DIR/live/common.sh"

live_require Xvfb
[ -x "$CHOOSER" ] || { echo "SKIP: $CHOOSER not built."; exit 0; }

live_init 200

BROWSER_TITLE='KohikoDE Session Launch - Zen Browser'

# A window by WM_CLASS *instance* (Navigator / Toolkit): both are class zen.
win_field()
{
    clients_lines | grep "\"instance\":\"$1\"" | head -1 \
        | sed -n "s/.*\"$2\":\([^,}]*\).*/\1/p" | tr -d '"'
}
inst_listed() { [ -n "$(win_field "$1" id)" ]; }

open_zen_browser()
{
    "$TEST_CLIENT" --title "$BROWSER_TITLE" --class zen --instance Navigator --role browser \
        --window-type _NET_WM_WINDOW_TYPE_NORMAL --set-pid --width 500 --height 400 \
        > "$WORKDIR/zen_browser.log" 2>&1 &
    LIVE_PIDS="$LIVE_PIDS $!"; BROWSER_PID=$!
    live_wait_for 60 inst_listed Navigator || echo "  (note: browser window never appeared)"
    sleep 0.15
}

open_zen_pip()
{
    "$TEST_CLIENT" --title "Picture-in-Picture" --class zen --instance Toolkit --role PictureInPicture \
        --window-type _NET_WM_WINDOW_TYPE_UTILITY --set-pid --width 320 --height 180 \
        > "$WORKDIR/zen_pip.log" 2>&1 &
    LIVE_PIDS="$LIVE_PIDS $!"; PIP_PID=$!
    live_wait_for 60 inst_listed Toolkit || echo "  (note: PiP window never appeared)"
    sleep 0.15
}

close_both()
{
    kill "$BROWSER_PID" "$PIP_PID" 2>/dev/null
    wait "$BROWSER_PID" "$PIP_PID" 2>/dev/null
    i=0
    while { inst_listed Navigator || inst_listed Toolkit; } && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
}

reload_and_wait() { "$KOHIKOCTL" reload >/dev/null; sleep 0.3; }

# ---------------------------------------------------------------------
echo "== 1. Independent rules for the two window kinds (generic rule also present) =="

live_write_config 'workspace.count=4
auto_start_programs=
windowrule=app:zen workspace:2
windowrule=app:zen role:browser workspace:3 mode:tiled horizontal:left
windowrule=app:zen role:PictureInPicture workspace:1 mode:floating horizontal:right vertical:bottom'
live_start_wm

open_zen_browser
open_zen_pip

check "$(win_field Navigator workspace)/$(win_field Navigator floating)" "3/false" \
    "normal window (role browser): its own rule - workspace 3, tiled (not the generic workspace 2)"
check "$(win_field Toolkit workspace)/$(win_field Toolkit floating)" "1/true" \
    "Picture-in-Picture (role PictureInPicture): its own rule - workspace 1, floating"
check "$(win_field Navigator workspace)" "3" "the PiP rule did not leak onto the browser window"
close_both

echo "== 2. Rule order in the file does not matter - the specific rule wins over a LATER generic one =="

live_write_config 'workspace.count=4
auto_start_programs=
windowrule=app:zen role:PictureInPicture workspace:1 mode:floating
windowrule=app:zen role:browser workspace:3
windowrule=app:zen workspace:2'
reload_and_wait
open_zen_browser; open_zen_pip
check "$(win_field Navigator workspace)" "3" "browser: specific rule beats the generic rule listed after it"
check "$(win_field Toolkit workspace)/$(win_field Toolkit floating)" "1/true" "PiP: specific rule beats the generic rule listed after it"
close_both

echo "== 3. A rule for only ONE kind of window does not become an application-wide rule =="

live_write_config 'workspace.count=4
auto_start_programs=
windowrule=app:zen role:PictureInPicture workspace:4 mode:floating'
reload_and_wait
open_zen_browser; open_zen_pip
check "$(win_field Toolkit workspace)" "4" "PiP matches its rule"
check "$(win_field Navigator workspace)/$(win_field Navigator floating)" "1/false" \
    "the browser gets NOTHING from the PiP rule (no generic rule exists): current workspace, tiled"
close_both

echo "== 4. Only the generic rule: both windows follow it (existing app-wide rules are unchanged) =="

live_write_config 'workspace.count=4
auto_start_programs=
windowrule=app:zen workspace:2'
reload_and_wait
open_zen_browser; open_zen_pip
check "$(win_field Navigator workspace)" "2" "browser follows the application-wide rule"
check "$(win_field Toolkit workspace)" "2" "PiP follows the application-wide rule"
close_both

echo "== 5. Missing or mismatched role / type =="

live_write_config 'workspace.count=4
auto_start_programs=
windowrule=app:zen role:browser workspace:3
windowrule=app:zen role:browser type:normal workspace:4
windowrule=app:nr role:browser workspace:2'
reload_and_wait

# 5a. no WM_WINDOW_ROLE at all
"$TEST_CLIENT" --title "no role" --class zen --instance NoRole --window-type _NET_WM_WINDOW_TYPE_NORMAL \
    > "$WORKDIR/c5a.log" 2>&1 & LIVE_PIDS="$LIVE_PIDS $!"; P5A=$!
live_wait_for 60 inst_listed NoRole; sleep 0.15
check "$(win_field NoRole workspace)" "1" "a zen window with no WM_WINDOW_ROLE matches neither role:browser rule"
kill "$P5A" 2>/dev/null; wait "$P5A" 2>/dev/null

# 5b. role matches but it is a dialog, and one rule also requires type:normal
"$TEST_CLIENT" --title "browser dialog" --class zen --instance Dlg --role browser --window-type _NET_WM_WINDOW_TYPE_DIALOG \
    > "$WORKDIR/c5b.log" 2>&1 & LIVE_PIDS="$LIVE_PIDS $!"; P5B=$!
live_wait_for 60 inst_listed Dlg; sleep 0.15
check "$(win_field Dlg workspace)" "3" "role right, type dialog: only the role-only rule applies (workspace 3), NOT the role+type:normal rule (4)"
kill "$P5B" 2>/dev/null; wait "$P5B" 2>/dev/null

# 5c. a different role that merely CONTAINS 'browser'
"$TEST_CLIENT" --title "popup" --class zen --instance Popup --role browser-popup --window-type _NET_WM_WINDOW_TYPE_NORMAL \
    > "$WORKDIR/c5c.log" 2>&1 & LIVE_PIDS="$LIVE_PIDS $!"; P5C=$!
live_wait_for 60 inst_listed Popup; sleep 0.15
check "$(win_field Popup workspace)" "1" "role browser-popup is not role:browser (exact match, no substring)"
kill "$P5C" 2>/dev/null; wait "$P5C" 2>/dev/null

# 5d. no declared type at all vs a type: rule
live_write_config 'workspace.count=4
auto_start_programs=
windowrule=app:zen type:normal workspace:3'
reload_and_wait
"$TEST_CLIENT" --title "no type" --class zen --instance NoType --role browser \
    > "$WORKDIR/c5d.log" 2>&1 & LIVE_PIDS="$LIVE_PIDS $!"; P5D=$!
live_wait_for 60 inst_listed NoType; sleep 0.15
check "$(win_field NoType workspace)" "1" "a window that declares no _NET_WM_WINDOW_TYPE does not match type:normal (never inferred)"
kill "$P5D" 2>/dev/null; wait "$P5D" 2>/dev/null

echo "== 6. Position and mode are independent per window kind =="

live_write_config 'workspace.count=4
auto_start_programs=
windowrule=app:zen role:browser horizontal:left vertical:top
windowrule=app:zen role:PictureInPicture mode:floating'
reload_and_wait
open_zen_browser; open_zen_pip
check "$(win_field Navigator floating)" "false" "browser: tiled (no mode rule for it)"
check "$(win_field Toolkit floating)" "true" "PiP: floating by its own mode rule"
close_both

echo "== 7. Moving windows by hand never creates or changes a rule =="

live_write_config 'workspace.count=4
auto_start_programs=
windowrule=app:zen role:PictureInPicture workspace:1 mode:floating
windowrule=app:zen role:browser workspace:1 mode:tiled'
reload_and_wait
BEFORE=$(cksum < "$CONF")
open_zen_browser; open_zen_pip
"$KOHIKOCTL" dispatch toggle_floating >/dev/null; sleep 0.2
"$KOHIKOCTL" dispatch toggle_floating >/dev/null; sleep 0.2
"$KOHIKOCTL" dispatch workspace 2 >/dev/null; "$KOHIKOCTL" dispatch workspace 1 >/dev/null; sleep 0.2
check "$(cksum < "$CONF")" "$BEFORE" "the config file is byte-identical after opening, floating/tiling and switching"
check "$(grep -c '^windowrule=' "$CONF")" "2" "still exactly the two written rules"
close_both

echo "== 8. kohiko-choose-window: creating and editing the scoped rules =="

live_write_config 'workspace.count=4
auto_start_programs=
bind=SUPER+Q close
windowrule=app:zen workspace:2
windowrule=app:other workspace:3'
reload_and_wait

"$CHOOSER" --config "$CONF" --no-reload --app zen --role PictureInPicture --type utility --scope role \
    --set "workspace:1 mode:floating horizontal:right vertical:bottom"
check "$(grep -c '^windowrule=app:zen role:PictureInPicture ' "$CONF")" "1" "scope role writes a role-scoped rule"
check "$(grep '^windowrule=app:zen role:PictureInPicture' "$CONF")" \
      "windowrule=app:zen role:PictureInPicture workspace:1 horizontal:right vertical:bottom mode:floating" "...with exactly that role and properties"
check "$(grep -c '^windowrule=app:zen workspace:2$' "$CONF")" "1" "the plain application rule is untouched"
check "$(grep -c '^bind=SUPER+Q close$' "$CONF")" "1" "the rest of the config is untouched"

"$CHOOSER" --config "$CONF" --no-reload --app zen --role browser --type normal --scope role+type --set "workspace:3 mode:tiled"
check "$(grep -c '^windowrule=app:zen role:browser type:normal workspace:3 mode:tiled$' "$CONF")" "1" "scope role+type writes role AND type"

check "$("$CHOOSER" --config "$CONF" --app zen --role PictureInPicture --scope role --show)" \
      "app:zen role:PictureInPicture workspace:1 horizontal:right vertical:bottom mode:floating" "--show reads back the scoped rule"
check "$("$CHOOSER" --config "$CONF" --app zen --show)" "app:zen workspace:2" "--show for the whole application is still the plain rule"

"$CHOOSER" --config "$CONF" --no-reload --app zen --role PictureInPicture --scope role --set "workspace:4 mode:floating"
check "$(grep -c '^windowrule=app:zen role:PictureInPicture ' "$CONF")" "1" "editing replaces the scoped rule - no duplicate"

reload_and_wait
open_zen_browser; open_zen_pip
check "$(win_field Navigator workspace)/$(win_field Navigator floating)" "3/false" "after the edits the browser follows its rule"
check "$(win_field Toolkit workspace)/$(win_field Toolkit floating)" "4/true" "...and the PiP window its (edited) rule"
close_both

"$CHOOSER" --config "$CONF" --no-reload --app zen --role PictureInPicture --scope role --delete
check "$(grep -c 'role:PictureInPicture' "$CONF")" "0" "deleting the PiP scope removes only that rule"
check "$(grep -c '^windowrule=' "$CONF")" "3" "the plain rule, the browser rule and the other application's rule remain"

check "$("$CHOOSER" --config "$CONF" --no-reload --app zen --scope role --set "workspace:1" 2>&1 >/dev/null | head -1 | cut -c1-60)" \
      "kohiko-choose-window: --scope role needs --role (this window" \
      "a scope the window cannot satisfy is refused, not widened into an application-wide rule"
check "$("$CHOOSER" --config "$CONF" --no-reload --app zen --role browser --scope role --set "role:other workspace:1" 2>&1 >/dev/null | head -1 | cut -c1-49)" \
      "kohiko-choose-window: --set may not contain role:" "--set cannot smuggle in a different scope"

echo "== 9. The window manager hands the focused window's role/type to the dialog =="

mkdir -p "$WORKDIR/stub"
cat > "$WORKDIR/stub/kohiko-choose-window" <<STUB
#!/bin/sh
echo "\$@" >> "$WORKDIR/chooser_args.log"
STUB
chmod +x "$WORKDIR/stub/kohiko-choose-window"
# Put the stub in front for the *window manager*, which spawns the dialog by name.
live_quit_wm
live_write_config 'workspace.count=4
auto_start_programs='
PATH="$WORKDIR/stub:$PATH" live_start_wm "PATH=$WORKDIR/stub:$PATH"
open_zen_pip      # floating + newest: it holds focus
"$KOHIKOCTL" dispatch choose_window role+type >/dev/null; sleep 0.3
ARGS=$(cat "$WORKDIR/chooser_args.log" 2>/dev/null)
check_true "choose_window role+type passes --role PictureInPicture" sh -c "echo \"\$1\" | grep -q -- '--role PictureInPicture'" _ "$ARGS"
check_true "...--type utility" sh -c "echo \"\$1\" | grep -q -- '--type utility'" _ "$ARGS"
check_true "...and --scope role+type" sh -c "echo \"\$1\" | grep -q -- '--scope role+type'" _ "$ARGS"
: > "$WORKDIR/chooser_args.log"
"$KOHIKOCTL" dispatch choose_window >/dev/null; sleep 0.3
ARGS=$(cat "$WORKDIR/chooser_args.log" 2>/dev/null)
check_true "plain choose_window keeps the whole-application default (no --scope)" sh -c "! echo \"\$1\" | grep -q -- '--scope'" _ "$ARGS"
check_true "...and still names the application" sh -c "echo \"\$1\" | grep -q -- '--app zen'" _ "$ARGS"
close_both

echo "== 10. Kohiko's view of a window follows its properties, and the dialog is handed the CURRENT role =="

# A client that changes WM_WINDOW_ROLE after it was mapped (sets its
# identity late, or changes it). What Kohiko read at map time would
# otherwise be stale forever - and the dialog would write a rule for a
# window kind this window no longer is.
"$TEST_CLIENT" --title "late role" --class zen --instance Late --role browser --change-role PictureInPicture \
    --change-after-ms 700 --window-type _NET_WM_WINDOW_TYPE_NORMAL --set-pid \
    > "$WORKDIR/c10.log" 2>&1 & LIVE_PIDS="$LIVE_PIDS $!"; P10=$!
live_wait_for 60 inst_listed Late; sleep 0.15
check "$(win_field Late role)" "browser" "at map time the window declared role browser"
sleep 1.3
check "$(win_field Late role)" "PictureInPicture" "after the client replaces WM_WINDOW_ROLE, kohikoctl clients reports the new role (PropertyNotify)"
: > "$WORKDIR/chooser_args.log"
"$KOHIKOCTL" dispatch choose_window >/dev/null; sleep 0.3
ARGS=$(cat "$WORKDIR/chooser_args.log" 2>/dev/null)
check_true "the dialog is handed the window's CURRENT role (--role PictureInPicture), not the one it had when first managed" \
    sh -c "echo \"\$1\" | grep -q -- '--role PictureInPicture'" _ "$ARGS"
check_true "...and not the stale one" sh -c "! echo \"\$1\" | grep -q -- '--role browser'" _ "$ARGS"
kill "$P10" 2>/dev/null; wait "$P10" 2>/dev/null

echo "== 11. The log says which rules matched a new window, and which same-application rules did not =="

live_write_config 'workspace.count=4
auto_start_programs=
windowrule=app:zen workspace:1
windowrule=app:zen role:browser workspace:1
windowrule=app:zen role:PictureInPicture workspace:1 mode:floating'
reload_and_wait
: > "$WORKDIR/kohiko.log"
open_zen_pip
PIPLINE=$(grep 'role "PictureInPicture", type "utility")' "$WORKDIR/kohiko.log" | head -2)
check_true "log: the PiP window matched 2 rules, generic first" \
    sh -c "echo \"\$1\" | grep -q 'matched 2 window rule(s), least specific first: app:zen workspace:1; app:zen role:PictureInPicture workspace:1 mode:floating'" _ "$PIPLINE"
check_true "log: ...and says the role:browser rule did not match it" \
    sh -c "echo \"\$1\" | grep -q 'did not match these rules for the same application: app:zen role:browser workspace:1'" _ "$PIPLINE"
close_both

live_finish
