// ChooseWindowModel: what each keypress does in kohiko-choose-window's
// dialog and which rule a set of choices produces - the whole logic of
// the dialog short of drawing it, tested without X11.

#include "ChooseWindowModel.h"

#include <cstdio>
#include <optional>

using namespace Kohiko;
using Model = ChooseWindowModel;
using K = ChooseWindowModel::Key::Kind;
using H = WindowRule::Horizontal;
using V = WindowRule::Vertical;
using M = WindowRule::Mode;

static int g_pass = 0;
static int g_fail = 0;

static void Check(bool condition, const char* message)
{
    if (condition) { ++g_pass; std::printf("  PASS: %s\n", message); }
    else           { ++g_fail; std::printf("  FAIL: %s\n", message); }
}

static Model::Action Press(Model& m, K kind, int digit = 0)
{
    Model::Key key;
    key.kind = kind;
    key.digit = digit;
    return m.HandleKey(key);
}

int main()
{
    std::printf("Fresh dialog (no saved rule):\n");
    {
        Model m(4, std::nullopt);
        Check(m.AllDefault(), "every row starts at Default");
        Check(!m.HasSavedRule(), "no saved rule to reset");
        Check(m.CurrentRow() == Model::Row::Workspace, "starts on the Workspace row");
        Check(m.OptionCount(Model::Row::Workspace) == 5 && m.OptionLabel(Model::Row::Workspace, 0) == "Default" &&
              m.OptionLabel(Model::Row::Workspace, 4) == "4", "workspace choices: Default, 1..4");
        Check(m.OptionCount(Model::Row::Horizontal) == 3 && m.OptionLabel(Model::Row::Horizontal, 1) == "Left" &&
              m.OptionLabel(Model::Row::Horizontal, 2) == "Right", "horizontal: Default, Left, Right");
        Check(m.OptionCount(Model::Row::Vertical) == 3 && m.OptionLabel(Model::Row::Vertical, 1) == "Top" &&
              m.OptionLabel(Model::Row::Vertical, 2) == "Bottom", "vertical: Default, Top, Bottom");
        Check(m.OptionCount(Model::Row::Mode) == 4 && m.OptionLabel(Model::Row::Mode, 1) == "Tiled" &&
              m.OptionLabel(Model::Row::Mode, 2) == "Floating" && m.OptionLabel(Model::Row::Mode, 3) == "Fullscreen",
              "mode: Default, Tiled, Floating, Fullscreen");

        WindowRule r = m.ToRule("firefox");
        Check(r.appKey == "firefox" && !r.HasProperty(), "an untouched dialog describes a rule with no properties (so saving it deletes)");
    }

    std::printf("\nChoice indexes ARE the WindowRule enum values (the model relies on it):\n");
    {
        Check(static_cast<int>(H::Left) == 1 && static_cast<int>(H::Right) == 2, "Horizontal order");
        Check(static_cast<int>(V::Top) == 1 && static_cast<int>(V::Bottom) == 2, "Vertical order");
        Check(static_cast<int>(M::Tiled) == 1 && static_cast<int>(M::Floating) == 2 && static_cast<int>(M::Fullscreen) == 3, "Mode order");
    }

    std::printf("\nKeyboard navigation:\n");
    {
        Model m(4, std::nullopt);

        Press(m, K::Down);  Check(m.CurrentRow() == Model::Row::Horizontal, "Down moves to the next row");
        Press(m, K::Down);  Press(m, K::Down);
        Check(m.CurrentRow() == Model::Row::Mode, "...down to the last row");
        Press(m, K::Down);  Check(m.CurrentRow() == Model::Row::Mode, "Down stops at the last row (no wrap)");
        Press(m, K::Up);    Check(m.CurrentRow() == Model::Row::Vertical, "Up moves back");

        Model t(4, std::nullopt);
        Press(t, K::BackTab); Check(t.CurrentRow() == Model::Row::Mode, "Shift+Tab from the first row wraps to the last");
        Press(t, K::Tab);     Check(t.CurrentRow() == Model::Row::Workspace, "Tab from the last row wraps to the first");

        Model c(4, std::nullopt);
        Press(c, K::Down); // Horizontal
        Press(c, K::Right); Check(c.Selected(Model::Row::Horizontal) == 1, "Right picks the next choice (Left)");
        Press(c, K::Right); Check(c.Selected(Model::Row::Horizontal) == 2, "...then Right");
        Press(c, K::Right); Check(c.Selected(Model::Row::Horizontal) == 0, "...wraps to Default");
        Press(c, K::Left);  Check(c.Selected(Model::Row::Horizontal) == 2, "Left wraps the other way");
        Check(c.Selected(Model::Row::Workspace) == 0, "changing one row leaves the others at Default");
    }

    std::printf("\nDigits pick a workspace directly:\n");
    {
        Model m(4, std::nullopt);
        Press(m, K::Down); Press(m, K::Down);  // park on Vertical
        Press(m, K::Digit, 3);
        Check(m.Selected(Model::Row::Workspace) == 3 && m.CurrentRow() == Model::Row::Workspace,
              "a digit selects that workspace and jumps to the Workspace row");
        Press(m, K::Digit, 9);
        Check(m.Selected(Model::Row::Workspace) == 3, "a workspace that doesn't exist (9 of 4) is ignored");
        Press(m, K::Digit, 0);
        Check(m.Selected(Model::Row::Workspace) == 0, "0 goes back to Default");
    }

    std::printf("\nFinishing keys:\n");
    {
        Model m(4, std::nullopt);
        Check(Press(m, K::Enter) == Model::Action::Save, "Enter saves");
        Check(Press(m, K::Escape) == Model::Action::Cancel, "Escape cancels");
        Check(Press(m, K::Delete) == Model::Action::Reset, "Delete resets (deletes the rule)");
        Check(Press(m, K::Left) == Model::Action::Continue && Press(m, K::Tab) == Model::Action::Continue,
              "ordinary keys keep the dialog open");
    }

    std::printf("\nChoices -> rule (every field independent):\n");
    {
        Model m(4, std::nullopt);
        Press(m, K::Digit, 3);
        WindowRule r = m.ToRule("firefox");
        Check(r.workspace == 3 && r.horizontal == H::Default && r.vertical == V::Default && r.mode == M::Default,
              "workspace only: position and mode stay unset");

        Model p(4, std::nullopt);
        Press(p, K::Down); Press(p, K::Right); Press(p, K::Right); // Horizontal = Right
        r = p.ToRule("x");
        Check(r.workspace == 0 && r.horizontal == H::Right && r.vertical == V::Default && r.mode == M::Default,
              "horizontal only: workspace stays unset");

        Model all(4, std::nullopt);
        Press(all, K::Digit, 2);
        Press(all, K::Down); Press(all, K::Right);                 // Horizontal Left
        Press(all, K::Down); Press(all, K::Right); Press(all, K::Right); // Vertical Bottom
        Press(all, K::Down); Press(all, K::Right); Press(all, K::Right); // Mode Floating
        r = all.ToRule("telegram-desktop");
        Check(r.workspace == 2 && r.horizontal == H::Left && r.vertical == V::Bottom && r.mode == M::Floating &&
              r.Serialize() == "app:telegram-desktop workspace:2 horizontal:left vertical:bottom mode:floating",
              "a combined rule serialises exactly as the config will hold it");
        Check(!all.AllDefault(), "...and is not 'all default'");
    }

    std::printf("\nOpening on an existing rule:\n");
    {
        WindowRule saved;
        saved.appKey = "firefox";
        saved.workspace = 3;
        saved.horizontal = H::Right;
        saved.vertical = V::Top;
        saved.mode = M::Tiled;

        Model m(4, saved);
        Check(m.HasSavedRule(), "knows there's a saved rule (enables Reset)");
        Check(m.Selected(Model::Row::Workspace) == 3 && m.Selected(Model::Row::Horizontal) == 2 &&
              m.Selected(Model::Row::Vertical) == 1 && m.Selected(Model::Row::Mode) == 1,
              "every row opens showing the saved value");
        Check(m.ToRule("firefox") == saved, "saving without touching anything reproduces the rule exactly");

        Press(m, K::Down); Press(m, K::Left); // Horizontal Right -> Left
        WindowRule changed = m.ToRule("firefox");
        Check(changed.horizontal == H::Left && changed.workspace == 3 && changed.vertical == V::Top && changed.mode == M::Tiled,
              "changing one field changes only that field");

        // A rule for a workspace beyond the current count survives.
        WindowRule far;
        far.appKey = "x"; far.workspace = 7;
        Model f(4, far);
        Check(f.Selected(Model::Row::Workspace) == 7 && f.ToRule("x").workspace == 7,
              "a saved workspace beyond workspace.count is kept, not silently reset to Default");

        // A legacy flag the dialog has no control for is carried through.
        WindowRule deny = saved;
        deny.denyFullscreen = true;
        Model d(4, deny);
        Check(d.ToRule("firefox").denyFullscreen, "a legacy `nofullscreen` on the rule isn't dropped by editing the rest");
    }

    std::printf("\nWorkspace count edge cases:\n");
    {
        Model zero(0, std::nullopt);
        Check(zero.OptionCount(Model::Row::Workspace) == 2, "a nonsensical workspace.count of 0 still offers workspace 1");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
