#pragma once

#include "Types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Kohiko
{

class UiWindow;

// What UiWindow::HandleEvent() translates an X11 KeyPress into before
// handing it to whichever widget currently has focus (see
// Widget::OnKeyInput below) - a small fixed vocabulary rather than a
// raw KeySym so widgets (see TextField) don't need to pull in
// <X11/keysym.h> themselves. `utf8` carries the actual character(s)
// only for Printable.
enum class KeyAction { Printable, Backspace, Delete, Enter, Escape, Left, Right, Home, End };

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

    // Removes `child` from this widget's children *without*
    // destroying it, handing ownership back to the caller - the one
    // thing ClearChildren()/a plain erase can't do. See
    // ScrollView::SetContent()'s comment for why this matters: a
    // widget can't always safely destroy one of its own children
    // immediately, specifically when that child is the very widget
    // currently executing the callback that triggered the removal.
    // Returns nullptr (and does nothing) if `child` isn't actually one
    // of this widget's children.
    std::unique_ptr<Widget> ReleaseChild(Widget* child);

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

    // True for widgets that actually implement OnScroll() themselves
    // (currently just ScrollView) - lets scroll-wheel dispatch find
    // the nearest scrollable *container* under the pointer instead of
    // whatever plain WantsInput() widget (a Slider, a Button, a
    // ListRow, ...) happens to sit at that exact pixel. Without this,
    // mouse-wheel scrolling a device/network/Bluetooth list only
    // worked over the sliver of "dead space" between rows, since the
    // ordinary click hit-test - correctly, for clicks - always
    // resolves to the most specific interactive widget under the
    // point, and everything except ScrollView silently no-ops
    // OnScroll() by inheriting the base-class default above.
    virtual bool WantsScroll() const { return false; }

    virtual void OnPress(Point p) { (void)p; }
    virtual void OnRelease(Point p) { (void)p; }
    virtual void OnMotion(Point p, bool pressed) { (void)p; (void)pressed; }
    virtual void OnScroll(Point p, int delta) { (void)p; (void)delta; }

    // Keyboard focus, for the one widget in this toolkit that needs
    // it (TextField). False/no-ops for everything else, so adding
    // this to the base class doesn't touch any existing widget.
    // UiWindow grants focus to whatever WantsFocus() widget a click
    // lands on (and takes it away, via OnBlur(), from whatever had it
    // before - including on a click that hits nothing focusable at
    // all), then routes KeyPress events to it via OnKeyInput().
    virtual bool WantsFocus() const { return false; }
    virtual void OnFocus() {}
    virtual void OnBlur() {}
    virtual void OnKeyInput(KeyAction action, const std::string& utf8 = {}) { (void)action; (void)utf8; }

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

    // The outline color/text when !primary - e.g. kohiko-network's
    // "Connect" (Accent) vs. "Disconnect" (Danger) buttons in a
    // selected network's details panel, sitting side by side with
    // otherwise-identical styling. Ignored when primary is set (that
    // style is always a filled accent button regardless of tone).
    enum class Tone { Neutral, Accent, Danger };

    std::string label;
    bool primary = false;   // filled accent style vs. plain outline style
    Tone tone = Tone::Neutral;
    bool enabled = true;
    std::function<void()> onClick;

    void Draw(UiWindow& window) override;
    bool WantsInput() const override { return enabled; }
    void OnPress(Point p) override;
    void OnRelease(Point p) override;

private:
    bool m_pressed = false;
};

// A small square icon-only button - kohiko-network/bluetooth's
// circular refresh control. Same press/hover states as Button, just
// without a text label.
class IconButton : public Widget
{
public:

    std::string iconName;
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

// A rounded-bordered box that visually groups a stack of other
// widgets - the "boxed list"/"panel" shape used everywhere in the
// kohiko-audio/network/bluetooth mockups (an OUTPUT DEVICES card
// holding every output device row, a selected-network details panel,
// a CONNECTED DEVICES card, ...). Deliberately just background +
// border + children, same as every other container here - the caller
// still positions every child's own bounds (see e.g.
// AudioWindow::RebuildPage()), consistent with this toolkit's
// existing caller-computes-the-rect convention rather than a new
// declarative layout system.
class Card : public Widget
{
public:
    void Draw(UiWindow& window) override;
};

// A single-line text input with real keyboard focus - currently just
// kohiko-network's "Search networks..." field, but written as a
// general-purpose widget rather than something search-specific.
// Needs UiWindow's focus-routing (see Widget::WantsFocus() and
// UiWindow::HandleEvent()'s KeyPress case) to actually receive
// OnKeyInput() calls.
class TextField : public Widget
{
public:

    std::string text;
    std::string placeholder;
    std::string iconName;   // optional leading icon, e.g. "edit-find"
    bool focused = false;
    std::function<void(const std::string&)> onChange;

    void Draw(UiWindow& window) override;
    bool WantsInput() const override { return true; }
    bool WantsFocus() const override { return true; }
    void OnFocus() override { focused = true; }
    void OnBlur() override { focused = false; }
    void OnKeyInput(KeyAction action, const std::string& utf8) override;
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

// A label on the left, one trailing control pinned to the right (a
// ToggleSwitch, most often) - the settings-row shape used throughout
// each app's Advanced Settings page (e.g. AudioWindow's "Notify when
// the default device changes", BluetoothWindow's per-device "Trust
// this device"). Adds both the label and `control` directly as
// children of `parent` and returns the now-parented control (like
// ListRow::Layout()'s return) so the caller can wire up onChange
// right away.
Widget* MakeSettingsRow(
    Widget& parent,
    const Rect& bounds,
    const std::string& label,
    std::unique_ptr<Widget> control,
    int controlWidth
);

// A small caption/value pair on one line - "Status"/"Connected",
// "Battery"/"82%" - the label/value rows inside kohiko-network's and
// kohiko-bluetooth's selected-item details panel. Returns a plain
// Widget wrapping both Labels; `valueColor` of 0 uses the theme's
// default foreground.
std::unique_ptr<Widget> MakeDetailRow(
    const Rect& bounds,
    const std::string& label,
    const std::string& value,
    std::uint32_t valueColor = 0
);

}
