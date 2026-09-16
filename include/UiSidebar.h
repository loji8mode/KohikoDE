#pragma once

#include "UiWidget.h"

#include <vector>

namespace Kohiko
{

// A vertical list of page names down one side of the window, one
// highlighted as current - what makes kohiko-audio's Output/Input
// split (and every future page the spec's "future-ready architecture"
// section lists: per-app volume, equalizer, ...) just another entry
// in a list rather than a hardcoded pair of tabs, and likewise for
// kohiko-network's Wi-Fi/Ethernet/VPN split and kohiko-bluetooth's
// (currently single) device page.
class Sidebar : public Widget
{
public:

    struct Item
    {
        std::string label;
        std::string iconName;
    };

    void SetItems(std::vector<Item> items) { m_items = std::move(items); }

    // Lays every item out vertically starting at `area`'s top-left,
    // `area`'s width, `rowHeight` px each - sets this widget's own
    // bounds to `area`.
    void Layout(const Rect& area, int rowHeight = 44);

    int Selected() const { return m_selected; }
    void SetSelected(int index) { m_selected = index; }

    std::function<void(int)> onSelect;

    void Draw(UiWindow& window) override;
    bool WantsInput() const override { return true; }
    void OnRelease(Point p) override;

private:

    std::vector<Item> m_items;
    std::vector<Rect> m_itemRects;
    int m_selected = 0;
};

}
