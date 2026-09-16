#include "UiSidebar.h"
#include "UiWindow.h"

namespace Kohiko
{

void Sidebar::Layout(const Rect& area, int rowHeight)
{
    bounds = area;
    m_itemRects.clear();

    for (std::size_t i = 0; i < m_items.size(); ++i)
        m_itemRects.push_back({ area.x, area.y + static_cast<int>(i) * rowHeight, area.width, rowHeight });
}

void Sidebar::Draw(UiWindow& window)
{
    const UiTheme& theme = window.Theme();

    for (std::size_t i = 0; i < m_items.size(); ++i)
    {
        const Rect& rect = m_itemRects[i];
        bool isSelected = static_cast<int>(i) == m_selected;

        if (isSelected)
            window.FillRoundedRect(rect.Shrunk(4), theme.surfaceActive, 8);

        Rect textRect = rect.Shrunk(4);
        int iconSize = 20;

        if (!m_items[i].iconName.empty())
        {
            Rect iconRect{ textRect.x + 4, textRect.y + (textRect.height - iconSize) / 2, iconSize, iconSize };
            window.DrawIcon(iconRect, m_items[i].iconName);
            textRect.x += iconSize + 12;
            textRect.width -= iconSize + 12;
        }

        window.DrawTextClipped(textRect, m_items[i].label,
            isSelected ? theme.foreground : theme.muted, Label::Align::Left);
    }

    DrawChildren(window);
}

void Sidebar::OnRelease(Point p)
{
    for (std::size_t i = 0; i < m_itemRects.size(); ++i)
    {
        if (m_itemRects[i].Contains(p))
        {
            m_selected = static_cast<int>(i);
            if (onSelect)
                onSelect(m_selected);
            return;
        }
    }
}

}
