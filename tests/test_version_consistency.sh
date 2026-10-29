#!/bin/sh
# Guards against version drift. include/Version.h is the single source
# of truth for the version string Kohiko reports at startup
# ("kohiko X.Y.Z starting"); it sat at 0.21.1 through the 0.21.2,
# 0.21.3 and 0.21.4 releases because nothing checked it. This fails if
# it ever disagrees with the newest numbered "## Version X.Y.Z" heading
# in CHANGELOG.md (an "## Unreleased" section above it is ignored).
#
# Pure text check: no build, no X display.

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT="$SCRIPT_DIR/.."

header_version=$(sed -n 's/.*VERSION = "\([0-9][0-9.]*\)".*/\1/p' "$REPO_ROOT/include/Version.h" | head -1)
changelog_version=$(sed -n 's/^## Version \([0-9][0-9.]*\).*/\1/p' "$REPO_ROOT/CHANGELOG.md" | head -1)

if [ -z "$header_version" ] || [ -z "$changelog_version" ]; then
    echo "FAIL: could not read a version (Version.h: [$header_version], CHANGELOG.md: [$changelog_version])"
    exit 1
fi

if [ "$header_version" = "$changelog_version" ]; then
    echo "  PASS: include/Version.h ($header_version) matches the newest CHANGELOG.md release ($changelog_version)"
    exit 0
fi

echo "  FAIL: include/Version.h says $header_version but the newest CHANGELOG.md release is $changelog_version"
exit 1
