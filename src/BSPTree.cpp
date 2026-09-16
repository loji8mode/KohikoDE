#include "BSPTree.h"

#include "ManagedWindow.h"

#include <algorithm>
#include <sstream>

namespace Kohiko
{

BSPTree::BSPTree()
{
}

bool BSPTree::Empty() const
{
    return m_root == nullptr;
}

void BSPTree::Clear()
{
    m_root.reset();
    m_lastFocused = nullptr;
}

BSPNode* BSPTree::Root() const
{
    return m_root.get();
}

int BSPTree::Count() const
{
    std::vector<BSPLeaf*> leaves;
    CollectLeaves(m_root.get(), leaves);
    return static_cast<int>(leaves.size());
}

ManagedWindow* BSPTree::FocusedWindow() const
{
    BSPLeaf* anchor = AnchorLeaf();
    return anchor ? anchor->Window() : nullptr;
}

BSPLeaf* BSPTree::AnchorLeaf() const
{
    if (m_lastFocused)
        return m_lastFocused;

    // Fallback: nothing has ever been focused (e.g. right after the
    // very first window opened) - just take whatever leaf exists.
    std::vector<BSPLeaf*> leaves;
    CollectLeaves(m_root.get(), leaves);

    return leaves.empty() ? nullptr : leaves.back();
}

void BSPTree::Insert(ManagedWindow* window)
{
    if (!window)
        return;

    if (!m_root)
    {
        auto leaf = std::make_unique<BSPLeaf>(window);
        m_lastFocused = leaf.get();
        m_root = std::move(leaf);
        return;
    }

    BSPLeaf* anchor = AnchorLeaf();

    if (!anchor)
        return;

    SplitDirection direction = DirectionForRect(anchor->Geometry());

    SpliceIn(window, anchor, direction);
}

bool BSPTree::Insert(
    ManagedWindow* window,
    const Rect& tilingArea,
    int innerGap,
    int floorWidth,
    int floorHeight)
{
    if (!window)
        return false;

    if (!m_root)
    {
        auto leaf = std::make_unique<BSPLeaf>(window);
        m_lastFocused = leaf.get();
        m_root = std::move(leaf);
        return true; // the first window on a workspace always fits
    }

    BSPLeaf* anchor = AnchorLeaf();

    if (!anchor)
        return false;

    Rect anchorRect;

    if (!ComputeLeafGeometry(m_root.get(), anchor, tilingArea, innerGap, anchorRect))
        return false; // unreachable in practice - anchor always comes from this same tree

    int anchorMinWidth;
    int anchorMinHeight;
    EffectiveMinSize(anchor->Window(), floorWidth, floorHeight, anchorMinWidth, anchorMinHeight);

    int newMinWidth;
    int newMinHeight;
    EffectiveMinSize(window, floorWidth, floorHeight, newMinWidth, newMinHeight);

    SplitDirection direction;

    if (!FeasibleSplitDirection(
            anchorRect, innerGap,
            anchorMinWidth, anchorMinHeight,
            newMinWidth, newMinHeight,
            direction))
    {
        std::vector<std::pair<BSPSplit*, float>> steps;

        if (!PlanReclaim(anchor, window, tilingArea, innerGap, floorWidth, floorHeight, direction, steps))
            return false; // no placement anywhere along this anchor's ancestor chain

        // Apply every ratio change the plan needs - shrinking other
        // tiles, but never below their own effective minimum anywhere,
        // exactly as PlanReclaim() already verified.
        for (auto& step : steps)
            step.first->SetRatio(step.second);
    }

    SpliceIn(window, anchor, direction);
    return true;
}

void BSPTree::SpliceIn(
    ManagedWindow* window,
    BSPLeaf* anchor,
    SplitDirection direction)
{
    auto newSplit = std::make_unique<BSPSplit>(direction);
    auto newLeaf  = std::make_unique<BSPLeaf>(window);

    BSPSplit* parent = anchor->Parent();

    // Detach the anchor from wherever it currently lives so it can be
    // re-parented under the new split, then put the new split back in
    // exactly the slot the anchor used to occupy.
    std::unique_ptr<BSPNode> anchorOwned =
        parent
            ? ((parent->Left() == anchor) ? parent->TakeLeft() : parent->TakeRight())
            : std::move(m_root);

    newSplit->SetLeft(std::move(anchorOwned));
    newSplit->SetRight(std::move(newLeaf));

    BSPLeaf* newLeafPtr = static_cast<BSPLeaf*>(newSplit->Right());

    if (parent)
    {
        if (parent->Left() == nullptr)
            parent->SetLeft(std::move(newSplit));
        else
            parent->SetRight(std::move(newSplit));
    }
    else
    {
        m_root = std::move(newSplit);
    }

    m_lastFocused = newLeafPtr;
}

void BSPTree::Remove(ManagedWindow* window)
{
    if (!window)
        return;

    BSPLeaf* leaf = FindLeaf(m_root.get(), window);

    if (!leaf)
        return;

    if (leaf == m_lastFocused)
        m_lastFocused = nullptr;

    if (leaf == m_root.get())
    {
        m_root.reset();
        return;
    }

    BSPSplit* parent = leaf->Parent();

    if (!parent)
        return;

    std::unique_ptr<BSPNode> survivor =
        (parent->Left() == leaf) ? parent->TakeRight() : parent->TakeLeft();

    BSPSplit* grandparent = parent->Parent();

    if (!grandparent)
    {
        m_root = std::move(survivor);

        if (m_root)
            m_root->SetParent(nullptr);

        return;
    }

    if (grandparent->Left() == parent)
        grandparent->SetLeft(std::move(survivor));
    else
        grandparent->SetRight(std::move(survivor));
}

void BSPTree::Focus(ManagedWindow* window)
{
    if (!window)
        return;

    BSPLeaf* leaf = FindLeaf(m_root.get(), window);

    if (leaf)
        m_lastFocused = leaf;
}

void BSPTree::Swap(ManagedWindow* first, ManagedWindow* second)
{
    if (!first || !second || first == second)
        return;

    BSPLeaf* leafA = FindLeaf(m_root.get(), first);
    BSPLeaf* leafB = FindLeaf(m_root.get(), second);

    if (!leafA || !leafB)
        return;

    // The actual fix versus the old implementation: re-point the
    // leaves at each other's windows instead of copying Geometry()
    // rects around. Geometry is untouched, exactly as the spec asks
    // ("не міняти геометрію"); only the window <-> leaf assignment
    // changes, so it survives the very next LayoutEngine pass instead
    // of being silently overwritten by it.
    leafA->SetWindow(second);
    leafB->SetWindow(first);

    if (m_lastFocused == leafA || m_lastFocused == leafB)
    {
        if (first->Focused())
            m_lastFocused = leafB;
        else if (second->Focused())
            m_lastFocused = leafA;
    }
}

void BSPTree::Resize(ManagedWindow* window, int dx, int dy)
{
    if (!window)
        return;

    BSPLeaf* leaf = FindLeaf(m_root.get(), window);

    if (!leaf)
        return;

    BSPSplit* parent = leaf->Parent();

    if (!parent)
        return; // only window on the workspace - nothing to resize against

    const Rect& area = parent->Geometry();

    float delta = 0.0f;

    if (parent->Direction() == SplitDirection::Vertical)
    {
        if (area.width > 0)
            delta = static_cast<float>(dx) / static_cast<float>(area.width);
    }
    else
    {
        if (area.height > 0)
            delta = static_cast<float>(dy) / static_cast<float>(area.height);
    }

    // Ratio is the *first* child's share, and the divider between the
    // two children sits at a screen coordinate of area.(x|y) +
    // first-child-size - so adding delta straight to it moves that
    // divider in the same screen direction you're actually dragging
    // the mouse (right/down = the divider's own coordinate increases),
    // for both children, not just the first one.
    //
    // An earlier version flipped delta's sign whenever `window` was
    // the *second* child, on the theory that "the grabbed window
    // should always grow toward wherever you drag it". In practice
    // that makes the divider itself walk backwards relative to the
    // mouse whenever you grab the right/bottom window: e.g. dragging
    // right with the cursor over a window docked on the right half of
    // the screen would shrink it - its divider sliding left while the
    // cursor moves right - which reads as the window moving inverted
    // to the mouse. Applying delta unconditionally keeps the divider
    // tracking the mouse 1:1 regardless of which side of it you
    // grabbed, matching ordinary drag-to-resize behaviour; which
    // window grows vs. shrinks is then just a consequence of which
    // side of the divider it's on, exactly like dragging a border
    // anywhere else.
    parent->SetRatio(parent->Ratio() + delta);
}

void BSPTree::Rotate(ManagedWindow* window)
{
    if (!window)
        return;

    BSPLeaf* leaf = FindLeaf(m_root.get(), window);

    if (!leaf)
        return;

    BSPSplit* parent = leaf->Parent();

    if (!parent)
        return;

    parent->SetDirection(
        parent->Direction() == SplitDirection::Vertical
            ? SplitDirection::Horizontal
            : SplitDirection::Vertical);
}

void BSPTree::Flip(ManagedWindow* window)
{
    if (!window)
        return;

    BSPLeaf* leaf = FindLeaf(m_root.get(), window);

    if (!leaf)
        return;

    BSPSplit* parent = leaf->Parent();

    if (!parent)
        return;

    parent->SwapChildren();
}

ManagedWindow* BSPTree::FindNeighbor(ManagedWindow* window, Direction direction) const
{
    if (!window)
        return nullptr;

    BSPLeaf* leaf = FindLeaf(m_root.get(), window);

    if (!leaf)
        return nullptr;

    std::vector<BSPLeaf*> leaves;
    CollectLeaves(m_root.get(), leaves);

    const Rect& from = leaf->Geometry();

    BSPLeaf* best = nullptr;
    int bestGap = 0;
    int bestOverlap = 0;

    for (BSPLeaf* candidate : leaves)
    {
        if (candidate == leaf)
            continue;

        const Rect& to = candidate->Geometry();

        bool matches = false;
        int gap = 0;
        int overlap = 0;

        switch (direction)
        {
            case Direction::Right:

                matches = to.x >= from.Right() - 1;
                gap     = to.x - from.Right();
                overlap = std::min(from.Bottom(), to.Bottom()) - std::max(from.y, to.y);
                break;

            case Direction::Left:

                matches = to.Right() <= from.x + 1;
                gap     = from.x - to.Right();
                overlap = std::min(from.Bottom(), to.Bottom()) - std::max(from.y, to.y);
                break;

            case Direction::Down:

                matches = to.y >= from.Bottom() - 1;
                gap     = to.y - from.Bottom();
                overlap = std::min(from.Right(), to.Right()) - std::max(from.x, to.x);
                break;

            case Direction::Up:

                matches = to.Bottom() <= from.y + 1;
                gap     = from.y - to.Bottom();
                overlap = std::min(from.Right(), to.Right()) - std::max(from.x, to.x);
                break;
        }

        if (!matches || overlap <= 0)
            continue;

        if (gap < 0)
            gap = 0;

        if (!best || gap < bestGap || (gap == bestGap && overlap > bestOverlap))
        {
            best = candidate;
            bestGap = gap;
            bestOverlap = overlap;
        }
    }

    return best ? best->Window() : nullptr;
}

ManagedWindow* BSPTree::HitTest(const Point& p) const
{
    std::vector<BSPLeaf*> leaves;
    CollectLeaves(m_root.get(), leaves);

    for (BSPLeaf* leaf : leaves)
    {
        if (leaf->Geometry().Contains(p))
            return leaf->Window();
    }

    return nullptr;
}

bool BSPTree::HasSpaceForAnotherWindow(
    ManagedWindow* window,
    const Rect& tilingArea,
    int innerGap,
    int floorWidth,
    int floorHeight) const
{
    if (!m_root)
        return true; // first window on this workspace always fits

    BSPLeaf* anchor = AnchorLeaf();

    if (!anchor)
        return true; // unreachable in practice (m_root implies >=1 leaf), but harmless

    Rect anchorRect;

    if (!ComputeLeafGeometry(m_root.get(), anchor, tilingArea, innerGap, anchorRect))
        return true; // couldn't locate the anchor - don't block insertion over it

    int anchorMinWidth;
    int anchorMinHeight;
    EffectiveMinSize(anchor->Window(), floorWidth, floorHeight, anchorMinWidth, anchorMinHeight);

    int newMinWidth;
    int newMinHeight;
    EffectiveMinSize(window, floorWidth, floorHeight, newMinWidth, newMinHeight);

    SplitDirection direction;

    if (FeasibleSplitDirection(
            anchorRect, innerGap,
            anchorMinWidth, anchorMinHeight,
            newMinWidth, newMinHeight,
            direction))
        return true;

    // The straightforward split doesn't fit either way - see whether
    // shrinking other tiles (PlanReclaim(), never below their own
    // effective minimum anywhere) would free up enough room. Read-only:
    // this is a probe, so any plan found here is simply discarded
    // rather than applied.
    std::vector<std::pair<BSPSplit*, float>> steps;

    return PlanReclaim(anchor, window, tilingArea, innerGap, floorWidth, floorHeight, direction, steps);
}

std::string BSPTree::Serialize() const
{
    return m_root ? SerializeNode(m_root.get()) : "null";
}

ManagedWindow* BSPTree::Representative(BSPNode* node) const
{
    while (!node->IsLeaf())
        node = static_cast<BSPSplit*>(node)->Left();

    return static_cast<BSPLeaf*>(node)->Window();
}

void BSPTree::CollectPlacementRulesRecursive(
    BSPNode* node,
    std::vector<PlacementRule>& out) const
{
    if (node->IsLeaf())
        return;

    auto* split = static_cast<BSPSplit*>(node);

    ManagedWindow* leftRep = Representative(split->Left());
    ManagedWindow* rightRep = Representative(split->Right());

    // Emitted *before* recursing: a rule nested inside the right
    // subtree may itself need rightRep as its own neighbor, and this
    // is the rule that actually puts rightRep in the tree in the
    // first place. See this method's declaration in BSPTree.h for
    // why post-order (rule-after-recursion) breaks replay.
    out.push_back(PlacementRule{rightRep->Id(), leftRep->Id(), split->Direction()});

    CollectPlacementRulesRecursive(split->Left(), out);
    CollectPlacementRulesRecursive(split->Right(), out);
}

std::vector<BSPTree::PlacementRule> BSPTree::CollectPlacementRules() const
{
    std::vector<PlacementRule> rules;

    if (m_root)
        CollectPlacementRulesRecursive(m_root.get(), rules);

    return rules;
}

bool BSPTree::InsertNextTo(
    ManagedWindow* window,
    ManagedWindow* neighbor,
    SplitDirection direction)
{
    if (!window || !neighbor)
        return false;

    BSPLeaf* anchor = FindLeaf(m_root.get(), neighbor);

    if (!anchor)
        return false;

    SpliceIn(window, anchor, direction);
    return true;
}

BSPLeaf* BSPTree::FindLeaf(BSPNode* node, ManagedWindow* window) const
{
    if (!node)
        return nullptr;

    if (node->IsLeaf())
    {
        auto* leaf = static_cast<BSPLeaf*>(node);
        return (leaf->Window() == window) ? leaf : nullptr;
    }

    auto* split = static_cast<BSPSplit*>(node);

    if (auto* found = FindLeaf(split->Left(), window))
        return found;

    return FindLeaf(split->Right(), window);
}

void BSPTree::CollectLeaves(BSPNode* node, std::vector<BSPLeaf*>& out) const
{
    if (!node)
        return;

    if (node->IsLeaf())
    {
        out.push_back(static_cast<BSPLeaf*>(node));
        return;
    }

    auto* split = static_cast<BSPSplit*>(node);

    CollectLeaves(split->Left(), out);
    CollectLeaves(split->Right(), out);
}

bool BSPTree::ComputeLeafGeometry(
    BSPNode* node,
    BSPLeaf* target,
    const Rect& area,
    int innerGap,
    Rect& out) const
{
    return ComputeNodeGeometry(node, target, area, innerGap, out);
}

bool BSPTree::ComputeNodeGeometry(
    BSPNode* node,
    BSPNode* target,
    const Rect& area,
    int innerGap,
    Rect& out) const
{
    if (!node)
        return false;

    if (node == target)
    {
        out = area;
        return true;
    }

    if (node->IsLeaf())
        return false; // a leaf has no children - `target` isn't under it

    auto* split = static_cast<BSPSplit*>(node);

    Rect first;
    Rect second;
    split->Subdivide(area, innerGap, first, second);

    if (ComputeNodeGeometry(split->Left(), target, first, innerGap, out))
        return true;

    return ComputeNodeGeometry(split->Right(), target, second, innerGap, out);
}

void BSPTree::EffectiveMinSize(
    ManagedWindow* window,
    int floorWidth,
    int floorHeight,
    int& outMinWidth,
    int& outMinHeight)
{
    // Deliberately floor-only, unconditionally, for every window this
    // is ever called with - only tiled ("father") windows ever reach
    // the BSP tree at all (a transient/dialog/utility "child" window,
    // or one matching `windowrule=float`, never gets past
    // WindowManager::Manage()'s wantsFloat check to be inserted here
    // in the first place), and once tiled, Kohiko already force-resizes
    // a window to whatever its tile says regardless of what it asked
    // for (see HandleConfigureRequest) - a client's own declared
    // WM_NORMAL_HINTS minimum was never actually a hard technical
    // requirement, just a size Kohiko used to also *demand* be
    // available before it would agree to tile the window at all. That
    // demand is what caused a real app (Discord, Telegram - both
    // declare a fairly large preferred minimum) to fall back to
    // floating on a small/cramped monitor even though there was room
    // for a smaller-than-preferred tile. So: no window's own
    // preference is treated as a requirement here - only the ordinary
    // general.min_tile_width/height floor is, for every window,
    // unconditionally. A tiled window only ever becomes floating again
    // through an explicit user action (SUPER+SPACE/toggle_floating -
    // see ToggleFloating()) or the separate, narrower
    // tiling-misbehavior escape valve for a client that keeps actively
    // fighting its assigned tile size once it already has one (see
    // ApplyTilingMisbehaviorFallback) - never merely because it asked
    // for more than it was given.
    (void)window;

    outMinWidth  = floorWidth;
    outMinHeight = floorHeight;
}

bool BSPTree::FeasibleSplitDirection(
    const Rect& rect,
    int innerGap,
    int firstMinWidth,
    int firstMinHeight,
    int secondMinWidth,
    int secondMinHeight,
    SplitDirection& outDirection)
{
    SplitDirection natural = DirectionForRect(rect);
    SplitDirection alternate =
        (natural == SplitDirection::Vertical) ? SplitDirection::Horizontal : SplitDirection::Vertical;

    auto fits = [&](SplitDirection dir)
    {
        BSPSplit probe(dir);

        Rect first;
        Rect second;
        probe.Subdivide(rect, innerGap, first, second);

        return first.width   >= firstMinWidth  && first.height  >= firstMinHeight &&
               second.width  >= secondMinWidth && second.height >= secondMinHeight;
    };

    if (fits(natural))
    {
        outDirection = natural;
        return true;
    }

    if (fits(alternate))
    {
        outDirection = alternate;
        return true;
    }

    return false;
}

bool BSPTree::SubtreeFits(
    BSPNode* node,
    const Rect& area,
    int innerGap,
    int floorWidth,
    int floorHeight)
{
    if (!node)
        return true; // nothing here - trivially fine

    if (node->IsLeaf())
    {
        auto* leaf = static_cast<BSPLeaf*>(node);

        int minWidth;
        int minHeight;
        EffectiveMinSize(leaf->Window(), floorWidth, floorHeight, minWidth, minHeight);

        return area.width >= minWidth && area.height >= minHeight;
    }

    auto* split = static_cast<BSPSplit*>(node);

    Rect first;
    Rect second;
    split->Subdivide(area, innerGap, first, second);

    return SubtreeFits(split->Left(),  first,  innerGap, floorWidth, floorHeight) &&
           SubtreeFits(split->Right(), second, innerGap, floorWidth, floorHeight);
}

bool BSPTree::PlanReclaim(
    BSPLeaf* leaf,
    ManagedWindow* window,
    const Rect& tilingArea,
    int innerGap,
    int floorWidth,
    int floorHeight,
    SplitDirection& outDirection,
    std::vector<std::pair<BSPSplit*, float>>& outSteps) const
{
    outSteps.clear();

    int anchorMinWidth;
    int anchorMinHeight;
    EffectiveMinSize(leaf->Window(), floorWidth, floorHeight, anchorMinWidth, anchorMinHeight);

    int newMinWidth;
    int newMinHeight;
    EffectiveMinSize(window, floorWidth, floorHeight, newMinWidth, newMinHeight);

    // The ancestor chain from `leaf` up to the root, nearest first,
    // remembering at each level which side `leaf` descends through -
    // that's the side every step below tries to grow, at the expense
    // of whatever's on the *other* side of that same split.
    struct Step
    {
        BSPSplit* split;
        bool anchorIsLeft;
    };

    std::vector<Step> chain;

    BSPNode* cursor = leaf;
    BSPSplit* parent = leaf->Parent();

    while (parent != nullptr)
    {
        chain.push_back({parent, parent->Left() == cursor});
        cursor = parent;
        parent = parent->Parent();
    }

    // Try progressively wider "reclaim scopes": first squeeze only
    // `leaf`'s immediate sibling, then - if that's still not enough -
    // also squeeze the next split up, and so on to the root. Each
    // level's ratio, once chosen, stays fixed while wider levels are
    // tried, so nearer (smaller, less disruptive) squeezes always win
    // out over farther ones whenever either alone would do.
    for (std::size_t i = 0; i < chain.size(); ++i)
    {
        BSPSplit* split = chain[i].split;
        bool anchorIsLeft = chain[i].anchorIsLeft;

        // This split's own rect, using every real (unmodified) ratio
        // above it - none of the overrides chosen in earlier
        // iterations touch anything above `split`, only nodes
        // strictly between it and `leaf`.
        Rect splitRect;

        if (!ComputeNodeGeometry(m_root.get(), split, tilingArea, innerGap, splitRect))
            return false; // unreachable - `split` is definitely somewhere under m_root

        float currentRatio = split->Ratio();
        float extreme = anchorIsLeft ? 0.95f : 0.05f;

        BSPNode* siblingNode = anchorIsLeft ? split->Right() : split->Left();

        // Bisect on how far toward `extreme` this split's ratio can
        // move (t=0 -> stay exactly where it is - always feasible,
        // it's the real current tree; t=1 -> all the way to
        // `extreme`) before the shrinking sibling side stops meeting
        // its own effective minimum (floorWidth x floorHeight, or a
        // leaf's own larger declared minimum) somewhere inside it.
        // Subdivide() is linear in ratio, so feasibility is monotonic
        // in t and plain bisection is exact enough at pixel
        // granularity well within the fixed iteration count below.
        float loT = 0.0f;
        float hiT = 1.0f;

        for (int iter = 0; iter < 24; ++iter)
        {
            float midT = (loT + hiT) * 0.5f;
            float ratio = currentRatio + midT * (extreme - currentRatio);

            BSPSplit probe(split->Direction());
            probe.SetRatio(ratio);

            Rect first;
            Rect second;
            probe.Subdivide(splitRect, innerGap, first, second);

            Rect siblingRect = anchorIsLeft ? second : first;

            if (SubtreeFits(siblingNode, siblingRect, innerGap, floorWidth, floorHeight))
                loT = midT;
            else
                hiT = midT;
        }

        float bestRatio = currentRatio + loT * (extreme - currentRatio);

        outSteps.push_back({split, bestRatio});

        // Walk back down from `split` to `leaf` - applying this
        // level's just-chosen ratio plus every nearer one already in
        // outSteps - to see whether `leaf` would now be big enough to
        // split.
        Rect current = splitRect;

        for (std::size_t idx = i + 1; idx-- > 0; )
        {
            BSPSplit* s = chain[idx].split;
            bool aLeft = chain[idx].anchorIsLeft;
            float ratio = outSteps[idx].second;

            BSPSplit probe(s->Direction());
            probe.SetRatio(ratio);

            Rect first;
            Rect second;
            probe.Subdivide(current, innerGap, first, second);

            current = aLeft ? first : second;
        }

        if (FeasibleSplitDirection(
                current, innerGap,
                anchorMinWidth, anchorMinHeight,
                newMinWidth, newMinHeight,
                outDirection))
            return true; // outSteps (indices 0..i) is the plan to apply
    }

    outSteps.clear();
    return false;
}

SplitDirection BSPTree::DirectionForRect(const Rect& rect)
{
    // Wide slot -> split it left/right; tall slot -> split it top/bottom.
    // Keeps automatically-tiled layouts from degenerating into thin strips.
    return (rect.width >= rect.height)
        ? SplitDirection::Vertical
        : SplitDirection::Horizontal;
}

std::string BSPTree::SerializeNode(const BSPNode* node) const
{
    if (!node)
        return "null";

    std::ostringstream out;

    if (node->IsLeaf())
    {
        const auto* leaf = static_cast<const BSPLeaf*>(node);
        const Rect& r = leaf->Geometry();

        out << "{\"type\":\"leaf\",\"window\":"
            << (leaf->Window() ? static_cast<unsigned long>(leaf->Window()->Id()) : 0UL)
            << ",\"x\":" << r.x << ",\"y\":" << r.y
            << ",\"width\":" << r.width << ",\"height\":" << r.height << "}";
    }
    else
    {
        const auto* split = static_cast<const BSPSplit*>(node);

        out << "{\"type\":\"split\",\"direction\":\""
            << (split->Direction() == SplitDirection::Vertical ? "vertical" : "horizontal")
            << "\",\"ratio\":" << split->Ratio()
            << ",\"first\":" << SerializeNode(split->Left())
            << ",\"second\":" << SerializeNode(split->Right())
            << "}";
    }

    return out.str();
}

}
