#pragma once

#include <string>
#include <vector>

namespace Kohiko
{

class Config;

// One `windowrule=` directive from the config file: an explicit,
// user-written statement of how a matching window must be managed.
//
// Kohiko decides the default layout. A window rule is how the user
// overrides it, deliberately, in a way that is deterministic: the same
// rule always produces the same placement, and nothing Kohiko observes
// (moving, dragging, resizing, swapping windows) ever creates, changes
// or removes one. Only editing the config - by hand, in
// kohiko-settings' Window Rules tab, or via kohiko-choose-window -
// does.
//
// A rule is a set of whitespace-separated `key:value` tokens, in any
// order, with at least one SELECTOR and at least one PROPERTY.
//
//   selectors (all present ones must match - an implicit AND):
//     app:<key>        the application: an exact, case-insensitive
//                      match on the window's application key (see
//                      AppKeyFor() - its WM_CLASS class, falling back to
//                      its WM_CLASS instance). This is what
//                      kohiko-choose-window writes.
//     class:<text>     substring of WM_CLASS class, case-insensitive
//     instance:<text>  substring of WM_CLASS instance, case-insensitive
//     role:<text>      the window's WM_WINDOW_ROLE, an EXACT
//                      case-insensitive match. Roles are how one
//                      application (Firefox, Zen, Thunderbird, most
//                      GTK/Qt apps) tells its own kinds of window apart
//                      - "browser" for the main window,
//                      "PictureInPicture" for the video overlay - so
//                      `role:browser` must never match
//                      "PictureInPicture" (or "browser-popup"). A
//                      window with no WM_WINDOW_ROLE matches no
//                      `role:` selector.
//     type:<name>      the window's primary _NET_WM_WINDOW_TYPE (the
//                      first value it lists): normal, dialog, utility,
//                      toolbar, menu, splash, dropdown_menu,
//                      popup_menu, tooltip, notification, combo, dnd,
//                      desktop or dock; `_NET_WM_WINDOW_TYPE_` may be
//                      written in front. Exact match. A window that
//                      declares no type matches no `type:` selector -
//                      the type is what the window said, never inferred.
//     title:<text>     substring of the window title, case-insensitive
//
//   A selector the window cannot satisfy (a role the window doesn't
//   have, a different role/type) makes THAT rule not match - it is
//   never quietly widened into an application-wide rule. A separately
//   written, less specific rule (for example plain `app:zen`) still
//   applies through the normal specificity ordering below.
//
//   properties (an omitted one means "no opinion" - Kohiko's normal
//   behaviour applies to that aspect):
//     workspace:<N>              open on workspace N (1-based)
//     horizontal:left|right      side of the layout to open on
//     vertical:top|bottom        top/bottom of the layout to open on
//     mode:tiled|floating|fullscreen
//
//   legacy bare words, still accepted anywhere in the rule:
//     tile | float | fullscreen  same as mode:tiled|floating|fullscreen
//     nofullscreen               never honour the window's own
//                                fullscreen requests (no mode: form)
//
// Examples:
//
//   windowrule=app:firefox workspace:3 horizontal:right vertical:top mode:tiled
//   windowrule=app:telegram-desktop workspace:2 horizontal:right vertical:bottom mode:floating
//   windowrule=app:kitty workspace:1 mode:fullscreen
//   windowrule=app:zen workspace:2                                    (every Zen window)
//   windowrule=app:zen role:browser workspace:2 mode:tiled            (the browser window only)
//   windowrule=app:zen role:PictureInPicture mode:floating horizontal:right vertical:bottom
//   windowrule=app:zen role:PictureInPicture type:utility workspace:1  (both must hold)
//   windowrule=tile class:tlauncher                    (legacy form)
//   windowrule=workspace:8 class:telegram title:photo  (legacy form)
//
// Selector values may use %XX escapes (so an application key or title
// containing spaces can be written: `app:My%20App`).
//
// When several rules match one window they are applied from the least
// to the most specific (the number of selectors), and among equally
// specific rules in file order - so for each property the most specific
// matching rule wins, and a later line beats an earlier one of the same
// specificity. See ResolveWindowRules().
// Everything about a window that a rule can select on, as read from X11
// when the window is first managed. `role` and `windowType` are empty
// when the window doesn't have them; an empty value never satisfies a
// selector (see WindowRule::Matches()).
struct WindowIdentity
{
    std::string className;
    std::string instanceName;
    std::string title;
    std::string role;        // WM_WINDOW_ROLE
    std::string windowType;  // primary _NET_WM_WINDOW_TYPE, normalised: "normal", "utility", ...
};

// Which windows of one application a per-application rule covers beyond
// "all of them": optionally only those with this WM_WINDOW_ROLE and/or
// this window type. An empty scope is the plain application rule that
// kohiko-choose-window and the Settings tab have always managed.
struct RuleScope
{
    std::string role;
    std::string windowType;

    bool IsApplication() const { return role.empty() && windowType.empty(); }
    bool operator==(const RuleScope& other) const;

    // Human-readable, for the chooser, Settings and log lines:
    // "All windows", "role PictureInPicture", "type utility" or
    // "role PictureInPicture + type utility".
    std::string Describe() const;
};

struct WindowRule
{
    enum class Horizontal { Default, Left, Right };
    enum class Vertical   { Default, Top, Bottom };
    enum class Mode       { Default, Tiled, Floating, Fullscreen };

