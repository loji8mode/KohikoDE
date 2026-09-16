// Regression test for the 0.20.4 kohiko-audio/network/bluetooth
// scroll-routing fix (see CHANGELOG.md).
//
// The real bug: mouse-wheel scrolling a device/network/Bluetooth list
// only worked when the pointer happened to be over "dead space"
// between rows. Hovering over a Slider, a Button ("Mute"/"Forget"/
// "Connect"), a ListRow, or a TextField and scrolling did nothing,
// because UiWindow::HandleEvent()'s Button4/5 dispatch used the same
// HitTest() as ordinary clicks - which, correctly for a click, always
// resolves to the *most specific* WantsInput() widget under the
// pointer - and called OnScroll() on THAT widget. Only ScrollView
// overrides OnScroll(); everything else silently no-ops it via the
// Widget base class default. In a dense list, "dead space" not
// covered by some interactive control is a small minority of the
// visible area, so this read as "scrolling is broken" in practice,
// and made rows below the fold effectively unreachable on any window
// short enough for content to overflow (easy to hit at the documented
// ~420x360 floor - see AudioWindow.h).
//
// The fix: a *separate* traversal, HitTestForScroll(), that looks for
// the nearest enclosing widget with WantsScroll() == true (only
// ScrollView) rather than the deepest WantsInput() one - so a wheel
// event finds the scrollable container regardless of what's drawn on
// top of it at that pixel. This file tests that traversal directly,
// against a small hand-built tree that reproduces the exact "Button
// sitting inside a ScrollView's content" shape, without needing a
// live X11 Display/UiWindow - see UiWindow.h's comment on why
// HitTest()/HitTestForScroll() are `static`.
//
// Also covers ScrollView::ComputeThumbRect() - the overlay scrollbar
// thumb added alongside the routing fix, since a working wheel with
// no visual cue that there's more content below the fold is only a
// half fix (see UiScrollView.cpp's own comment).

