#pragma once

#include "WindowRule.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace Kohiko
{

// The state and keyboard behaviour of kohiko-choose-window's dialog,
// with no X11 in it - so exactly what a keypress does, and exactly which
// rule a given set of choices produces, can be tested directly.
//
// The dialog edits ONE rule at a time (see WindowRuleStore). It has an
// "Applies to" row choosing WHICH windows of the application that rule is
// for, followed by four rows of mutually exclusive choices, each with an
// explicit "Default" meaning "this rule has no opinion about that":
//
//   Applies to   All windows | role <R> | type <T> | role <R> + type <T>
//   Workspace    Default | 1 | 2 | ... | N
//   Horizontal   Default | Left | Right
//   Vertical     Default | Top | Bottom
//   Mode         Default | Tiled | Floating | Fullscreen
//
// so a rule can be "always on workspace 3" without touching position, or
// "always on the right" without touching the workspace.
//
// "Applies to" is what keeps one application's differently-behaving
// windows apart: a browser and its Picture-in-Picture overlay share a
// WM_CLASS but not a WM_WINDOW_ROLE, so they get independent rules
// (`app:zen role:browser ...`, `app:zen role:PictureInPicture ...`)
// instead of fighting over the one `app:zen` rule. The row is shown only
// when the window offers a choice (it declared a role or a type). Each
// kind keeps its OWN four choices: switching kind shows that kind's saved
// rule, and Save/Reset act on the kind that is selected - never on the
// others.
class ChooseWindowModel
{
public:

    // Property rows first (the values are array indexes and chip
    // positions that existing code relies on), then the kind row.
    enum class Row { Workspace = 0, Horizontal, Vertical, Mode, Scope };
    static constexpr int kPropertyRowCount = 4;

    // Continue = the dialog stays open (not named None: Xlib #defines it).
    enum class Action { Continue, Save, Cancel, Reset };

    struct Key
    {
        enum class Kind { Up, Down, Left, Right, Tab, BackTab, Enter, Escape, Delete, Digit };

        Kind kind = Kind::Enter;
        int digit = 0; // for Kind::Digit: 0-9
    };

    // One thing "Applies to" can be set to.
    struct ScopeChoice
    {
        std::string label;                // "All windows", "role PictureInPicture", ...
        RuleScope scope;                  // empty = the whole application
        std::optional<WindowRule> saved;  // the rule currently in the config for this kind
    };

    // What BuildScopeChoices() works out for a window.
    struct ScopeChoices
    {
        std::vector<ScopeChoice> choices;
        int defaultIndex = 0;  // which one the dialog opens on
    };

    // `saved` is the rule currently in the config for this application
    // (nullopt if none) - the dialog opens showing it. A single kind: no
    // "Applies to" row. (What the dialog was before kinds existed, and
    // still right for a window that declares no role or type.)
    ChooseWindowModel(
        int workspaceCount,
        const std::optional<WindowRule>& saved);

    // The kinds this window offers, the first being the whole
    // application. `initialScope` is which one to open on.
    ChooseWindowModel(
        int workspaceCount,
        std::vector<ScopeChoice> scopes,
        int initialScope);

    // Which kinds of rule a window with this role/windowType can have,
    // each with the rule saved for it in `lines` (the `windowrule=`
    // values), and which one to open on:
    //
    //   - `requested`, when the caller named one (`--scope`);
    //   - otherwise the most specific role/type kind that already has a
    //     saved rule (that is the rule which actually applies to this
    //     window, so reopening the dialog shows what is in force);
    //   - otherwise, if this application already has rules scoped to a
    //     role or type (the person is evidently using per-kind rules, with
    //     any application-wide rule as the fallback), the window's role
    //     kind - a window that declares a role gets the rule for its own
    //     kind rather than rewriting one that also covers its siblings;
    //   - otherwise "All windows", as it always was (including when only
    //     an application-wide rule exists - that is the rule shown).
    //
    // A role or type the window did not declare is simply not offered:
    // a kind the window cannot satisfy is never quietly replaced by a
    // broader one.
    static ScopeChoices BuildScopeChoices(
        const std::vector<std::string>& lines,
        const std::string& appKey,
        const std::string& role,
        const std::string& windowType,
        const std::optional<RuleScope>& requested = std::nullopt);

    // The rows in the order they are shown and navigated: the kind row
    // first (only when there is a choice to make), then the four
    // property rows.
    std::vector<Row> Rows() const;

    int OptionCount(Row row) const;
    std::string OptionLabel(Row row, int index) const;
    static std::string RowLabel(Row row);

    int Selected(Row row) const;
    void Select(Row row, int index);

    Row CurrentRow() const { return m_row; }
    void SetCurrentRow(Row row) { m_row = row; }

    // --- "Applies to" ---

    bool HasScopeRow() const { return m_scopes.size() > 1; }
    int ScopeCount() const { return static_cast<int>(m_scopes.size()); }
    int CurrentScope() const { return m_scope; }
    const RuleScope& CurrentRuleScope() const { return m_scopes[static_cast<std::size_t>(m_scope)].scope; }
    const RuleScope& ScopeAt(int index) const { return m_scopes[static_cast<std::size_t>(index)].scope; }

    // Applies one keypress and says whether it finished the dialog.
    //   Up/Down (k/j)     previous / next row
    //   Left/Right (h/l)  previous / next choice in the current row (wraps)
    //   Tab / Shift+Tab   next / previous row (wraps)
    //   0-9               pick that workspace directly (0 = Default)
    //   Enter             Save       Escape  Cancel       Delete  Reset
    Action HandleKey(const Key& key);

    // The rule these choices describe, for `appKey`, for the kind that
    // is selected. Fields at Default stay unset. A legacy `nofullscreen`
    // on the rule being edited is carried through untouched - this
    // dialog has no control for it, so editing the rest must not
    // silently drop it.
    WindowRule ToRule(const std::string& appKey) const;

    // Same, for an explicit kind (the pre-"Applies to" signature).
    WindowRule ToRule(const std::string& appKey, const RuleScope& scope) const;

    // The selectors the rule being edited will have, as written in the
    // config: "app:zen role:PictureInPicture".
    std::string SelectorText(const std::string& appKey) const;

    // True when every row of the SELECTED kind is at Default - saving
    // that is the same as deleting that kind's rule.
    bool AllDefault() const;

    // Whether the config already holds a rule for this application in
    // the selected kind (what "Reset" would delete).
    bool HasSavedRule() const { return m_scopes[static_cast<std::size_t>(m_scope)].saved.has_value(); }

private:

    using Choices = std::array<int, kPropertyRowCount>;

    static int RowIndex(Row row) { return static_cast<int>(row); }
    Choices& Current() { return m_choices[static_cast<std::size_t>(m_scope)]; }
    const Choices& Current() const { return m_choices[static_cast<std::size_t>(m_scope)]; }

    void Init(int workspaceCount, int initialScope);

    int m_workspaceCount = 1;
    std::vector<ScopeChoice> m_scopes;
    std::vector<Choices> m_choices; // parallel to m_scopes
    int m_scope = 0;

    Row m_row = Row::Workspace;
};

}
