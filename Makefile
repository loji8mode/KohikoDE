# Plain-make alternative to CMakeLists.txt - no cmake required, just
# g++, libX11, and libXft/fontconfig (used for the bar/launcher/
# notepad's text rendering - see include/Font.h), plus optionally
# libXrandr for multi-monitor support (auto-detected below).

CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra

# Per-.o header dependency files (build/*.d, pulled in via -include at
# the bottom of this file) - without these, `make` only knows a .o
# depends on its own .cpp, not on whatever headers that .cpp
# transitively includes. Editing a shared header (UiWidget.h, say)
# would then leave every .o that includes it stale but unrebuilt,
# and a subsequent incremental `make` would happily link old and new
# object code together - readable as a valid build with no warnings,
# but Undefined Behaviour at runtime (mismatched struct layouts,
# ODR violations) the moment those inconsistent .o files actually
# interact. `make clean && make` sidesteps this by rebuilding
# everything, but shouldn't be required just to pick up a header
# change.
DEPFLAGS := -MMD -MP

INCLUDES := -Iinclude
INCLUDES += $(shell pkg-config --cflags xft fontconfig)

LIBS := -lX11 -lImlib2 -lpam
LIBS += $(shell pkg-config --libs xft fontconfig)

# SettingsWindow.cpp exists only for kohiko-settings (its own separate
# binary/process - see include/SettingsWindow.h's own header comment
# for why) - keeping it out of $(OBJ) is what keeps the `kohiko`
# binary itself free of GUI code it never runs, per "keep the WM
# lightweight". ConfigWriter.cpp used to get the same treatment, but
# ConfigMigration.cpp (part of ordinary WM startup - see
# Application::Run()) now reuses it too, so it's shared infrastructure
# now, the same as ConfigSchema.cpp already was. The
# desktop-integration apps' sources (kohiko-audio/network/bluetooth
# and their shared infrastructure) still get the GUI-only treatment,
# for the same "lightweight WM binary" reason - see
# DESKTOP_SHARED_SRC/DESKTOP_APP_ONLY_SRC below.
DESKTOP_SHARED_SRC := src/DBusValue.cpp src/DBusClient.cpp src/AppInstanceLock.cpp \
    src/AppConfigStore.cpp src/NotificationClient.cpp src/UiWindow.cpp src/UiWidget.cpp \
    src/UiListRow.cpp src/UiScrollView.cpp src/UiSidebar.cpp src/UiPopupMenu.cpp \
    src/UiIconCache.cpp src/SvgRenderer.cpp src/TrayIconClient.cpp
DESKTOP_APP_ONLY_SRC := src/PipeWireClient.cpp src/NetworkManagerClient.cpp src/BluezClient.cpp \
    src/AudioWindow.cpp src/NetworkWindow.cpp src/BluetoothWindow.cpp

GUI_ONLY_SRC := src/SettingsWindow.cpp $(DESKTOP_SHARED_SRC) $(DESKTOP_APP_ONLY_SRC)

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
# Gated on dbus-1 + libpipewire-0.3 + librsvg-2.0 all actually being
# present - unlike kohiko's own *optional* dbus-1 usage just above
# (which only loses a feature without it), DBusClient.cpp,
# PipeWireClient.cpp, and SvgRenderer.cpp all `#include` their real
# headers unconditionally, so these six binaries have no fallback at
# compile time. See CMakeLists.txt's identical
# KOHIKO_DESKTOP_APPS_AVAILABLE check for the same reasoning.
KOHIKO_HAVE_DBUS_PKG := $(shell pkg-config --exists dbus-1 && echo yes)

ifeq ($(shell pkg-config --exists libpipewire-0.3 && echo yes),yes)
    KOHIKO_HAVE_PIPEWIRE := yes
    CXXFLAGS     += $(shell pkg-config --cflags libpipewire-0.3)
    PIPEWIRE_LIBS := $(shell pkg-config --libs libpipewire-0.3)
endif

# librsvg + Cairo - SvgRenderer.cpp renders .svg/.svgz icons directly
# through these, rather than through Imlib2's own bundled SVG loader
# plugin - see SvgRenderer.h's own comment for why. Same
# no-fallback-at-compile-time treatment as libpipewire-0.3 just above.
ifeq ($(shell pkg-config --exists librsvg-2.0 && echo yes),yes)
    KOHIKO_HAVE_LIBRSVG := yes
    CXXFLAGS  += $(shell pkg-config --cflags librsvg-2.0)
    RSVG_LIBS := $(shell pkg-config --libs librsvg-2.0)
