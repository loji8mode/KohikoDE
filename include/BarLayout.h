#pragma once

#include "Types.h"

namespace Kohiko::BarLayout
{

// Where the taskbar's workspace numbers are, in ONE place: Bar::Redraw()
// draws each label at WorkspaceLabelX(i), and a click is mapped back to a
// workspace with WorkspaceAt() - so what is drawn and what is clickable
// can never drift apart, and the hit test is plain arithmetic that can be
// tested without an X server.
//
// Every number owns a clickable cell exactly kWorkspaceStep wide that
// starts kWorkspaceLeftPad before its label and ends where the next
// cell begins: the cells tile the strip with no gap and no overlap, so a
// click is never ambiguous between two neighbours and never lands in a
// dead pixel between them, and a two-digit label ("10") still fits
// inside its own cell.
constexpr int kWorkspaceFirstX  = 10; // x of workspace 1's label
constexpr int kWorkspaceStep    = 22; // label-to-label distance = cell width
constexpr int kWorkspaceLeftPad = 4;  // cell starts this far left of its label

// x at which workspace `index` (1-based) is drawn.
inline int WorkspaceLabelX(int index)
{
    return kWorkspaceFirstX + (index - 1) * kWorkspaceStep;
}

// The clickable cell of workspace `index` (1-based) on a bar `barHeight` tall.
inline Rect WorkspaceCell(int index, int barHeight)
{
    return Rect{ WorkspaceLabelX(index) - kWorkspaceLeftPad, 0, kWorkspaceStep, barHeight };
}

// The workspace (1-based) under bar-relative point (x, y), or 0 when the
// point is not on any of the `count` workspace numbers.
inline int WorkspaceAt(int x, int y, int count, int barHeight)
{
    if (count <= 0 || y < 0 || y >= barHeight)
        return 0;

    const int offset = x - (kWorkspaceFirstX - kWorkspaceLeftPad);

    if (offset < 0)
        return 0;

    const int index = offset / kWorkspaceStep + 1;

    return index <= count ? index : 0;
}

}