    // --- selectors - case preserved as written, compared case-insensitively; empty means "don't check this field" ---
    std::string appKey;
    std::string classPattern;
    std::string instancePattern;
    std::string titlePattern;
    std::string role;        // WM_WINDOW_ROLE, exact (case-insensitive)
    std::string windowType;  // normalised primary window type, exact

    // --- properties ---
    int workspace = 0;                       // 0 = no rule; else 1-based
    Horizontal horizontal = Horizontal::Default;
    Vertical vertical = Vertical::Default;
    Mode mode = Mode::Default;
    bool denyFullscreen = false;             // legacy `nofullscreen`

    // The application identity used for `app:` matching and by the
    // chooser: the window's WM_CLASS class, or - only if that is empty -
    // its WM_CLASS instance. Not the title, not the PID, not the window
    // id (X11 window ids are recycled, see SessionStore).
    static std::string AppKeyFor(
        const std::string& className,
        const std::string& instanceName);

    int SelectorCount() const;
    bool HasSelector() const { return SelectorCount() > 0; }
    bool HasProperty() const;

    // True for a rule whose ONLY selector is `app:` - the kind
    // kohiko-choose-window and the Settings tab manage as "the rule for
    // this application". A rule that also carries class:/instance:/
    // title: is an advanced, hand-written one and is never touched by
    // them.
    bool IsAppRule() const;

    // `app:` plus optionally `role:`/`type:`, and nothing else - one
    // application's rule, whole or narrowed to a kind of its windows
    // (see RuleScope). IsAppRule() is the special case with no role or
    // type. These are what kohiko-choose-window can write; a rule that
    // also has class:/instance:/title: stays hand-written.
    bool IsScopedAppRule() const;

    RuleScope Scope() const;

    // Legacy form: no role or type known, so a rule with a `role:` or
    // `type:` selector cannot match.
    bool Matches(
        const std::string& className,
        const std::string& instanceName,
        const std::string& title) const;

    bool Matches(
        const WindowIdentity& window) const;

    // The normalised spelling of a window type ("utility"), or an empty
    // string when `text` names no known type. Accepts any case and an
    // optional `_NET_WM_WINDOW_TYPE_` prefix.
    static std::string NormalizeWindowType(
        const std::string& text);

    // Every window type a `type:` selector accepts, in the order the
    // Settings editor cycles through them.
    static const std::vector<std::string>& KnownWindowTypes();

    // Parses one `windowrule=` value (the text after the `=`). Returns
    // false - leaving `out` untouched and, if `error` is non-null,
    // saying why - for anything that isn't a complete, valid rule: an
    // unknown token or value, no selector (a bare `windowrule=tile`
    // would otherwise apply to every window in existence), or no
    // property. Strict on purpose: silently applying only part of a
    // rule is exactly the kind of unpredictability this system exists
    // to avoid.
    static bool Parse(
        const std::string& text,
        WindowRule& out,
        std::string* error = nullptr);

    // Canonical text for this rule (selectors, then workspace,
    // horizontal, vertical, mode, nofullscreen; selector values
    // %-escaped). Parse(Serialize(r)) == r.
    std::string Serialize() const;

    // %XX escaping of the characters that would otherwise break a
    // whitespace-separated token (whitespace, '%', control chars).
    static std::string EncodeValue(const std::string& value);
    static std::string DecodeValue(const std::string& value);

    bool operator==(const WindowRule& other) const;
};

// The combined effect of every rule that matches one window - what
// WindowManager::Manage() actually acts on, rather than walking
// m_windowRules itself at every call site.
struct WindowRuleEffect
{
    // At most one of these three is true: they are derived from the
    // single winning `mode`.
    bool forceFloat = false;
    bool forceTile = false;
    bool forceFullscreen = false;

    bool denyFullscreen = false;

    // 0 = no override; otherwise a 1-based workspace id.
    int forceWorkspace = 0;

    WindowRule::Horizontal horizontal = WindowRule::Horizontal::Default;
    WindowRule::Vertical vertical = WindowRule::Vertical::Default;
};

// The rules of `rules` that match this window, in the order they are
// APPLIED: least specific (fewest selectors) first, equally specific
// ones in file order. The last one to set a property wins that
// property. This is the single place that order is decided -
// ResolveWindowRules() folds exactly this list, and the window
// manager logs it - so "which rule won" never has two answers.
std::vector<WindowRule> MatchingWindowRules(
    const std::vector<WindowRule>& rules,
    const WindowIdentity& window);

// Folds every rule in `rules` that matches this window into one effect,
// least specific first, ties in file order, later overriding earlier
// per property. Pure: no X11, no config, no state - which is what makes
// it testable.
WindowRuleEffect ResolveWindowRules(
    const std::vector<WindowRule>& rules,
    const std::string& className,
    const std::string& instanceName,
    const std::string& title);

WindowRuleEffect ResolveWindowRules(
    const std::vector<WindowRule>& rules,
    const WindowIdentity& window);

// Parses every `windowrule=` line out of `config`, in file order. A
// line that doesn't parse is skipped with a logged warning saying why
// (so a typo shows up in the log instead of silently doing nothing).
std::vector<WindowRule> LoadWindowRules(
    const Config& config);

}
