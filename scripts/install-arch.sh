#!/bin/bash
# One-shot Kohiko installer for Arch Linux:
#   scripts/install-arch.sh
#
# Installs build/runtime dependencies via pacman, builds with the same
# `make -j$(nproc)` used everywhere else in this project, installs the
# binaries system-wide (`sudo make install`), drops a default config
# in ~/.config/kohiko if one isn't there yet, and registers Kohiko as
# a selectable session (an xsessions .desktop entry for display
# managers like SDDM/GDM/LightDM, plus a ~/.xinitrc fallback for
# plain startx - but only if you don't already have one, so it never
# clobbers an existing session setup).
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
#                   display-sleep inhibition, auto-detected by the Makefile
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
    flameshot

echo "==> Building (make -j\$(nproc))"
make -j"$(nproc)"

echo "==> Installing binaries (sudo make install)"
sudo make install
# This also installs kohiko-settings (Kohiko Settings' GUI) and its
# .desktop entry/icon - see the Makefile's own install: target. No
# extra dependency for it: it's plain X11/Xft, both already installed
# above for kohiko itself.

CONFIG_DIR="$HOME/.config/kohiko"
CONFIG_FILE="$CONFIG_DIR/kohiko.conf"

if [ ! -f "$CONFIG_FILE" ]; then
    echo "==> Installing default config to $CONFIG_FILE"
    mkdir -p "$CONFIG_DIR"
    cp config/default.conf "$CONFIG_FILE"
else
    echo "==> $CONFIG_FILE already exists - leaving it alone"
fi

XSESSION_FILE="/usr/share/xsessions/kohiko.desktop"

echo "==> Registering the Kohiko session ($XSESSION_FILE)"
sudo tee "$XSESSION_FILE" >/dev/null <<'EOF'
[Desktop Entry]
Name=Kohiko
Comment=Minimalist X11 tiling window manager
Exec=/usr/local/bin/kohiko
Type=Application
EOF

XINITRC="$HOME/.xinitrc"

if [ ! -f "$XINITRC" ]; then
    echo "==> No ~/.xinitrc - creating one that starts Kohiko (for plain startx, no display manager)"
    echo "exec /usr/local/bin/kohiko" > "$XINITRC"
elif grep -q "kohiko" "$XINITRC"; then
    echo "==> ~/.xinitrc already starts kohiko - leaving it alone"
else
    echo "==> ~/.xinitrc already exists and doesn't mention kohiko - leaving it alone."
    echo "    If you use 'startx' (no display manager), add this line to it yourself:"
    echo "        exec /usr/local/bin/kohiko"
fi

echo
echo "==> Done. Pick \"Kohiko\" from your display manager's session list at login,"
echo "    or run 'startx' if you're using ~/.xinitrc directly."
echo "    Once you're in: Super+D opens the launcher, and \"Kohiko Settings\" is"
echo "    in there too - or run 'kohiko-settings' directly."