endif

DESKTOP_SHARED_OBJ := $(patsubst src/%.cpp,build/%.o,$(DESKTOP_SHARED_SRC))

# The handful of already-shared low-level pieces kohiko itself also
# needs (config parsing, icon resolution, XDG paths, string helpers,
# Xft text rendering) - reused as-is via the same generic
# `build/%.o: src/%.cpp` pattern rule below rather than recompiled a
# second time under a different name.
DESKTOP_COMMON_OBJ := build/Font.o build/Config.o build/IconResolver.o build/Xdg.o build/Utils.o build/IniFile.o \
    build/NotificationPopup.o build/NotificationCenter.o

.PHONY: all clean test install uninstall

all: kohiko kohikoctl kohiko-settings

ifeq ($(KOHIKO_HAVE_PIPEWIRE)-$(KOHIKO_HAVE_DBUS_PKG)-$(KOHIKO_HAVE_LIBRSVG),yes-yes-yes)
all: kohiko-audio kohiko-network kohiko-bluetooth kohiko-audio-tray kohiko-network-tray kohiko-bluetooth-tray
else
$(info Skipping kohiko-audio/kohiko-network/kohiko-bluetooth and their tray widgets - install libdbus-1-dev, libpipewire-0.3-dev, and librsvg2-dev to enable them.)
endif

kohiko: $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) -o $@ $(LIBS)

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

build:
	mkdir -p build

kohikoctl: tools/kohikoctl.cpp src/IpcPath.cpp src/AutologinConfigurator.cpp src/ConsoleAutologinConfigurator.cpp src/Utils.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

# A completely ordinary X11 client application (not part of the WM
# process at all - see include/SettingsWindow.h) - only needs
# X11/Xft/fontconfig, all of which `kohiko` itself already requires,
# so this adds no new dependency to the project as a whole.
kohiko-settings: $(SETTINGS_OBJ) build/kohiko-settings-main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ -lX11 $(shell pkg-config --libs xft fontconfig)

build/kohiko-settings-main.o: tools/kohiko-settings.cpp | build
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

# --- kohiko-audio -----------------------------------------------------------------

kohiko-audio: $(DESKTOP_SHARED_OBJ) $(DESKTOP_COMMON_OBJ) build/PipeWireClient.o build/AudioWindow.o build/kohiko-audio-main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS) $(PIPEWIRE_LIBS) $(RSVG_LIBS)

build/kohiko-audio-main.o: tools/kohiko-audio.cpp | build
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

kohiko-audio-tray: $(DESKTOP_SHARED_OBJ) $(DESKTOP_COMMON_OBJ) build/PipeWireClient.o build/kohiko-audio-tray-main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS) $(PIPEWIRE_LIBS) $(RSVG_LIBS)

build/kohiko-audio-tray-main.o: tools/kohiko-audio-tray.cpp | build
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

# --- kohiko-network ---------------------------------------------------------------

kohiko-network: $(DESKTOP_SHARED_OBJ) $(DESKTOP_COMMON_OBJ) build/NetworkManagerClient.o build/NetworkWindow.o build/kohiko-network-main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS) $(RSVG_LIBS)

build/kohiko-network-main.o: tools/kohiko-network.cpp | build
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

kohiko-network-tray: $(DESKTOP_SHARED_OBJ) $(DESKTOP_COMMON_OBJ) build/NetworkManagerClient.o build/kohiko-network-tray-main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS) $(RSVG_LIBS)

build/kohiko-network-tray-main.o: tools/kohiko-network-tray.cpp | build
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

# --- kohiko-bluetooth --------------------------------------------------------------

kohiko-bluetooth: $(DESKTOP_SHARED_OBJ) $(DESKTOP_COMMON_OBJ) build/BluezClient.o build/BluetoothWindow.o build/kohiko-bluetooth-main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS) $(RSVG_LIBS)

build/kohiko-bluetooth-main.o: tools/kohiko-bluetooth.cpp | build
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

