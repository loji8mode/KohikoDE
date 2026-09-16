#include "UiListRow.h"
#include "UiWindow.h"

namespace Kohiko
{

Widget* ListRow::Layout(const Rect& rowBounds, std::unique_ptr<Widget> trailing, int trailingWidth)
{
    bounds = rowBounds;

    const int margin = 14;
    const int iconSize = std::min(28, rowBounds.height - 8);

    m_iconRect = {
        rowBounds.x + margin,
        rowBounds.y + (rowBounds.height - iconSize) / 2,
        iconSize,
        iconSize
    };

    int textLeft = iconName.empty() ? rowBounds.x + margin : m_iconRect.Right() + margin;
    int textRight = rowBounds.Right() - margin - (trailingWidth > 0 ? trailingWidth + margin : 0);

    m_textRect = { textLeft, rowBounds.y, std::max(0, textRight - textLeft), rowBounds.height };

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

    window.FillRoundedRect(bounds, selected ? theme.surfaceActive : theme.surface, 8);

    if (!iconName.empty())
        window.DrawIcon(m_iconRect, iconName);

    if (subtitle.empty())
    {
        window.DrawTextClipped(m_textRect, title, theme.foreground, Label::Align::Left);
    }
    else
    {
        Rect titleRect = m_textRect;
        titleRect.height = m_textRect.height / 2 + 2;
        Rect subtitleRect = m_textRect;
        subtitleRect.y = titleRect.Bottom();
        subtitleRect.height = m_textRect.height - titleRect.height;

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
