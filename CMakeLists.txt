cmake_minimum_required(VERSION 3.16)
project(kohiko CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(X11 REQUIRED)

# Xft/fontconfig - used by Font.cpp for the bar/launcher/notepad's
# text rendering (see include/Font.h for why plain libX11 alone isn't
# enough for that). Neither has an upstream CMake find-module, so this
# goes through pkg-config the same way the Makefile does.
find_package(PkgConfig REQUIRED)
pkg_check_modules(Xft REQUIRED IMPORTED_TARGET xft fontconfig)
pkg_check_modules(Imlib2 REQUIRED IMPORTED_TARGET imlib2)

# libpam - LockScreen.cpp's own PAM authentication (see
# include/Authenticator.h). No pkg-config file on most distros, so
# this goes through a plain find_library the way the Makefile's -lpam
# does instead. Checked manually rather than via find_library()'s own
# REQUIRED (only available since CMake 3.18) to stay compatible with
# this file's declared cmake_minimum_required(3.16).
find_library(PAM_LIBRARY NAMES pam)
if(NOT PAM_LIBRARY)
    message(FATAL_ERROR "libpam not found - install libpam0g-dev (Debian/Ubuntu) or pam-devel (Fedora)")
endif()

# libdbus - the primary mechanism behind display-sleep inhibition (see
# include/ScreenSaverInhibitor.h): NOT required, since Kohiko still
# works fine without it (the X11 fullscreen-window heuristic in
# WindowManager::CheckSleepInhibition() is the fallback whenever this
# isn't available, exactly like XRandr/XScreenSaver below).
#
# It's also what every one of kohiko-audio/network/bluetooth's D-Bus
# backends (DBusClient, NetworkManagerClient, BluezClient,
# NotificationClient - see include/DBusClient.h) is built on, where
# it's a hard requirement rather than an optional one - see the
# KOHIKO_DESKTOP_APPS_AVAILABLE check further down, which skips
# building those three apps and their tray widgets entirely (with a
# warning) rather than failing the whole project's build if it's
# missing.
pkg_check_modules(DBus IMPORTED_TARGET dbus-1)

# libpipewire - kohiko-audio's only audio backend (see
# include/PipeWireClient.h) - also folded into the
# KOHIKO_DESKTOP_APPS_AVAILABLE check below.
pkg_check_modules(PipeWire IMPORTED_TARGET libpipewire-0.3)

# librsvg - SvgRenderer.cpp's direct librsvg/Cairo rendering path for
# .svg/.svgz icons (see SvgRenderer.h's own comment for why this
# exists instead of going through Imlib2's own SVG loader), used by
# UiIconCache.cpp and so, like DBus/PipeWire above, folded into the
# same KOHIKO_DESKTOP_APPS_AVAILABLE check below.
pkg_check_modules(Rsvg IMPORTED_TARGET librsvg-2.0)

# --- kohiko: the window manager ------------------------------------------------

file(GLOB KOHIKO_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)

# SettingsWindow.cpp exists only for kohiko-settings, its own separate
# binary/process (see include/SettingsWindow.h's own header comment
# for why) - excluded here so `kohiko` itself stays free of GUI code
# it never runs, per "keep the WM lightweight". ConfigWriter.cpp used
# to get the same treatment, but ConfigMigration.cpp (part of ordinary
# WM startup - see Application::Run()) now reuses it too, so it's
# shared infrastructure now, the same as ConfigSchema.cpp already was
# (never excluded here to begin with). The desktop-integration apps'
# sources (kohiko-audio/network/bluetooth and their shared
# infrastructure, built as their own targets further down) still get
# the GUI-only exclusion, for the same "lightweight WM binary" reason.
list(REMOVE_ITEM KOHIKO_SOURCES
    ${CMAKE_SOURCE_DIR}/src/SettingsWindow.cpp
    ${CMAKE_SOURCE_DIR}/src/DBusValue.cpp
    ${CMAKE_SOURCE_DIR}/src/DBusClient.cpp
    ${CMAKE_SOURCE_DIR}/src/AppInstanceLock.cpp
    ${CMAKE_SOURCE_DIR}/src/AppConfigStore.cpp
    ${CMAKE_SOURCE_DIR}/src/NotificationClient.cpp
    ${CMAKE_SOURCE_DIR}/src/UiWindow.cpp
    ${CMAKE_SOURCE_DIR}/src/UiWidget.cpp
    ${CMAKE_SOURCE_DIR}/src/UiListRow.cpp
    ${CMAKE_SOURCE_DIR}/src/UiScrollView.cpp
    ${CMAKE_SOURCE_DIR}/src/UiSidebar.cpp
    ${CMAKE_SOURCE_DIR}/src/UiPopupMenu.cpp
    ${CMAKE_SOURCE_DIR}/src/UiIconCache.cpp
    ${CMAKE_SOURCE_DIR}/src/SvgRenderer.cpp
    ${CMAKE_SOURCE_DIR}/src/TrayIconClient.cpp
    ${CMAKE_SOURCE_DIR}/src/PipeWireClient.cpp
    ${CMAKE_SOURCE_DIR}/src/NetworkManagerClient.cpp
    ${CMAKE_SOURCE_DIR}/src/BluezClient.cpp
    ${CMAKE_SOURCE_DIR}/src/AudioWindow.cpp
    ${CMAKE_SOURCE_DIR}/src/NetworkWindow.cpp
    ${CMAKE_SOURCE_DIR}/src/BluetoothWindow.cpp
)

add_executable(kohiko ${KOHIKO_SOURCES})
target_include_directories(kohiko PRIVATE ${CMAKE_SOURCE_DIR}/include ${X11_INCLUDE_DIR})
target_link_libraries(kohiko PRIVATE ${X11_LIBRARIES} PkgConfig::Xft PkgConfig::Imlib2 ${PAM_LIBRARY})
target_compile_options(kohiko PRIVATE -Wall -Wextra)

# Multi-monitor geometry via XRandr, enabled only if it's actually
# available - kohiko still works fine as a single-monitor WM without it.
if(X11_Xrandr_FOUND)
    target_compile_definitions(kohiko PRIVATE KOHIKO_HAVE_XRANDR)
    target_link_libraries(kohiko PRIVATE ${X11_Xrandr_LIB})
endif()

# Idle-timeout locking and display-sleep inhibition, both via the
# XScreenSaver extension (see include/IdleWatcher.h) - same "enabled
# only if actually available" treatment as XRandr above; neither
# feature does anything on a server without it.
if(X11_Xss_FOUND)
    target_compile_definitions(kohiko PRIVATE KOHIKO_HAVE_XSS)
    target_link_libraries(kohiko PRIVATE ${X11_Xss_LIB})
endif()

if(DBus_FOUND)
    target_compile_definitions(kohiko PRIVATE KOHIKO_HAVE_DBUS)
    target_include_directories(kohiko PRIVATE ${DBus_INCLUDE_DIRS})
    target_link_libraries(kohiko PRIVATE ${DBus_LIBRARIES})
endif()

# --- kohikoctl: the IPC command-line client --------------------------------------

add_executable(kohikoctl tools/kohikoctl.cpp src/IpcPath.cpp src/AutologinConfigurator.cpp src/Utils.cpp)
target_include_directories(kohikoctl PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(kohikoctl PRIVATE -Wall -Wextra)

# --- kohiko-settings: Kohiko Settings' GUI ---------------------------------------
#
# A completely ordinary X11 client application, not part of the WM
# process at all - see include/SettingsWindow.h. Only needs
# X11/Xft/fontconfig, all of which `kohiko` itself already requires
# above, so this adds no new dependency to the project as a whole.
# Installing Kohiko always installs this alongside it (see this
# file's own install() below) - it's part of Kohiko itself, not a
# separate package.

add_executable(kohiko-settings
    tools/kohiko-settings.cpp
    src/SettingsWindow.cpp
    src/ConfigWriter.cpp
    src/ConfigSchema.cpp
    src/Config.cpp
    src/Utils.cpp
    src/Font.cpp
)
target_include_directories(kohiko-settings PRIVATE ${CMAKE_SOURCE_DIR}/include ${X11_INCLUDE_DIR})
target_link_libraries(kohiko-settings PRIVATE ${X11_LIBRARIES} PkgConfig::Xft)
target_compile_options(kohiko-settings PRIVATE -Wall -Wextra)

# --- kohiko-audio / kohiko-network / kohiko-bluetooth + tray widgets -------------
#
# The native desktop-integration apps: standalone binaries sharing a
# UI toolkit (UiWindow/UiWidget/UiListRow/UiScrollView/UiSidebar/
# UiPopupMenu/UiIconCache), a tray-docking framework (TrayIconClient),
# and a D-Bus layer (DBusClient/DBusValue) used by NetworkManagerClient
# and BluezClient - see each header for the reasoning. kohiko-audio
# additionally needs PipeWireClient (libpipewire), which has no D-Bus
# interface of its own.
#
# Gated as a whole on dbus-1 + libpipewire-0.3 + librsvg-2.0 rather
# than built unconditionally: unlike kohiko itself (which merely loses
# the display-sleep-inhibition *feature* without dbus-1, see above),
# these six binaries have no fallback for a missing D-Bus/PipeWire/
# librsvg dependency at *compile* time - DBusClient.cpp,
# PipeWireClient.cpp, and SvgRenderer.cpp all `#include` their real
# headers unconditionally. Skipping the whole section (with a clear
# warning) keeps a system without those dev packages able to build
# `kohiko` itself, same as any other optional dependency in this file.
if(DBus_FOUND AND PipeWire_FOUND AND Rsvg_FOUND)
    set(KOHIKO_DESKTOP_APPS_AVAILABLE TRUE)
else()
    set(KOHIKO_DESKTOP_APPS_AVAILABLE FALSE)
    message(WARNING "libdbus-1-dev, libpipewire-0.3-dev, and/or librsvg2-dev not found - "
        "skipping kohiko-audio, kohiko-network, kohiko-bluetooth, and their "
        "tray widgets. Install all three to enable them.")
endif()

if(KOHIKO_DESKTOP_APPS_AVAILABLE)
    # Shared infrastructure, common to every one of the six binaries
    # below - built once as a static library rather than recompiled
    # per binary.
    add_library(kohiko-desktop-shared STATIC
        src/DBusValue.cpp
        src/DBusClient.cpp
        src/AppInstanceLock.cpp
        src/AppConfigStore.cpp
        src/NotificationClient.cpp
        src/UiWindow.cpp
        src/UiWidget.cpp
        src/UiListRow.cpp
        src/UiScrollView.cpp
        src/UiSidebar.cpp
        src/UiPopupMenu.cpp
        src/UiIconCache.cpp
        src/SvgRenderer.cpp
        src/TrayIconClient.cpp
        src/NotificationPopup.cpp
        src/NotificationCenter.cpp
        src/Font.cpp
        src/Config.cpp
        src/IconResolver.cpp
        src/Xdg.cpp
        src/Utils.cpp
        src/IniFile.cpp
    )
    target_include_directories(kohiko-desktop-shared PUBLIC ${CMAKE_SOURCE_DIR}/include ${X11_INCLUDE_DIR})
    target_link_libraries(kohiko-desktop-shared PUBLIC ${X11_LIBRARIES} PkgConfig::Xft PkgConfig::Imlib2 PkgConfig::DBus PkgConfig::Rsvg)
    target_compile_options(kohiko-desktop-shared PRIVATE -Wall -Wextra)

    add_executable(kohiko-audio tools/kohiko-audio.cpp src/AudioWindow.cpp src/PipeWireClient.cpp)
    target_link_libraries(kohiko-audio PRIVATE kohiko-desktop-shared PkgConfig::PipeWire)
    target_compile_options(kohiko-audio PRIVATE -Wall -Wextra)

    add_executable(kohiko-network tools/kohiko-network.cpp src/NetworkWindow.cpp src/NetworkManagerClient.cpp)
    target_link_libraries(kohiko-network PRIVATE kohiko-desktop-shared)
    target_compile_options(kohiko-network PRIVATE -Wall -Wextra)

    add_executable(kohiko-bluetooth tools/kohiko-bluetooth.cpp src/BluetoothWindow.cpp src/BluezClient.cpp)
    target_link_libraries(kohiko-bluetooth PRIVATE kohiko-desktop-shared)
    target_compile_options(kohiko-bluetooth PRIVATE -Wall -Wextra)

    add_executable(kohiko-audio-tray tools/kohiko-audio-tray.cpp src/PipeWireClient.cpp)
    target_link_libraries(kohiko-audio-tray PRIVATE kohiko-desktop-shared PkgConfig::PipeWire)
    target_compile_options(kohiko-audio-tray PRIVATE -Wall -Wextra)

    add_executable(kohiko-network-tray tools/kohiko-network-tray.cpp src/NetworkManagerClient.cpp)
    target_link_libraries(kohiko-network-tray PRIVATE kohiko-desktop-shared)
    target_compile_options(kohiko-network-tray PRIVATE -Wall -Wextra)

    add_executable(kohiko-bluetooth-tray tools/kohiko-bluetooth-tray.cpp src/BluezClient.cpp)
    target_link_libraries(kohiko-bluetooth-tray PRIVATE kohiko-desktop-shared)
    target_compile_options(kohiko-bluetooth-tray PRIVATE -Wall -Wextra)

    # Same "needs a real X11 connection, kept out of `make test`" deal
    # as test_bar/test_monitormanager/test_svgrenderer - regression
    # test for the 0.20.8 tray-icon fallback fix (see that file's own
    # header comment and the CHANGELOG's 0.20.8 entry). Guarded by the
    # same KOHIKO_DESKTOP_APPS_AVAILABLE condition as the rest of this
    # block since it links kohiko-desktop-shared, which only exists
    # here.
    add_executable(test_trayiconclient tests/test_trayiconclient.cpp)
    target_link_libraries(test_trayiconclient PRIVATE kohiko-desktop-shared)
    add_test(NAME TrayIconClient COMMAND test_trayiconclient)

    # Same "needs a real X11 connection, kept out of `make test`" deal
    # again - focused tests for the new native notification mechanism
    # itself (NotificationPopup/NotificationCenter - see that file's
    # own header comment). Only actually needs Font.cpp (part of
    # kohiko-desktop-shared already) - deliberately does not use
    # XConnection/XAtoms/WindowManager at all, since neither of the
    # classes under test depends on them; see
    # test_windowclassification's own IsNotificationWindowType() checks
    # for the complementary "WindowManager::Manage() leaves this window
    # alone" coverage instead.
    add_executable(test_notificationcenter tests/test_notificationcenter.cpp)
    target_link_libraries(test_notificationcenter PRIVATE kohiko-desktop-shared)
    add_test(NAME NotificationCenter COMMAND test_notificationcenter)
endif()

# --- tests ------------------------------------------------------------------------

enable_testing()

add_executable(test_bsptree
    tests/test_bsptree.cpp
    src/BSPTree.cpp
    src/BSPLeaf.cpp
    src/BSPSplit.cpp
    src/ManagedWindow.cpp
    src/LayoutEngine.cpp
)
target_include_directories(test_bsptree PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME BSPTree COMMAND test_bsptree)

# Pure string/data-structure logic, same as test_bsptree above - no
# X11 display needed.
add_executable(test_launcherscoring
    tests/test_launcherscoring.cpp
    src/LauncherScoring.cpp
    src/Utils.cpp
)
target_include_directories(test_launcherscoring PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME LauncherScoring COMMAND test_launcherscoring)

# Pure logic + a throwaway XDG_DATA_HOME the test redirects itself to
# (see the file) - no X11 display needed.
add_executable(test_placementhabits
    tests/test_placementhabits.cpp
    src/PlacementHabitStore.cpp
    src/Xdg.cpp
)
target_include_directories(test_placementhabits PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME PlacementHabitStore COMMAND test_placementhabits)

# Needs a real X11/XRandR connection (skips its X-dependent checks
# gracefully if $DISPLAY isn't set - see the file for why) - the same
# libraries kohiko itself links against for monitor detection.
add_executable(test_monitormanager
    tests/test_monitormanager.cpp
    src/Monitor.cpp
    src/MonitorManager.cpp
    src/MonitorRule.cpp
    src/Workspace.cpp
    src/WorkspaceManager.cpp
    src/BSPTree.cpp
    src/BSPLeaf.cpp
    src/BSPSplit.cpp
    src/ManagedWindow.cpp
    src/LayoutEngine.cpp
    src/Config.cpp
    src/XConnection.cpp
    src/XAtoms.cpp
    src/Utils.cpp
    src/Logger.cpp
)
target_include_directories(test_monitormanager PRIVATE ${CMAKE_SOURCE_DIR}/include ${X11_INCLUDE_DIR})
target_link_libraries(test_monitormanager PRIVATE ${X11_LIBRARIES})
if(X11_Xrandr_FOUND)
    target_compile_definitions(test_monitormanager PRIVATE KOHIKO_HAVE_XRANDR)
    target_link_libraries(test_monitormanager PRIVATE ${X11_Xrandr_LIB})
endif()
add_test(NAME MonitorManager COMMAND test_monitormanager)

# Same "real X11 connection, gracefully skips if $DISPLAY isn't set"
# deal as test_monitormanager just above - regression test for the
# tray-window classification fix (XConnection::IsXEmbedWindow()).
add_executable(test_windowclassification
    tests/test_windowclassification.cpp
    src/XConnection.cpp
    src/XAtoms.cpp
    src/Logger.cpp
)
target_include_directories(test_windowclassification PRIVATE ${CMAKE_SOURCE_DIR}/include ${X11_INCLUDE_DIR})
target_link_libraries(test_windowclassification PRIVATE ${X11_LIBRARIES})
add_test(NAME WindowClassification COMMAND test_windowclassification)

# Same "real X11 connection, gracefully skips display-dependent
# checks if $DISPLAY isn't set" deal - regression test for the SVG-
# rendering-path fix (SvgRenderer.h's own comment has the full
# "why"). Only built/registered when Rsvg is actually available,
# matching kohiko-audio/etc.'s own KOHIKO_DESKTOP_APPS_AVAILABLE
# gating just above, since SvgRenderer.cpp has no compile-time
# fallback for a missing librsvg-2.0 either.
if(Rsvg_FOUND)
    add_executable(test_svgrenderer
        tests/test_svgrenderer.cpp
        src/SvgRenderer.cpp
    )
    target_include_directories(test_svgrenderer PRIVATE ${CMAKE_SOURCE_DIR}/include ${X11_INCLUDE_DIR})
    target_link_libraries(test_svgrenderer PRIVATE ${X11_LIBRARIES} PkgConfig::Rsvg)
    add_test(NAME SvgRenderer COMMAND test_svgrenderer)
endif()

# Same "real X11 connection, gracefully skips if $DISPLAY isn't set"
# deal as test_monitormanager/test_svgrenderer above -
# regression/performance test for the 0.20.6 audit's Bar::Redraw()
# fix (see that file's own header comment and the CHANGELOG's 0.20.6
# entry).
add_executable(test_bar
    tests/test_bar.cpp
    src/Bar.cpp
    src/SystemTray.cpp
    src/Font.cpp
    src/Config.cpp
    src/XConnection.cpp
    src/XAtoms.cpp
    src/Utils.cpp
    src/Logger.cpp
)
target_include_directories(test_bar PRIVATE ${CMAKE_SOURCE_DIR}/include ${X11_INCLUDE_DIR})
target_link_libraries(test_bar PRIVATE ${X11_LIBRARIES} PkgConfig::Xft PkgConfig::Imlib2)
add_test(NAME Bar COMMAND test_bar)

# Pure data-structure logic (no D-Bus connection involved - see the
# file itself), same category as test_launcherscoring/
# test_placementhabits above.
add_executable(test_dbusvalue
    tests/test_dbusvalue.cpp
    src/DBusValue.cpp
)
target_include_directories(test_dbusvalue PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME DBusValue COMMAND test_dbusvalue)

# Like test_placementhabits, only exercises Load()'s on-disk format by
# hand-writing session files - no X11/XRandR connection needed (see
# the file itself) - but SessionStore.cpp's Save() still references
# the full Monitor/Workspace/BSP chain, so that has to be linked in
# too even though this test never calls Save().
add_executable(test_sessionstore
    tests/test_sessionstore.cpp
    src/SessionStore.cpp
    src/Xdg.cpp
    src/ManagedWindow.cpp
    src/Monitor.cpp
    src/MonitorManager.cpp
    src/MonitorRule.cpp
    src/Workspace.cpp
    src/WorkspaceManager.cpp
    src/BSPTree.cpp
    src/BSPLeaf.cpp
    src/BSPSplit.cpp
    src/LayoutEngine.cpp
    src/XConnection.cpp
    src/XAtoms.cpp
    src/Config.cpp
    src/ConfigParser.cpp
    src/IniFile.cpp
    src/Utils.cpp
    src/Logger.cpp
)
target_include_directories(test_sessionstore PRIVATE ${CMAKE_SOURCE_DIR}/include ${X11_INCLUDE_DIR})
target_link_libraries(test_sessionstore PRIVATE ${X11_LIBRARIES})
if(X11_Xrandr_FOUND)
    target_compile_definitions(test_sessionstore PRIVATE KOHIKO_HAVE_XRANDR)
    target_link_libraries(test_sessionstore PRIVATE ${X11_Xrandr_LIB})
endif()
add_test(NAME SessionStore COMMAND test_sessionstore)

# Pure logic + plain file I/O, no X11 needed at all - see the file
# itself.
add_executable(test_configmigration
    tests/test_configmigration.cpp
    src/ConfigMigration.cpp
    src/ConfigWriter.cpp
    src/ConfigSchema.cpp
    src/Utils.cpp
)
target_include_directories(test_configmigration PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME ConfigMigration COMMAND test_configmigration)

# Pure logic + plain file I/O too - no X11, no real crash involved,
# just the marker file described in RecoveryMode.h's own comment.
add_executable(test_recoverymode
    tests/test_recoverymode.cpp
    src/RecoveryMode.cpp
    src/Xdg.cpp
)
target_include_directories(test_recoverymode PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME RecoveryMode COMMAND test_recoverymode)

# Same shape as test_recoverymode just above (plain file I/O, no X11,
# no real crash needed) - covers the "crashed while locked resumes
# fully exposed" fix, see LockRecovery.h and the 0.20.4 CHANGELOG.
add_executable(test_lockrecovery
    tests/test_lockrecovery.cpp
    src/LockRecovery.cpp
    src/Xdg.cpp
)
target_include_directories(test_lockrecovery PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME LockRecovery COMMAND test_lockrecovery)

# A real functional test against actual Linux inotify (via throwaway
# temp directories) - no X11 needed, see the file itself.
add_executable(test_appdirwatcher
    tests/test_appdirwatcher.cpp
    src/AppDirWatcher.cpp
    src/Xdg.cpp
)
target_include_directories(test_appdirwatcher PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME AppDirWatcher COMMAND test_appdirwatcher)

# Pure logic + plain file I/O, no X11 needed - every scenario runs
# against a real throwaway fake filesystem tree (see the file itself
# and AutologinConfigurator.h's own `root` parameter).
add_executable(test_autologinconfigurator
    tests/test_autologinconfigurator.cpp
    src/AutologinConfigurator.cpp
    src/Utils.cpp
)
target_include_directories(test_autologinconfigurator PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME AutologinConfigurator COMMAND test_autologinconfigurator)

# Only exercises Configure()/ResolveFor() (pure parsing/priority
# logic) - never ApplyToRoot() itself, which needs a live X
# connection and isn't something a unit test should be driving anyway
# (see the file itself).
add_executable(test_wallpapermanager
    tests/test_wallpapermanager.cpp
    src/WallpaperManager.cpp
    src/ImageRenderer.cpp
    src/Config.cpp
    src/ConfigParser.cpp
    src/IniFile.cpp
    src/Monitor.cpp
    src/MonitorManager.cpp
    src/MonitorRule.cpp
    src/Workspace.cpp
    src/WorkspaceManager.cpp
    src/BSPTree.cpp
    src/BSPLeaf.cpp
    src/BSPSplit.cpp
    src/ManagedWindow.cpp
    src/LayoutEngine.cpp
    src/XConnection.cpp
    src/XAtoms.cpp
    src/Utils.cpp
)
target_include_directories(test_wallpapermanager PRIVATE ${CMAKE_SOURCE_DIR}/include ${X11_INCLUDE_DIR})
target_link_libraries(test_wallpapermanager PRIVATE ${X11_LIBRARIES} PkgConfig::Imlib2)
if(X11_Xrandr_FOUND)
    target_compile_definitions(test_wallpapermanager PRIVATE KOHIKO_HAVE_XRANDR)
    target_link_libraries(test_wallpapermanager PRIVATE ${X11_Xrandr_LIB})
endif()
add_test(NAME WallpaperManager COMMAND test_wallpapermanager)

# Pure logic + plain file I/O, no X11 needed - covers ShouldAutostart()
# and Xdg::AutostartDirs(), the two testable halves of the tray-icon
# autostart fix (see WindowManager::RunXdgAutostartEntries()'s own
# comment). Redirects XDG_CONFIG_HOME/XDG_CONFIG_DIRS/PATH to
# throwaway temp locations itself (see the file).
add_executable(test_desktopentry
    tests/test_desktopentry.cpp
    src/DesktopEntry.cpp
    src/IniFile.cpp
    src/Xdg.cpp
    src/Utils.cpp
)
target_include_directories(test_desktopentry PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME DesktopEntry COMMAND test_desktopentry)

# Pure logic + plain file I/O, no X11 needed - covers the
# "-symbolic" fallback in IconResolver::Resolve() (see IconResolver.h)
# added alongside the tray-icon autostart fix. Redirects
# HOME/XDG_DATA_HOME to a throwaway temp theme (see the file).
add_executable(test_iconresolver
    tests/test_iconresolver.cpp
    src/IconResolver.cpp
    src/IniFile.cpp
    src/Xdg.cpp
    src/Utils.cpp
)
target_include_directories(test_iconresolver PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME IconResolver COMMAND test_iconresolver)

# Pure chrono arithmetic, no X11/D-Bus/real waiting needed - the
# TickSchedule functions are header-only inline (EventLoop.h)
# specifically so this doesn't need to link EventLoop.cpp itself,
# which would drag in the whole WindowManager dependency graph.
# Regression test for the taskbar/clock "frozen until something else
# forces a repaint" fix - see CHANGELOG.md.
add_executable(test_eventloop
    tests/test_eventloop.cpp
)
target_include_directories(test_eventloop PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME EventLoop COMMAND test_eventloop)

# Pure string formatting, no WindowManager/X11 dependency - see
# WindowPlacementNotice.h's own comment. Regression test for the "app
# opens on a background workspace with zero on-screen signal" fix -
# see CHANGELOG.md.
add_executable(test_windowplacementnotice
    tests/test_windowplacementnotice.cpp
)
target_include_directories(test_windowplacementnotice PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME WindowPlacementNotice COMMAND test_windowplacementnotice)

# Pure struct/arithmetic logic, no dependency beyond Types.h itself.
# Covers Rect::ClampedTo() - existing logic, but newly depended on
# directly by the Swap-drag animation clipping fix - see CHANGELOG.md.
add_executable(test_rect_clamping
    tests/test_rect_clamping.cpp
)
target_include_directories(test_rect_clamping PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME RectClamping COMMAND test_rect_clamping)

# Pure placement/sizing/stacking arithmetic behind the native
# notification popup mechanism - no X11 dependency, same reasoning as
# test_rect_clamping just above (in fact it directly reuses
# Rect::ClampedTo()). See NotificationLayout.h's own header comment.
add_executable(test_notificationlayout
    tests/test_notificationlayout.cpp
)
target_include_directories(test_notificationlayout PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME NotificationLayout COMMAND test_notificationlayout)

# Regression tests for the device-connect/disconnect diffing logic -
# extracted into DeviceNotificationDiff.h after a real field bug
# (duplicate notifications from a sink's own monitor-source companion)
# shipped precisely because this logic used to live inline, untested,
# in kohiko-audio-tray.cpp. Pure logic, no X11/PipeWire dependency.
add_executable(test_devicenotificationdiff
    tests/test_devicenotificationdiff.cpp
)
target_include_directories(test_devicenotificationdiff PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME DeviceNotificationDiff COMMAND test_devicenotificationdiff)

# Pure logic, no live D-Bus connection or running NetworkManager
# needed - but still needs libdbus-1-dev to compile (links
# DBusClient.cpp), hence gated on DBus_FOUND rather than unconditional
# like DesktopEntry/IconResolver just above. Regression test for the
# BytesToString()/ConnectToAccessPoint() fix - see CHANGELOG.md.
if(DBus_FOUND)
    add_executable(test_networkmanagerclient
        tests/test_networkmanagerclient.cpp
        src/NetworkManagerClient.cpp
        src/DBusClient.cpp
        src/DBusValue.cpp
    )
    target_include_directories(test_networkmanagerclient PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(test_networkmanagerclient PRIVATE PkgConfig::DBus)
    add_test(NAME NetworkManagerClient COMMAND test_networkmanagerclient)

    # A real round-trip against tests/mock_networkmanager.py rather
    # than pure logic - regression test for the credential-reuse fix
    # (see CHANGELOG.md's 0.20.4 entry). The shell script itself
    # gracefully skips (exit 0) if python3-dbus/PyGObject aren't
    # available, same reasoning as gating this block on DBus_FOUND at
    # all rather than making it unconditional.
    add_executable(test_networkmanager_live_harness
        tests/live/test_networkmanager_live_harness.cpp
        src/NetworkManagerClient.cpp
        src/DBusClient.cpp
        src/DBusValue.cpp
    )
    target_include_directories(test_networkmanager_live_harness PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(test_networkmanager_live_harness PRIVATE PkgConfig::DBus)
    add_test(NAME NetworkManagerLive
        COMMAND sh ${CMAKE_SOURCE_DIR}/tests/test_networkmanager_live.sh $<TARGET_FILE:test_networkmanager_live_harness>)
endif()

# Regression test for the scroll-routing fix (see CHANGELOG.md) -
# links kohiko-desktop-shared directly rather than listing individual
# sources, since that's exactly the UiWindow/UiWidget/UiScrollView
# group this test exercises. Gated the same way as
# kohiko-audio/network/bluetooth themselves (KOHIKO_DESKTOP_APPS_AVAILABLE),
# since that shared library only exists when they do.
if(KOHIKO_DESKTOP_APPS_AVAILABLE)
    add_executable(test_scrollhittest
        tests/test_scrollhittest.cpp
    )
    target_include_directories(test_scrollhittest PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(test_scrollhittest PRIVATE kohiko-desktop-shared)
    add_test(NAME ScrollHitTest COMMAND test_scrollhittest)

# Same gate, and links src/NetworkWindow.cpp + src/NetworkManagerClient.cpp
# directly (matching kohiko-network's own add_executable() above) since
# those aren't part of kohiko-desktop-shared - FilterPastedPasswordText(),
# the only thing this test calls, lives in the former.
    add_executable(test_networkwindow
        tests/test_networkwindow.cpp
        src/NetworkWindow.cpp
        src/NetworkManagerClient.cpp
    )
    target_include_directories(test_networkwindow PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(test_networkwindow PRIVATE kohiko-desktop-shared)
    add_test(NAME NetworkWindow COMMAND test_networkwindow)

# Same gate/reasoning, for BluetoothWindow.cpp + BluezClient.cpp's own
# ComputeDetailsPanelHeight()/BuildDetailsPanel().
    add_executable(test_bluetoothwindow
        tests/test_bluetoothwindow.cpp
        src/BluetoothWindow.cpp
        src/BluezClient.cpp
    )
    target_include_directories(test_bluetoothwindow PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(test_bluetoothwindow PRIVATE kohiko-desktop-shared)
    add_test(NAME BluetoothWindow COMMAND test_bluetoothwindow)

    # Live regression test for the 0.20.10/0.20.11 native-notification
    # production bugs and the removal of the old D-Bus notification
    # path (see CHANGELOG.md) - runs the real kohiko/kohiko-audio-tray/
    # kohiko-audio binaries against a real pipewire+wireplumber session,
    # gracefully skipped by the script itself if any of that optional
    # runtime infrastructure isn't available.
    add_test(
        NAME AudioNotificationLive
        COMMAND sh ${CMAKE_SOURCE_DIR}/tests/test_audio_notification_live.sh
            $<TARGET_FILE:kohiko> $<TARGET_FILE:kohiko-audio-tray> $<TARGET_FILE:kohiko-audio>
    )
endif()

# Shell-script regression test for scripts/kohiko-session - no
# compiled binary involved, see the test file itself.
add_test(
    NAME KohikoSession
    COMMAND sh ${CMAKE_SOURCE_DIR}/tests/test_kohiko_session.sh
)

# Plain X11 client (no Kohiko headers/objects) driving
# test_xembed_dock_timeout.sh - see that script and
# tests/live/x11_test_client.cpp's own header comment. Unconditional,
# same as the kohiko target itself - only needs libX11, already a hard
# requirement for the whole project.
add_executable(x11_test_client tests/live/x11_test_client.cpp)
target_include_directories(x11_test_client PRIVATE ${X11_INCLUDE_DIR})
target_link_libraries(x11_test_client PRIVATE ${X11_LIBRARIES})

# Live regression test for the 0.20.4 XEmbed dock-timeout fallback fix
# (see CHANGELOG.md) - runs the real `kohiko` target as an actual
# window manager under Xvfb, gracefully skipped by the script itself
# if Xvfb/xwininfo aren't available.
add_test(
    NAME XEmbedDockTimeout
    COMMAND sh ${CMAKE_SOURCE_DIR}/tests/test_xembed_dock_timeout.sh
        $<TARGET_FILE:kohiko> $<TARGET_FILE:x11_test_client>
)

# --- install ------------------------------------------------------------------------

install(TARGETS kohiko kohikoctl kohiko-settings RUNTIME DESTINATION bin)
install(FILES config/default.conf DESTINATION share/kohiko)
file(GLOB WALLPAPER_ASSETS ${CMAKE_SOURCE_DIR}/assets/wallpapers/*.png)
install(FILES ${WALLPAPER_ASSETS} DESTINATION share/kohiko/wallpapers)
install(FILES desktop/kohiko-settings.desktop DESTINATION share/applications)
install(FILES assets/icons/kohiko-settings.svg DESTINATION share/icons/hicolor/scalable/apps)
# See the Makefile's own install: target for why this second copy,
# at an absolute (not ${CMAKE_INSTALL_PREFIX}-relative) path, exists -
# IconResolver's guaranteed-checked pixmaps fallback, working around
# its theme search only reading one base directory per theme.
install(FILES assets/icons/kohiko-settings.svg DESTINATION /usr/share/pixmaps)

# scripts/kohiko-session - the actual session entry point (see its
# own header comment); the .desktop entry below execs this, not
# `kohiko` directly. install(PROGRAMS ...), not FILES, to preserve
# the executable bit on a plain shell script the same way a compiled
# TARGETS install already would.
install(PROGRAMS scripts/kohiko-session DESTINATION bin)

# Deliberately an absolute, non-${CMAKE_INSTALL_PREFIX}-relative path -
# same reasoning, and the same kind of deliberate exception, as the
# pixmaps install just above: display managers (LightDM/SDDM/GDM) scan
# the fixed, well-known /usr/share/xsessions regardless of where
# Kohiko itself is installed.
install(FILES desktop/kohiko.desktop DESTINATION /usr/share/xsessions)

if(KOHIKO_DESKTOP_APPS_AVAILABLE)
    install(TARGETS
        kohiko-audio kohiko-network kohiko-bluetooth
        kohiko-audio-tray kohiko-network-tray kohiko-bluetooth-tray
        RUNTIME DESTINATION bin
    )

    install(FILES
        desktop/kohiko-audio.desktop
        desktop/kohiko-network.desktop
        desktop/kohiko-bluetooth.desktop
        DESTINATION share/applications
    )

    install(FILES
        assets/icons/kohiko-audio.svg
        assets/icons/kohiko-network.svg
        assets/icons/kohiko-bluetooth.svg
        DESTINATION share/icons/hicolor/scalable/apps
    )
    install(FILES
        assets/icons/kohiko-audio.svg
        assets/icons/kohiko-network.svg
        assets/icons/kohiko-bluetooth.svg
        DESTINATION /usr/share/pixmaps
    )

    # System-wide autostart per the freedesktop Desktop Application
    # Autostart Specification - read from $XDG_CONFIG_DIRS/autostart,
    # whose default (/etc/xdg) is what this installs into; a per-user
    # override still works normally by dropping a same-named file in
    # ~/.config/autostart.
    install(FILES
        desktop/kohiko-audio-tray.desktop
        desktop/kohiko-network-tray.desktop
        desktop/kohiko-bluetooth-tray.desktop
        DESTINATION /etc/xdg/autostart
    )
endif()
