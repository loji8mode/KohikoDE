#pragma once

#include <string>

// The text shown on the bar when Manage() places a plain top-level
// window on a workspace nothing is currently displaying - see
// Manage()'s own comment (WindowManager.cpp) for why that placement
// itself is correct, existing, deliberate behaviour and this
// notification is the actual fix: without it, a `workspace<N>=`
// autostart line, a `windowrule=workspace:N`, or a learned
// adaptive-placement habit sending a freshly launched application
// (Telegram and Discord, sharing one `workspace2=` line in the
// shipped default config, were the reproducer - see the 0.20.4
// CHANGELOG entry) to a background workspace is completely
// indistinguishable, from the user's side, from that application
// simply failing to open at all. Kept free of any WindowManager/X11/
// ManagedWindow dependency, purely so it's directly testable
// (tests/test_windowplacementnotice.cpp) without linking the rest of
// WindowManager.cpp - the same reasoning as EventLoop.h's
// TickSchedule namespace.
namespace Kohiko::WindowPlacementNotice
{

// `appName` is whatever WindowManager::AppClassKey() resolved (WM_CLASS
// class, falling back to instance, falling back to "" if the window
// set neither) - "" is rare in practice (well-behaved clients always
// set WM_CLASS) but not impossible, and "A window opened on
// workspace 3" reads fine as a fallback where an empty string
// wouldn't. `workspace` is 1-based, matching every other workspace
// number shown anywhere else in Kohiko's own UI (the bar's own
// workspace list, kohikoctl, ...), not 0-based.
inline std::string BackgroundPlacementText(const std::string& appName, int workspace)
{
    return (appName.empty() ? std::string("A window") : appName) +
        " opened on workspace " + std::to_string(workspace);
}

}
