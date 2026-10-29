#pragma once

#include <string>

namespace Kohiko
{

// A windowrule= line's structured fields, parsed leniently from (and
// re-serialized exactly back to) one line of kohiko-settings' Window Rules
// editor.
//
// It lives apart from SettingsWindow so the editing semantics - above all
// "changing one cell never changes another selector" - can be tested
// without an X11 display. Deliberately its own lightweight,
// tolerant-of-partial-input type rather than reusing WindowRule::Parse()
// directly: that parser rejects anything incomplete outright, whereas a
// row being actively edited is expected to pass through invalid in-between
// states (e.g. no selector at all yet) - and it keeps the text exactly as
// typed. (Whether a finished line is VALID is a separate question,
// answered by the editor asking WindowRule::Parse(), so the editor and the
// window manager can never disagree about what a rule means.)
//
// Every choice has an explicit Default (index 0 / empty text) that means
// "this rule has no opinion about that aspect".
struct WindowRuleDraft
{
    // Selectors, as typed (decoded - %XX escapes shown as the character
    // they stand for; SerializeWindowRuleDraft() re-escapes). appText is
    // the exact application key (`app:`, what kohiko-choose-window
    // writes); classText/instanceText/titleText are the substring
    // selectors of hand-written rules.
    std::string appText;
    std::string classText;
    std::string instanceText;
    std::string titleText;

    // `role:` (exact WM_WINDOW_ROLE) and `type:` (primary window type)
    // selectors - the "window kind". They are editable in Settings (a
    // role cell and a type button), and ANY OTHER edit of a row keeps
    // them: dropping one would silently turn a rule for one kind of
    // window (a browser's Picture-in-Picture overlay) into a rule for
    // every window of the application.
    std::string roleText;
    std::string typeText;

    std::string workspaceText;   // digits; "" = Default (no workspace rule)
    int horizontalIndex = 0;     // 0 Default, 1 Left, 2 Right
    int verticalIndex = 0;       // 0 Default, 1 Top, 2 Bottom
    int modeIndex = 0;           // 0 Default, 1 Tiled, 2 Floating, 3 Fullscreen
    bool denyFullscreen = false; // legacy `nofullscreen` - kept, not editable here

    // True when the first text cell is showing/editing the legacy
    // `class:` substring rather than the exact `app:` key: only for a
    // hand-written rule that has class: and no app:.
    bool firstCellIsClass() const { return appText.empty() && !classText.empty(); }

    // The window kind this rule is for, in words: "All windows",
    // "role PictureInPicture", "type utility" or "role X + type Y".
    std::string KindDescription() const;
};

WindowRuleDraft ParseWindowRuleDraft(const std::string& line);
std::string SerializeWindowRuleDraft(const WindowRuleDraft& draft);

// The next value of a `type:` button: "" (Any) -> normal -> dialog -> ...
// -> "" again, in WindowRule::KnownWindowTypes() order. A value that is
// not a known type (a hand-typed typo) moves to Any.
std::string NextWindowTypeText(const std::string& current);

}
