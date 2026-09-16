#pragma once

#include <string>
#include <vector>

namespace Kohiko
{

// Backs `kohikoctl configure-autologin` - detecting the active display
// manager and, only with explicit interactive confirmation (never
// automatically - see tools/kohikoctl.cpp, which is where the actual
// prompt and its "default answer is No" live; nothing in this class
// itself prompts for anything), setting it up to automatically log
// one specific, already-chosen user straight into a Kohiko session -
// see the "Boot -> Display Manager/autologin -> kohiko-session ->
// kohiko -> Kohiko's own lockscreen" flow described in
// CHANGELOG.md/README.md's "Configuration migration & recovery mode"
// neighbourhood.
//
// Every method here takes a `root` parameter (defaulting to "/" for
// real use) that every path this class touches is resolved under -
// purely so tests can point the exact same logic at a throwaway fake
// filesystem tree instead of real system files. This isn't a
// production feature (there's no reason to ever pass anything but the
// default outside a test), just the only realistic way to actually
// exercise detection/write/undo logic like this end-to-end, given
// this can't safely run destructive tests against a machine's real
// /etc regardless of environment.
//
// Deliberately narrow in scope - see DisplayManager's own comment for
// exactly which display managers get full read/write support versus
// detection-only-then-documentation, and why.
class AutologinConfigurator
{
public:

    enum class DisplayManager
    {
        // No supported display manager was found running at all -
        // Configure() always fails for this; fall back to
        // documentation (a getty-autologin + `~/.xinitrc` `startx`
        // setup, for a machine with no display manager at all, is a
        // real alternative but a genuinely different mechanism this
        // class doesn't attempt to automate - see its header comment).
        None,

        // More than one candidate display manager looked plausibly
        // active and this class couldn't tell which one actually is -
        // Configure() always fails for this too. Better to make no
        // changes at all than guess wrong about which one to edit.
        Ambiguous,

        // Full support: detection, existing-autologin checking, and
        // Configure()/Undo() via a dedicated drop-in file this class
        // creates itself - see LightDMDropInPath()/SddmDropInPath() -
        // rather than editing lightdm.conf/sddm.conf in place. That
        // means Undo() is always exactly "delete the one file we
        // created", never "restore a modified copy of a file we don't
        // fully control the rest of the structure of".
        LightDM,
        Sddm,

        // Detected, but Configure() always fails for this on purpose:
        // GDM has no equivalent drop-in mechanism for this setting
        // (autologin only lives in the one shared custom.conf, whose
        // other content this class has no business assuming anything
        // about), so this falls back to precise manual documentation
        // instead of an in-place edit of a file it doesn't fully
        // control.
        Gdm,

        // A display manager is clearly active (found via the systemd
        // display-manager.service symlink or a known binary) but isn't
        // one this class has specific support for. Configure() always
        // fails; falls back to documentation.
        Unknown,
    };

    struct DetectionResult
    {
        DisplayManager manager = DisplayManager::None;

        // Human-readable explanation of how `manager` was determined,
        // or - for None/Ambiguous/Unknown - why it couldn't be
        // determined more precisely. Always safe to print directly.
        std::string detail;
    };

    // Never writes anything - purely inspects the filesystem under
    // `root`. Prefers the systemd display-manager.service symlink
    // (the actual *enabled* display manager, not just an installed
    // one) and only falls back to checking for known binaries when
    // that symlink doesn't exist (a non-systemd init, or no display
    // manager enabled via systemd at all).
    static DetectionResult DetectDisplayManager(
        const std::string& root = "/"
    );

    struct ExistingAutologinCheck
    {
        bool found = false;

        // Which file (and, where meaningful, which line) an existing
        // autologin directive was found in - always safe to print
        // directly. Empty when !found.
        std::string location;
    };

    // Read-only, like DetectDisplayManager() - checks both the display
    // manager's main config file and its conf.d/ drop-in directory
    // (where either supports one), so an existing autologin
    // configuration is found regardless of which style set it up.
    // Only meaningful for LightDM/Sddm; returns found=false
    // unconditionally for every other DisplayManager value.
    static ExistingAutologinCheck CheckExistingAutologin(
        DisplayManager manager,
        const std::string& root = "/"
    );

    struct ConfigureResult
    {
        bool success = false;

        // On success, exactly what happened, suitable for printing
        // directly - including the created file's path. On failure,
        // why, and what to do about it instead (documentation).
        std::string message;
    };

    // Only ever called after the caller (tools/kohikoctl.cpp) has
    // already gotten explicit interactive confirmation - this
    // function itself has no concept of "confirmed" and performs the
    // write unconditionally once called, provided every safety check
    // below passes:
    //
    //   - manager must be LightDM or Sddm (every other value fails
    //     immediately, see DisplayManager's own comment)
    //   - CheckExistingAutologin() must report nothing found (refuses
    //     outright otherwise - "existing autologin configurations
    //     must never be overwritten")
    //   - the drop-in directory must exist and be writable
    //   - the drop-in file DropInPath() would use must not already
    //     exist (belt and suspenders on top of the
    //     CheckExistingAutologin() check above, since that only looks
    //     for an active `autologin-user=`/`User=` line, not the
    //     file's mere existence)
    //
    // Writes the drop-in, then immediately re-reads it back and
    // confirms the content matches exactly what was intended
    // (validation - catches a partial write from a full disk or a
    // permissions/ownership problem that let the write syscall
    // itself succeed but left something unexpected on disk) before
    // reporting success. Any failure at any step leaves the
    // filesystem exactly as it was found - no partial file is ever
    // left behind on a validation failure (removed again
    // immediately).
    static ConfigureResult Configure(
        DisplayManager manager,
        const std::string& username,
        const std::string& root = "/"
    );

    // Removes exactly the one drop-in file Configure() would have
    // created for `manager`, if (and only if) it currently exists -
    // there is nothing else for this class to ever "roll back", since
    // Configure() never modifies an existing file, only ever creates
    // this one new one. Returns true if a file was actually removed.
    static bool Undo(
        DisplayManager manager,
        const std::string& root = "/"
    );

    // The exact path Configure()/Undo() use for `manager` - exposed so
    // tools/kohikoctl.cpp can show the user the precise path *before*
    // asking for confirmation, without duplicating the path logic
    // itself. Empty string for any DisplayManager value Configure()
    // doesn't support.
    static std::string DropInPath(
        DisplayManager manager,
        const std::string& root = "/"
    );

    // The exact content Configure() would write for `manager`/
    // `username` - Configure() itself calls this internally, so
    // there's exactly one place this content is ever generated.
    // Exposed so tools/kohikoctl.cpp can show the user precisely what
    // will be written *before* asking for confirmation. Empty for any
    // DisplayManager value Configure() doesn't support.
    static std::string PreviewContent(
        DisplayManager manager,
        const std::string& username
    );

};

}