kohiko-bluetooth-tray: $(DESKTOP_SHARED_OBJ) $(DESKTOP_COMMON_OBJ) build/BluezClient.o build/kohiko-bluetooth-tray-main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS) $(RSVG_LIBS)

build/kohiko-bluetooth-tray-main.o: tools/kohiko-bluetooth-tray.cpp | build
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

# test_networkmanagerclient needs libdbus-1-dev to even compile
# (NetworkManagerClient.cpp/DBusClient.cpp #include <dbus/dbus.h>
# unconditionally, same "no fallback at compile time" situation as
# kohiko-network itself) - added to the `test` prerequisite list and
# run conditionally, the same way kohiko-network's own six-binary
# group is gated, rather than unconditionally like the tests above it
# that need nothing beyond the C++ standard library.
ifeq ($(KOHIKO_HAVE_DBUS_PKG),yes)
    TEST_NETWORKMANAGERCLIENT := build/test_networkmanagerclient
    # test_networkmanager_live.sh drives this harness against
    # mock_networkmanager.py - gated the same as TEST_NETWORKMANAGERCLIENT
    # above (just NetworkManagerClient/DBusClient/DBusValue) since the
    # shell script itself separately, gracefully skips if python3-dbus
    # isn't available too. Not added to TEST_NETWORKMANAGERCLIENT's own
    # variable since the two are invoked differently (a binary run
    # directly vs. a shell script that builds its own throwaway D-Bus
    # environment around the binary).
    TEST_NETWORKMANAGER_LIVE_HARNESS := build/test_networkmanager_live_harness
endif

# test_scrollhittest links the full DESKTOP_SHARED_OBJ group (it
# exercises UiWindow/UiWidget/UiScrollView, but SvgRenderer.cpp and
# DBusClient.cpp are in that same object group and #include their
# real headers unconditionally) - gated the same three-way check as
# kohiko-audio/network/bluetooth themselves, since this is the shared
# UI toolkit those three apps are the only reason to build at all.
ifeq ($(KOHIKO_HAVE_PIPEWIRE)-$(KOHIKO_HAVE_DBUS_PKG)-$(KOHIKO_HAVE_LIBRSVG),yes-yes-yes)
    TEST_SCROLLHITTEST := build/test_scrollhittest
    TEST_NETWORKWINDOW := build/test_networkwindow
    TEST_BLUETOOTHWINDOW := build/test_bluetoothwindow
    TEST_AUDIO_LIVE_DEPS := kohiko-audio kohiko-audio-tray
endif

test: kohiko build/x11_test_client build/test_bsptree build/test_launcherscoring build/test_placementhabits build/test_dbusvalue build/test_sessionstore build/test_configmigration build/test_recoverymode build/test_lockrecovery build/test_appdirwatcher build/test_autologinconfigurator build/test_consoleautologinconfigurator build/test_wallpapermanager build/test_desktopentry build/test_iconresolver build/test_eventloop build/test_windowplacementnotice build/test_rect_clamping build/test_notificationlayout build/test_devicenotificationdiff $(TEST_NETWORKMANAGERCLIENT) $(TEST_SCROLLHITTEST) $(TEST_NETWORKWINDOW) $(TEST_NETWORKMANAGER_LIVE_HARNESS) $(TEST_BLUETOOTHWINDOW) $(TEST_AUDIO_LIVE_DEPS)
	./build/test_bsptree
	./build/test_launcherscoring
	./build/test_placementhabits
	./build/test_dbusvalue
	./build/test_sessionstore
	./build/test_configmigration
	./build/test_recoverymode
	./build/test_lockrecovery
	./build/test_appdirwatcher
	./build/test_autologinconfigurator
	./build/test_consoleautologinconfigurator
	./build/test_wallpapermanager
	./build/test_desktopentry
	./build/test_iconresolver
	./build/test_eventloop
	./build/test_windowplacementnotice
	./build/test_rect_clamping
	./build/test_notificationlayout
	./build/test_devicenotificationdiff
	sh tests/test_kohiko_session.sh
	sh tests/test_networkmanager_live.sh
	sh tests/test_xembed_dock_timeout.sh
ifeq ($(KOHIKO_HAVE_DBUS_PKG),yes)
	./build/test_networkmanagerclient
