#pragma once

#include <string>

namespace Kohiko
{

// Keeps an existing kohiko.conf in sync with whatever settings the
// *current* Kohiko build actually knows about, so picking up a new
// release never means "go re-diff config/default.conf by hand" - see
// ConfigSchema, which is this class's whole source of truth for what
// "every setting" means. Every user-facing requirement this exists
// for boils down to one rule: touch only what's genuinely missing,
// and never anything the user (or a previous migration) already
// wrote.
//
// Deliberately reuses ConfigWriter - the exact same "keep the file as
// literal lines of text, touch only the one line a key owns" engine
// Kohiko Settings' own Save() already relies on - rather than a
// second, parallel way of editing kohiko.conf. The only thing new
// here is *which* keys to write (every ConfigSchema key missing from
// the file) and *when* (once, automatically, at startup - see
// Application::Run()), not how a line gets written.
class ConfigMigration
{
public:

    // If `configPath` doesn't exist or can't be read, this is a no-op
    // that returns false - there is deliberately no such thing as
    // "migrating" a config file that was never created in the first
    // place (see README's "first run" section: an absent config file
    // is Kohiko running on built-in defaults by design, not something
    // to silently materialize a file for).
    //
    // Otherwise, appends `key=defaultValue` (from ConfigSchema::All())
    // for every schema key with no active line anywhere in the file
    // yet, under its own clearly-marked section (kMigrationMarker) -
    // never touching a key that's already present, active or not (a
    // commented-out `#foo=bar` is left exactly as the user left it;
    // see ConfigWriter::GetScalar()'s own "ignoring commented-out
    // lines" - that means a deliberately-disabled setting is still
    // "already there" from a migration's point of view, so it's never
    // duplicated with an active copy underneath it).
    //
    // Before writing anything, the file's current contents are copied
    // to `configPath + ".bak"` (overwriting any previous backup) -
    // this is also the one backup RecoveryMode's "restore the
    // previous configuration" falls back to (see RecoveryMode.h), so
    // there is exactly one backup file to reason about, not two
    // independent backup mechanisms doing almost the same thing.
    //
    // Returns true only if the file was actually modified (at least
    // one key was missing) - Application::Run() uses this to decide
    // whether m_config needs reloading, and whether it's worth a log
    // line; a config that's already fully up to date is left with its
    // mtime untouched.
    static bool MigrateIfNeeded(
        const std::string& configPath
    );

    // Copies `configPath` to `configPath + ".bak"` (overwriting any
    // previous backup), if `configPath` exists and is readable.
    // Broken out of MigrateIfNeeded() (which already calls this
    // itself, before writing anything) so RecoveryMode can call the
    // exact same backup step when it falls back to safe defaults -
    // see RecoveryMode.h. One backup mechanism, two callers, rather
    // than two copies of "how to safely snapshot kohiko.conf".
    static bool BackupConfig(
        const std::string& configPath
    );

};

}
