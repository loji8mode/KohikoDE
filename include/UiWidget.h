#pragma once

#include "Types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Kohiko
{

class UiWindow;

// Base of every widget in the shared UI toolkit. Deliberately much
// smaller than SettingsWindow's own hand-rolled per-field widget
// handling (see include/SettingsWindow.h) - that window predates this
// toolkit and already works, so it's left exactly as it is (per the
// spec's own "no regressions" rule) rather than migrated onto this;
// this toolkit exists so kohiko-audio/network/bluetooth - three
// *new* apps with a shared, simpler "list of rows with a control on
// the right" shape - don't each reinvent it a third and fourth and
// fifth time.
//
// Composite by default (every widget can have children) rather than a
// separate Container type: a plain Widget with no drawing/input logic
// of its own is already a perfectly good grouping box.
class Widget
{
public:

    virtual ~Widget() = default;

    Rect bounds;
    bool visible = true;

    Widget* AddChild(std::unique_ptr<Widget> child);
    const std::vector<std::unique_ptr<Widget>>& Children() const { return m_children; }
    void ClearChildren() { m_children.clear(); }

    // Draws this widget, then every child, in order (later children
    // paint over earlier ones and are hit-tested first - "last added
    // wins", the same stacking order convention window managers use).
    virtual void Draw(UiWindow& window);

    // Whether this exact widget (not its children) should ever be
    // returned by hit-testing - false for purely decorative/layout
    // widgets (Label, SectionHeader, Separator, a plain grouping
    // Widget) so a click that lands on one of those but not on any
    // interactive child falls through to whatever's underneath.
    virtual bool WantsInput() const { return false; }

    // True for containers (see ScrollView) whose children can be
    // positioned outside the parent's own bounds - a scrolled-off row
    // is still a real child with real coordinates, just not currently
    // visible. Hit-testing checks a clipping widget's own bounds
    // *before* descending into its children, rather than after (the
    // default for every other widget), so a click can never land on a
    // child that's scrolled out of view merely because that child's
    // stale coordinates happen to overlap something else on screen.
    virtual bool ClipsHitTesting() const { return false; }

    virtual void OnPress(Point p) { (void)p; }
    virtual void OnRelease(Point p) { (void)p; }
    virtual void OnMotion(Point p, bool pressed) { (void)p; (void)pressed; }
    virtual void OnScroll(Point p, int delta) { (void)p; (void)delta; }

protected:

    void DrawChildren(UiWindow& window);

private:

    std::vector<std::unique_ptr<Widget>> m_children;
};

class Label : public Widget
{
public:

    std::string text;
    std::uint32_t color = 0; // 0 means "use theme foreground" - see Label.cpp
    bool bold = false;       // toolkit has no bold font variant; reserved for a future second Font
    enum class Align { Left, Center, Right } align = Align::Left;

    void Draw(UiWindow& window) override;
};

class Separator : public Widget
{
public:
    void Draw(UiWindow& window) override;
};

// A plain rounded-rect background that highlights when `selected` and
// fires `onClick` when released inside its bounds - everything
// ListRow provides *except* the fixed icon/title/subtitle/one-trailing-
// control layout, for the cases that need more than one independent
// control inside a row (see kohiko-audio's device rows, which need
// both a volume Slider and a mute ToggleSwitch as siblings, each
// positioned by the caller rather than by a fixed "one trailing slot"
// layout).
class ClickableContainer : public Widget
{
public:

    bool selected = false;
    std::function<void()> onClick;

    void Draw(UiWindow& window) override;
    bool WantsInput() const override { return static_cast<bool>(onClick); }
    void OnRelease(Point p) override;
};

class SectionHeader : public Widget
{
public:
    std::string text;
    void Draw(UiWindow& window) override;
};

class Button : public Widget
{
public:

    std::string label;
    bool primary = false;   // filled accent style vs. plain outline style
    bool enabled = true;
    std::function<void()> onClick;

    void Draw(UiWindow& window) override;
    bool WantsInput() const override { return enabled; }
    void OnPress(Point p) override;
    void OnRelease(Point p) override;

private:
    bool m_pressed = false;
};

class ToggleSwitch : public Widget
{
public:

    bool value = false;
    bool enabled = true;
    std::function<void(bool)> onChange;

    void Draw(UiWindow& window) override;
    bool WantsInput() const override { return enabled; }
    void OnRelease(Point p) override;
};

// A horizontal 0.0-1.0 slider (volume, in the current UI) - drag
// anywhere on the track, not just the thumb, matching the "click
// anywhere on a volume bar to jump there" behaviour of every other
// desktop volume control.
class Slider : public Widget
{
public:

    float value = 1.0f;      // 0.0 - maxValue
    float maxValue = 1.0f;
    std::function<void(float)> onChange;    // called continuously while dragging
    std::function<void(float)> onCommit;    // called once on release

    void Draw(UiWindow& window) override;
    bool WantsInput() const override { return true; }
    void OnPress(Point p) override;
    void OnMotion(Point p, bool pressed) override;
    void OnRelease(Point p) override;

private:
    void SetFromX(int x);
};

// A single centered icon, resolved/rendered via UiWindow::DrawIcon()
// (backed by UiIconCache) - the one-line "give a row an icon" widget
// every device/network/connection row needs, pulled into the shared
// toolkit rather than the same five-line class redefined in each
// app's own .cpp.
class IconView : public Widget
{
public:
    std::string name;
    void Draw(UiWindow& window) override;
};

// A small rounded status pill - "Connected", "Default Device",
// "Good signal", "Off" - used throughout kohiko-audio/network/
// bluetooth's card-based layouts to surface state at a glance without
// it competing with a row's primary title/subtitle text. `tone`
// picks the background/text colors from the theme (Neutral is the
// default muted-gray pill; the others read as a clear "this matters"
// signal without introducing new colors of their own).
class Badge : public Widget
{
public:

    enum class Tone { Neutral, Accent, Positive, Danger };

    std::string text;
    Tone tone = Tone::Neutral;

    void Draw(UiWindow& window) override;

    // The width this badge needs to fit `text` comfortably - callers
    // size `bounds.width` from this (there's no auto-sizing pass in
    // this toolkit; see e.g. ListRow::Layout() for the same
    // caller-computes-the-rect convention).
    static int MeasureWidth(UiWindow& window, const std::string& text);
};

// Composes the title/subtitle/optional-trailing-control header every
// page across all three apps opens with (see e.g. kohiko-network's
// "Wi-Fi / Connected to X" header with its on/off switch, or
// kohiko-audio's "Output Devices" header with its "Test Sound"
// button) - pulled out once rather than hand-built per page, since
// it's the same shape everywhere: a bold title, a muted subtitle
// below it, and one optional right-aligned control.
std::unique_ptr<Widget> MakePageHeader(
    const Rect& bounds,
    const std::string& title,
    const std::string& subtitle,
    std::unique_ptr<Widget> trailing = nullptr,
    int trailingWidth = 0
);

}
