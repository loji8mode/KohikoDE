#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace Kohiko
{

// Kohiko Settings' write path back into kohiko.conf. Config (Config.h)
// is the *read* path everywhere else in this project uses, and stays
// exactly as it was - a parsed, unordered key/value store with no way
// to write anything back. ConfigWriter is the opposite: it keeps the
// file as literal lines of text and only ever touches the specific
// line(s) a given key or repeatable-directive prefix owns, so every
// comment, blank line, and everything the person hasn't touched
// through the GUI comes back out of Save() exactly as it went into
// Load() - "the configuration files remain the source of truth" is
// what this class exists to guarantee, not just a comment.
//
// Two shapes of key are handled, matching config/default.conf's own
// two shapes of key (see its own header comment):
//
//   - Scalar keys ("last one wins" if repeated) - SetScalar()/GetScalar().
//   - Repeatable directives (`bind=`, `exec.<name>=`, `windowrule=`,
//     `monitor=`) where the same prefix legitimately appears many
//     times - GetRawBlock()/ReplaceRawBlock(), which Kohiko Settings
//     uses to back a plain multi-line text editor rather than
//     pretending each is an individually-typed scalar setting (see
//     SettingsWindow.cpp's RawBlockPanel).
class ConfigWriter
{
public:

    // The section marker every ordinary SetScalar() call (i.e. every
    // call that doesn't pass its own `marker`) appends new keys under -
    // what Kohiko Settings itself has always used. Public so
    // ConfigMigration.cpp doesn't need to duplicate this string to
    // recognise/skip Settings' own section, even though migration
    // writes under its own distinct marker instead.
    static const char* const kDefaultMarker;

    bool Load(
        const std::string& path
    );

    // Writes back to the path last given to Load() (or explicitly
    // here). Atomic: written to a temp file in the same directory
    // first, then renamed over the original, so a write that's
    // interrupted partway (disk full, killed process, ...) can never
    // leave kohiko.conf half-written.
    bool Save(
        const std::string& path = ""
    );

    // Last value for `key`, ignoring commented-out lines - "" if the
    // key has no active line at all. Only needed by ConfigWriter's own
    // SetScalar() and by callers that specifically need to know
    // whether a key is *actively* set (vs relying on some other
    // fallback) rather than its effective value - most reads should
    // still go through Config, which this intentionally mirrors the
    // parsing rules of (see Config::Load()).
    std::string GetScalar(
        const std::string& key
    ) const;

    // Whether `key` has an active line at all, regardless of its
    // value - unlike GetScalar(key).empty(), this correctly says
    // "yes" for a key the user deliberately set to an empty value
    // (several ConfigSchema defaults, e.g. lockscreen.background_image,
    // are themselves "" - GetScalar() alone can't tell "never set"
    // apart from "set to the empty string on purpose", but this can).
    // Mirrors Config::Contains() exactly, for the read-only class this
    // one otherwise parallels.
    bool Contains(
        const std::string& key
    ) const;

    // Updates the last active `key=...` line's value in place if one
    // exists; otherwise appends a new `key=value` line under a
    // clearly-marked section at the end of the file (created once,
    // reused for every later addition under the same `marker`) rather
    // than silently guessing where it belongs. `marker` defaults to
    // Kohiko Settings' own section, so every existing call site keeps
    // writing to exactly the section it always has; ConfigMigration
    // passes its own distinct marker (see ConfigMigration.h) so an
    // auto-migrated key is never mislabeled as something the user
    // configured through the GUI.
    void SetScalar(
        const std::string& key,
        const std::string& value,
        const std::string& marker = kDefaultMarker
    );

    // Every active line whose trimmed content starts with `prefix`
    // (e.g. "windowrule="), in file order, with `prefix` itself
    // stripped off - e.g. GetRawBlock("windowrule=") on a line reading
    // "windowrule=fullscreen class:x" returns "fullscreen class:x".
    std::vector<std::string> GetRawBlock(
        const std::string& prefix
    ) const;

    // Removes every currently active line starting with `prefix`, and
    // inserts `prefix + suffix` for each entry of `newSuffixes` in its
    // place (in the order given) - at the position the old block
    // occupied if there was one, otherwise appended under the same
    // "added by Kohiko Settings" section SetScalar() uses. Passing an
    // empty `newSuffixes` simply deletes the whole block. See this
    // class's own header comment for the one simplification this
    // makes: unrelated lines that were interspersed *between*
    // individual matches of `prefix` end up moved to just after the
    // new block rather than staying woven through it.
    void ReplaceRawBlock(
        const std::string& prefix,
        const std::vector<std::string>& newSuffixes
    );

private:

    std::string m_path;
    std::vector<std::string> m_lines;

    // Index of an existing `marker` line, or `m_lines.size()` (append
    // at the very end) if that marker hasn't been created yet this
    // session - see SetScalar()/ReplaceRawBlock().
    std::size_t EnsureAddedMarker(
        const std::string& marker
    );

};

}
