#!/bin/sh
# Structural guard (CHANGELOG.md 0.22.0): Kohiko no longer learns window
# placement from anything the user does. This fails if any part of the
# removed adaptive-placement system - or a name that suggests one - comes
# back into the source tree, outside the deliberate retirement code that
# cleans up after it.
#
# The behavioural side (moving windows by hand any number of times never
# creates a rule or writes anything) is tested live, in
# tests/test_window_rules_live.sh section 2; this is the cheap static
# half: it catches a re-introduced learning path before it ever runs.
#
# Run from anywhere; skips if the source tree isn't present.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT="$SCRIPT_DIR/.."

[ -d "$ROOT/src" ] && [ -d "$ROOT/include" ] || { echo "SKIP: no source tree next to this script."; exit 0; }

pass=0
fail=0

expect_none()
{
    # expect_none <message> <grep -E pattern> <paths...>
    msg=$1; pattern=$2; shift 2
    # (this script contains the very patterns it searches for)
    found=$(grep -rIlE --exclude=test_no_adaptive_placement.sh "$pattern" "$@" 2>/dev/null)
    if [ -z "$found" ]; then
        pass=$((pass + 1)); echo "  PASS: $msg"
    else
        fail=$((fail + 1)); echo "  FAIL: $msg"; echo "$found" | sed 's/^/        /'
    fi
}

cd "$ROOT" || exit 1

echo "-- the removed system is really gone --"

for gone in include/PlacementHabitStore.h src/PlacementHabitStore.cpp tests/test_placementhabits.cpp; do
    if [ ! -e "$gone" ]; then
        pass=$((pass + 1)); echo "  PASS: $gone no longer exists"
    else
        fail=$((fail + 1)); echo "  FAIL: $gone is back"
    fi
done

expect_none "no learning/prediction API names anywhere in the code" \
    'PlacementHabit|PredictWorkspace|PredictPositions|RecordWorkspaceMove|RecordPositionMove|RecordAdaptive|AdaptiveWorkspaceHabit|ApplyAdaptivePlacementNudge|m_placementHabits' \
    src include tools tests

# (Application.cpp and ConfigMigration.* are the retirement code: they
# name the old data file/setting in order to delete it.)
expect_none "the code has no concept of a placement 'habit' at all (outside the retirement code)" \
    '[Hh]abit' \
    $(ls src/* include/* tools/* | grep -v -e 'src/Application.cpp' -e 'src/ConfigMigration.cpp' -e 'include/ConfigMigration.h')

expect_none "nothing still reads or writes the old data file or config key (outside the retirement code and its tests)" \
    'placement_habits|adaptive_placement' \
    $(ls src/* include/* tools/* config/* 2>/dev/null | grep -v -e 'src/ConfigMigration.cpp' -e 'include/ConfigMigration.h')

expect_none "the shipped default config has no adaptive_placement setting" \
    'adaptive_placement' config/default.conf

echo
echo "-- manual actions have no way to write a rule --"

# SwapWindows / MoveFocusedToWorkspace must not touch rule storage or
# any persistent store: rules are written only by WindowRuleStore, whose
# only callers are the chooser tool and the Settings editor.
expect_none "the window manager itself never writes window rules (WindowRuleStore is not used by it)" \
    'WindowRuleStore' \
    src/WindowManager.cpp include/WindowManager.h src/EventLoop.cpp src/MouseManager.cpp src/KeyboardManager.cpp

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
