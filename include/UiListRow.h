#pragma once

#include "UiWidget.h"

namespace Kohiko
{

// One row in a device/network/connection list - an icon, a title, an
// optional subtitle, and an optional trailing control (a
// ToggleSwitch, a Button, or just a Label showing "87%"/"-52 dBm"/...).
// This is the single most-reused piece of UI across kohiko-audio (one
// row per output/input device), kohiko-network (one row per Wi-Fi
// network/saved connection) and kohiko-bluetooth (one row per
// paired/discovered device) - every one of the spec's three device
// lists reduces to "rows of this shape", which is exactly why this
// exists as shared infrastructure rather than three near-identical
// hand-rolled row types.
class ListRow : public Widget
{
public:

    std::string iconName;
    std::string title;
    std::string subtitle;

    // Highlights the row (e.g. "this is the current default output
    // device", "this Wi-Fi network is the one currently connected") -
    // distinct from any interactive state, since a row can be the
    // current selection without being clickable at all (see
    // kohiko-network's read-only "currently connected" summary row).
    bool selected = false;

    // Mockup-style row: no rounded card of its own, just a thin
    // bottom divider and (when selected) a flat highlight plus a
    // left accent bar, meant to sit packed edge-to-edge inside a
    // shared Card alongside sibling rows - see e.g. kohiko-network's
    // AVAILABLE NETWORKS list or kohiko-audio's OUTPUT DEVICES list.
    // false keeps the original look (its own rounded surface/
    // surfaceActive background, no divider) for anything still using
    // ListRow as a standalone card.
    bool flat = false;

    // Only meaningful when flat - the caller sets this false on the
    // last row of a group so the enclosing Card's own bottom border
    // is the only line there, rather than doubling up.
    bool showDivider = true;

    std::function<void()> onClick;

    // Positions this row within `rowBounds`; if `trailing` is given,
    // it's added as a child pinned to the right edge, `trailingWidth`
    // px wide and vertically centered, and the returned pointer is the
    // *trailing* widget (so the caller can immediately wire up e.g. a
    // ToggleSwitch's onChange) - not this row itself, since the row is
    // already in hand as `this`.
    Widget* Layout(const Rect& rowBounds, std::unique_ptr<Widget> trailing = nullptr, int trailingWidth = 0);

    void Draw(UiWindow& window) override;
    bool WantsInput() const override { return static_cast<bool>(onClick); }
    void OnRelease(Point p) override;

private:

    // Icon/text position and size relative to bounds.x/y - not
    // absolute Rects - computed once in Layout() and turned back into
    // real Rects fresh in every Draw()/hit-test by adding the
    // *current* bounds.x/y. This is what keeps them correct if bounds
    // ever moves after Layout() (e.g. ScrollView::SetContent()
    // translating a freshly-built page into place) without needing
    // every such move to also know how to update a row's internal
    // layout state.
    bool m_hasIcon = false;
    int m_iconOffsetX = 0;
    int m_iconOffsetY = 0;
    int m_iconSize = 0;

    int m_textOffsetX = 0;
    int m_textOffsetY = 0;
    int m_textWidth = 0;
    int m_textHeight = 0;
};

}
