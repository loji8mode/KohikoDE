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
    flameshot

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
elif grep -q "kohiko" "$XINITRC"; then
    echo "==> ~/.xinitrc already starts kohiko - leaving it alone"
else
    echo "==> ~/.xinitrc already exists and doesn't mention kohiko - leaving it alone."
    echo "    If you use 'startx' (no display manager), add this line to it yourself:"
    echo "        exec /usr/local/bin/kohiko-session"
fi

echo
echo "==> Done. Pick \"Kohiko\" from your display manager's session list at login,"
echo "    or run 'startx' if you're using ~/.xinitrc directly."
echo "    Once you're in: Super+D opens the launcher, and \"Kohiko Settings\" is"
echo "    in there too - or run 'kohiko-settings' directly."