#include "UiWidget.h"
#include "UiScrollView.h"
#include "UiWindow.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

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
    std::printf("UiWindow::HitTestForScroll() tests:\n");

    // Reproduces the real widget shape: a ScrollView viewport (only
    // 100px tall) holding a 300px-tall content Widget, with a Button
    // ("Mute", in the real case) positioned at y=[10,40] - fully
    // inside the visible viewport, at a point a real cursor would
    // sit on to scroll the list it's part of.
    {
        auto scrollView = std::make_unique<ScrollView>();
        scrollView->bounds = Rect{ 0, 0, 200, 100 };

        auto content = std::make_unique<Widget>();
        content->bounds = Rect{ 0, 0, 200, 300 };

        auto button = std::make_unique<Button>();
        button->label = "Mute";
        button->bounds = Rect{ 10, 10, 80, 30 };
        Widget* buttonPtr = content->AddChild(std::move(button));

        scrollView->SetContent(std::move(content));
        ScrollView* scrollViewPtr = scrollView.get();

        auto root = std::make_unique<Widget>();
        root->bounds = Rect{ 0, 0, 200, 100 };
        root->AddChild(std::move(scrollView));

        const Point overButton{ 30, 20 };

        std::printf("-- THE regression guard: scrolling over an interactive widget --\n");

        bool hitSomething = false;
        Widget* clickTarget = UiWindow::HitTest(root.get(), overButton, hitSomething);
        Check(clickTarget == buttonPtr,
              "sanity check: ordinary click hit-testing still resolves to the Button here "
              "(proves this scenario really does put the Button under the pointer, matching "
              "the real bug reproduction, not some other widget)");

        Widget* scrollTarget = UiWindow::HitTestForScroll(root.get(), overButton);
        Check(scrollTarget == scrollViewPtr,
              "HitTestForScroll(), at the exact same point, returns the enclosing ScrollView "
              "instead - this is the fix: before it existed, dispatch used HitTest() for "
              "scroll too, got the Button back, and called OnScroll() on it - a silent no-op, "
              "since only ScrollView overrides OnScroll()");
        Check(scrollTarget != clickTarget,
              "...and that's a genuinely different widget than the click target, not a "
              "coincidental match - the fix only matters because these two differ");

        std::printf("-- A point over genuine dead space (no child there) still finds the ScrollView --\n");
        Widget* deadSpaceTarget = UiWindow::HitTestForScroll(root.get(), Point{ 150, 80 });
        Check(deadSpaceTarget == scrollViewPtr,
              "scrolling over an area with no interactive child still hits the ScrollView, "
              "same as before this fix - the fix doesn't regress the one case that used to work");

        std::printf("-- A point outside the ScrollView's own bounds hits nothing --\n");
        Widget* outsideTarget = UiWindow::HitTestForScroll(root.get(), Point{ 190, 150 });
        Check(outsideTarget == nullptr,
              "a point below the 100px-tall viewport (y=150) isn't inside the ScrollView's own "
              "clipped bounds, so it correctly finds nothing to scroll, rather than matching "
              "some scrolled-off child's stale absolute coordinates");
    }

    std::printf("\n-- A Button that ISN'T inside any ScrollView is simply not scrollable --\n");
    {
        auto root = std::make_unique<Widget>();
        root->bounds = Rect{ 0, 0, 200, 100 };

        auto button = std::make_unique<Button>();
        button->label = "Advanced Settings";
        button->bounds = Rect{ 10, 10, 100, 30 };
        root->AddChild(std::move(button));

        Widget* scrollTarget = UiWindow::HitTestForScroll(root.get(), Point{ 30, 20 });
        Check(scrollTarget == nullptr,
              "no ScrollView anywhere in the tree means no scroll target, however many plain "
              "WantsInput() widgets sit at that point - HitTestForScroll() never falls back to "
              "an ordinary interactive widget the way the old code accidentally relied on");
    }

    std::printf("\nScrollView::ComputeThumbRect() tests:\n");

    std::printf("-- Content shorter than the viewport: no thumb --\n");
    {
        Rect viewport{ 0, 0, 200, 500 };
        Rect thumb = ScrollView::ComputeThumbRect(viewport, /*contentHeight=*/300, /*scrollOffset=*/0);
        Check(thumb.height == 0,
              "content (300) fitting entirely within the viewport (500) means nothing "
              "overflows, so there's genuinely nothing to indicate - a zero-height thumb "
              "tells Draw() to skip drawing anything");
    }

    std::printf("-- Content taller than the viewport, scrolled to the top --\n");
    {
        Rect viewport{ 0, 0, 200, 100 };
        Rect thumb = ScrollView::ComputeThumbRect(viewport, /*contentHeight=*/300, /*scrollOffset=*/0);
        Check(thumb.height > 0, "content (300) taller than the viewport (100) does produce a thumb");
        Check(thumb.y == viewport.y,
              "scrolled all the way to the top (offset 0) puts the thumb flush with the "
              "viewport's own top edge");
        Check(thumb.height < viewport.height,
              "the thumb itself is shorter than the full track - it's an indicator of the "
              "visible fraction, not a full-height bar");
        Check(thumb.Right() <= viewport.Right() && thumb.x >= viewport.x,
              "the thumb sits horizontally inside the viewport, not overflowing either edge");
    }

    std::printf("-- Content taller than the viewport, scrolled to the bottom --\n");
    {
        Rect viewport{ 0, 0, 200, 100 };
        const int contentHeight = 300;
        const int maxOffset = contentHeight - viewport.height; // 200
        Rect thumb = ScrollView::ComputeThumbRect(viewport, contentHeight, maxOffset);
        Check(thumb.Bottom() == viewport.Bottom(),
              "scrolled all the way to the bottom (the real OnScroll() clamp's own maxOffset) "
              "puts the thumb's bottom edge flush with the viewport's own bottom edge - it "
              "never overshoots past the track even though the arithmetic is integer division");
    }

    std::printf("-- A very tall content relative to viewport: thumb clamped to a usable minimum --\n");
    {
        Rect viewport{ 0, 0, 200, 400 };
        Rect thumb = ScrollView::ComputeThumbRect(viewport, /*contentHeight=*/20000, /*scrollOffset=*/0);
        Check(thumb.height == 24,
              "an extreme content/viewport ratio (a hundred-network Wi-Fi list, say) would "
              "otherwise compute a several-pixel sliver - clamped to a 24px floor so it stays "
              "visible and grabbable rather than vanishing");
    }

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    return 0;
}
