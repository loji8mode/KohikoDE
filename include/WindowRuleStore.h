#pragma once

#include "WindowRule.h"

#include <optional>
#include <string>
#include <vector>

namespace Kohiko
{

// Reading and editing the per-application window rule - the single,
// explicit `windowrule=app:<key> ...` line that says how one
// application's windows should be placed - in a list of `windowrule=`
// values (the text after the `=`, in file order).
//
// Shared by kohiko-choose-window and the Settings Window Rules tab so
// that "the rule for application X" means exactly one thing everywhere.
// Every function that edits returns a new list and never touches
// anything it doesn't own:
//
//   - only a rule whose selectors are `app:` plus optionally `role:`
//     and/or `type:` (see WindowRule::IsScopedAppRule()) is ever read,
//     replaced or removed here, and only the one with exactly the
//     requested RuleScope: the plain application rule, the rule for
//     that application's `role:PictureInPicture` windows and the rule
//     for its `role:browser` windows are three independent rules, and
//     editing or deleting one never touches the others. Hand-written
//     rules with class:/instance:/title: selectors, and lines that
//     don't parse at all, pass through verbatim;
//   - an existing app rule is replaced IN PLACE, so its position among
//     the other rules - which decides ties between equally specific
//     rules - is preserved; a new one is appended;
//   - nothing here is ever called by window movement, dragging,
//     resizing or swapping. A rule changes only when a person asks for
//     it to.
class WindowRuleStore
{
public:

    static constexpr const char* kPrefix = "windowrule=";

    // The rule saved for this application (in this scope), if any.
    static std::optional<WindowRule> FindAppRule(
        const std::vector<std::string>& lines,
        const std::string& appKey,
        const RuleScope& scope = RuleScope());

    // Saves `rule` (which must have an appKey; its role/type, if any,
    // are its scope) as that application's rule for that scope. A rule with no properties set at all - every field back at
    // "default" - is not a rule, so it removes the application's rule
    // instead. Any duplicate app rules for the same application are
    // collapsed into the one that is kept.
    static std::vector<std::string> UpsertAppRule(
        std::vector<std::string> lines,
        const WindowRule& rule);

    // Deletes the application's rule; behaviour for that application
    // returns to Kohiko's default placement logic.
    static std::vector<std::string> RemoveAppRule(
        std::vector<std::string> lines,
        const std::string& appKey,
        const RuleScope& scope = RuleScope());

    // Rules (as written) that also affect a window with this identity
    // but are NOT the rule being edited (the application's rule in
    // `ownScope`) - hand-written class:/title: rules, or the same
    // application's other scopes. Lets the chooser say "N other rules
    // also apply".
    static std::vector<std::string> OtherMatchingRules(
        const std::vector<std::string>& lines,
        const std::string& className,
        const std::string& instanceName,
        const std::string& title);

    static std::vector<std::string> OtherMatchingRules(
        const std::vector<std::string>& lines,
        const WindowIdentity& window,
        const RuleScope& ownScope);

    // --- config file access (through ConfigWriter) ---

    // False if the file can't be read (it is never created here: a
    // config this code fabricated from scratch would have no bind=
    // lines, and so silently remove every keybinding).
    static bool LoadLines(
        const std::string& configPath,
        std::vector<std::string>& lines);

    static bool SaveLines(
        const std::string& configPath,
        const std::vector<std::string>& lines);
};

}
