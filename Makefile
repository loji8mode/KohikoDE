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
# per "keep the WM lightweight".
GUI_ONLY_SRC := src/SettingsWindow.cpp src/ConfigWriter.cpp

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

.PHONY: all clean test install

all: kohiko kohikoctl kohiko-settings

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

test: build/test_bsptree build/test_launcherscoring build/test_placementhabits
	./build/test_bsptree
	./build/test_launcherscoring
	./build/test_placementhabits

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

clean:
	rm -rf build kohiko kohikoctl kohiko-settings
