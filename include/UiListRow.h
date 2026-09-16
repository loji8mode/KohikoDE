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

    Rect m_iconRect;
    Rect m_textRect;
};

}
