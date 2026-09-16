# Plain-make alternative to CMakeLists.txt - no cmake required, just
# g++, libX11, and libXft/fontconfig (used for the bar/launcher/
# notepad's text rendering - see include/Font.h), plus optionally
# libXrandr for multi-monitor support (auto-detected below).

CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra

INCLUDES := -Iinclude
INCLUDES += $(shell pkg-config --cflags xft fontconfig)

LIBS := -lX11 -lImlib2 -lpam
LIBS += $(shell pkg-config --libs xft fontconfig)

# SettingsWindow.cpp/ConfigWriter.cpp exist only for kohiko-settings
# (its own separate binary/process - see include/SettingsWindow.h's
# own header comment for why) - keeping them out of $(OBJ) is what
# keeps the `kohiko` binary itself free of GUI code it never runs,
# per "keep the WM lightweight". The desktop-integration apps'
# sources (kohiko-audio/network/bluetooth and their shared
# infrastructure) get the exact same treatment for the exact same
# reason - see DESKTOP_SHARED_SRC/DESKTOP_APP_ONLY_SRC below.
DESKTOP_SHARED_SRC := src/DBusValue.cpp src/DBusClient.cpp src/AppInstanceLock.cpp \
    src/AppConfigStore.cpp src/NotificationClient.cpp src/UiWindow.cpp src/UiWidget.cpp \
    src/UiListRow.cpp src/UiScrollView.cpp src/UiSidebar.cpp src/UiPopupMenu.cpp \
    src/UiIconCache.cpp src/TrayIconClient.cpp
DESKTOP_APP_ONLY_SRC := src/PipeWireClient.cpp src/NetworkManagerClient.cpp src/BluezClient.cpp \
    src/AudioWindow.cpp src/NetworkWindow.cpp src/BluetoothWindow.cpp

GUI_ONLY_SRC := src/SettingsWindow.cpp src/ConfigWriter.cpp $(DESKTOP_SHARED_SRC) $(DESKTOP_APP_ONLY_SRC)

