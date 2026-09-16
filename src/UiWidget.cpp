#include "UiWidget.h"
#include "UiWindow.h"

#include <algorithm>

namespace Kohiko
{

Widget* Widget::AddChild(std::unique_ptr<Widget> child)
{
    Widget* raw = child.get();
    m_children.push_back(std::move(child));
    return raw;
}

std::unique_ptr<Widget> Widget::ReleaseChild(Widget* child)
{
    for (auto it = m_children.begin(); it != m_children.end(); ++it)
    {
        if (it->get() == child)
        {
            std::unique_ptr<Widget> released = std::move(*it);
            m_children.erase(it);
            return released;
        }
    }
    return nullptr;
}

void Widget::Draw(UiWindow& window)
{
    DrawChildren(window);
}

void Widget::DrawChildren(UiWindow& window)
{
    for (auto& child : m_children)
    {
        if (child->visible)
            child->Draw(window);
    }
}

// --- Label ---------------------------------------------------------------

void Label::Draw(UiWindow& window)
{
    std::uint32_t c = color != 0 ? color : window.Theme().foreground;
    window.DrawTextClipped(bounds, text, c, align);
    DrawChildren(window);
}

// --- Separator -------------------------------------------------------------

void Separator::Draw(UiWindow& window)
{
    window.FillRect(bounds, window.Theme().border);
    DrawChildren(window);
}

// --- ClickableContainer -----------------------------------------------------

void ClickableContainer::Draw(UiWindow& window)
{
    const UiTheme& theme = window.Theme();
    window.FillRoundedRect(bounds, selected ? theme.surfaceActive : theme.surface, 8);
    DrawChildren(window);
}

void ClickableContainer::OnRelease(Point p)
{
    if (onClick && bounds.Contains(p))
        onClick();
}

// --- SectionHeader -----------------------------------------------------------

void SectionHeader::Draw(UiWindow& window)
{
    window.DrawTextClipped(bounds, text, window.Theme().accent, Label::Align::Left);
    DrawChildren(window);
}

// --- Button ------------------------------------------------------------------

void Button::Draw(UiWindow& window)
{
    const UiTheme& theme = window.Theme();

    std::uint32_t outline = theme.border;
    std::uint32_t outlineText = theme.foreground;
    if (!primary)
    {
        switch (tone)
        {
            case Tone::Accent: outline = theme.accent; outlineText = theme.accent; break;
            case Tone::Danger: outline = theme.danger;  outlineText = theme.danger;  break;
            case Tone::Neutral: default: break;
        }
    }

    std::uint32_t bg = primary ? theme.accent : theme.surface;
    if (!enabled)
        bg = theme.surface;
    else if (m_pressed)
        bg = primary ? theme.accent : theme.surfaceActive;

    window.FillRoundedRect(bounds, bg, 8);
    if (!primary)
        window.DrawBorder(bounds, enabled ? outline : theme.border);

    std::uint32_t fg = primary ? theme.accentForeground : outlineText;
    if (!enabled)
        fg = theme.muted;

    window.DrawTextClipped(bounds, label, fg, Label::Align::Center);
    DrawChildren(window);
}

void Button::OnPress(Point)
{
    m_pressed = true;
}

void Button::OnRelease(Point p)
{
    bool wasInside = bounds.Contains(p);
    m_pressed = false;

    if (wasInside && enabled && onClick)
        onClick();
}

// --- IconButton ------------------------------------------------------------------

void IconButton::Draw(UiWindow& window)
{
    const UiTheme& theme = window.Theme();

    std::uint32_t bg = theme.surface;
    if (enabled && m_pressed)
        bg = theme.surfaceActive;

    window.FillRoundedRect(bounds, bg, 8);
    window.DrawBorder(bounds, theme.border);
    window.DrawIcon(bounds.Shrunk(9), iconName);

    DrawChildren(window);
}

void IconButton::OnPress(Point)
{
    m_pressed = true;
}

void IconButton::OnRelease(Point p)
{
    bool wasInside = bounds.Contains(p);
    m_pressed = false;

    if (wasInside && enabled && onClick)
        onClick();
}

// --- ToggleSwitch --------------------------------------------------------------

void ToggleSwitch::Draw(UiWindow& window)
{
    const UiTheme& theme = window.Theme();

    std::uint32_t trackColor = value ? theme.accent : theme.surfaceActive;
    if (!enabled)
        trackColor = theme.surface;

    window.FillRoundedRect(bounds, trackColor, bounds.height / 2);

    int knobDiameter = bounds.height - 6;
    int knobX = value ? bounds.Right() - knobDiameter - 3 : bounds.x + 3;
    Rect knob{ knobX, bounds.y + 3, knobDiameter, knobDiameter };
    window.FillRoundedRect(knob, theme.foreground, knobDiameter / 2);

    DrawChildren(window);
}

void ToggleSwitch::OnRelease(Point p)
{
    if (!enabled || !bounds.Contains(p))
        return;

    value = !value;
    if (onChange)
        onChange(value);
}

// --- Slider --------------------------------------------------------------------

void Slider::Draw(UiWindow& window)
{
    const UiTheme& theme = window.Theme();

    Rect track = bounds;
    track.y = bounds.y + bounds.height / 2 - 3;
    track.height = 6;

    window.FillRoundedRect(track, theme.surfaceActive, 3);

    float fraction = maxValue > 0.0f ? std::clamp(value / maxValue, 0.0f, 1.0f) : 0.0f;
    Rect fill = track;
    fill.width = static_cast<int>(track.width * fraction);
    if (fill.width > 0)
        window.FillRoundedRect(fill, theme.accent, 3);

    int knobDiameter = bounds.height;
    int knobX = bounds.x + static_cast<int>((bounds.width - knobDiameter) * fraction);
    Rect knob{ knobX, bounds.y, knobDiameter, knobDiameter };
    window.FillRoundedRect(knob, theme.foreground, knobDiameter / 2);

    DrawChildren(window);
}

void Slider::SetFromX(int x)
{
    float fraction = bounds.width > 0
        ? std::clamp(static_cast<float>(x - bounds.x) / static_cast<float>(bounds.width), 0.0f, 1.0f)
        : 0.0f;

    value = fraction * maxValue;
    if (onChange)
        onChange(value);
}

void Slider::OnPress(Point p)
{
    SetFromX(p.x);
}

void Slider::OnMotion(Point p, bool pressed)
{
    if (pressed)
        SetFromX(p.x);
}

void Slider::OnRelease(Point p)
{
    SetFromX(p.x);
    if (onCommit)
        onCommit(value);
}

// --- IconView ------------------------------------------------------------------

void IconView::Draw(UiWindow& window)
{
    window.DrawIcon(bounds, name);
    DrawChildren(window);
}

// --- Badge -----------------------------------------------------------------------

int Badge::MeasureWidth(UiWindow& window, const std::string& text)
{
    return window.TextWidth(text) + 20;
}

void Badge::Draw(UiWindow& window)
{
    const UiTheme& theme = window.Theme();

    std::uint32_t background = theme.surfaceActive;
    std::uint32_t foreground = theme.foreground;

    switch (tone)
    {
        case Tone::Accent:  background = theme.accent;  foreground = theme.accentForeground; break;
        case Tone::Positive: background = theme.success;  foreground = theme.accentForeground; break;
        case Tone::Danger:  background = theme.danger;   foreground = theme.accentForeground; break;
        case Tone::Neutral: default: break;
    }

    window.FillRoundedRect(bounds, background, bounds.height / 2);
    window.DrawTextClipped(bounds, text, foreground, Label::Align::Center);

    DrawChildren(window);
}

// --- Card ------------------------------------------------------------------------

void Card::Draw(UiWindow& window)
{
    const UiTheme& theme = window.Theme();
    window.FillRoundedRect(bounds, theme.surface, 12);
    window.DrawBorder(bounds, theme.border);
    DrawChildren(window);
}

// --- TextField -------------------------------------------------------------------

void TextField::Draw(UiWindow& window)
{
    const UiTheme& theme = window.Theme();

    window.FillRoundedRect(bounds, theme.surface, 8);
    window.DrawBorder(bounds, focused ? theme.accent : theme.border);

    int textX = bounds.x + 14;
    if (!iconName.empty())
    {
        int iconSize = std::min(18, bounds.height - 8);
        Rect iconRect{ bounds.x + 12, bounds.y + (bounds.height - iconSize) / 2, iconSize, iconSize };
        window.DrawIcon(iconRect, iconName);
        textX = iconRect.Right() + 10;
    }

    Rect textRect{ textX, bounds.y, std::max(0, bounds.Right() - textX - 12), bounds.height };

    if (text.empty() && !focused)
    {
        window.DrawTextClipped(textRect, placeholder, theme.muted, Label::Align::Left);
    }
    else
    {
        window.DrawTextClipped(textRect, text, theme.foreground, Label::Align::Left);

        if (focused)
        {
            int cursorX = textRect.x + window.TextWidth(text) + 1;
            cursorX = std::min(cursorX, textRect.Right() - 2);
            Rect cursor{ cursorX, bounds.y + 8, 2, std::max(1, bounds.height - 16) };
            window.FillRect(cursor, theme.accent);
        }
    }

    DrawChildren(window);
}

void TextField::OnKeyInput(KeyAction action, const std::string& utf8)
{
    switch (action)
    {
        case KeyAction::Printable:
            text += utf8;
            break;

        case KeyAction::Backspace:
            if (!text.empty())
            {
                // Drop one whole UTF-8 codepoint, not just the last
                // byte, so backspacing over an accented or non-Latin
                // character removes it in a single press instead of
                // leaving a mangled trailing byte behind.
                std::size_t cut = text.size() - 1;
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
                    --cut;
                text.erase(cut);
            }
            break;

        default:
            return; // no text change - don't fire onChange for e.g. arrow keys
    }

    if (onChange)
        onChange(text);
}

// --- MakePageHeader ------------------------------------------------------------

std::unique_ptr<Widget> MakePageHeader(
    const Rect& bounds,
    const std::string& title,
    const std::string& subtitle,
    std::unique_ptr<Widget> trailing,
    int trailingWidth)
{
    auto header = std::make_unique<Widget>();
    header->bounds = bounds;

    int textRight = subtitle.empty() && !trailing ? bounds.width : bounds.width - (trailing ? trailingWidth + 20 : 0);

    auto titleLabel = std::make_unique<Label>();
    titleLabel->text = title;
    titleLabel->bounds = { bounds.x, bounds.y, textRight, subtitle.empty() ? bounds.height : bounds.height / 2 + 4 };
    header->AddChild(std::move(titleLabel));

    if (!subtitle.empty())
    {
        auto subtitleLabel = std::make_unique<Label>();
        subtitleLabel->text = subtitle;
        subtitleLabel->color = UiTheme::Default().muted;
        subtitleLabel->bounds = { bounds.x, bounds.y + bounds.height / 2 + 2, textRight, bounds.height / 2 };
        header->AddChild(std::move(subtitleLabel));
    }

    if (trailing)
    {
        trailing->bounds = {
            bounds.Right() - trailingWidth,
            bounds.y + (bounds.height - trailing->bounds.height) / 2,
            trailingWidth,
            trailing->bounds.height > 0 ? trailing->bounds.height : bounds.height
        };
        header->AddChild(std::move(trailing));
    }

    return header;
}

// --- MakeSettingsRow -------------------------------------------------------------

Widget* MakeSettingsRow(
    Widget& parent,
    const Rect& bounds,
    const std::string& label,
    std::unique_ptr<Widget> control,
    int controlWidth)
{
    auto labelWidget = std::make_unique<Label>();
    labelWidget->text = label;
    labelWidget->bounds = { bounds.x, bounds.y, bounds.width - controlWidth - 16, bounds.height };
    parent.AddChild(std::move(labelWidget));

    int controlHeight = control->bounds.height > 0 ? control->bounds.height : bounds.height;
    control->bounds = {
        bounds.Right() - controlWidth,
        bounds.y + (bounds.height - controlHeight) / 2,
        controlWidth,
        controlHeight
    };

    return parent.AddChild(std::move(control));
}

// --- MakeDetailRow -----------------------------------------------------------------

std::unique_ptr<Widget> MakeDetailRow(
    const Rect& bounds,
    const std::string& label,
    const std::string& value,
    std::uint32_t valueColor)
{
    auto row = std::make_unique<Widget>();
    row->bounds = bounds;

    auto labelWidget = std::make_unique<Label>();
    labelWidget->text = label;
    labelWidget->color = UiTheme::Default().muted;
    labelWidget->bounds = { bounds.x, bounds.y, bounds.width / 2, bounds.height };
    row->AddChild(std::move(labelWidget));

    auto valueWidget = std::make_unique<Label>();
    valueWidget->text = value;
    valueWidget->color = valueColor;
    valueWidget->align = Label::Align::Right;
    valueWidget->bounds = { bounds.x + bounds.width / 2, bounds.y, bounds.width / 2, bounds.height };
    row->AddChild(std::move(valueWidget));

    return row;
}

}
