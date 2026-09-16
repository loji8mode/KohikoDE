// Standalone correctness test for Rect::ClampedTo() (Types.h) - no
// X11 display needed, pure struct/arithmetic logic.
//
// Written primarily to cover the exact scenario behind the 0.20.4
// Swap-drag animation fix (see CHANGELOG.md): a rect whose position
// legitimately goes off-screen (dragging a window carried at a fixed
// grab-offset from the cursor, past the edge of the screen) but whose
// *size* already fits comfortably within bounds - ClampedTo() should
// reposition it back on-screen without touching width/height at all,
// since there's nothing forcing a resize here, only a reposition.
// ClampedTo() itself already existed and is used elsewhere (floating
// windows); this test file didn't, before the drag fix depended on
// it directly for the first time.

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
    Rect bounds{0, 0, 1280, 800};

    std::printf("Rect::ClampedTo() tests:\n");

    std::printf("-- Already fully inside bounds: untouched --\n");
    {
        Rect r{100, 100, 400, 300};
        Rect clamped = r.ClampedTo(bounds);
        Check(clamped == r, "a rect already comfortably inside bounds comes back unchanged");
    }

    std::printf("\n-- THE regression guard: off-screen position, size already fits - "
                 "reposition only, size untouched (the Swap-drag animation fix) --\n");
    {
        // Exactly the shape of bug this was written for: a window
        // carried at a fixed grab-offset from the cursor can end up
        // with a negative x once dragged far enough left - harmless
        // while actually dragging, wrong as an animation's starting
        // rect (see EndSwapDrag()'s own comment, WindowManager.cpp).
        Rect r{-108, 50, 628, 628};
        Rect clamped = r.ClampedTo(bounds);

        Check(clamped.width == 628 && clamped.height == 628,
              "size is completely untouched - it already fit within bounds, nothing "
              "here should ever shrink it");
        Check(clamped.x == 0,
              "x is pulled back to the left edge of bounds rather than left negative "
              "- this exact repositioning-without-resizing is what fixed the visible "
              "clipped/missing edge in the live Xvfb reproduction");
        Check(clamped.y == 50, "y was already in-bounds (50 + 628 <= 800) and is left alone");
    }

    std::printf("\n-- Off the right/bottom edge instead of left/top --\n");
    {
        Rect r{1100, 700, 300, 200};
        Rect clamped = r.ClampedTo(bounds);

        Check(clamped.width == 300 && clamped.height == 200, "size untouched here too");
        Check(clamped.x == bounds.Right() - 300, "x pulled back so the right edge lands exactly on bounds' own right edge");
        Check(clamped.y == bounds.Bottom() - 200, "y pulled back so the bottom edge lands exactly on bounds' own bottom edge");
    }

    std::printf("\n-- Oversized rect: shrunk to fit, not just repositioned --\n");
    {
        Rect r{50, 50, 2000, 1500};
        Rect clamped = r.ClampedTo(bounds);

        Check(clamped.width == bounds.width && clamped.height == bounds.height,
              "a rect wider/taller than bounds itself is shrunk down to exactly fit");
        Check(clamped.x == bounds.x && clamped.y == bounds.y,
              "...and repositioned to bounds' own origin once shrunk to fit exactly");
    }

    std::printf("\n-- borderWidth accounted for (window border is outside width/height, "
                 "see the function's own comment) --\n");
    {
        // A 628-wide window with a 2px border actually occupies 632px
        // on screen (628 + 2*2) - clamping needs to leave room for
        // that, not just the bare content width.
        Rect r{1270, 100, 628, 628};
        Rect clamped = r.ClampedTo(bounds, 2);

        Check(clamped.width == 628 && clamped.height == 628,
              "content width/height still untouched - border isn't part of them");
        Check(clamped.x == bounds.Right() - 628 - 2 * 2,
              "x leaves room for the border on both sides when pulling back onto bounds");
    }

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    return 0;
}
