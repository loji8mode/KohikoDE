#pragma once

#include "ChooseWindowModel.h"
#include "Font.h"
#include "Types.h"

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include <functional>
#include <string>
#include <vector>

namespace Kohiko
{

class Config;

// Who the dialog is editing a rule for - what the window manager
// reported about the focused window.
struct ChooseWindowIdentity
{
    std::string appKey;       // the rule key (WindowRule::AppKeyFor of the two below)
    std::string className;
    std::string instanceName;
    std::string title;

    // What the window declared about itself, and which of it the rule
    // being edited is scoped to (see RuleScope): empty = the whole
    // application, as always.
    std::string role;
    std::string windowType;
    RuleScope scope;

    // "role PictureInPicture", "role browser + type normal", ... or "" for the whole application.
    std::string ScopeLabel() const
    {
        return scope.IsApplication() ? std::string() : scope.Describe();
    }
};

// kohiko-choose-window's popup: a small, fixed-size, keyboard-driven
// dialog. A completely ordinary X11 client (like kohiko-settings) in
// its own process - nothing in the window manager draws it, and it
// needs nothing beyond X11/Xft/fontconfig, so it exists on every
// install (unlike the D-Bus/PipeWire-based desktop apps). It marks
// itself _NET_WM_WINDOW_TYPE_DIALOG so Kohiko floats and centres it
// instead of tiling it into the layout.
//
// It only ever shows and edits a ChooseWindowModel and reports which
// button was pressed; saving is the caller's job.
class ChooseWindowDialog
{
public:

    // `otherRules(scope)`: how many OTHER rules (hand-written class:/title:
    // ones, or a rule of another kind for this application) also apply
    // to this window while a rule of `scope` is being edited - shown as
    // a note, since they interact with the one being edited. Asked again
    // whenever the selected kind changes.
    using OtherRuleCounter = std::function<int(const RuleScope&)>;

    ChooseWindowDialog(
        const Config& config,
        const ChooseWindowIdentity& identity,
        ChooseWindowModel& model,
        OtherRuleCounter otherRules);

    ~ChooseWindowDialog();

    ChooseWindowDialog(const ChooseWindowDialog&) = delete;
    ChooseWindowDialog& operator=(const ChooseWindowDialog&) = delete;

    // Opens the window and runs until the user saves, cancels or
    // resets. Returns Action::Cancel if the display can't be opened.
    ChooseWindowModel::Action Run();

    // True when another kohiko-choose-window window is already open on
    // this display (so a second keypress doesn't stack a second one).
    static bool AlreadyOpen(Display* display);

private:

    struct Hit
    {
        Rect rect;
        enum class Kind { Chip, Save, Cancel, Reset } kind = Hit::Kind::Chip;
        ChooseWindowModel::Row row = ChooseWindowModel::Row::Workspace;
        int index = 0;
    };

    void ComputeLayout();
    void Draw();
    void FillRect(const Rect& r, unsigned long pixel);
    void StrokeRect(const Rect& r, unsigned long pixel);
    void Text(int x, int baseline, const std::string& text, const XftColor& color, bool bold = false);
    ChooseWindowModel::Action HandleKeyPress(XKeyEvent& event);
    ChooseWindowModel::Action HandleClick(int x, int y);

private:

    const Config& m_config;
    ChooseWindowIdentity m_identity;
    ChooseWindowModel& m_model;
    OtherRuleCounter m_otherRules;
    bool m_reserveNote = false;

    Display* m_display = nullptr;
    int m_screen = 0;
    Window m_window = 0;
    Pixmap m_backBuffer = 0;
    GC m_gc = nullptr;
    XftDraw* m_xftDraw = nullptr;

    Font m_font;
    Font m_boldFont;

    unsigned long m_background = 0, m_foreground = 0, m_accent = 0,
                  m_field = 0, m_border = 0, m_muted = 0, m_panel = 0;

    int m_width = 0;
    int m_height = 0;
    std::vector<Hit> m_hits;
    std::vector<int> m_rowLabelY;   // baseline of each row's label, indexed by ChooseWindowModel::Row
    int m_headerBaseline = 0;
    int m_appBaseline = 0;
    int m_detailBaseline = 0;
    int m_kindBaseline = 0;      // role / type this window declared
    int m_selectorBaseline = 0;  // the selectors the rule being edited has
    int m_noteBaseline = 0;
    int m_hintBaseline = 0;
};

}
