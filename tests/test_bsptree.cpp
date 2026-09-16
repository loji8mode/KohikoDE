// Standalone correctness test for BSPTree - no X11 display needed,
// since BSPTree/ManagedWindow only depend on the WindowID *type*
// (an unsigned long alias), never on an actual X connection.
//
// Build & run: see the "test" target in the Makefile.

#include "BSPTree.h"
#include "LayoutEngine.h"
#include "ManagedWindow.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <memory>
#include <vector>

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
    std::vector<std::unique_ptr<ManagedWindow>> windows;

    auto makeWindow = [&](WindowID id)
    {
        windows.push_back(std::make_unique<ManagedWindow>(id));
        return windows.back().get();
    };

    ManagedWindow* A = makeWindow(1);
    ManagedWindow* B = makeWindow(2);
    ManagedWindow* C = makeWindow(3);
    ManagedWindow* D = makeWindow(4);

    BSPTree tree;
    LayoutEngine layout;

    LayoutEngine::Params params;
    params.innerGap = 4;
    params.outerGap = 4;
    params.borderWidth = 2;
    params.smartGaps = false;
    params.smartBorders = false;

    Rect area{0, 0, 1920, 1080};

    std::printf("-- Insert (Hyprland-style anchor insertion) --\n");

    tree.Insert(A);
    tree.Focus(A);
    layout.Apply(tree.Root(), area, params);
    Check(tree.Count() == 1, "count == 1 after inserting A");

    tree.Insert(B);
    tree.Focus(B);
    layout.Apply(tree.Root(), area, params);
    Check(tree.Count() == 2, "count == 2 after inserting B");

    tree.Insert(C); // near B (focused)
    tree.Focus(C);
    layout.Apply(tree.Root(), area, params);
    Check(tree.Count() == 3, "count == 3 after inserting C near B");

    tree.Focus(A);
    tree.Insert(D); // near A (re-focused) -> ((A|D)|(B|C)), matching the spec's diagram exactly
    layout.Apply(tree.Root(), area, params);
    Check(tree.Count() == 4, "count == 4 after inserting D near A");

    BSPNode* root = tree.Root();
    Check(!root->IsLeaf(), "root is a split");

    auto* rootSplit = static_cast<BSPSplit*>(root);
    auto* leftSplit  = static_cast<BSPSplit*>(rootSplit->Left());
    auto* rightSplit = static_cast<BSPSplit*>(rootSplit->Right());

    Check(leftSplit->Left()->IsLeaf() && static_cast<BSPLeaf*>(leftSplit->Left())->Window() == A,
          "left split's first child is A");
    Check(static_cast<BSPLeaf*>(leftSplit->Right())->Window() == D,
          "left split's second child is D");
    Check(static_cast<BSPLeaf*>(rightSplit->Left())->Window() == B,
          "right split's first child is B");
    Check(static_cast<BSPLeaf*>(rightSplit->Right())->Window() == C,
          "right split's second child is C  ->  tree is exactly ((A|D)|(B|C))");

    std::printf("\n-- Swap (Super+LMB): the core bug fix --\n");

    Rect geomA_before = A->Geometry();
    Rect geomD_before = D->Geometry();

    tree.Swap(A, D);

    // This is the crucial regression check: the ORIGINAL implementation
    // swapped Geometry() rects directly, which LayoutEngine would just
    // overwrite on the very next pass. A correct Swap() must survive a
    // fresh relayout because it changed the *tree*, not just cached
    // coordinates.
    layout.Apply(tree.Root(), area, params);

    Check(D->Geometry().x == geomA_before.x && D->Geometry().y == geomA_before.y,
          "after Swap()+relayout, D now renders where A used to be");
    Check(A->Geometry().x == geomD_before.x && A->Geometry().y == geomD_before.y,
          "after Swap()+relayout, A now renders where D used to be");
    Check(static_cast<BSPLeaf*>(leftSplit->Left())->Window() == D,
          "left split's first child is now D (leaf position unchanged, window swapped)");
    Check(static_cast<BSPLeaf*>(leftSplit->Right())->Window() == A,
          "left split's second child is now A");

    std::printf("\n-- Resize (Super+RMB): the divider always tracks the mouse, not the grabbed window --\n");

    // The split holding D/A could have ended up Vertical or Horizontal
    // depending on the anchor's aspect ratio at insert time (that's
    // the adaptive DirectionForRect() logic working as intended) - so
    // drive whichever axis this particular split actually uses.
    bool leftIsVertical = (leftSplit->Direction() == SplitDirection::Vertical);
    int dx = leftIsVertical ? 100 : 0;
    int dy = leftIsVertical ? 0   : 100;

    // Same drag (same dx/dy, same sign) regardless of which of the two
    // windows is under the cursor: the divider between them - and
    // therefore the ratio, which is the FIRST child's share - must
    // move the same way both times. That's the actual regression this
    // test guards: an earlier version flipped the sign whenever
    // `window` was the second child, which moved the divider backwards
    // relative to the mouse and read as inverted dragging for whatever
    // window happened to be on that side of the split.
    float ratio0 = leftSplit->Ratio();
    tree.Resize(D, dx, dy); // D is the FIRST child here
    float ratio1 = leftSplit->Ratio();
    Check(ratio1 > ratio0, "dragging in the positive direction increases the first child's ratio (grabbed the FIRST child)");

    tree.Resize(A, dx, dy); // A is the SECOND child - same dx/dy as above
    float ratio2 = leftSplit->Ratio();
    Check(ratio2 > ratio1, "the same drag increases the first child's ratio again, even though this time the SECOND child was grabbed");

    std::printf("\n-- Rotate / Flip --\n");

    SplitDirection dirBefore = leftSplit->Direction();
    tree.Rotate(D);
    Check(leftSplit->Direction() != dirBefore, "Rotate() flips the split direction");

    ManagedWindow* leftBefore  = static_cast<BSPLeaf*>(leftSplit->Left())->Window();
    ManagedWindow* rightBefore = static_cast<BSPLeaf*>(leftSplit->Right())->Window();
    tree.Flip(D);
    Check(static_cast<BSPLeaf*>(leftSplit->Left())->Window() == rightBefore, "Flip() swaps child order (1/2)");
    Check(static_cast<BSPLeaf*>(leftSplit->Right())->Window() == leftBefore, "Flip() swaps child order (2/2)");

    std::printf("\n-- FindNeighbor / HitTest --\n");

    layout.Apply(tree.Root(), area, params);

    ManagedWindow* neighborRight = tree.FindNeighbor(A, Direction::Right);
    Check(neighborRight != nullptr, "FindNeighbor(A, Right) finds something");

    ManagedWindow* hit = tree.HitTest(Point{C->Geometry().CenterX(), C->Geometry().CenterY()});
    Check(hit == C, "HitTest() at C's own center finds C");

    std::printf("\n-- HasSpaceForAnotherWindow (bug #4: minimum tile size) --\n");

    Check(BSPTree().HasSpaceForAnotherWindow(nullptr, area, 4, 100, 60),
          "a brand-new empty tree always has room for the first window");

    Check(tree.HasSpaceForAnotherWindow(nullptr, area, params.innerGap, 50, 50),
          "the existing 4-window 1920x1080 tree still has room for a modest 50x50 minimum");
    Check(!tree.HasSpaceForAnotherWindow(nullptr, area, params.innerGap, 2000, 2000),
          "the existing 4-window tree has no room left for an unreasonably large minimum");

    // Precise boundary check on a tree that has *never* been laid out
    // (Arrange() only lays out the current workspace, so a tree living
    // on some other workspace can easily be in exactly this state) -
    // HasSpaceForAnotherWindow() must still get the right answer by
    // recomputing from `tilingArea` itself rather than trusting any
    // node's (here, nonexistent) cached Geometry().
    //
    // A 220x100 area is wider than it is tall, so DirectionForRect()
    // picks a Vertical (left/right) split: with a 4px gap that divides
    // into two 108px-wide children (Insert() always uses a fresh 50/50
    // ratio), so a 100px minimum should just clear and a 110px minimum
    // should not.
    ManagedWindow* E = makeWindow(5);
    BSPTree soloTree;
    soloTree.Insert(E);

    Rect soloArea{0, 0, 220, 100};
    Check(soloTree.HasSpaceForAnotherWindow(nullptr, soloArea, 4, 100, 60),
          "never-laid-out tree: 108px-wide half still clears a 100px minimum width");
    Check(!soloTree.HasSpaceForAnotherWindow(nullptr, soloArea, 4, 110, 60),
          "never-laid-out tree: 108px-wide half no longer clears a 110px minimum width");

    std::printf("\n-- Insert() with placement (\"Try Alternative Layouts\") --\n");

    // 300x310 is very slightly taller than it is wide, so
    // DirectionForRect() picks Horizontal (split the height) as the
    // "natural" direction - which only gives each half ~153px of the
    // 310px height, well short of the 170px floor this test asks for.
    // Splitting the *other* way (Vertical, splitting the 300px width
    // instead) leaves height untouched at 310 (>= 170) and still
    // clears the 100px floor width on both ~148px-wide halves - so a
    // naive "only ever try the natural direction" implementation would
    // wrongly refuse this insertion, even though a perfectly good
    // placement exists one axis over. (A window's own declared
    // WM_NORMAL_HINTS minimum is never consulted for this any more -
    // see EffectiveMinSize() - only the ordinary floor passed in here
    // ever gates a placement, which is why this drives the check via
    // floorHeight=170 directly rather than G->SetMinSize().)
    ManagedWindow* F = makeWindow(6);
    ManagedWindow* G = makeWindow(7);

    BSPTree altTree;
    altTree.Insert(F);

    Rect altArea{0, 0, 300, 310};

    Check(altTree.HasSpaceForAnotherWindow(G, altArea, 4, 100, 170),
          "HasSpaceForAnotherWindow: alternative direction rescues an anchor "
          "that's infeasible in its own natural direction");
    Check(altTree.Insert(G, altArea, 4, 100, 170),
          "Insert(): the same alternative-direction placement actually succeeds");
    Check(altTree.Count() == 2, "count == 2 after the alternative-direction insert");

    layout.Apply(altTree.Root(), altArea, params);
    Check(F->Geometry().height >= 170 && G->Geometry().height >= 170,
          "both panes still clear the 170px floor height after relayout");
    Check(F->Geometry().width >= 100 && G->Geometry().width >= 100,
          "both panes still clear the 100px floor width after relayout");

    std::printf("\n-- Insert() with placement (\"Shrink Existing Tiles\") --\n");

    // Two 250x250 panes side by side (H|I, root ratio 0.5, 4px gaps -
    // usable = 500, so 250 each). A uniform 150px floor (applied to
    // every window alike now - see EffectiveMinSize()) means a new
    // window J can't get a slot by splitting I alone: either direction
    // leaves I's own width unchanged or halved, and a plain 50/50 split
    // of just I's 250px only gives ~123px per side - short of 150.
    // Shrinking H down to the 150px floor frees enough of the shared
    // 500px for I and J to split ~175px each once combined, clearing
    // the floor on all three without ever taking H below it.
    ManagedWindow* H = makeWindow(8);
    ManagedWindow* I = makeWindow(9);
    ManagedWindow* J = makeWindow(10);

    BSPTree reclaimTree;
    reclaimTree.Insert(H);
    reclaimTree.Insert(I); // anchor is H (only leaf so far) -> (H|I)

    Rect reclaimArea{0, 0, 504, 250};
    LayoutEngine::Params reclaimParams;
    reclaimParams.innerGap = 4;
    reclaimParams.outerGap = 0;
    reclaimParams.borderWidth = 0;

    layout.Apply(reclaimTree.Root(), reclaimArea, reclaimParams);
    Check(I->Geometry().width < 150 * 2,
          "sanity check: I's plain 50/50 share (~250px) can't just be re-split in half and still clear the 150px floor");

    Check(reclaimTree.HasSpaceForAnotherWindow(J, reclaimArea, 4, 150, 60),
          "HasSpaceForAnotherWindow: shrinking H down to the 150px floor is enough to fit J");
    Check(reclaimTree.Insert(J, reclaimArea, 4, 150, 60),
          "Insert(): the same shrink-existing-tiles placement actually succeeds");
    Check(reclaimTree.Count() == 3, "count == 3 after the reclaim-based insert");

    layout.Apply(reclaimTree.Root(), reclaimArea, reclaimParams);
    Check(H->Geometry().width >= 150,
          "H was shrunk to make room, but never below the 150px floor");
    Check(J->Geometry().width >= 150 && J->Geometry().height >= 60,
          "J actually got a slot that clears the floor");

    // Negative case: nothing (however extreme) reclaims 600px out of a
    // 504px-wide area - Insert() must leave the tree completely
    // untouched rather than silently violating the floor to "make it
    // work" anyway.
    ManagedWindow* K = makeWindow(11);

    Check(!reclaimTree.HasSpaceForAnotherWindow(K, reclaimArea, 4, 600, 60),
          "HasSpaceForAnotherWindow: correctly refuses a 600px requirement no reclaim can satisfy");
    Check(!reclaimTree.Insert(K, reclaimArea, 4, 600, 60),
          "Insert(): correctly refuses the same impossible placement, touching nothing");
    Check(reclaimTree.Count() == 3, "count is still 3 - the failed Insert() didn't mutate the tree");

    std::printf("\n-- Remove (no empty nodes left behind) --\n");

    tree.Remove(B);
    Check(tree.Count() == 3, "count == 3 after removing B");

    tree.Remove(C);
    tree.Remove(D);
    tree.Remove(A);
    Check(tree.Empty(), "tree is empty after removing every window");

    std::printf(
        "\n-- OccupiesTreeSlot (regression: a tiled window that went "
        "fullscreen must still free its slot when it closes) --\n");

    // Reproduces the exact bug report: two ordinary tiled windows
    // (Discord, Telegram) plus a third (Telegram's Media Viewer) that
    // gets tiled too, then asks for real EWMH fullscreen (which -
    // correctly - leaves it in the tree so un-fullscreening can
    // restore it to the same slot) and is then closed *while still
    // fullscreen*, without ever un-fullscreening first.
    ManagedWindow* discord = makeWindow(10);
    ManagedWindow* telegram = makeWindow(11);
    ManagedWindow* mediaViewer = makeWindow(12);

    BSPTree wsTree;
    wsTree.Insert(discord);
    wsTree.Focus(discord);
    wsTree.Insert(telegram);
    wsTree.Focus(telegram);
    wsTree.Insert(mediaViewer); // lands near Telegram, the focused window
    wsTree.Focus(mediaViewer);

    discord->SetState(WindowState::Tiled);
    telegram->SetState(WindowState::Tiled);
    mediaViewer->SetState(WindowState::Tiled);

    layout.Apply(wsTree.Root(), area, params);
    Check(wsTree.Count() == 3, "3 leaves once the Media Viewer tiles alongside Discord/Telegram");

    // The Media Viewer asks for real fullscreen - Manage()/
    // HandleClientMessage() record this exactly as WindowState::Tiled
    // (PreviousState) -> WindowState::Fullscreen (State), and
    // deliberately do NOT touch the tree.
    mediaViewer->SetPreviousState(mediaViewer->State());
    mediaViewer->SetState(WindowState::Fullscreen);

    Check(!mediaViewer->IsTiled(), "Media Viewer's current State() is Fullscreen, not Tiled");
    Check(mediaViewer->OccupiesTreeSlot(),
          "...but it still logically occupies a tree slot (PreviousState() == Tiled)");
    Check(wsTree.Count() == 3, "tree still has 3 leaves while it's fullscreen (by design, for restoring later)");

    // It closes right here, still fullscreen - this is exactly what
    // WindowManager::Unmanage() does now:
    if (mediaViewer->OccupiesTreeSlot())
        wsTree.Remove(mediaViewer);

    Check(wsTree.Count() == 2,
          "FIXED: closing it while fullscreen still frees its slot (the bug: "
          "checking IsTiled() instead of OccupiesTreeSlot() left this at 3 forever)");

    Rect discordBefore = discord->Geometry();
    Rect telegramBefore = telegram->Geometry();

    layout.Apply(wsTree.Root(), area, params);

    Check(discord->Geometry().width  != discordBefore.width ||
          telegram->Geometry().width != telegramBefore.width ||
          discord->Geometry().height  != discordBefore.height ||
          telegram->Geometry().height != telegramBefore.height,
          "Discord and/or Telegram actually grow into the freed space on relayout "
          "(this is the \"black empty area\"/\"stuck at its old size\" symptom - "
          "with the bug, these rects would be unchanged since the tree still "
          "thought there were 3 windows sharing the screen)");

    // No dead space left at all: the two survivors' tiles should
    // account for the *entire* tiling area between them (minus gaps),
    // not just "some" of it.
    long long survivorsArea =
        static_cast<long long>(discord->Geometry().width)  * discord->Geometry().height +
        static_cast<long long>(telegram->Geometry().width) * telegram->Geometry().height;
    long long totalArea = static_cast<long long>(area.width) * area.height;

    // Allow for the gaps/border insets LayoutEngine subtracts - this
    // just needs to be "the overwhelming majority of the screen", not
    // pixel-exact, to rule out a leftover reserved-but-unfilled slot.
    Check(survivorsArea > (totalArea * 9) / 10,
          "no large reserved-but-empty area is left over - the two survivors "
          "between them account for essentially the whole screen");

    std::printf(
        "\n-- Collapse-on-remove preserves the surrounding layout "
        "(A|B/C -> close B -> A|C, never a full re-stack) --\n");

    {
        // Exactly the release-note example: A takes the left half, B/C
        // stack top/bottom in the right half. Closing B must promote C
        // to fill the whole right half (A|C, still side by side) -
        // never collapse the *outer* split into a top/bottom stack of
        // A over C.
        ManagedWindow* wA = makeWindow(20);
        ManagedWindow* wB = makeWindow(21);
        ManagedWindow* wC = makeWindow(22);

        BSPTree collapseTree;

        collapseTree.Insert(wA, area, 4, 50, 50);
        collapseTree.Focus(wA);
        collapseTree.Insert(wB, area, 4, 50, 50);
        collapseTree.Focus(wB);
        collapseTree.Insert(wC, area, 4, 50, 50);
        collapseTree.Focus(wC);

        BSPNode* preRoot = collapseTree.Root();
        Check(!preRoot->IsLeaf(), "A|B/C: root is a split before removal");
        auto* preRootSplit = static_cast<BSPSplit*>(preRoot);
        Check(preRootSplit->Direction() == SplitDirection::Vertical,
              "A|B/C: outer split is side-by-side (A on the left)");
        Check(static_cast<BSPLeaf*>(preRootSplit->Left())->Window() == wA,
              "A|B/C: left side of the outer split is A");
        Check(!preRootSplit->Right()->IsLeaf(),
              "A|B/C: right side of the outer split is itself a split (B/C stacked)");

        collapseTree.Remove(wB);
        layout.Apply(collapseTree.Root(), area, params);

        Check(collapseTree.Count() == 2, "A|C: two windows remain after closing B");

        BSPNode* postRoot = collapseTree.Root();
        Check(!postRoot->IsLeaf(), "A|C: root is still a split (not a single collapsed leaf)");

        auto* postRootSplit = static_cast<BSPSplit*>(postRoot);
        Check(postRootSplit->Direction() == SplitDirection::Vertical,
              "A|C: the surviving split is side-by-side, exactly like the original outer "
              "split - NOT a top/bottom stack of A over C");
        Check(postRootSplit->Left()->IsLeaf() &&
              static_cast<BSPLeaf*>(postRootSplit->Left())->Window() == wA,
              "A|C: A is still the left/first child, untouched");
        Check(postRootSplit->Right()->IsLeaf() &&
              static_cast<BSPLeaf*>(postRootSplit->Right())->Window() == wC,
              "A|C: C was promoted straight into B's old slot as the right/second child");

        // C should now span the *entire* right half B and C used to
        // share, not just B's old (smaller) top slice of it.
        Check(wC->Geometry().height > wA->Geometry().height / 2,
              "A|C: C actually expanded to fill the whole right half vertically, "
              "rather than staying pinned to B's old (half-height) slot");
    }

    std::printf(
        "\n-- Collapse-on-remove in a deeper tree only touches the removed "
        "leaf's own sibling, nothing further up the tree --\n");

    {
        // A | (B / (C|D)): removing the deeply-nested C must only
        // promote its sibling D into the C|D slot - A and B, and the
        // ratio between A and the right column as a whole, must be
        // completely undisturbed.
        ManagedWindow* wA = makeWindow(23);
        ManagedWindow* wB = makeWindow(24);
        ManagedWindow* wC = makeWindow(25);
        ManagedWindow* wD = makeWindow(26);

        BSPTree deepTree;

        deepTree.Insert(wA, area, 4, 50, 50);
        deepTree.Focus(wA);
        deepTree.Insert(wB, area, 4, 50, 50);
        deepTree.Focus(wB);
        deepTree.Insert(wC, area, 4, 50, 50);
        deepTree.Focus(wC);
        deepTree.Insert(wD, area, 4, 50, 50);
        deepTree.Focus(wD);

        layout.Apply(deepTree.Root(), area, params);

        Rect aBefore = wA->Geometry();
        Rect bBefore = wB->Geometry();

        deepTree.Remove(wC);
        layout.Apply(deepTree.Root(), area, params);

        Check(deepTree.Count() == 3, "3 windows remain after closing the deeply-nested C");
        Check(wA->Geometry().x == aBefore.x && wA->Geometry().y == aBefore.y &&
              wA->Geometry().width == aBefore.width && wA->Geometry().height == aBefore.height,
              "A's geometry is byte-for-byte unchanged - removing C two levels down "
              "never touches an unrelated ancestor's split");
        Check(wB->Geometry().x == bBefore.x && wB->Geometry().y == bBefore.y &&
              wB->Geometry().width == bBefore.width && wB->Geometry().height == bBefore.height,
              "B's geometry is likewise completely unchanged");

        Rect dAfter = wD->Geometry();
        Check(dAfter.width > (bBefore.width / 2),
              "D (C's sibling, previously side-by-side with C) is the one window that "
              "actually grew, expanding sideways to fill C's freed slot");
    }

    std::printf(
        "\n-- Collapse-on-remove re-derives direction inside a PROMOTED "
        "SPLIT, not just a promoted leaf - the actual narrow-strip bug "
        "(user-reported, live-reproduced under Xvfb - see CHANGELOG) --\n");

    {
        // A | (B / (C|D)): unlike the two tests just above, this
        // removes B - the direct sibling of the WHOLE (C|D) split, not
        // one of C/D themselves. C and D share a short, wide slot
        // under B, so they're naturally side by side. Promoting (C|D)
        // as a unit into B's old (much taller) slot is exactly the
        // shape that produces two unnaturally narrow columns without
        // the fix.
        auto buildTree = [&](ManagedWindow* wA, ManagedWindow* wB, ManagedWindow* wC, ManagedWindow* wD)
        {
            auto t = std::make_unique<BSPTree>();
            t->Insert(wA, area, 4, 50, 50);
            t->Focus(wA);
            t->Insert(wB, area, 4, 50, 50);
            t->Focus(wB);
            t->Insert(wC, area, 4, 50, 50);
            t->Focus(wC);
            t->Insert(wD, area, 4, 50, 50);
            t->Focus(wD);
            layout.Apply(t->Root(), area, params);
            return t;
        };

        ManagedWindow* wA = makeWindow(27);
        ManagedWindow* wB = makeWindow(28);
        ManagedWindow* wC = makeWindow(29);
        ManagedWindow* wD = makeWindow(30);

        std::unique_ptr<BSPTree> fixedTree = buildTree(wA, wB, wC, wD);

        Rect cBefore = wC->Geometry();
        Rect dBefore = wD->Geometry();
        Rect aBefore = wA->Geometry();

        Check(cBefore.y == dBefore.y && cBefore.x != dBefore.x,
              "setup sanity: C and D start out side by side (same y, different x)");
        Check(cBefore.width < cBefore.height,
              "setup sanity: side by side in B's short, wide leftover slot means "
              "C (and D) each start out narrower than they are tall");

        fixedTree->Remove(wB, area, 4);
        layout.Apply(fixedTree->Root(), area, params);

        Rect cFixed = wC->Geometry();
        Rect dFixed = wD->Geometry();

        Check(fixedTree->Count() == 3, "3 windows remain after closing B");
        Check(cFixed.x == dFixed.x && cFixed.y != dFixed.y,
              "THE FIX: with the placement-aware Remove() overload, C and D come "
              "out STACKED (same x, different y) - re-derived against the new, "
              "much taller area they actually inherited, not left side by side");
        Check(cFixed.width > cBefore.width * 1.8,
              "...and consequently each is now close to the FULL column width, "
              "not still squeezed to roughly half of it");
        Check(wA->Geometry().x == aBefore.x && wA->Geometry().y == aBefore.y &&
              wA->Geometry().width == aBefore.width && wA->Geometry().height == aBefore.height,
              "A - outside the promoted subtree entirely - is still completely "
              "untouched, exactly like the two simpler collapse tests above");

        // Same structural scenario, replayed against the OLD,
        // geometry-agnostic Remove(window) overload, to demonstrate
        // this is genuinely what the placement-aware overload fixes -
        // not some incidental side effect of the test setup.
        ManagedWindow* wA2 = makeWindow(31);
        ManagedWindow* wB2 = makeWindow(32);
        ManagedWindow* wC2 = makeWindow(33);
        ManagedWindow* wD2 = makeWindow(34);

        std::unique_ptr<BSPTree> buggyTree = buildTree(wA2, wB2, wC2, wD2);

        buggyTree->Remove(wB2);
        layout.Apply(buggyTree->Root(), area, params);

        Rect c2 = wC2->Geometry();
        Rect d2 = wD2->Geometry();

        Check(c2.y == d2.y && c2.x != d2.x,
              "confirmed: the plain structural Remove(window) overload (unchanged, "
              "still used by simple/legacy callers) reproduces the original bug - "
              "C2/D2 stay side by side, now unnaturally narrow in the taller slot");
        Check(c2.width < c2.height,
              "...narrower than tall, exactly the reported symptom - this is what "
              "the placement-aware overload above fixes");
    }

    std::printf(
        "\n-- Collapse-on-remove: no spurious flip when the promoted "
        "split's direction already suits its new area --\n");

    {
        ManagedWindow* wA = makeWindow(35);
        ManagedWindow* wB = makeWindow(36);
        ManagedWindow* wC = makeWindow(37);
        ManagedWindow* wD = makeWindow(38);

        BSPTree tree2;

        tree2.Insert(wA, area, 4, 50, 50);
        tree2.Focus(wA);
        tree2.Insert(wB, area, 4, 50, 50);
        tree2.Focus(wB);
        tree2.Insert(wC, area, 4, 50, 50);
        tree2.Focus(wC);
        tree2.Insert(wD, area, 4, 50, 50);
        tree2.Focus(wD);
        layout.Apply(tree2.Root(), area, params);

        // C|D starts out Vertical (side by side, see the reproduction
        // above) - manually rotate it to Horizontal first, which is
        // already what it would need to become once promoted, so the
        // fix has nothing to correct.
        tree2.Rotate(wC);
        layout.Apply(tree2.Root(), area, params);

        Rect cBefore = wC->Geometry();
        Rect dBefore = wD->Geometry();
        Check(cBefore.x == dBefore.x && cBefore.y != dBefore.y,
              "setup sanity: manually rotated to stacked before B is ever removed");

        tree2.Remove(wB, area, 4);
        layout.Apply(tree2.Root(), area, params);

        Rect cAfter = wC->Geometry();
        Rect dAfter = wD->Geometry();
        Check(cAfter.x == dAfter.x && cAfter.y != dAfter.y,
              "still stacked after removal - already correct, so nothing needed "
              "flipping, and nothing did");
    }

    std::printf(
        "\n-- Collapse-on-remove preserves a manually-resized ratio "
        "across the flip, just applied to the other axis --\n");

    {
        ManagedWindow* wA = makeWindow(39);
        ManagedWindow* wB = makeWindow(40);
        ManagedWindow* wC = makeWindow(41);
        ManagedWindow* wD = makeWindow(42);

        BSPTree tree3;

        tree3.Insert(wA, area, 4, 50, 50);
        tree3.Focus(wA);
        tree3.Insert(wB, area, 4, 50, 50);
        tree3.Focus(wB);
        tree3.Insert(wC, area, 4, 50, 50);
        tree3.Focus(wC);
        tree3.Insert(wD, area, 4, 50, 50);
        tree3.Focus(wD);
        layout.Apply(tree3.Root(), area, params);

        // C|D is Vertical (side by side) here - drag the divider so C
        // gets roughly 70% of the shared width instead of 50%.
        int combinedWidth = wC->Geometry().width + wD->Geometry().width;
        tree3.Resize(wC, static_cast<int>(combinedWidth * 0.2), 0);
        layout.Apply(tree3.Root(), area, params);

        Rect cRatioBefore = wC->Geometry();
        Rect dRatioBefore = wD->Geometry();
        float widthShareBefore =
            static_cast<float>(cRatioBefore.width) /
            static_cast<float>(cRatioBefore.width + dRatioBefore.width);

        Check(widthShareBefore > 0.6f,
              "setup sanity: C now holds noticeably more than half the shared "
              "width (manually resized, roughly 70/30)");

        tree3.Remove(wB, area, 4);
        layout.Apply(tree3.Root(), area, params);

        Rect cAfter = wC->Geometry();
        Rect dAfter = wD->Geometry();

        Check(cAfter.x == dAfter.x && cAfter.y != dAfter.y,
              "flipped to stacked, same as the undisturbed-ratio case above");

        float heightShareAfter =
            static_cast<float>(cAfter.height) /
            static_cast<float>(cAfter.height + dAfter.height);

        Check(heightShareAfter > 0.6f,
              "the ~70/30 split survives the flip, now expressed as height "
              "share instead of width share - the user's manual resize wasn't "
              "silently discarded, just re-applied to the axis that now matters");
    }

    std::printf(
        "\n-- Collapse-on-remove: no leaf ends up unnaturally stretched "
        "in a several-levels-deep tree, before or after a removal that "
        "promotes a nested split --\n");

    {
        // Six windows, inserted one after another exactly the way a
        // user opening one application after another would (each new
        // window anchored on whichever one was focused last) - the
        // same sequence, and the same live Xvfb reproduction, used to
        // first confirm the *insertion* side of this algorithm was
        // already sound before concluding the bug was specifically in
        // Remove()'s collapse step (see the 0.20.4 CHANGELOG entry).
        std::vector<ManagedWindow*> ws;
        for (WindowID id = 43; id <= 48; ++id)
            ws.push_back(makeWindow(id));

        BSPTree deepTree;

        for (ManagedWindow* w : ws)
        {
            deepTree.Insert(w, area, 4, 50, 50);
            deepTree.Focus(w);
            layout.Apply(deepTree.Root(), area, params);
        }

        auto worstAspectRatio = [&]()
        {
            float worst = 1.0f;
            for (ManagedWindow* w : ws)
            {
                if (!w->OccupiesTreeSlot())
                    continue;

                Rect g = w->Geometry();
                float longSide = static_cast<float>(std::max(g.width, g.height));
                float shortSide = static_cast<float>(std::max(1, std::min(g.width, g.height)));
                worst = std::max(worst, longSide / shortSide);
            }
            return worst;
        };

        Check(worstAspectRatio() < 3.0f,
              "after inserting all 6 windows one after another: no window's "
              "longer side is more than 3x its shorter side");

        // Close two of the middle windows (not the most- or
        // least-nested), which forces at least one promoted subtree to
        // contain further nested splits of its own - the deep case the
        // top-level-only version of this fix wouldn't have covered.
        deepTree.Remove(ws[1], area, 4);
        layout.Apply(deepTree.Root(), area, params);
        deepTree.Remove(ws[3], area, 4);
        layout.Apply(deepTree.Root(), area, params);

        Check(deepTree.Count() == 4, "4 windows remain after closing 2 of the 6");
        Check(worstAspectRatio() < 3.0f,
              "...and still true after two removals that each promote a "
              "subtree somewhere in the middle of the tree - nothing was left "
              "unnaturally stretched or narrow at any depth");
    }

    std::printf(
        "\n-- Session Restore: CollectPlacementRules() produces an order "
        "InsertNextTo() can actually replay --\n");

    {
        // A | (B / C): the same shape as the very first collapse test
        // above, but this time exercising the *other* direction -
        // reconstructing a tree from scratch via CollectPlacementRules()
        // + InsertNextTo(), the way session restore rebuilds a
        // workspace's BSP layout across a restart. Deliberately picked
        // because it's the smallest tree where a naive post-order rule
        // collection gets the replay order backwards: the B|C split
        // sits *inside* the A|(B|C) split, so its own connecting rule
        // must not be handed to InsertNextTo() before the outer rule
        // that actually places one of B or C in the tree in the first
        // place.
        ManagedWindow* wA = makeWindow(30);
        ManagedWindow* wB = makeWindow(31);
        ManagedWindow* wC = makeWindow(32);

        BSPTree original;
        original.Insert(wA, area, 4, 50, 50);
        original.Focus(wA);
        original.Insert(wB, area, 4, 50, 50);
        original.Focus(wB);
        original.Insert(wC, area, 4, 50, 50);
        original.Focus(wC);

        std::vector<BSPTree::PlacementRule> rules = original.CollectPlacementRules();
        Check(rules.size() == 2, "A|(B/C): exactly 2 placement rules for 3 windows");

        // A has no rule about it anywhere - it's the implicit seed.
        bool aHasNoRule = true;
        for (const auto& r : rules)
            if (r.window == wA->Id())
                aHasNoRule = false;
        Check(aHasNoRule, "A|(B/C): the leftmost window (A) needs no placement rule");

        BSPTree rebuilt;
        rebuilt.Insert(wA); // the implicit seed, placed the ordinary way
        bool replayOk = true;
        for (const auto& rule : rules)
        {
            ManagedWindow* w = (rule.window == wB->Id()) ? wB : wC;
            ManagedWindow* n = (rule.neighbor == wA->Id()) ? wA
                              : (rule.neighbor == wB->Id()) ? wB : wC;
            if (!rebuilt.InsertNextTo(w, n, rule.direction))
                replayOk = false;
        }

        Check(replayOk,
              "A|(B/C): every rule replayed successfully in the order "
              "CollectPlacementRules() returned it - this is the exact "
              "case a naive post-order collection gets backwards, "
              "failing to find B or C's neighbour still in the tree");
        Check(rebuilt.Count() == 3, "A|(B/C): all 3 windows present after replay");
    }

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    return 0;
}
