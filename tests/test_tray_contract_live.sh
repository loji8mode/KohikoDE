#!/bin/sh
# Live test: the native tray interaction contract, end to end.
#
#   Left click   open/raise the main window; never a menu.
#   Right click  the compact quick-action menu.
#
# The REAL kohiko (its bar hosts the XEmbed tray) under Xvfb, the REAL
# TrayIconClient / TrayInteraction / PopupMenu / AppInstanceLock docked
# into it (tests/live/tray_contract_probe.cpp - only the artwork and the
# menu actions are fakes), real XTEST pointer and keyboard input, and a
# real second process standing in for the main control window
# (tests/live/lock_probe_app.cpp). Not a run against kohiko-audio /
# -network / -bluetooth themselves: those need D-Bus/PipeWire, which the
# sandbox lacks; they install the same TrayInteraction::Spec.
#
# Usage: test_tray_contract_live.sh [kohiko] [kohikoctl] [tray_contract_probe] [lock_probe_app] [x11_probe] [x11_input]

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT="$SCRIPT_DIR/.."
KOHIKO_BIN="${1:-$REPO_ROOT/kohiko}"
KOHIKOCTL="${2:-$REPO_ROOT/kohikoctl}"
TRAY_PROBE="${3:-$REPO_ROOT/build/tray_contract_probe}"
APP_PROBE="${4:-$REPO_ROOT/build/lock_probe_app}"
PROBE="${5:-$REPO_ROOT/build/x11_probe}"
INPUT="${6:-$REPO_ROOT/build/x11_input}"
TEST_CLIENT="$REPO_ROOT/build/x11_test_client"

. "$SCRIPT_DIR/live/common.sh"

live_require Xvfb
for b in "$TRAY_PROBE" "$APP_PROBE" "$PROBE" "$INPUT"; do
    [ -x "$b" ] || { echo "SKIP: $b not built."; exit 0; }
done

live_init 320

APPLOG="$WORKDIR/app.log"; TRAYLOG="$WORKDIR/tray.log"; : > "$APPLOG"; : > "$TRAYLOG"

cat > "$WORKDIR/fakeapp.sh" <<APP
#!/bin/sh
exec "$APP_PROBE" --id fakeapp --log "$APPLOG" --startup-ms 600 --stay-ms 60000
APP
chmod +x "$WORKDIR/fakeapp.sh"

live_write_config 'workspace.count=4
auto_start_programs='
live_start_wm
sleep 0.5

cnt()  { grep -c "^$1" "$2" 2>/dev/null || true; }
popups() { "$PROBE" tree | awk '/ state=mapped or=1 /{ w=0; h=0; for(i=1;i<=NF;i++){ if($i ~ /^w=/) w=substr($i,3)+0; if($i ~ /^h=/) h=substr($i,3)+0 } if (w>=150 && w<=700 && h>=40 && h<=400) print }'; }
npopups() { popups | wc -l | tr -d ' '; }
wait_for_popups() { i=0; while [ "$(npopups)" != "$1" ] && [ "$i" -lt 30 ]; do sleep 0.1; i=$((i+1)); done; }

# --- dock a native icon (red) and a third-party-style icon (blue) ---
"$TRAY_PROBE" --log "$TRAYLOG" --app "$WORKDIR/fakeapp.sh" --color CC2222 > "$WORKDIR/native.out" 2>&1 &
LIVE_PIDS="$LIVE_PIDS $!"
"$TRAY_PROBE" --log "$TRAYLOG" --plain --color 2222CC > "$WORKDIR/plain.out" 2>&1 &
LIVE_PIDS="$LIVE_PIDS $!"

