// Unit test for NotificationLayout.h - the pure placement/sizing math
// behind the new native notification popup mechanism (see
// NotificationPopup.h/NotificationCenter.h). No X11 dependency at
// all, same "test the decision, not the whole pipeline around it"
// approach test_windowplacementnotice.cpp and test_rect_clamping.cpp
// already take - see those files' own header comments. The live,
// real-window behaviour (override-redirect, correct EWMH type, no
// focus stolen, actual on-screen position, expiry/cleanup) is covered
// separately by tests/test_notificationcenter.cpp, which needs a real
// (even if headless/Xvfb) X server; this file needs none.

#include "NotificationLayout.h"
#include "Types.h"

#include <cstdio>
#include <cstdlib>

using namespace Kohiko;

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
    std::printf("NotificationLayout tests (pure logic, no X11):\n");

    std::printf("-- The spec's exact 2.5-second lifetime --\n");
    {
        Check(NotificationLayout::kDefaultLifetime == std::chrono::milliseconds(2500),
              "kDefaultLifetime - what every real Post() caller uses unless it overrides it - is exactly 2500ms, "
              "per \"Automatically disappear after exactly 2.5 seconds\"");
    }

    std::printf("\n-- ComputeWidth(): auto-sized to content, clamped to a sane range --\n");
    {
        Check(NotificationLayout::ComputeWidth(0) == NotificationLayout::kMinWidth,
              "a zero-width text still produces at least kMinWidth (never a degenerate/invisible card)");

        int mid = 200; // textWidth + 2*kHorizontalPadding (232) sits comfortably inside [kMinWidth, kMaxWidth] - genuinely exercises the unclamped pass-through, not the min-width floor
        Check(NotificationLayout::ComputeWidth(mid) == mid + NotificationLayout::kHorizontalPadding * 2,
              "a normal-length device name's width is exactly textWidth plus padding on both sides (auto-sized "
              "to content, per the spec)");

        Check(NotificationLayout::ComputeWidth(10000) == NotificationLayout::kMaxWidth,
              "an unreasonably long piece of text is clamped to kMaxWidth rather than spanning arbitrarily far "
              "across the screen (\"text should remain readable for longer device names\", not \"unbounded\")");
    }

    std::printf("\n-- ComputeHeight(): auto-sized to the font's own line height --\n");
    {
        int fontHeight = 16;
        Check(NotificationLayout::ComputeHeight(fontHeight) == fontHeight + NotificationLayout::kVerticalPadding * 2,
              "height is exactly the font's line height plus padding on both top and bottom");
    }

    std::printf("\n-- ComputeRect(): bottom-right placement, with a sensible margin --\n");
    {
        Rect monitor{0, 0, 1920, 1080};
        int width = 240;
        int height = 48;

        Rect rect = NotificationLayout::ComputeRect(monitor, width, height, 0);

        Check(rect.width == width && rect.height == height,
              "the first (only) notification's own size is left exactly as given");

        Check(rect.Right() == monitor.Right() - NotificationLayout::kMargin,
              "its right edge sits exactly kMargin pixels in from the monitor's right edge");

        Check(rect.Bottom() == monitor.Bottom() - NotificationLayout::kMargin,
              "its bottom edge sits exactly kMargin pixels in from the monitor's bottom edge, i.e. bottom-right "
              "placement with no other notification stacked above it yet");
    }

    std::printf("\n-- ComputeRect(): a second, stacked notification never overlaps the first --\n");
    {
        Rect monitor{0, 0, 1920, 1080};
        int width = 240;
        int firstHeight = 48;
        int secondHeight = 60;

        Rect first = NotificationLayout::ComputeRect(monitor, width, firstHeight, 0);

        int stackOffset = firstHeight + NotificationLayout::kGap;
        Rect second = NotificationLayout::ComputeRect(monitor, width, secondHeight, stackOffset);

        Check(second.Bottom() <= first.y,
              "the second (older, stacked-above) notification's bottom edge sits at or above the first "
              "(newest) notification's top edge - \"place them without overlap\", per the spec");

        Check(first.y - second.Bottom() == NotificationLayout::kGap,
              "there is exactly kGap pixels of breathing room between the two stacked cards, not zero (which "
              "would read as one solid block) and not some other arbitrary amount");

        Check(second.x == first.x && second.width == first.width,
              "both stacked notifications share the same horizontal position/width (a clean vertical stack, "
              "not drifting sideways)");
    }

    std::printf("\n-- ComputeRect(): never placed off-screen, even on a monitor too small/short for it --\n");
    {
        // A monitor deliberately smaller than the card that would
        // otherwise be requested - the "multiple window/screen sizes
        // where practical" the spec asks be tested.
        Rect tinyMonitor{100, 50, 200, 120};

        Rect rect = NotificationLayout::ComputeRect(tinyMonitor, 240, 48, 0);

        Check(rect.x >= tinyMonitor.x && rect.y >= tinyMonitor.y,
              "even an oversized card's top-left never lands outside the monitor's own top-left bound");

        Check(rect.Right() <= tinyMonitor.Right() && rect.Bottom() <= tinyMonitor.Bottom(),
              "...nor does its bottom-right corner ever land past the monitor's own bottom-right bound "
              "(ClampedTo() clamping both size and position together)");
    }

    std::printf("\n-- ComputeRect(): a second monitor (multi-monitor session) is completely independent --\n");
    {
        Rect monitorA{0, 0, 1920, 1080};
        Rect monitorB{1920, 0, 1280, 1024}; // to the right, a different size - a realistic mixed-DPI/size setup

        Rect onA = NotificationLayout::ComputeRect(monitorA, 240, 48, 0);
        Rect onB = NotificationLayout::ComputeRect(monitorB, 240, 48, 0);

        Check(onA.Right() == monitorA.Right() - NotificationLayout::kMargin,
              "the notification on monitor A is placed against monitor A's own right edge");

        Check(onB.Right() == monitorB.Right() - NotificationLayout::kMargin,
              "...and the one on monitor B is placed against monitor B's own right edge, not monitor A's "
              "(each monitor's notifications are positioned independently) - \"work correctly on all "
              "workspaces/monitors\", per the spec");
    }

    std::printf("\n-- kMaxStacked: a sane, finite cap --\n");
    {
        Check(NotificationLayout::kMaxStacked >= 1 && NotificationLayout::kMaxStacked <= 20,
              "kMaxStacked is a small, sane, finite number - enough to actually show several at once, not "
              "an accidental 0 (which would silently drop every notification) or an unbounded-in-practice value");
    }

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    return 0;
}