endif
ifeq ($(KOHIKO_HAVE_PIPEWIRE)-$(KOHIKO_HAVE_DBUS_PKG)-$(KOHIKO_HAVE_LIBRSVG),yes-yes-yes)
	./build/test_scrollhittest
	./build/test_networkwindow
	./build/test_bluetoothwindow
	sh tests/test_audio_notification_live.sh
endif

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

# Like test_placementhabits, this only exercises Load()'s on-disk
# format by hand-writing session files (no live X11/XRandr connection
# needed - see the file itself), but SessionStore.cpp's Save() still
# references the full Monitor/Workspace/BSP chain, so that has to be
# linked in too even though this test never calls Save(). -lX11 is
# needed for the same reason (XConnection.o references it), not
# because any test here opens a real display. Same deal for the
# trailing pkg-config xrandr clause as test_monitormanager's below: it
# also pulls in MonitorManager.cpp, which - whenever this build has
# KOHIKO_HAVE_XRANDR on, i.e. the normal case, matching the main
# `kohiko` binary above - refers to real XRandr symbols regardless of
# whether this particular test ever exercises that code path.
build/test_sessionstore: tests/test_sessionstore.cpp src/SessionStore.cpp src/Xdg.cpp src/ManagedWindow.cpp src/Monitor.cpp src/MonitorRule.cpp src/MonitorManager.cpp src/Workspace.cpp src/WorkspaceManager.cpp src/BSPTree.cpp src/BSPLeaf.cpp src/BSPSplit.cpp src/LayoutEngine.cpp src/XConnection.cpp src/XAtoms.cpp src/Config.cpp src/ConfigParser.cpp src/IniFile.cpp src/Utils.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ -lX11 $(shell pkg-config --exists xrandr && pkg-config --libs xrandr)

# Pure logic + plain file I/O, no X11 needed at all - see the file
# itself. Exercises ConfigMigration.cpp against ConfigSchema's real,
# full option list, so it also incidentally re-validates that every
# schema entry is well-formed enough to round-trip through
# ConfigWriter.
build/test_configmigration: tests/test_configmigration.cpp src/ConfigMigration.cpp src/ConfigWriter.cpp src/ConfigSchema.cpp src/Utils.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

# Pure logic + plain file I/O too - no X11, no real crash involved,
# just the marker file described in RecoveryMode.h's own comment.
build/test_recoverymode: tests/test_recoverymode.cpp src/RecoveryMode.cpp src/Xdg.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

# Same shape as test_recoverymode just above (plain file I/O, no X11,
# no real crash needed) - covers the "crashed while locked resumes
# fully exposed" fix, see LockRecovery.h and the 0.20.4 CHANGELOG.
build/test_lockrecovery: tests/test_lockrecovery.cpp src/LockRecovery.cpp src/Xdg.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

# Real functional test against actual Linux inotify (via throwaway
# temp directories) - no X11 needed, see the file itself.
build/test_appdirwatcher: tests/test_appdirwatcher.cpp src/AppDirWatcher.cpp src/Xdg.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

# Pure logic + plain file I/O, no X11 needed - every scenario runs
# against a real throwaway fake filesystem tree (see the file itself
# and AutologinConfigurator.h's own `root` parameter).
build/test_autologinconfigurator: tests/test_autologinconfigurator.cpp src/AutologinConfigurator.cpp src/Utils.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

# The console/no-display-manager counterpart above - same pure logic +
# plain file I/O, no X11 needed, same fake-filesystem-tree approach
# (see ConsoleAutologinConfigurator.h's own `root` parameter and the
# test file itself).
build/test_consoleautologinconfigurator: tests/test_consoleautologinconfigurator.cpp src/ConsoleAutologinConfigurator.cpp src/Utils.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

# Only exercises Configure()/ResolveFor() (pure parsing/priority
# logic) - never ApplyToRoot() itself, which needs a live X connection
# and isn't something a unit test should be driving anyway (see the
# file itself). Still needs Imlib2/X11 to link, since WallpaperManager.o
# references ImageRenderer::Render() regardless of whether this test
# calls the code path that uses it. Same MonitorManager.cpp/XRandr
# situation as test_sessionstore just above - see its comment.
build/test_wallpapermanager: tests/test_wallpapermanager.cpp src/WallpaperManager.cpp src/ImageRenderer.cpp src/Config.cpp src/ConfigParser.cpp src/IniFile.cpp src/Monitor.cpp src/MonitorManager.cpp src/MonitorRule.cpp src/Workspace.cpp src/WorkspaceManager.cpp src/BSPTree.cpp src/BSPLeaf.cpp src/BSPSplit.cpp src/ManagedWindow.cpp src/LayoutEngine.cpp src/XConnection.cpp src/XAtoms.cpp src/Utils.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ -lX11 -lImlib2 $(shell pkg-config --exists xrandr && pkg-config --libs xrandr)

