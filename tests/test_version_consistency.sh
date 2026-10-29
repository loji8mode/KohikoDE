#!/bin/sh
# Version consistency. include/Version.h is the ONE authoritative version
# (it is what the startup log line "kohiko <version> starting" prints);
# everything that names the current version must agree with it, so a bump
# that forgets one of them - or a changelog entry written without a bump -
# fails here instead of shipping.
#
# Checked:
#   1. Version.h holds a well-formed MAJOR.MINOR.PATCH.
#   2. The newest `## Version X.Y.Z` heading in CHANGELOG.md is that version
#      (an `## Unreleased` section above it is allowed: it is work in
#      progress, not a release), appears exactly once, and is followed by
#      a `Release date: YYYY-MM-DD` line.
#   3. The README's "Current version" line names it.
#   4. No older version's heading is newer than the one in Version.h
#      (versions in the changelog are strictly descending).
#   5. If a built `kohiko` is available and Xvfb is installed: the running
#      window manager really logs "kohiko <version> starting".
#
# Historical references to older versions (changelog entries, "since
# 0.22.0" notes) are deliberately NOT checked: they are history.
#
# Usage: test_version_consistency.sh [path-to-kohiko] [path-to-kohikoctl]
set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT="$SCRIPT_DIR/.."
KOHIKO_BIN="${1:-$ROOT/kohiko}"
KOHIKOCTL="${2:-$ROOT/kohikoctl}"

vc_pass=0
vc_fail=0

vc_check()
{
    # vc_check <message> <expected> <actual>
    if [ "$2" = "$3" ]; then
        vc_pass=$((vc_pass + 1)); echo "  PASS: $1"
    else
        vc_fail=$((vc_fail + 1)); echo "  FAIL: $1 (expected [$2], got [$3])"
    fi
}

vc_true()
{
    msg=$1; shift
    if "$@" >/dev/null 2>&1; then
        vc_pass=$((vc_pass + 1)); echo "  PASS: $msg"
    else
        vc_fail=$((vc_fail + 1)); echo "  FAIL: $msg"
    fi
}

VERSION=$(sed -n 's/.*VERSION = "\([^"]*\)".*/\1/p' "$ROOT/include/Version.h")

echo "-- include/Version.h is the source of truth: $VERSION --"
vc_true "it is a well-formed MAJOR.MINOR.PATCH" sh -c "echo '$VERSION' | grep -Eq '^[0-9]+\\.[0-9]+\\.[0-9]+\$'"

echo "-- CHANGELOG.md --"
NEWEST=$(sed -n 's/^## Version \([0-9][0-9.]*\)[[:space:]]*$/\1/p' "$ROOT/CHANGELOG.md" | head -1)
vc_check "the newest released heading is the version in Version.h" "$VERSION" "$NEWEST"
vc_check "that version has exactly one heading" "1" "$(grep -c "^## Version $VERSION[[:space:]]*\$" "$ROOT/CHANGELOG.md")"

DATE_LINE=$(awk -v v="## Version $VERSION" '$0 == v {found = 1; next} found && NF {print; exit}' "$ROOT/CHANGELOG.md")
vc_true "its first line is 'Release date: YYYY-MM-DD'" sh -c "echo '$DATE_LINE' | grep -Eq '^Release date: [0-9]{4}-[0-9]{2}-[0-9]{2}\$'"

# Strictly descending: every heading is older than the one above it.
ORDER=$(sed -n 's/^## Version \([0-9][0-9.]*\)[[:space:]]*$/\1/p' "$ROOT/CHANGELOG.md")
SORTED=$(echo "$ORDER" | sort -t. -k1,1nr -k2,2nr -k3,3nr)
vc_check "release headings are in strictly descending version order" "$SORTED" "$ORDER"

echo "-- README.md --"
vc_true "the 'Current version' line names $VERSION" grep -q "^\*\*Current version: $VERSION\*\*" "$ROOT/README.md"

echo "-- the running window manager --"
# (in a subshell: live/common.sh has its own helpers and counters)
banner_logged()
{
    (
        . "$SCRIPT_DIR/live/common.sh"
        live_init 60
        live_write_config 'workspace.count=2
auto_start_programs='
        live_start_wm
        grep -q "kohiko $VERSION starting" "$WORKDIR/kohiko.log"
        rc=$?
        live_quit_wm
        live_cleanup
        exit $rc
    )
}

if [ -x "$KOHIKO_BIN" ] && [ -x "$KOHIKOCTL" ] && command -v Xvfb >/dev/null 2>&1; then
    vc_true "the running window manager logs 'kohiko $VERSION starting'" banner_logged
else
    echo "  SKIP: no built kohiko/kohikoctl or no Xvfb - the startup banner is not checked"
fi

echo
echo "$vc_pass passed, $vc_fail failed"
[ "$vc_fail" -eq 0 ]
