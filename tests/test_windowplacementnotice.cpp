// Regression test for the "app opens on a background workspace with
// zero on-screen signal, indistinguishable from just not opening at
// all" bug - see WindowPlacementNotice.h's own comment, and the
// 0.20.4 CHANGELOG entry, for the full story and the live-session
// reproduction (Telegram/Discord sharing a `workspace2=` autostart
// line in the shipped default config) that found it.
//
// This only covers the pure text-formatting logic
// (WindowPlacementNotice::BackgroundPlacementText()) directly, with
// no WindowManager/X11 dependency - it cannot exercise the actual
// *decision* of when Manage() calls it (that needs a real
// ManagedWindow/MonitorManager/WorkspaceManager, and was instead
// verified by direct execution: a live kohiko-session under Xvfb,
// launching an application autostarted to a workspace not currently
// visible, confirming the notification actually renders in the bar -
// see the CHANGELOG for that verification's specifics).

#include "WindowPlacementNotice.h"

#include <cstdio>
#include <cstdlib>

using namespace Kohiko::WindowPlacementNotice;

namespace
{

int g_pass = 0;

void Check(bool condition, const char* what)
{
    if (condition)
    {
        ++g_pass;
        std::printf("  PASS: %s\n", what);
    }
    else
    {
        std::printf("  FAIL: %s\n", what);
        std::exit(1);
    }
}

}

int main()
{
    std::printf("BackgroundPlacementText() tests:\n");

    Check(BackgroundPlacementText("Telegram", 2) == "Telegram opened on workspace 2",
          "an ordinary WM_CLASS name is used verbatim, workspace number appended");

    Check(BackgroundPlacementText("discord", 2) == "discord opened on workspace 2",
          "a second, differently-cased app sharing the same workspace formats independently "
          "(the actual Telegram/Discord shared-`workspace2=`-line reproduction)");

    Check(BackgroundPlacementText("", 3) == "A window opened on workspace 3",
          "an empty appName (WM_CLASS unset on both class and instance - rare, but a real "
          "client can do it) falls back to \"A window\" rather than an empty/broken message");

    Check(BackgroundPlacementText("Steam", 1) == "Steam opened on workspace 1",
          "workspace 1 formats the same as any other - no off-by-one in the number itself");

    Check(BackgroundPlacementText("X", 10) == "X opened on workspace 10",
          "a double-digit workspace number formats correctly, not truncated or misparsed");

    Check(BackgroundPlacementText("Zen Browser", 4).find("Zen Browser") == 0,
          "an appName containing a space (a real WM_CLASS value, e.g. a Flatpak's) is used "
          "as-is, not split or mangled");

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    return 0;
}