# Pure logic + plain file I/O, no X11 needed - covers ShouldAutostart()
# and Xdg::AutostartDirs(), the two testable halves of the tray-icon
# autostart fix (see WindowManager::RunXdgAutostartEntries()'s own
# comment). Redirects XDG_CONFIG_HOME/XDG_CONFIG_DIRS/PATH to
# throwaway temp locations itself (see the file), same as
# test_placementhabits redirecting XDG_DATA_HOME.
build/test_desktopentry: tests/test_desktopentry.cpp src/DesktopEntry.cpp src/IniFile.cpp src/Xdg.cpp src/Utils.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

# Pure logic + plain file I/O, no X11 needed - covers the
# "-symbolic" fallback in IconResolver::Resolve() (see IconResolver.h)
# added alongside the tray-icon autostart fix, once real testing
# showed the tray icons still rendered blank even once docked.
# Redirects HOME/XDG_DATA_HOME to a throwaway temp theme (see the
# file).
build/test_iconresolver: tests/test_iconresolver.cpp src/IconResolver.cpp src/IniFile.cpp src/Xdg.cpp src/Utils.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

# Pure chrono arithmetic, no X11/D-Bus/real waiting needed - the
# TickSchedule functions are header-only inline (EventLoop.h)
# specifically so this test doesn't need to link EventLoop.cpp itself,
# which would drag in the whole WindowManager dependency graph for
# logic that has none of its own. Regression test for the taskbar/
# clock "frozen until something else forces a repaint" fix - see
# CHANGELOG.md.
build/test_eventloop: tests/test_eventloop.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

# Pure string formatting, no WindowManager/X11 dependency - see
# WindowPlacementNotice.h's own comment for why. Regression test for
# the "app opens on a background workspace with zero on-screen signal"
# fix - see CHANGELOG.md.
build/test_windowplacementnotice: tests/test_windowplacementnotice.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

# Pure struct/arithmetic logic, no dependency beyond Types.h itself.
# Covers Rect::ClampedTo() - existing logic, but newly depended on
# directly by the Swap-drag animation clipping fix - see CHANGELOG.md.
build/test_rect_clamping: tests/test_rect_clamping.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

# Pure placement/sizing/stacking arithmetic behind the native
# notification popup mechanism (NotificationPopup.h/NotificationCenter.h) -
# no X11 dependency at all, same reasoning as test_rect_clamping just
# above (in fact it directly reuses Rect::ClampedTo()). See
# NotificationLayout.h's own header comment for why this is tested
# separately from the live-X11 window behaviour (test-notificationcenter,
# below - needs a real display, so it's kept out of `make test` the
# same way test-bar/test-windowclassification/test-trayiconclient are).
build/test_notificationlayout: tests/test_notificationlayout.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

# Regression tests for the diffing logic behind device-connect/
# disconnect notifications, extracted into DeviceNotificationDiff.h
# after a real field bug (duplicate notifications from a sink's own
# monitor-source companion) shipped precisely because this logic used
# to live inline, untested, in kohiko-audio-tray.cpp. Pure logic, no
# X11/PipeWire dependency.
build/test_devicenotificationdiff: tests/test_devicenotificationdiff.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

# Pure logic, no live D-Bus connection or running NetworkManager
# needed (see the file itself) - but still needs libdbus-1-dev to
# compile at all, since it links DBusClient.cpp - hence the
# TEST_NETWORKMANAGERCLIENT gating above rather than being
# unconditional like the tests just above it. Regression test for the
# BytesToString()/ConnectToAccessPoint() fix - see CHANGELOG.md.
build/test_networkmanagerclient: tests/test_networkmanagerclient.cpp src/NetworkManagerClient.cpp src/DBusClient.cpp src/DBusValue.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ $(LIBS)

build/test_networkmanager_live_harness: tests/live/test_networkmanager_live_harness.cpp src/NetworkManagerClient.cpp src/DBusClient.cpp src/DBusValue.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ $(LIBS)

