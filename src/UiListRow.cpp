#include "UiListRow.h"
#include "UiWindow.h"

namespace Kohiko
{

Widget* ListRow::Layout(const Rect& rowBounds, std::unique_ptr<Widget> trailing, int trailingWidth)
{
    bounds = rowBounds;

    const int margin = 14;
    m_hasIcon = !iconName.empty();
    m_iconSize = std::min(28, rowBounds.height - 8);
    m_iconOffsetX = margin;
    m_iconOffsetY = (rowBounds.height - m_iconSize) / 2;

    int textLeft = m_hasIcon ? (margin + m_iconSize + margin) : margin;
    int textRight = rowBounds.width - margin - (trailingWidth > 0 ? trailingWidth + margin : 0);

    m_textOffsetX = textLeft;
    m_textOffsetY = 0;
    m_textWidth = std::max(0, textRight - textLeft);
    m_textHeight = rowBounds.height;

    Widget* trailingPtr = nullptr;
    if (trailing)
    {
        int trailingHeight = std::min(trailing->bounds.height > 0 ? trailing->bounds.height : rowBounds.height - 16, rowBounds.height - 8);
        trailing->bounds = {
            rowBounds.Right() - margin - trailingWidth,
            rowBounds.y + (rowBounds.height - trailingHeight) / 2,
            trailingWidth,
            trailingHeight
        };
        trailingPtr = AddChild(std::move(trailing));
    }

    return trailingPtr;
}

void ListRow::Draw(UiWindow& window)
{
    const UiTheme& theme = window.Theme();

    if (flat)
    {
        if (selected)
        {
            window.FillRect(bounds, theme.surfaceHover);
            window.FillRect({ bounds.x, bounds.y, 3, bounds.height }, theme.accent);
        }
        if (showDivider)
            window.FillRect({ bounds.x, bounds.Bottom() - 1, bounds.width, 1 }, theme.border);
    }
    else
    {
        window.FillRoundedRect(bounds, selected ? theme.surfaceActive : theme.surface, 8);
    }

    if (m_hasIcon)
    {
        Rect iconRect{ bounds.x + m_iconOffsetX, bounds.y + m_iconOffsetY, m_iconSize, m_iconSize };
        window.DrawIcon(iconRect, iconName);
    }

    Rect textRect{ bounds.x + m_textOffsetX, bounds.y + m_textOffsetY, m_textWidth, m_textHeight };

    if (subtitle.empty())
    {
        window.DrawTextClipped(textRect, title, theme.foreground, Label::Align::Left);
    }
    else
    {
        int lineHeight = window.TextHeight() + 4;
        int blockTop = textRect.y + (textRect.height - lineHeight * 2) / 2;

        Rect titleRect{ textRect.x, blockTop, textRect.width, lineHeight };
        Rect subtitleRect{ textRect.x, blockTop + lineHeight, textRect.width, lineHeight };

        window.DrawTextClipped(titleRect, title, theme.foreground, Label::Align::Left);
        window.DrawTextClipped(subtitleRect, subtitle, theme.muted, Label::Align::Left);
    }

    DrawChildren(window);
}

void ListRow::OnRelease(Point p)
{
    if (onClick && bounds.Contains(p))
        onClick();
}

}
