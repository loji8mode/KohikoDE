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
pkg_check_modules(DBus dbus-1)

# --- kohiko: the window manager ------------------------------------------------

file(GLOB KOHIKO_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)

# SettingsWindow.cpp/ConfigWriter.cpp exist only for kohiko-settings,
# its own separate binary/process (see include/SettingsWindow.h's own
# header comment for why) - excluded here so `kohiko` itself stays
# free of GUI code it never runs, per "keep the WM lightweight".
list(REMOVE_ITEM KOHIKO_SOURCES
    ${CMAKE_SOURCE_DIR}/src/SettingsWindow.cpp
    ${CMAKE_SOURCE_DIR}/src/ConfigWriter.cpp
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

add_executable(kohikoctl tools/kohikoctl.cpp src/IpcPath.cpp)
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

# --- install ------------------------------------------------------------------------

install(TARGETS kohiko kohikoctl kohiko-settings RUNTIME DESTINATION bin)
install(FILES config/default.conf DESTINATION share/kohiko)
install(FILES desktop/kohiko-settings.desktop DESTINATION share/applications)
install(FILES assets/icons/kohiko-settings.svg DESTINATION share/icons/hicolor/scalable/apps)
# See the Makefile's own install: target for why this second copy,
# at an absolute (not ${CMAKE_INSTALL_PREFIX}-relative) path, exists -
# IconResolver's guaranteed-checked pixmaps fallback, working around
# its theme search only reading one base directory per theme.
install(FILES assets/icons/kohiko-settings.svg DESTINATION /usr/share/pixmaps)