# Plain X11 client (no Kohiko headers/objects at all) driving
# tests/test_xembed_dock_timeout.sh - see that script and
# tests/live/x11_test_client.cpp's own header comment.
build/x11_test_client: tests/live/x11_test_client.cpp | build
	$(CXX) $(CXXFLAGS) -Wall -Wextra $< -o $@ -lX11

# Regression test for the scroll-routing fix (see CHANGELOG.md) -
# links the full DESKTOP_SHARED_OBJ/DESKTOP_COMMON_OBJ group since
# that's what actually builds UiWindow/UiWidget/UiScrollView, the same
# gating reasoning as TEST_SCROLLHITTEST above.
build/test_scrollhittest: tests/test_scrollhittest.cpp $(DESKTOP_SHARED_OBJ) $(DESKTOP_COMMON_OBJ) | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) tests/test_scrollhittest.cpp $(DESKTOP_SHARED_OBJ) $(DESKTOP_COMMON_OBJ) -o $@ $(LIBS) $(RSVG_LIBS)

# Links NetworkWindow.o itself (not just DESKTOP_SHARED_OBJ) since
# FilterPastedPasswordText() - the only thing this test actually calls -
# is defined there, plus NetworkManagerClient.o since NetworkWindow.cpp
# constructs one as a member. Same three-way gate as test_scrollhittest:
# this is exactly the dependency chain kohiko-network itself needs.
build/test_networkwindow: tests/test_networkwindow.cpp build/NetworkWindow.o build/NetworkManagerClient.o $(DESKTOP_SHARED_OBJ) $(DESKTOP_COMMON_OBJ) | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) tests/test_networkwindow.cpp build/NetworkWindow.o build/NetworkManagerClient.o $(DESKTOP_SHARED_OBJ) $(DESKTOP_COMMON_OBJ) -o $@ $(LIBS) $(RSVG_LIBS)

build/test_bluetoothwindow: tests/test_bluetoothwindow.cpp build/BluetoothWindow.o build/BluezClient.o $(DESKTOP_SHARED_OBJ) $(DESKTOP_COMMON_OBJ) | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) tests/test_bluetoothwindow.cpp build/BluetoothWindow.o build/BluezClient.o $(DESKTOP_SHARED_OBJ) $(DESKTOP_COMMON_OBJ) -o $@ $(LIBS) $(RSVG_LIBS)

# Needs a real X11/XRandr connection - gracefully skips the checks
# that need one if $DISPLAY isn't set (see the file itself), so it's
# kept separate from `make test` rather than folded into it.
test-monitors: build/test_monitormanager
	./build/test_monitormanager

build/test_monitormanager: tests/test_monitormanager.cpp src/Monitor.cpp src/MonitorManager.cpp src/MonitorRule.cpp src/Workspace.cpp src/WorkspaceManager.cpp src/BSPTree.cpp src/BSPLeaf.cpp src/BSPSplit.cpp src/ManagedWindow.cpp src/LayoutEngine.cpp src/Config.cpp src/XConnection.cpp src/XAtoms.cpp src/Utils.cpp src/Logger.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ -lX11 $(shell pkg-config --exists xrandr && pkg-config --libs xrandr)

# Same "needs a real X11 connection, kept out of `make test`" deal as
# test-monitors just above - regression test for the tray-window
# classification fix (XConnection::IsXEmbedWindow(), see that
# function's own comment and WindowManager::Manage()'s use of it).
test-windowclassification: build/test_windowclassification
	./build/test_windowclassification

build/test_windowclassification: tests/test_windowclassification.cpp src/XConnection.cpp src/XAtoms.cpp src/Logger.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ -lX11

# Same "needs a real X11 connection, kept out of `make test`" deal
# again - CanHandle() (pure logic) always runs regardless; only the
# RenderToPixmaps() stress/safety section needs $DISPLAY (see the
# file itself). Regression test for the SVG-rendering-path fix (see
# SvgRenderer.h's own comment).
test-svgrenderer: build/test_svgrenderer
	./build/test_svgrenderer

build/test_svgrenderer: tests/test_svgrenderer.cpp src/SvgRenderer.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ -lX11 $(RSVG_LIBS)

