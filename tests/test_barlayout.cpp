// BarLayout: where the taskbar's workspace numbers are, and which one a
// click at a given x/y belongs to. Pure arithmetic, no X server - the
// same functions Bar::Redraw() draws with and Bar::WorkspaceAt() hits
// with, so a drawn label and its clickable cell cannot drift apart.

#include "BarLayout.h"

#include <cstdio>

using namespace Kohiko;

static int g_pass = 0;
static int g_fail = 0;

static void Check(bool condition, const char* message)
{
    if (condition) { ++g_pass; std::printf("  PASS: %s\n", message); }
    else           { ++g_fail; std::printf("  FAIL: %s\n", message); }
}

int main()
{
    constexpr int kHeight = 26;

    std::printf("Every workspace number owns exactly one cell, and the cells tile the strip:\n");
    {
        bool tiles = true, labelInside = true, centreHits = true, edgesHit = true;

        for (int count = 1; count <= 10; ++count)
        {
            for (int i = 1; i <= count; ++i)
            {
                Rect cell = BarLayout::WorkspaceCell(i, kHeight);
                int labelX = BarLayout::WorkspaceLabelX(i);

                // adjacent cells share an edge exactly: no gap, no overlap
                if (i > 1 && BarLayout::WorkspaceCell(i - 1, kHeight).Right() != cell.x)
                    tiles = false;

                // the drawn label (up to two 8px glyphs) is inside its own cell
                if (labelX < cell.x || labelX + 16 > cell.Right())
                    labelInside = false;

                if (BarLayout::WorkspaceAt(cell.CenterX(), kHeight / 2, count, kHeight) != i)
                    centreHits = false;

                // first and last pixel of the cell belong to it, the pixel
                // after the last one belongs to the NEXT number (or nothing)
                if (BarLayout::WorkspaceAt(cell.x, 0, count, kHeight) != i ||
                    BarLayout::WorkspaceAt(cell.Right() - 1, kHeight - 1, count, kHeight) != i)
                    edgesHit = false;

                int after = BarLayout::WorkspaceAt(cell.Right(), 5, count, kHeight);
                if (after != (i < count ? i + 1 : 0))
                    edgesHit = false;
            }
        }

        Check(tiles, "adjacent cells meet exactly (no gap, no overlap) for 1..10 workspaces");
        Check(labelInside, "each drawn label (even two digits) lies inside its own cell");
        Check(centreHits, "a click at a cell's centre selects that workspace, for every count 1..10");
        Check(edgesHit, "the first/last pixel of a cell are its own; the next pixel is its neighbour (or nothing)");
    }

    std::printf("\nClicks that are not on a workspace number do nothing:\n");
    {
        Check(BarLayout::WorkspaceAt(0, 10, 4, kHeight) == 0, "left of the first cell");
        Check(BarLayout::WorkspaceAt(BarLayout::kWorkspaceFirstX - BarLayout::kWorkspaceLeftPad - 1, 10, 4, kHeight) == 0, "one pixel before the first cell");
        Check(BarLayout::WorkspaceAt(BarLayout::WorkspaceCell(4, kHeight).Right(), 10, 4, kHeight) == 0, "right of the last configured workspace (count 4)");
        Check(BarLayout::WorkspaceAt(500, 10, 4, kHeight) == 0, "far right (clock / tray / power area)");
        Check(BarLayout::WorkspaceAt(20, -1, 4, kHeight) == 0, "above the bar");
        Check(BarLayout::WorkspaceAt(20, kHeight, 4, kHeight) == 0, "below the bar");
        Check(BarLayout::WorkspaceAt(20, 10, 0, kHeight) == 0, "no workspaces configured");
        Check(BarLayout::WorkspaceAt(-5, 10, 4, kHeight) == 0, "negative x");
    }

    std::printf("\nA different workspace.count changes only how many cells exist:\n");
    {
        Check(BarLayout::WorkspaceAt(BarLayout::WorkspaceCell(9, kHeight).CenterX(), 10, 9, kHeight) == 9, "workspace 9 is clickable with count 9");
        Check(BarLayout::WorkspaceAt(BarLayout::WorkspaceCell(9, kHeight).CenterX(), 10, 4, kHeight) == 0, "...and not with count 4");
        Check(BarLayout::WorkspaceAt(BarLayout::WorkspaceCell(10, kHeight).CenterX(), 10, 10, kHeight) == 10, "workspace 10 (two digits) is clickable with count 10");
        Check(BarLayout::WorkspaceLabelX(1) == 10 && BarLayout::WorkspaceLabelX(2) == 32, "label positions are unchanged from before the cells existed (10, 32, ...)");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
