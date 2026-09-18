#!/bin/bash
# Companion to scripts/install-arch.sh:
#   scripts/uninstall-arch.sh
#
# Removes exactly what install-arch.sh put in place, in the same two
# contexts install-arch.sh itself uses it in:
#
#   - System-wide (needs root - this script calls sudo itself, the
#     same way install-arch.sh does): `sudo make uninstall`, which
#     removes every file the Makefile's own install/install-desktop-apps
#     targets create (binaries, .desktop entries, icons, the xsessions
#     entry, and the kohiko-bt-agent.service unit definition) - see the
#     Makefile's own uninstall/uninstall-desktop-apps targets, kept as
#     a literal file-for-file mirror of install/install-desktop-apps on
#     purpose.
#   - Per-user (no root - run as the actual logged-in user, in their
#     own systemd --user session, for the same reason install-arch.sh's
#     own Bluetooth section has to be): disables kohiko-bt-agent.service
#     for *this* user, and - only if it still looks like the exact file
#     install-arch.sh itself created, never a user's own customized one
#     - removes the ~/.config/autostart/blueman.desktop override,
#     restoring blueman-applet's normal autostart behavior now that
#     Kohiko itself is going away (see "clean up only what KohikoDE
#     owns, without breaking the user's unrelated Bluetooth
#     configuration").
#
# Deliberately does NOT touch ~/.config/kohiko/kohiko.conf or
# ~/.xinitrc - install-arch.sh never overwrites those if they already
# exist, and removing a user's own configuration on uninstall isn't
# something either script does anywhere else; this is exactly the same
# "leave anything that might be the user's own" philosophy, not an
# oversight.
#
# Only ever removes bluez/bluez-tools themselves if you ask Arch's own
# package manager to (`sudo pacman -Rns bluez-tools` etc.) - this
# script doesn't touch either package, since bluetoothd and bt-agent
# are general system Bluetooth infrastructure Kohiko happens to use,
# not something Kohiko owns.
set -e

if [ "$(id -u)" -eq 0 ]; then
    echo "uninstall-arch.sh: run this as your normal user, not root - it calls sudo itself where it needs to." >&2
    exit 1
fi

cd "$(dirname "$0")/.."

if [ ! -f Makefile ]; then
    echo "uninstall-arch.sh: no Makefile here - run this from inside the kohiko repo (scripts/uninstall-arch.sh)." >&2
    exit 1
fi

if command -v systemctl >/dev/null 2>&1; then
    echo "==> Disabling kohiko-bt-agent.service for $USER"
    disable_err=$(mktemp)
    if systemctl --user disable --now kohiko-bt-agent.service 2>"$disable_err"; then
        :
    else
        # Already disabled, never enabled, or no systemd --user session
        # in this shell - none of those are failures worth stopping an
        # uninstall over.
        sed 's/^/    /' "$disable_err"
    fi
    rm -f "$disable_err"
fi

BLUEMAN_OVERRIDE="$HOME/.config/autostart/blueman.desktop"
BLUEMAN_OVERRIDE_MARKER="# Installed by kohiko's install-arch.sh - see CHANGELOG.md's \"Blueman\" entries"

if [ -f "$BLUEMAN_OVERRIDE" ] && grep -qF "$BLUEMAN_OVERRIDE_MARKER" "$BLUEMAN_OVERRIDE" 2>/dev/null; then
    echo "==> Removing $BLUEMAN_OVERRIDE (restoring blueman-applet's normal autostart)"
    rm -f "$BLUEMAN_OVERRIDE"
elif [ -f "$BLUEMAN_OVERRIDE" ]; then
    echo "==> $BLUEMAN_OVERRIDE exists but wasn't created by install-arch.sh - leaving it alone"
fi

echo "==> Removing system-wide files (sudo make uninstall)"
sudo make uninstall

echo
echo "==> Done. bluez/bluez-tools themselves are untouched - remove them yourself"
echo "    (sudo pacman -Rns bluez-tools) only if nothing else on this system needs them."
echo "    ~/.config/kohiko/kohiko.conf and ~/.xinitrc are untouched too."