# Same "needs a real X11 connection, kept out of `make test`" deal
# again - regression/performance test for the 0.20.6 audit's
# Bar::Redraw() fix (see that file's own header comment and the
# CHANGELOG's 0.20.6 entry).
test-bar: build/test_bar
	./build/test_bar

build/test_bar: tests/test_bar.cpp src/Bar.cpp src/SystemTray.cpp src/Font.cpp src/Config.cpp src/XConnection.cpp src/XAtoms.cpp src/Utils.cpp src/Logger.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ $(LIBS)

# Same "needs a real X11 connection, kept out of `make test`" deal
# again - regression test for the 0.20.8 tray-icon fallback fix (see
# that file's own header comment and the CHANGELOG's 0.20.8 entry).
# Reuses DESKTOP_COMMON_OBJ/DESKTOP_SHARED_OBJ (the same dependency
# set every one of the three real tray tools already links) rather
# than hand-picking a smaller subset, since TrayIconClient/UiIconCache
# are already part of that shared object set for exactly this reason.
test-trayiconclient: build/test_trayiconclient
	./build/test_trayiconclient

build/test_trayiconclient: tests/test_trayiconclient.cpp $(DESKTOP_SHARED_OBJ) $(DESKTOP_COMMON_OBJ) | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ $(LIBS) $(RSVG_LIBS)

# Same "needs a real X11 connection, kept out of `make test`" deal
# again - focused tests for the new native notification mechanism
# itself (creation, window type/properties, no focus stealing,
# placement, expiry, multiple/stacking, cleanup - see that file's own
# header comment). Only needs Font.o/Config.o (part of
# DESKTOP_COMMON_OBJ already) - deliberately does NOT link
# XConnection.o/XAtoms.o/WindowManager.o at all, since NotificationPopup/
# NotificationCenter depend on neither (see their own header comments);
# the complementary "WindowManager::Manage() correctly leaves a
# NOTIFICATION-type window alone" check lives in
# test-windowclassification instead, which already links those.
test-notificationcenter: build/test_notificationcenter
	./build/test_notificationcenter

