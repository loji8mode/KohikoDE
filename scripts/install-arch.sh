#!/bin/bash
# One-shot Kohiko installer for Arch Linux:
#   scripts/install-arch.sh
#
# Installs build/runtime dependencies via pacman, builds with the same
# `make -j$(nproc)` used everywhere else in this project, installs the
# binaries system-wide (`sudo make install` - which is also what
# registers Kohiko as a selectable session for display managers like
# SDDM/GDM/LightDM, see the Makefile's own install: target), drops a
# default config in ~/.config/kohiko if one isn't there yet, and sets
# up a ~/.xinitrc fallback for plain startx - but only if you don't
# already have one, so it never clobbers an existing session setup.
set -e

if ! command -v pacman >/dev/null 2>&1; then
    echo "install-arch.sh: pacman not found - this script is for Arch Linux only." >&2
    exit 1
fi

if [ "$(id -u)" -eq 0 ]; then
    echo "install-arch.sh: run this as your normal user, not root - it calls sudo itself where it needs to." >&2
    exit 1
fi

cd "$(dirname "$0")/.."

if [ ! -f Makefile ]; then
    echo "install-arch.sh: no Makefile here - run this from inside the kohiko repo (scripts/install-arch.sh)." >&2
    exit 1
fi

echo "==> Installing dependencies (pacman)"
# base-devel      - gcc/g++, make, pkgconf
# libx11          - core Xlib (window manager, bar, launcher, notepad)
# libxrandr       - optional multi-monitor support, auto-detected by the Makefile
# imlib2          - icon loading and rendering (Launcher/Notepad) - icon-theme
#                   *lookup* is Kohiko's own freedesktop Icon Theme
#                   Specification implementation (see include/IconResolver.h),
#                   no toolkit dependency needed for that anymore
# libxft          - text rendering with automatic per-glyph font fallback
#                   across whatever's installed (see include/Font.h) - this
#                   is what lets Bar/Launcher/Notepad render languages the
#                   currently-configured `general.font=` doesn't itself
#                   cover (Cyrillic, CJK, ...), not just English
# ttf-dejavu      - a base font with solid Latin/Cyrillic/Greek coverage, so
#                   there's at least *something* for Xft to load out of the
#                   box - for CJK glyphs too, also install e.g. noto-fonts-cjk
#                   (a large package, so not pulled in automatically here)
# xorg-server     - to have an X server to run a window manager under at all
# libxss          - optional idle-timeout locking, and the X11 fallback half
#                   of display-sleep inhibition, auto-detected by the Makefile
# dbus            - optional; provides libdbus, the primary (D-Bus) half of
#                   display-sleep inhibition, auto-detected by the Makefile;
#                   also required (along with pipewire and librsvg just
#                   below) for kohiko-audio/kohiko-network/kohiko-bluetooth
#                   and their tray widgets - see Makefile/README.md's
#                   "Audio, network, and Bluetooth" section
# pipewire        - kohiko-audio's only audio backend (see
#                   include/PipeWireClient.h) - was already a real build
#                   requirement for that binary before this line was added;
#                   omitting it here just meant a fresh install-arch.sh run
#                   silently skipped building kohiko-audio/kohiko-audio-tray
#                   (and, since the Makefile gates all six of them on the
#                   same dbus+pipewire+librsvg check together, all five of
#                   the others too) rather than actually failing loudly
# librsvg         - SvgRenderer.cpp's direct .svg/.svgz rendering path (see
#                   include/SvgRenderer.h) for the same six binaries' icons
# flameshot       - default.conf's exec.screenshot, bound to Print - swap the
#                   package here too if you point exec.screenshot at something else
# bluez           - bluetoothd itself - kohiko-bluetooth/kohiko-bluetooth-tray
#                   talk to it directly over D-Bus (org.bluez), the same way
#                   kohiko-network talks to NetworkManager and kohiko-audio
#                   talks to PipeWire; not pulled in by pipewire/dbus above
# bluez-tools     - provides bt-agent, a genuine, minimal, headless BlueZ
#                   pairing agent with no GUI, no tray icon, and no
#                   notifications of its own. Needed because
#                   BluezClient::PairDevice() (src/BluezClient.cpp) calls
#                   BlueZ's Pair() directly and registers no org.bluez.Agent1
#                   of its own - something has to be, or pairing has nothing
#                   to ask for a PIN/passkey/confirmation. See this script's
#                   own Bluetooth section below for how bt-agent actually gets
#                   enabled (not here - this line only makes the binary
#                   available) and CHANGELOG.md's "Blueman" entries for the
#                   full investigation this replaces (blueman-applet's own
#                   agent plugin is hard-coupled to its tray icon, which is
#                   why running blueman-applet at all - not just its
#                   notifications - was the actual "old UI" bug)
sudo pacman -S --needed --noconfirm \
    base-devel \
    libx11 \
    libxrandr \
    imlib2 \
    libxft \
    ttf-dejavu \
    xorg-server \
    libxss \
    dbus \
    pipewire \
    librsvg \
    flameshot \
    bluez \
    bluez-tools