i=0; while { ! grep -q CENTER "$WORKDIR/native.out" || ! grep -q CENTER "$WORKDIR/plain.out"; } && [ "$i" -lt 60 ]; do sleep 0.1; i=$((i+1)); done
sleep 1.0   # let the tray finish laying the two icons out; the LAST report is the settled position
NATIVE=$(sed -n 's/^CENTER=//p' "$WORKDIR/native.out" | tail -1); PLAIN=$(sed -n 's/^CENTER=//p' "$WORKDIR/plain.out" | tail -1)
NX=${NATIVE%,*}; NY=${NATIVE#*,}; PX=${PLAIN%,*}; PY=${PLAIN#*,}
echo "  (native icon at $NATIVE, third-party-style icon at $PLAIN)"
check_true "both icons docked into the bar, on screen" test -n "$NATIVE" -a -n "$PLAIN"
check "$(live_pixel "$NX" "$NY")" "cc2222" "the native icon is drawn"
check "$(live_pixel "$PX" "$PY")" "2222cc" "the third-party-style icon is drawn"
check "$(npopups)" "0" "no popup is open at the start"

echo "== 1. Left click: open / raise the main window, never a menu =="
"$INPUT" click "$NX" "$NY" 1
check "$(npopups)" "0" "a left click opens no menu"
sleep 0.15
"$INPUT" click "$NX" "$NY" 1          # a second click while the app is still starting (600 ms)
sleep 0.15
"$INPUT" click "$NX" "$NY" 1          # and a third
sleep 1.0
check "$(cnt STARTED "$APPLOG")" "1" "three rapid left clicks started the main window process exactly once"
check "$(cnt LOCKED "$APPLOG")" "1" "exactly one instance holds the window"
check "$(cnt LOST "$APPLOG")" "0" "no second copy ever had to give up"
"$INPUT" click "$NX" "$NY" 1          # now it is up: this one must raise it
sleep 0.4
check "$(cnt RAISED "$APPLOG")" "1" "a left click on the running window raises it"
check "$(cnt STARTED "$APPLOG")" "1" "...without starting anything"
check "$(npopups)" "0" "still no menu from left clicks"

echo "== 2. Right click: the quick-action menu =="
"$INPUT" click "$NX" "$NY" 3
wait_for_popups 1
check "$(npopups)" "1" "a right click opens exactly one menu"
POP=$(popups | head -1)
PXW=$(echo "$POP" | sed -n 's/.* x=\([-0-9]*\) .*/\1/p'); PYW=$(echo "$POP" | sed -n 's/.* y=\([-0-9]*\) .*/\1/p')
PWW=$(echo "$POP" | sed -n 's/.* w=\([0-9]*\) .*/\1/p')
check_true "the menu is entirely on the screen (x=$PXW w=$PWW of 1920)" test "${PXW:-9999}" -ge 0 -a $((${PXW:-9999} + ${PWW:-9999})) -le 1920
check "$(cnt STARTED "$APPLOG")" "1" "a right click starts nothing"

echo "== 3. Repeated clicks on the icon do not trigger the menu's first item =="
"$INPUT" click "$NX" "$NY" 3          # a quick second right click, same spot (lands on the menu's corner)
sleep 0.3
check "$(cnt ACTION_QUICK "$TRAYLOG")" "0" "the repeat click ran no action"
check_true "...and never left more than one menu (0 or 1: the click on the menu's corner may also dismiss it)" test "$(npopups)" -le 1

echo "== 4. Dismissal =="
"$INPUT" key Escape
sleep 0.2
[ "$(npopups)" != 0 ] || true
wait_for_popups 0
check "$(npopups)" "0" "Escape closes the menu (or it was already closed by the repeat click)"
check "$(live_pixel "$NX" "$NY")" "cc2222" "the native icon is still drawn correctly after the menu closed"
check "$(live_pixel "$PX" "$PY")" "2222cc" "the neighbouring icon is unaffected"

"$INPUT" click "$NX" "$NY" 3
wait_for_popups 1
"$INPUT" click 700 600 1              # click on empty desktop
wait_for_popups 0
check "$(npopups)" "0" "a click outside closes the menu"
check "$(cnt ACTION_ "$TRAYLOG")" "0" "...and runs nothing"
check "$(live_pixel "$NX" "$NY")" "cc2222" "the icon is still drawn correctly after an outside-click dismissal"

echo "== 5. Choosing an item =="
"$INPUT" click "$NX" "$NY" 3
wait_for_popups 1
POP=$(popups | head -1)
PXW=$(echo "$POP" | sed -n 's/.* x=\([-0-9]*\) .*/\1/p'); PYW=$(echo "$POP" | sed -n 's/.* y=\([-0-9]*\) .*/\1/p')
sleep 0.5                              # a deliberate click, not a repeat
"$INPUT" click $((PXW + 30)) $((PYW + 19)) 1      # "Quick action"
wait_for_popups 0
check "$(cnt ACTION_QUICK "$TRAYLOG")" "1" "the chosen action ran, once"
check "$(npopups)" "0" "and the menu is gone"
check "$(live_pixel "$NX" "$NY")" "cc2222" "the icon is drawn correctly after choosing an item"

echo "== 6. A slow action does not hold the desktop's input grabs =="
"$INPUT" click "$NX" "$NY" 3
wait_for_popups 1
POP=$(popups | head -1)
PXW=$(echo "$POP" | sed -n 's/.* x=\([-0-9]*\) .*/\1/p'); PYW=$(echo "$POP" | sed -n 's/.* y=\([-0-9]*\) .*/\1/p')
sleep 0.5
"$INPUT" click $((PXW + 30)) $((PYW + 49)) 1      # "Slow action" (blocks 700 ms)
i=0; while ! grep -q ACTION_SLOW_BEGIN "$TRAYLOG" && [ "$i" -lt 30 ]; do sleep 0.05; i=$((i+1)); done
# The action is now running (it blocks for 700 ms). Ask from ANOTHER client whether the pointer can be grabbed:
# a menu that were still up would still hold its grab.
check "$("$PROBE" grab-test)" "free" "while the action runs, another client can grab the pointer (the menu's grab is released)"
check "$(npopups)" "0" "...and the menu is already off the screen"
sleep 1.0
check "$(cnt ACTION_SLOW_END "$TRAYLOG")" "1" "the action completed"

echo "== 7. The contract's own last item opens the main window =="
"$INPUT" click "$NX" "$NY" 3
wait_for_popups 1
POP=$(popups | head -1)
PXW=$(echo "$POP" | sed -n 's/.* x=\([-0-9]*\) .*/\1/p'); PYW=$(echo "$POP" | sed -n 's/.* y=\([-0-9]*\) .*/\1/p')
sleep 0.5
BEFORE=$(cnt RAISED "$APPLOG")
"$INPUT" click $((PXW + 30)) $((PYW + 88)) 1      # "Open Fake App"
sleep 0.5
check "$(( $(cnt RAISED "$APPLOG") - BEFORE ))" "1" "'Open ...' raises the running window"
check "$(cnt STARTED "$APPLOG")" "1" "...and still nothing was started twice"

echo "== 8. A third-party tray icon is not affected =="
"$INPUT" click "$PX" "$PY" 1
sleep 0.2
"$INPUT" click "$PX" "$PY" 3
sleep 0.3
check "$(cnt PLAIN_LEFT "$TRAYLOG")" "1" "its left click reaches it, as always"
check "$(cnt PLAIN_RIGHT "$TRAYLOG")" "1" "its right click reaches it, as always"
check "$(npopups)" "0" "and neither opens a native popup"
check "$(cnt STARTED "$APPLOG")" "1" "nor starts the native app"
check "$(live_pixel "$PX" "$PY")" "2222cc" "it is still drawn correctly"
check "$(live_pixel "$NX" "$NY")" "cc2222" "...and so is the native icon"

live_finish
