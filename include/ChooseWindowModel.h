#pragma once

#include "WindowRule.h"

#include <optional>
#include <string>
#include <vector>

namespace Kohiko
{

// The state and keyboard behaviour of kohiko-choose-window's dialog,
// with no X11 in it - so exactly what a keypress does, and exactly which
// rule a given set of choices produces, can be tested directly.
//
// The dialog edits ONE per-application rule (see WindowRuleStore). It is
// four rows of mutually exclusive choices, each with an explicit
// "Default" meaning "this rule has no opinion about that":
//
//   Workspace    Default | 1 | 2 | ... | N
//   Horizontal   Default | Left | Right
//   Vertical     Default | Top | Bottom
//   Mode         Default | Tiled | Floating | Fullscreen
//
// so a rule can be "always on workspace 3" without touching position, or
// "always on the right" without touching the workspace.
class ChooseWindowModel
{
public:

    enum class Row { Workspace = 0, Horizontal, Vertical, Mode };
    static constexpr int kRowCount = 4;

    // Continue = the dialog stays open (not named None: Xlib #defines it).
    enum class Action { Continue, Save, Cancel, Reset };

    struct Key
    {
        enum class Kind { Up, Down, Left, Right, Tab, BackTab, Enter, Escape, Delete, Digit };

        Kind kind = Kind::Enter;
        int digit = 0; // for Kind::Digit: 0-9
    };

    // `saved` is the rule currently in the config for this application
    // (nullopt if none) - the dialog opens showing it.
    ChooseWindowModel(
        int workspaceCount,
        const std::optional<WindowRule>& saved);

    int OptionCount(Row row) const;
    std::string OptionLabel(Row row, int index) const;
    static std::string RowLabel(Row row);

    int Selected(Row row) const;
    void Select(Row row, int index);

    Row CurrentRow() const { return m_row; }
    void SetCurrentRow(Row row) { m_row = row; }

    // Applies one keypress and says whether it finished the dialog.
    //   Up/Down (k/j)     previous / next row
    //   Left/Right (h/l)  previous / next choice in the current row (wraps)
    //   Tab / Shift+Tab   next / previous row (wraps)
    //   0-9               pick that workspace directly (0 = Default)
    //   Enter             Save       Escape  Cancel       Delete  Reset
    Action HandleKey(const Key& key);

    // The rule these choices describe, for `appKey`. Fields at Default
    // stay unset. A legacy `nofullscreen` on the rule being edited is
    // carried through untouched - this dialog has no control for it, so
    // editing the rest must not silently drop it.
    WindowRule ToRule(const std::string& appKey, const RuleScope& scope = RuleScope()) const;

    // True when every row is at Default - saving that is the same as
    // deleting the rule.
    bool AllDefault() const;

    // Whether the config already holds a rule for this application
    // (what "Reset" would delete).
    bool HasSavedRule() const { return m_saved.has_value(); }

private:

    int m_workspaceCount;
    std::optional<WindowRule> m_saved;

    int m_selected[kRowCount] = { 0, 0, 0, 0 };
    Row m_row = Row::Workspace;
};

}
