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
    window.DrawTextClipped(bounds, text, window.Theme().muted, Label::Align::Left);
    DrawChildren(window);
}

// --- Button ------------------------------------------------------------------

void Button::Draw(UiWindow& window)
{
    const UiTheme& theme = window.Theme();

    std::uint32_t bg = primary ? theme.accent : theme.surface;
    if (!enabled)
        bg = theme.surface;
    else if (m_pressed)
        bg = primary ? theme.accent : theme.surfaceActive;

    window.FillRoundedRect(bounds, bg, 6);
    if (!primary)
        window.DrawBorder(bounds, theme.border);

    std::uint32_t fg = primary ? theme.accentForeground : theme.foreground;
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

}
