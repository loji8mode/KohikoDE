// ChooseWindowModel: what each keypress does in kohiko-choose-window's
// dialog and which rule a set of choices produces - the whole logic of
// the dialog short of drawing it, tested without X11.

#include "ChooseWindowModel.h"
#include "WindowRuleStore.h"

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

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

    std::printf("\nKinds of window (\"Applies to\"): Zen's normal window and its Picture-in-Picture window:\n");
    {
        const std::vector<std::string> lines = {
            "app:zen workspace:2",
            "app:zen role:browser workspace:3 horizontal:left mode:tiled",
            "app:zen role:PictureInPicture workspace:4 horizontal:right mode:floating",
            "app:firefox workspace:1",
        };

        // Which kinds each window offers - from what IT declared.
        Model::ScopeChoices pipChoices = Model::BuildScopeChoices(lines, "zen", "PictureInPicture", "utility");
        Check(pipChoices.choices.size() == 4 &&
              pipChoices.choices[0].label == "All windows" &&
              pipChoices.choices[1].label == "role PictureInPicture" &&
              pipChoices.choices[2].label == "type utility" &&
              pipChoices.choices[3].label == "role PictureInPicture + type utility",
              "a window with a role and a type offers: all windows, its role, its type, role + type");
        Check(pipChoices.choices[0].saved && pipChoices.choices[0].saved->workspace == 2 &&
              pipChoices.choices[1].saved && pipChoices.choices[1].saved->workspace == 4 &&
              !pipChoices.choices[2].saved && !pipChoices.choices[3].saved,
              "each kind carries ITS OWN saved rule (generic ws2, PiP ws4; none for type / role+type)");
        Check(pipChoices.defaultIndex == 1, "reopening on a PiP window opens on the rule in force for it: the PiP rule, not the generic one");

        Model::ScopeChoices browserChoices = Model::BuildScopeChoices(lines, "zen", "browser", "normal");
        Check(browserChoices.choices[1].label == "role browser" && browserChoices.choices[1].saved &&
              browserChoices.choices[1].saved->workspace == 3 && browserChoices.defaultIndex == 1,
              "the normal window opens on ITS rule (role browser, ws3)");

        Model::ScopeChoices bare = Model::BuildScopeChoices(lines, "zen", "", "");
        Check(bare.choices.size() == 1 && bare.defaultIndex == 0, "a window that declares no role or type has only 'All windows' - a kind it cannot satisfy is not offered");

        Model::ScopeChoices roleOnly = Model::BuildScopeChoices(lines, "zen", "browser", "");
        Check(roleOnly.choices.size() == 2 && roleOnly.choices[1].label == "role browser", "role without a type: all windows + role");

        // A window kind with nothing saved, in an app that already has per-kind rules: opens on its own role, not the generic rule.
        Model::ScopeChoices other = Model::BuildScopeChoices(lines, "zen", "Dialog-Settings", "dialog");
        Check(other.defaultIndex == 1 && other.choices[1].label == "role Dialog-Settings" && !other.choices[1].saved,
              "a new kind in an app already using per-kind rules defaults to that kind's own role (an explicit, visible choice)");

        // Nothing saved at all: as before, the whole application.
        Model::ScopeChoices fresh = Model::BuildScopeChoices(lines, "mpv", "", "normal");
        Model::ScopeChoices freshRole = Model::BuildScopeChoices(lines, "mpv", "x11", "normal");
        Check(fresh.defaultIndex == 0 && freshRole.defaultIndex == 0, "an application with no rules opens on 'All windows', as it always did");

        // An explicit --scope.
        RuleScope want; want.role = "PictureInPicture"; want.windowType = "utility";
        Model::ScopeChoices req = Model::BuildScopeChoices(lines, "zen", "PictureInPicture", "utility", want);
        Check(req.defaultIndex == 3, "--scope role+type opens on the role + type kind");
        Model::ScopeChoices reqApp = Model::BuildScopeChoices(lines, "zen", "PictureInPicture", "utility", RuleScope());
        Check(reqApp.defaultIndex == 0, "--scope app opens on all windows even though a PiP rule exists");

        // The dialog with a kind row.
        Model m(4, pipChoices.choices, pipChoices.defaultIndex);
        Check(m.HasScopeRow() && m.ScopeCount() == 4, "the Applies-to row exists when there is a choice");
        auto rows = m.Rows();
        Check(rows.size() == 5 && rows[0] == Model::Row::Scope && rows[1] == Model::Row::Workspace && rows[4] == Model::Row::Mode,
              "rows in order: Applies to, Workspace, Horizontal, Vertical, Mode");
        Check(m.CurrentRow() == Model::Row::Scope, "it starts on the Applies-to row: choosing which windows the rule is for comes first");
        Check(Model::RowLabel(Model::Row::Scope) == "Applies to" && m.OptionLabel(Model::Row::Scope, 1) == "role PictureInPicture",
              "labelled 'Applies to', options are the kinds");
        Check(m.CurrentRuleScope().role == "PictureInPicture" && m.CurrentRuleScope().windowType.empty(), "current kind: role PictureInPicture");
        Check(m.Selected(Model::Row::Workspace) == 4 && m.Selected(Model::Row::Horizontal) == 2 && m.Selected(Model::Row::Mode) == 2,
              "it shows the saved PiP rule: ws4, right, floating");
        Check(m.SelectorText("zen") == "app:zen role:PictureInPicture", "the selectors being edited, as written in the config");
        Check(m.ToRule("zen") == WindowRule([]{ WindowRule r; WindowRule::Parse("app:zen role:PictureInPicture workspace:4 horizontal:right mode:floating", r); return r; }()),
              "ToRule(): the PiP rule, with its role selector, unchanged by simply opening and saving");

        // Switching kind shows THAT kind's rule...
        Press(m, K::Left);
        Check(m.CurrentScope() == 0 && m.CurrentRuleScope().IsApplication() &&
              m.Selected(Model::Row::Workspace) == 2 && m.Selected(Model::Row::Horizontal) == 0 && m.Selected(Model::Row::Mode) == 0,
              "Left on Applies-to: All windows - and the rows now show the generic rule (ws2, defaults)");
        Check(m.SelectorText("zen") == "app:zen", "generic selectors: app:zen only");

        // ...and editing it does not touch the other kinds' values.
        Press(m, K::Digit, 1);
        Check(m.ToRule("zen").workspace == 1 && m.ToRule("zen").role.empty() && m.ToRule("zen").Scope().IsApplication(),
              "editing the generic rule: workspace 1, still app:zen with no role");
        Press(m, K::Up);
        Press(m, K::Right);
        Check(m.CurrentScope() == 1 && m.Selected(Model::Row::Workspace) == 4 && m.Selected(Model::Row::Horizontal) == 2,
              "back on PiP: ITS values are untouched by the edit made on the generic kind");
        Press(m, K::Right); Press(m, K::Right); Press(m, K::Right);
        Check(m.CurrentScope() == 0, "Right wraps around the kinds");

        // Moving between rows: the Applies-to row is part of the cycle.
        Model nav(4, pipChoices.choices, 1);
        Press(nav, K::Down);
        Check(nav.CurrentRow() == Model::Row::Workspace, "Down from Applies-to: Workspace");
        Press(nav, K::Up); Press(nav, K::Up);
        Check(nav.CurrentRow() == Model::Row::Scope, "Up clamps at Applies-to");
        Press(nav, K::BackTab);
        Check(nav.CurrentRow() == Model::Row::Mode, "Shift+Tab wraps from Applies-to to Mode");
        Press(nav, K::Tab);
        Check(nav.CurrentRow() == Model::Row::Scope, "Tab wraps from Mode to Applies-to");
        Press(nav, K::Digit, 3);
        Check(nav.CurrentRow() == Model::Row::Workspace && nav.Selected(Model::Row::Workspace) == 3 && nav.CurrentScope() == 1,
              "a digit jumps to Workspace for the CURRENT kind, without changing the kind");

        // A model with a single kind behaves exactly as before: no row, starts on Workspace.
        Model single(4, std::nullopt);
        Check(!single.HasScopeRow() && single.Rows().size() == 4 && single.CurrentRow() == Model::Row::Workspace,
              "no role/type: no Applies-to row, starts on Workspace (unchanged from before)");
    }

    std::printf("\nSaving and resetting one kind never touches another (the actual chooser save path):\n");
    {
        const std::vector<std::string> start = {
            "app:zen workspace:2",
            "app:zen role:browser workspace:3 horizontal:left mode:tiled",
            "app:firefox workspace:1",
        };

        // 1. Create the Picture-in-Picture rule, from the PiP window.
        Model::ScopeChoices c = Model::BuildScopeChoices(start, "zen", "PictureInPicture", "utility");
        Check(c.defaultIndex == 1, "no PiP rule yet, but the app has per-kind rules: the dialog opens on role PictureInPicture");
        Model pip(6, c.choices, c.defaultIndex);
        pip.Select(Model::Row::Workspace, 5);
        pip.Select(Model::Row::Horizontal, 2);
        pip.Select(Model::Row::Mode, 2);

        std::vector<std::string> afterPip = WindowRuleStore::UpsertAppRule(start, pip.ToRule("zen"));
        auto has = [](const std::vector<std::string>& v, const std::string& text)
        {
            for (const std::string& line : v) if (line == text) return true;
            return false;
        };

        Check(has(afterPip, "app:zen role:PictureInPicture workspace:5 horizontal:right mode:floating"),
              "saved rule value: app:zen role:PictureInPicture workspace:5 horizontal:right mode:floating");
        Check(has(afterPip, "app:zen workspace:2") &&
              has(afterPip, "app:zen role:browser workspace:3 horizontal:left mode:tiled") &&
              has(afterPip, "app:firefox workspace:1"),
              "the generic zen rule, the browser rule and firefox's rule are all untouched");
        Check(afterPip.size() == start.size() + 1, "exactly one line was added");

        // 2. Reopen from the PiP window: it loads what was saved.
        Model::ScopeChoices again = Model::BuildScopeChoices(afterPip, "zen", "PictureInPicture", "utility");
        Model reopened(6, again.choices, again.defaultIndex);
        Check(reopened.CurrentRuleScope().role == "PictureInPicture" && reopened.Selected(Model::Row::Workspace) == 5 &&
              reopened.Selected(Model::Row::Horizontal) == 2 && reopened.Selected(Model::Row::Mode) == 2 && reopened.HasSavedRule(),
              "reopening for the PiP window loads the saved PiP rule (ws5, right, floating) with Reset enabled");

        // 3. Edit the browser's rule from the BROWSER window: PiP's must not change.
        Model::ScopeChoices bc = Model::BuildScopeChoices(afterPip, "zen", "browser", "normal");
        Model browser(6, bc.choices, bc.defaultIndex);
        Check(browser.CurrentRuleScope().role == "browser" && browser.Selected(Model::Row::Workspace) == 3, "the normal window opens on role browser (ws3)");
        browser.Select(Model::Row::Workspace, 4);
        std::vector<std::string> afterBrowser = WindowRuleStore::UpsertAppRule(afterPip, browser.ToRule("zen"));
        Check(has(afterBrowser, "app:zen role:browser workspace:4 horizontal:left mode:tiled") &&
              has(afterBrowser, "app:zen role:PictureInPicture workspace:5 horizontal:right mode:floating") &&
              has(afterBrowser, "app:zen workspace:2") && afterBrowser.size() == afterPip.size(),
              "editing the browser rule changes only that line; the PiP and generic rules are intact and nothing was added");

        // 4. Choosing 'All windows' from the PiP window and saving edits the generic rule - not the PiP one.
        Model::ScopeChoices pc = Model::BuildScopeChoices(afterBrowser, "zen", "PictureInPicture", "utility");
        Model fromPip(6, pc.choices, pc.defaultIndex);
        fromPip.Select(Model::Row::Scope, 0);
        Check(fromPip.Selected(Model::Row::Workspace) == 2, "switching to All windows in the dialog shows the generic rule (ws2)");
        fromPip.Select(Model::Row::Workspace, 1);
        std::vector<std::string> afterGeneric = WindowRuleStore::UpsertAppRule(afterBrowser, fromPip.ToRule("zen"));
        Check(has(afterGeneric, "app:zen workspace:1") &&
              has(afterGeneric, "app:zen role:PictureInPicture workspace:5 horizontal:right mode:floating") &&
              has(afterGeneric, "app:zen role:browser workspace:4 horizontal:left mode:tiled"),
              "saving the 'All windows' kind edits only the generic rule");

        // 5. Reset (delete) acts on the selected kind only.
        Model::ScopeChoices dc = Model::BuildScopeChoices(afterGeneric, "zen", "PictureInPicture", "utility");
        Model del(6, dc.choices, dc.defaultIndex);
        std::vector<std::string> afterReset = WindowRuleStore::RemoveAppRule(afterGeneric, "zen", del.CurrentRuleScope());
        Check(!has(afterReset, "app:zen role:PictureInPicture workspace:5 horizontal:right mode:floating") &&
              has(afterReset, "app:zen workspace:1") &&
              has(afterReset, "app:zen role:browser workspace:4 horizontal:left mode:tiled") &&
              afterReset.size() == afterGeneric.size() - 1,
              "Reset on the PiP kind deletes only the PiP rule; the generic rule and the browser rule remain");

        // 6. All-Default saves as a deletion of that kind only.
        Model::ScopeChoices ac = Model::BuildScopeChoices(afterGeneric, "zen", "browser", "normal");
        Model clear(6, ac.choices, ac.defaultIndex);
        clear.Select(Model::Row::Workspace, 0); clear.Select(Model::Row::Horizontal, 0); clear.Select(Model::Row::Mode, 0);
        Check(clear.AllDefault(), "every choice of the browser kind set to Default");
        std::vector<std::string> afterClear = WindowRuleStore::UpsertAppRule(afterGeneric, clear.ToRule("zen"));
        Check(!has(afterClear, "app:zen role:browser workspace:4 horizontal:left mode:tiled") &&
              has(afterClear, "app:zen workspace:1") &&
              has(afterClear, "app:zen role:PictureInPicture workspace:5 horizontal:right mode:floating"),
              "saving an all-Default browser kind removes just that rule");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
