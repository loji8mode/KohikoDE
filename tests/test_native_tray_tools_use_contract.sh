#!/bin/sh
# Guard: every Kohiko native tray tool must get its click behaviour from the
# ONE shared contract (include/NativeTrayContract.h: left = open/raise the
# main window, right = the compact menu) and never register private click
# handlers. 0.21.5 had the audio tray on left=window/right=menu while the
# network and Bluetooth trays had the two SWAPPED; this keeps them from
# drifting apart again. Pure text check: no build, no X display.
#
# A new tray tool (any tools/kohiko-*-tray.cpp) is picked up automatically.

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TOOLS="$SCRIPT_DIR/../tools"
pass=0; fail=0

for tool in "$TOOLS"/kohiko-*-tray.cpp; do
    name=$(basename "$tool")

    if grep -q 'InstallNativeTrayContract' "$tool"; then
        pass=$((pass + 1)); echo "  PASS: $name installs the shared native-tray contract"
    else
        fail=$((fail + 1)); echo "  FAIL: $name does not call InstallNativeTrayContract()"
    fi

    if grep -qE 'SetLeftClickHandler|SetRightClickHandler' "$tool"; then
        fail=$((fail + 1)); echo "  FAIL: $name registers its own click handlers instead of using the contract"
    else
        pass=$((pass + 1)); echo "  PASS: $name registers no private click handlers"
    fi
done

[ "$pass" -gt 0 ] || { echo "  FAIL: found no tray tools to check"; exit 1; }
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
