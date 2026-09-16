#include "UiScrollView.h"
#include "UiWindow.h"

#include <algorithm>

namespace Kohiko
{

void ScrollView::Translate(Widget* widget, int dx, int dy)
{
    widget->bounds.x += dx;
    widget->bounds.y += dy;

    for (auto& child : widget->Children())
        Translate(child.get(), dx, dy);
}

void ScrollView::SetContent(std::unique_ptr<Widget> content)
{
    ClearChildren();
    m_scrollOffset = 0;

    // Callers set ScrollView::bounds before calling this (see the
    // header comment) - content starts pinned to that same top-left
    // corner and width, keeping only the height the caller already
    // computed.
    content->bounds.x = bounds.x;
    content->bounds.y = bounds.y;
    content->bounds.width = bounds.width;

    m_content = AddChild(std::move(content));
}

void ScrollView::Draw(UiWindow& window)
{
    window.SetClip(bounds);
    DrawChildren(window);
    window.ClearClip();
}

void ScrollView::OnScroll(Point, int delta)
{
    if (!m_content)
        return;

    const int step = 48;
    int maxOffset = std::max(0, m_content->bounds.height - bounds.height);
    int newOffset = std::clamp(m_scrollOffset - delta * step, 0, maxOffset);

    int diff = m_scrollOffset - newOffset;
    if (diff != 0)
        Translate(m_content, 0, diff);

    m_scrollOffset = newOffset;
}

}