build/test_notificationcenter: tests/test_notificationcenter.cpp $(DESKTOP_COMMON_OBJ) | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ $(LIBS)

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
	for wallpaper in assets/wallpapers/*.png; do \
		install -Dm644 "$$wallpaper" "$(DESTDIR)/usr/local/share/kohiko/wallpapers/$$(basename $$wallpaper)"; \
	done
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
	# scripts/kohiko-session - the actual session entry point (see its
	# own header comment); the .desktop entry below execs this, not
	# `kohiko` directly. Installed alongside the other binaries even
	# though it's a shell script, not a compiled one - same -Dm755
	# either way.
	install -Dm755 scripts/kohiko-session $(DESTDIR)/usr/local/bin/kohiko-session
	# Deliberately *not* under /usr/local like everything else above -
	# display managers (LightDM/SDDM/GDM) scan the fixed, well-known
	# /usr/share/xsessions regardless of where Kohiko itself is
	# installed; a copy under /usr/local/share/xsessions would go
	# entirely unfound by most of them. Same reasoning, and the same
	# kind of deliberate exception, as the pixmaps fallback just above.
	install -Dm644 desktop/kohiko.desktop $(DESTDIR)/usr/share/xsessions/kohiko.desktop

.PHONY: install-desktop-apps
ifeq ($(KOHIKO_HAVE_PIPEWIRE)-$(KOHIKO_HAVE_DBUS_PKG)-$(KOHIKO_HAVE_LIBRSVG),yes-yes-yes)
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
	# The headless BlueZ pairing agent kohiko-bluetooth itself needs
	# but doesn't register (see BluezClient::PairDevice()'s own
	# comment and systemd/kohiko-bt-agent.service's own header) -
	# ships the unit system-wide, same location third-party packages
	# like blueman use for their own user units, but does NOT enable
	# it here: that's scripts/install-arch.sh's job, run as the actual
	# logged-in user in their own systemd --user session - see that
	# script's own comment for why a root-context package-install step
	# can't safely do this instead.
	install -Dm644 systemd/kohiko-bt-agent.service $(DESTDIR)/usr/lib/systemd/user/kohiko-bt-agent.service
endif

# Removes exactly what install/install-desktop-apps put on the system -
# nothing this project doesn't itself own (never bluez/bluez-tools
# themselves, never anything under a user's own $HOME - see
# scripts/uninstall-arch.sh for that half, run by the actual user for
# the same reason install-arch.sh's own per-user steps are). Kept as a
# literal mirror of install/install-desktop-apps's own file list
# on purpose - a new install -D line without a matching rm -f here is
# a bug, and the symmetry makes that easy to spot on review.
uninstall: uninstall-desktop-apps
	rm -f $(DESTDIR)/usr/local/bin/kohiko
	rm -f $(DESTDIR)/usr/local/bin/kohikoctl
	rm -f $(DESTDIR)/usr/local/bin/kohiko-settings
	rm -f $(DESTDIR)/usr/local/share/kohiko/default.conf
	rm -f $(DESTDIR)/usr/local/share/kohiko/wallpapers/*.png
	-rmdir $(DESTDIR)/usr/local/share/kohiko/wallpapers $(DESTDIR)/usr/local/share/kohiko 2>/dev/null
	rm -f $(DESTDIR)/usr/local/share/applications/kohiko-settings.desktop
	rm -f $(DESTDIR)/usr/local/share/icons/hicolor/scalable/apps/kohiko-settings.svg
	rm -f $(DESTDIR)/usr/share/pixmaps/kohiko-settings.svg
	rm -f $(DESTDIR)/usr/local/bin/kohiko-session
	rm -f $(DESTDIR)/usr/share/xsessions/kohiko.desktop

.PHONY: uninstall-desktop-apps
ifeq ($(KOHIKO_HAVE_PIPEWIRE)-$(KOHIKO_HAVE_DBUS_PKG)-$(KOHIKO_HAVE_LIBRSVG),yes-yes-yes)
uninstall-desktop-apps:
	rm -f $(DESTDIR)/usr/local/bin/kohiko-audio
	rm -f $(DESTDIR)/usr/local/bin/kohiko-network
	rm -f $(DESTDIR)/usr/local/bin/kohiko-bluetooth
	rm -f $(DESTDIR)/usr/local/bin/kohiko-audio-tray
	rm -f $(DESTDIR)/usr/local/bin/kohiko-network-tray
	rm -f $(DESTDIR)/usr/local/bin/kohiko-bluetooth-tray
	rm -f $(DESTDIR)/usr/local/share/applications/kohiko-audio.desktop
	rm -f $(DESTDIR)/usr/local/share/applications/kohiko-network.desktop
	rm -f $(DESTDIR)/usr/local/share/applications/kohiko-bluetooth.desktop
	rm -f $(DESTDIR)/usr/local/share/icons/hicolor/scalable/apps/kohiko-audio.svg
	rm -f $(DESTDIR)/usr/local/share/icons/hicolor/scalable/apps/kohiko-network.svg
	rm -f $(DESTDIR)/usr/local/share/icons/hicolor/scalable/apps/kohiko-bluetooth.svg
	rm -f $(DESTDIR)/usr/share/pixmaps/kohiko-audio.svg
	rm -f $(DESTDIR)/usr/share/pixmaps/kohiko-network.svg
	rm -f $(DESTDIR)/usr/share/pixmaps/kohiko-bluetooth.svg
	rm -f $(DESTDIR)/etc/xdg/autostart/kohiko-audio-tray.desktop
	rm -f $(DESTDIR)/etc/xdg/autostart/kohiko-network-tray.desktop
	rm -f $(DESTDIR)/etc/xdg/autostart/kohiko-bluetooth-tray.desktop
	rm -f $(DESTDIR)/usr/lib/systemd/user/kohiko-bt-agent.service
endif

clean:
	rm -rf build kohiko kohikoctl kohiko-settings \
		kohiko-audio kohiko-network kohiko-bluetooth \
		kohiko-audio-tray kohiko-network-tray kohiko-bluetooth-tray

# Must come after every other rule (a target's own rule always wins
# over one reconstructed from a stale .d file, but make still needs
# the "real" rules parsed first) - see DEPFLAGS' comment up top for
# why these matter. The wildcard - rather than deriving names from
# $(OBJ) etc. - means this also picks up build/*-main.o's .d files
# without needing its own separate list. Silently does nothing before
# the first build, when build/ (and so every .d file) doesn't exist
# yet.
-include $(wildcard build/*.d)