echo "==> Building (make -j\$(nproc))"
make -j"$(nproc)"

echo "==> Installing binaries (sudo make install)"
sudo make install
# This also installs kohiko-settings (Kohiko Settings' GUI) and its
# .desktop entry/icon, the kohiko-session wrapper, and the xsessions
# entry that points to it (see the Makefile's own install: target,
# and scripts/kohiko-session's own header comment for what the
# wrapper does) - registering Kohiko as a selectable session is no
# longer Arch-specific, so this script doesn't need to do it itself
# anymore. No extra dependency for kohiko-settings either: it's plain
# X11/Xft, both already installed above for kohiko itself.

CONFIG_DIR="$HOME/.config/kohiko"
CONFIG_FILE="$CONFIG_DIR/kohiko.conf"

if [ ! -f "$CONFIG_FILE" ]; then
    echo "==> Installing default config to $CONFIG_FILE"
    mkdir -p "$CONFIG_DIR"
    cp config/default.conf "$CONFIG_FILE"
else
    echo "==> $CONFIG_FILE already exists - leaving it alone"
fi

XINITRC="$HOME/.xinitrc"

if [ ! -f "$XINITRC" ]; then
    echo "==> No ~/.xinitrc - creating one that starts Kohiko (for plain startx, no display manager)"
    echo "exec /usr/local/bin/kohiko-session" > "$XINITRC"
elif grep -q "kohiko-session" "$XINITRC"; then
    echo "==> ~/.xinitrc already starts kohiko-session - leaving it alone"
elif grep -q "kohiko" "$XINITRC"; then
    # Specifically "exec kohiko" or "exec /usr/local/bin/kohiko" with no
    # -session - a real, easy-to-miss gap: it still starts Kohiko, but
    # silently loses kohiko-session's crash-restart supervision. Worth
    # a loud warning rather than the old blanket "already mentions
    # kohiko, leave it alone", which used to match this case too and
    # say nothing further.
    echo "==> ~/.xinitrc starts kohiko directly, not through kohiko-session - leaving it alone,"
    echo "    but this means a crash ends your whole X session instead of being restarted."
    echo "    Change its 'exec kohiko' (or 'exec /usr/local/bin/kohiko') line to:"
    echo "        exec kohiko-session"
    echo "    to get that back."
else
    echo "==> ~/.xinitrc already exists and doesn't mention kohiko - leaving it alone."
    echo "    If you use 'startx' (no display manager), add this line to it yourself:"
    echo "        exec /usr/local/bin/kohiko-session"
fi