SRC := $(filter-out $(GUI_ONLY_SRC),$(wildcard src/*.cpp))
OBJ := $(patsubst src/%.cpp,build/%.o,$(SRC))

# Everything kohiko-settings actually needs: its own GUI code plus
# just the handful of shared, low-level pieces it has in common with
# `kohiko` (config parsing/schema, Xft text rendering, string
# helpers) - deliberately NOT the whole $(SRC) list above, which is
# full of WM-only code (BSP layout, EWMH, XRandr monitor handling,
# ...) kohiko-settings has no use for.
SETTINGS_SRC := src/SettingsWindow.cpp src/ConfigWriter.cpp src/ConfigSchema.cpp src/Config.cpp src/Utils.cpp src/Font.cpp
SETTINGS_OBJ := $(patsubst src/%.cpp,build/%.o,$(SETTINGS_SRC))

ifeq ($(shell pkg-config --exists xrandr && echo yes),yes)
    CXXFLAGS += -DKOHIKO_HAVE_XRANDR
    LIBS     += $(shell pkg-config --libs xrandr)
endif

# XScreenSaver extension - idle-timeout locking and display-sleep
# inhibition (see include/IdleWatcher.h). Same auto-detected,
# gracefully-optional treatment as XRandr above.
ifeq ($(shell pkg-config --exists xscrnsaver && echo yes),yes)
    CXXFLAGS += -DKOHIKO_HAVE_XSS
    LIBS     += $(shell pkg-config --libs xscrnsaver)
endif

# libdbus - the primary mechanism behind display-sleep inhibition (see
# include/ScreenSaverInhibitor.h). NOT required: Kohiko still works
# fine without it (falls back to the X11 fullscreen-window heuristic
# alone in WindowManager::CheckSleepInhibition()).
ifeq ($(shell pkg-config --exists dbus-1 && echo yes),yes)
    CXXFLAGS += -DKOHIKO_HAVE_DBUS
    CXXFLAGS += $(shell pkg-config --cflags dbus-1)
    LIBS     += $(shell pkg-config --libs dbus-1)
endif

# --- kohiko-audio / kohiko-network / kohiko-bluetooth + tray widgets -------------
#
# Gated on dbus-1 + libpipewire-0.3 both actually being present -
# unlike kohiko's own *optional* dbus-1 usage just above (which only
# loses a feature without it), DBusClient.cpp and PipeWireClient.cpp
# both `#include` their real headers unconditionally, so these six
# binaries have no fallback at compile time. See CMakeLists.txt's
# identical KOHIKO_DESKTOP_APPS_AVAILABLE check for the same reasoning.
KOHIKO_HAVE_DBUS_PKG := $(shell pkg-config --exists dbus-1 && echo yes)

ifeq ($(shell pkg-config --exists libpipewire-0.3 && echo yes),yes)
    KOHIKO_HAVE_PIPEWIRE := yes
    CXXFLAGS     += $(shell pkg-config --cflags libpipewire-0.3)
    PIPEWIRE_LIBS := $(shell pkg-config --libs libpipewire-0.3)
endif

DESKTOP_SHARED_OBJ := $(patsubst src/%.cpp,build/%.o,$(DESKTOP_SHARED_SRC))

# The handful of already-shared low-level pieces kohiko itself also
# needs (config parsing, icon resolution, XDG paths, string helpers,
# Xft text rendering) - reused as-is via the same generic
# `build/%.o: src/%.cpp` pattern rule below rather than recompiled a
# second time under a different name.
DESKTOP_COMMON_OBJ := build/Font.o build/Config.o build/IconResolver.o build/Xdg.o build/Utils.o build/IniFile.o

.PHONY: all clean test install

all: kohiko kohikoctl kohiko-settings

ifeq ($(KOHIKO_HAVE_PIPEWIRE)-$(KOHIKO_HAVE_DBUS_PKG),yes-yes)
all: kohiko-audio kohiko-network kohiko-bluetooth kohiko-audio-tray kohiko-network-tray kohiko-bluetooth-tray
else
$(info Skipping kohiko-audio/kohiko-network/kohiko-bluetooth and their tray widgets - install libdbus-1-dev and libpipewire-0.3-dev to enable them.)
endif

kohiko: $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) -o $@ $(LIBS)

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

build:
	mkdir -p build

kohikoctl: tools/kohikoctl.cpp src/IpcPath.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

# A completely ordinary X11 client application (not part of the WM
# process at all - see include/SettingsWindow.h) - only needs
# X11/Xft/fontconfig, all of which `kohiko` itself already requires,
# so this adds no new dependency to the project as a whole.
kohiko-settings: $(SETTINGS_OBJ) build/kohiko-settings-main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ -lX11 $(shell pkg-config --libs xft fontconfig)

build/kohiko-settings-main.o: tools/kohiko-settings.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# --- kohiko-audio -----------------------------------------------------------------

kohiko-audio: $(DESKTOP_SHARED_OBJ) $(DESKTOP_COMMON_OBJ) build/PipeWireClient.o build/AudioWindow.o build/kohiko-audio-main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS) $(PIPEWIRE_LIBS)

build/kohiko-audio-main.o: tools/kohiko-audio.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

kohiko-audio-tray: $(DESKTOP_SHARED_OBJ) $(DESKTOP_COMMON_OBJ) build/PipeWireClient.o build/kohiko-audio-tray-main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS) $(PIPEWIRE_LIBS)

build/kohiko-audio-tray-main.o: tools/kohiko-audio-tray.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# --- kohiko-network ---------------------------------------------------------------

kohiko-network: $(DESKTOP_SHARED_OBJ) $(DESKTOP_COMMON_OBJ) build/NetworkManagerClient.o build/NetworkWindow.o build/kohiko-network-main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS)

build/kohiko-network-main.o: tools/kohiko-network.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

kohiko-network-tray: $(DESKTOP_SHARED_OBJ) $(DESKTOP_COMMON_OBJ) build/NetworkManagerClient.o build/kohiko-network-tray-main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS)

build/kohiko-network-tray-main.o: tools/kohiko-network-tray.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# --- kohiko-bluetooth --------------------------------------------------------------

kohiko-bluetooth: $(DESKTOP_SHARED_OBJ) $(DESKTOP_COMMON_OBJ) build/BluezClient.o build/BluetoothWindow.o build/kohiko-bluetooth-main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS)

build/kohiko-bluetooth-main.o: tools/kohiko-bluetooth.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

kohiko-bluetooth-tray: $(DESKTOP_SHARED_OBJ) $(DESKTOP_COMMON_OBJ) build/BluezClient.o build/kohiko-bluetooth-tray-main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS)

build/kohiko-bluetooth-tray-main.o: tools/kohiko-bluetooth-tray.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

test: build/test_bsptree build/test_launcherscoring build/test_placementhabits build/test_dbusvalue
	./build/test_bsptree
	./build/test_launcherscoring
	./build/test_placementhabits
	./build/test_dbusvalue

build/test_bsptree: tests/test_bsptree.cpp src/BSPTree.cpp src/BSPLeaf.cpp src/BSPSplit.cpp src/ManagedWindow.cpp src/LayoutEngine.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

# No X11/display needed either - pure string/data-structure logic,
# same reasoning as test_bsptree above - so this is folded straight
# into `make test` rather than kept separate like test-monitors is.
build/test_launcherscoring: tests/test_launcherscoring.cpp src/LauncherScoring.cpp src/Utils.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

# Same reasoning again - pure logic, plus a throwaway XDG_DATA_HOME
# the test redirects itself to (see the file) so it never touches a
# real user's actual learned habits.
build/test_placementhabits: tests/test_placementhabits.cpp src/PlacementHabitStore.cpp src/Xdg.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

# DBusValue is pure data-structure logic too (no actual D-Bus
# connection involved - see the file itself).
build/test_dbusvalue: tests/test_dbusvalue.cpp src/DBusValue.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

# Needs a real X11/XRandr connection - gracefully skips the checks
# that need one if $DISPLAY isn't set (see the file itself), so it's
# kept separate from `make test` rather than folded into it.
test-monitors: build/test_monitormanager
	./build/test_monitormanager

build/test_monitormanager: tests/test_monitormanager.cpp src/Monitor.cpp src/MonitorManager.cpp src/MonitorRule.cpp src/Workspace.cpp src/WorkspaceManager.cpp src/BSPTree.cpp src/BSPLeaf.cpp src/BSPSplit.cpp src/ManagedWindow.cpp src/LayoutEngine.cpp src/Config.cpp src/XConnection.cpp src/XAtoms.cpp src/Utils.cpp src/Logger.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ -lX11 $(shell pkg-config --exists xrandr && pkg-config --libs xrandr)

# Installing kohiko always installs Kohiko Settings alongside it -
# it's part of Kohiko itself, not a separate package (see
# include/SettingsWindow.h) - so there's no separate `install-settings`
# target, just these two extra `install -D`s. The .desktop entry +
# icon are what let it show up in Kohiko's own launcher (and any
# other XDG-compliant one) through completely ordinary desktop-file
# indexing - nothing is hardcoded into Launcher.cpp/AppIndex.cpp
# to make that happen.
install: kohiko kohikoctl kohiko-settings
	install -Dm755 kohiko $(DESTDIR)/usr/local/bin/kohiko
	install -Dm755 kohikoctl $(DESTDIR)/usr/local/bin/kohikoctl
	install -Dm755 kohiko-settings $(DESTDIR)/usr/local/bin/kohiko-settings
	install -Dm644 config/default.conf $(DESTDIR)/usr/local/share/kohiko/default.conf
	install -Dm644 desktop/kohiko-settings.desktop $(DESTDIR)/usr/local/share/applications/kohiko-settings.desktop
	install -Dm644 assets/icons/kohiko-settings.svg $(DESTDIR)/usr/local/share/icons/hicolor/scalable/apps/kohiko-settings.svg
	# Belt-and-suspenders alongside the properly-themed hicolor install
	# above: IconResolver's theme search (see include/IconResolver.h)
	# only reads one base directory per theme - whichever it finds an
	# index.theme in first - so on a real system, where
	# /usr/share/icons/hicolor already has one, an icon installed only
	# under /usr/local/share/icons/hicolor could go unfound. The
	# hardcoded pixmaps fallback IconResolver checks last (matching the
	# freedesktop Icon Theme spec's own final fallback step) isn't
	# subject to that, and isn't affected by $PREFIX either - it's
	# always /usr/share/pixmaps regardless (see Xdg::PixmapsDir()).
	install -Dm644 assets/icons/kohiko-settings.svg $(DESTDIR)/usr/share/pixmaps/kohiko-settings.svg

.PHONY: install-desktop-apps
ifeq ($(KOHIKO_HAVE_PIPEWIRE)-$(KOHIKO_HAVE_DBUS_PKG),yes-yes)
install: install-desktop-apps
install-desktop-apps: kohiko-audio kohiko-network kohiko-bluetooth kohiko-audio-tray kohiko-network-tray kohiko-bluetooth-tray
	install -Dm755 kohiko-audio $(DESTDIR)/usr/local/bin/kohiko-audio
	install -Dm755 kohiko-network $(DESTDIR)/usr/local/bin/kohiko-network
	install -Dm755 kohiko-bluetooth $(DESTDIR)/usr/local/bin/kohiko-bluetooth
	install -Dm755 kohiko-audio-tray $(DESTDIR)/usr/local/bin/kohiko-audio-tray
	install -Dm755 kohiko-network-tray $(DESTDIR)/usr/local/bin/kohiko-network-tray
	install -Dm755 kohiko-bluetooth-tray $(DESTDIR)/usr/local/bin/kohiko-bluetooth-tray
	install -Dm644 desktop/kohiko-audio.desktop $(DESTDIR)/usr/local/share/applications/kohiko-audio.desktop
	install -Dm644 desktop/kohiko-network.desktop $(DESTDIR)/usr/local/share/applications/kohiko-network.desktop
	install -Dm644 desktop/kohiko-bluetooth.desktop $(DESTDIR)/usr/local/share/applications/kohiko-bluetooth.desktop
	install -Dm644 assets/icons/kohiko-audio.svg $(DESTDIR)/usr/local/share/icons/hicolor/scalable/apps/kohiko-audio.svg
	install -Dm644 assets/icons/kohiko-network.svg $(DESTDIR)/usr/local/share/icons/hicolor/scalable/apps/kohiko-network.svg
	install -Dm644 assets/icons/kohiko-bluetooth.svg $(DESTDIR)/usr/local/share/icons/hicolor/scalable/apps/kohiko-bluetooth.svg
	install -Dm644 assets/icons/kohiko-audio.svg $(DESTDIR)/usr/share/pixmaps/kohiko-audio.svg
	install -Dm644 assets/icons/kohiko-network.svg $(DESTDIR)/usr/share/pixmaps/kohiko-network.svg
	install -Dm644 assets/icons/kohiko-bluetooth.svg $(DESTDIR)/usr/share/pixmaps/kohiko-bluetooth.svg
	# System-wide autostart per the freedesktop Desktop Application
	# Autostart Specification (read from $$XDG_CONFIG_DIRS/autostart,
	# whose default is /etc/xdg) - a per-user override still works
	# normally via ~/.config/autostart.
	install -Dm644 desktop/kohiko-audio-tray.desktop $(DESTDIR)/etc/xdg/autostart/kohiko-audio-tray.desktop
	install -Dm644 desktop/kohiko-network-tray.desktop $(DESTDIR)/etc/xdg/autostart/kohiko-network-tray.desktop
	install -Dm644 desktop/kohiko-bluetooth-tray.desktop $(DESTDIR)/etc/xdg/autostart/kohiko-bluetooth-tray.desktop
endif

clean:
	rm -rf build kohiko kohikoctl kohiko-settings \
		kohiko-audio kohiko-network kohiko-bluetooth \
		kohiko-audio-tray kohiko-network-tray kohiko-bluetooth-tray
