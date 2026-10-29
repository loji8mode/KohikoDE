// Pure-logic regression test for the taskbar's clickable workspace
// numbers: the hit-test geometry (Bar::WorkspaceAtX and friends) that the
// bar's drawing code and WindowManager's click handler share. No X server
// needed. The end-to-end behaviour (a real click on the real bar switches
// the real workspace, via the same function the keyboard uses) is in
// tests/test_taskbar_live.sh.
//
// What matters about hit boxes: every x belongs to AT MOST ONE indicator
// (no click can hit a neighbour), the boxes are contiguous across the row,
// each glyph sits inside its own box, and nothing outside the row is claimed.

#include "Bar.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using Kohiko::Bar;

static int g_pass = 0;

static void Check(bool ok, const std::string& what)
{
    if (ok) { ++g_pass; std::printf("  PASS: %s\n", what.c_str()); }
    else    { std::printf("  FAIL: %s\n", what.c_str()); std::exit(1); }
}

int main()
{
    std::printf("-- Every indicator is hit at its own glyph --\n");
    for (int count : {1, 4, 9, 10})
    {
        bool ok = true;
        for (int i = 1; i <= count; ++i)
        {
            ok = ok && Bar::WorkspaceAtX(Bar::WorkspaceGlyphX(i), count) == i;
            ok = ok && Bar::WorkspaceAtX(Bar::WorkspaceGlyphX(i) + 7, count) == i; // right side of a glyph
        }
        Check(ok, "with " + std::to_string(count) + " workspaces, clicking each number's glyph selects exactly that workspace");
    }

    std::printf("-- Boundaries: no overlap, no gaps, no hit on a neighbour --\n");
    {
        const int count = 9;
        bool ok = true;
        for (int i = 1; i <= count; ++i)
        {
            int left = Bar::WorkspaceSlotLeft(i);
            ok = ok && Bar::WorkspaceAtX(left, count) == i;                              // first pixel of its box
            ok = ok && Bar::WorkspaceAtX(left + Bar::kWorkspaceSlotWidth - 1, count) == i; // last pixel of its box
            if (i > 1)
                ok = ok && Bar::WorkspaceAtX(left - 1, count) == i - 1;                   // pixel just before = previous box
        }
        Check(ok, "the first and last pixel of each box select it; the pixel just left of it selects the previous one");

        int claimedBy = 0;
        bool singleOwner = true;
        int last = 0;
        bool monotonic = true;
        for (int x = -40; x < 400; ++x)
        {
            int w = Bar::WorkspaceAtX(x, count);
            if (w != 0) { ++claimedBy; monotonic = monotonic && w >= last; last = w; }
        }
        Check(singleOwner && monotonic, "across the whole row the answer is a single, non-decreasing workspace per x");
        Check(claimedBy == count * Bar::kWorkspaceSlotWidth, "the row claims exactly count x slot-width pixels: contiguous, no gaps, no extras");
    }

    std::printf("-- Nothing outside the row is claimed --\n");
    {
        Check(Bar::WorkspaceAtX(Bar::WorkspaceSlotLeft(1) - 1, 9) == 0, "just left of the first indicator: no workspace");
        Check(Bar::WorkspaceAtX(0, 9) == 0 && Bar::WorkspaceAtX(-5, 9) == 0, "the bar's left edge / negative x: no workspace");
        Check(Bar::WorkspaceAtX(Bar::WorkspaceSlotLeft(9) + Bar::kWorkspaceSlotWidth, 9) == 0,
              "just right of the last indicator (where notification text / [S] / [N] start): no workspace");
        Check(Bar::WorkspaceAtX(5000, 9) == 0, "far right (clock, tray, [Power]): no workspace");
        Check(Bar::WorkspaceAtX(Bar::WorkspaceGlyphX(1), 0) == 0, "zero workspaces: nothing to hit");
    }

    std::printf("-- Fewer workspaces than the row could hold --\n");
    {
        Check(Bar::WorkspaceAtX(Bar::WorkspaceGlyphX(5), 4) == 0, "an indicator that is not configured (5 of 4) is not clickable");
        Check(Bar::WorkspaceAtX(Bar::WorkspaceGlyphX(4), 4) == 4, "the last configured one still is");
    }

    std::printf("-- Two-digit workspace numbers fit their own box --\n");
    {
        // Two glyphs at ~8 px each: 16 px from the glyph origin still lands in box 10.
        Check(Bar::WorkspaceAtX(Bar::WorkspaceGlyphX(10) + 16, 10) == 10, "the right half of '10' selects workspace 10, not a neighbour");
        Check(Bar::WorkspaceAtX(Bar::WorkspaceSlotLeft(10) - 1, 10) == 9, "the pixel just before '10's box still belongs to '9'");
        Check(Bar::WorkspaceAtX(Bar::WorkspaceGlyphX(10) - 1, 10) == 10, "the glyph is inset into its own box, so the pixel just left of the glyph is still '10'");
    }

    std::printf("\nAll %d checks passed.\n", g_pass);
    return 0;
}