# --- Bluetooth: suppress blueman-applet's competing UI, enable bt-agent ----
#
# kohiko-bluetooth is meant to be the only Bluetooth UI - see
# CHANGELOG.md's "Blueman" entries for the investigation that found
# blueman-applet running at all (not just its notifications) is what
# caused a competing tray icon/popups, and why the fix is suppressing
# blueman-applet's own autostart rather than trying to configure it
# into a state its own developers explicitly didn't support (its
# pairing-agent plugin is hard-coupled to its tray icon plugin).
#
# Both steps below are deliberately done here, as the actual logged-in
# user (this whole script refuses to run as root - see the top), never
# from `sudo make install` or any other root-context package-install
# step: creating a *user's own* ~/.config/autostart override is
# inherently per-user, and `systemctl --user` only ever makes sense
# inside a real user's own systemd session, which a privileged
# installer process may not have any connection to at all.

BLUEMAN_OVERRIDE_DIR="$HOME/.config/autostart"
BLUEMAN_OVERRIDE="$BLUEMAN_OVERRIDE_DIR/blueman.desktop"
BLUEMAN_OVERRIDE_MARKER="# Installed by kohiko's install-arch.sh - see CHANGELOG.md's \"Blueman\" entries"

if command -v blueman-applet >/dev/null 2>&1; then
    if [ ! -f "$BLUEMAN_OVERRIDE" ] || grep -qF "$BLUEMAN_OVERRIDE_MARKER" "$BLUEMAN_OVERRIDE" 2>/dev/null; then
        echo "==> Suppressing blueman-applet's autostart (kohiko-bluetooth is the intended Bluetooth UI)"
        mkdir -p "$BLUEMAN_OVERRIDE_DIR"
        # Standard XDG autostart override: a user-level entry with the
        # same filename as /etc/xdg/autostart/blueman.desktop takes
        # precedence for this user, and Hidden=true means "treat this
        # as though it doesn't exist" (see DesktopEntry.cpp's own
        # ShouldAutostart()) - never touches the system copy Blueman's
        # own package owns, so a pacman upgrade of blueman can't
        # restore anything here. blueman-manager and friends are
        # untouched and still run fine if launched manually.
        cat > "$BLUEMAN_OVERRIDE" <<EOF
$BLUEMAN_OVERRIDE_MARKER
[Desktop Entry]
Type=Application
Name=Bluetooth Manager
Exec=blueman-applet
Hidden=true
EOF
    else
        echo "==> $BLUEMAN_OVERRIDE already exists and wasn't created by this script - leaving it alone."
        echo "    To suppress blueman-applet's tray icon/notifications yourself, add Hidden=true to it."
    fi
else
    echo "==> blueman-applet not installed - nothing to suppress"
fi

if command -v bt-agent >/dev/null 2>&1 && command -v systemctl >/dev/null 2>&1; then
    echo "==> Enabling kohiko-bt-agent.service (headless BlueZ pairing agent) for $USER"
    enable_err=$(mktemp)
    if systemctl --user enable --now kohiko-bt-agent.service 2>"$enable_err"; then
        :
    else
        echo "    Could not enable kohiko-bt-agent.service in your current session:"
        sed 's/^/    /' "$enable_err"
        echo "    This is expected if you're running this script outside a real login"
        echo "    session (e.g. over a plain SSH connection with no systemd --user"
        echo "    instance). Run this once you're logged into a real session instead:"
        echo "        systemctl --user enable --now kohiko-bt-agent.service"
    fi
    rm -f "$enable_err"
else
    echo "==> bt-agent/systemctl not available - skipping Bluetooth pairing agent setup"
fi

echo
echo "==> Done. Pick \"Kohiko\" from your display manager's session list at login,"
echo "    or run 'startx' if you're using ~/.xinitrc directly."
echo "    Once you're in: Super+D opens the launcher, and \"Kohiko Settings\" is"
echo "    in there too - or run 'kohiko-settings' directly."
echo
echo "    No display manager at all, just a console login + 'startx' by hand?"
echo "    'sudo kohikoctl configure-console-autologin' can remove that manual step -"
echo "    see the README's \"Automatic login\" section."
