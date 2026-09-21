#pragma once

#include <string>

namespace Kohiko
{

// Backs `kohikoctl configure-console-autologin` - the console/no-display-
// manager counterpart to AutologinConfigurator, for exactly the case that
// class's own DisplayManager::None documents as a real alternative it
// deliberately doesn't automate: boot -> plain text console `login:`
// prompt -> `startx` typed by hand -> `~/.xinitrc` -> `kohiko-session` ->
// kohiko -> Kohiko's own lock screen. This class automates the first two
// steps only - a getty running with `agetty --autologin`, plus a guarded
// `startx` line appended to the target user's own login-shell profile -
// and never touches anything past that. Kohiko's own lock screen remains
// the one real authentication step either way, exactly as with
// AutologinConfigurator's own display-manager flow - see this file's
// Configure() for the precise, narrow thing "console autologin" means
// here.
//
// Every method takes a `root` parameter (defaulting to "/" for real use)
// that every path this class touches - including the passwd database it
// reads usernames/shells/home directories from - is resolved under, for
// the same reason and in exactly the same way as AutologinConfigurator's
// own `root` parameter: so tests can point this at a throwaway fake
// filesystem tree instead of real system files. See that class's own
// header comment; nothing here repeats it.
class ConsoleAutologinConfigurator
{
public:

    struct DetectionResult
    {
        // False for any non-systemd init, or a systemd machine with no
        // getty@.service template at all (unusual, but not treated as
        // an error - just "unsupported, fall back to documentation",
        // the same graceful-degradation this whole feature area always
        // uses for anything it can't confidently automate).
        bool supported = false;

        // Human-readable explanation either way - always safe to print
        // directly.
        std::string detail;
    };

    // Never writes anything - purely inspects the filesystem under
    // `root`. Systemd is detected the same way AutologinConfigurator
    // detects an active display manager: real infrastructure actually
    // present (the getty@.service vendor unit), not just a directory
    // that happens to exist.
    static DetectionResult DetectSupport(
        const std::string& root = "/"
    );

    struct PasswdEntry
    {
        bool found = false;
        std::string home;
        std::string shell;   // full path, e.g. "/usr/bin/bash"
    };

    // Reads `root`+"/etc/passwd" (never the live system's own NSS/
    // getpwnam(), so this stays exercisable against a fake tree the
    // same way every other lookup in this class is) for `username`'s
    // home directory and login shell. found=false if `username` isn't
    // present at all - Configure() refuses outright in that case,
    // exactly like an unsupported display manager.
    static PasswdEntry LookupUser(
        const std::string& username,
        const std::string& root = "/"
    );

    enum class LoginShell
    {
        Bash,
        Zsh,
        // Any other POSIX-ish shell (dash, plain sh, ...) that reads
        // ~/.profile as a login shell - genuinely correct for some
        // shells (dash) and a reasonable, documented best-effort for
        // others, rather than silently doing nothing.
        PosixSh,
        // A shell this class has no specific profile-file knowledge
        // for at all (fish, csh/tcsh, a custom login shell, ...) -
        // Configure() refuses this one specifically rather than
        // guessing at a syntax that shell might not even support.
        Unknown,
    };

    // Classifies `shellPath` (as returned in PasswdEntry::shell) by its
    // basename - "/usr/bin/bash" and "/bin/bash" both match Bash, etc.
    static LoginShell ClassifyShell(
        const std::string& shellPath
    );

    // The profile file ClassifyShell()'s shell reads as a login shell -
    // "" for LoginShell::Unknown. This is the file Configure() appends
    // its guarded `startx` block to.
    static std::string ProfilePath(
        LoginShell shell,
        const std::string& homeDir
    );

    // The exact block Configure() appends to that profile file, for
    // `tty` (e.g. "tty1") - wrapped in a pair of `# >>> kohiko
    // console-autologin >>>` / `# <<< ... <<<` marker comment lines
    // that Undo() looks for to remove precisely this block and nothing
    // else the user's own profile might contain around it. Only runs
    // `startx` when every one of these holds: not already inside an X
    // session (`$DISPLAY` unset - covers a plain `su`/nested shell,
    // not just a genuinely different login), and this is specifically
    // the console `tty` this was configured for (checked via `tty`,
    // not assumed from context) - so opening a terminal, an SSH
    // session, or a different virtual console never triggers it.
    static std::string ProfileBlock(
        const std::string& tty
    );

    // Whether `profilePath` already contains a block with the marker
    // above - Configure()'s own "never overwrite" check, and what lets
    // Undo() find exactly what to remove.
    static bool ProfileHasBlock(
        const std::string& profilePath
    );

    // The getty drop-in path for `tty` under `root` -
    // ".../etc/systemd/system/getty@<tty>.service.d/60-kohiko-autologin.conf",
    // a dedicated file this class creates itself rather than editing
    // the vendor getty@.service unit, for exactly the same reason
    // AutologinConfigurator never edits lightdm.conf/sddm.conf directly
    // - Undo() is always just "delete the one file we created".
    static std::string GettyDropInPath(
        const std::string& tty,
        const std::string& root = "/"
    );

    // The exact content Configure() writes to that path for `username` -
    // a standard systemd "clear then redefine ExecStart=" override, the
    // documented way to add a flag to a template unit's ExecStart
    // without forking the whole vendor unit file.
    static std::string GettyPreviewContent(
        const std::string& username
    );

    struct ExistingCheck
    {
        bool found = false;
        std::string location;
    };

    // Read-only - scans every "*.conf" file already inside
    // getty@<tty>.service.d/ (not the vendor unit itself; see
    // GettyDropInPath()'s own comment on why nothing here ever needs to
    // look there) for an ExecStart= line that already passes
    // `--autologin` - Kohiko's own drop-in or anyone else's. A
    // commented-out line, or an ExecStart= line without --autologin,
    // does not count.
    static ExistingCheck CheckExistingGettyAutologin(
        const std::string& tty,
        const std::string& root = "/"
    );

    struct ConfigureResult
    {
        bool success = false;

        // On success: exactly what was created (both the getty drop-in
        // and the profile block), suitable for printing directly. Also
        // carries an advisory (non-fatal) note when `homeDir`/.xinitrc
        // execs `kohiko` directly rather than `kohiko-session` - see
        // XinitrcBypassesSessionWrapper() - since that would otherwise
        // silently defeat kohiko-session's crash-restart behaviour
        // right underneath this feature with no other indication
        // anything is wrong.
        //
        // On failure: why, and what to do about it instead.
        std::string message;
    };

    // Only ever called after the caller (tools/kohikoctl.cpp) already
    // has explicit interactive confirmation, exactly like
    // AutologinConfigurator::Configure() - this function itself has no
    // concept of "confirmed". Fails outright, making no changes at all,
    // if any of:
    //
    //   - DetectSupport() reports unsupported
    //   - LookupUser() finds no such user
    //   - ClassifyShell() is Unknown for that user's shell
    //   - CheckExistingGettyAutologin() finds anything already active
    //   - the getty drop-in file already exists
    //   - ProfileHasBlock() is already true for that user's profile
    //
    // Writes the getty drop-in first, validating it the same
    // write-then-read-back way AutologinConfigurator::Configure() does,
    // then appends the profile block. If the profile half fails for any
    // reason (unwritable file, disk full, ...), the getty drop-in just
    // written is removed again before returning failure - Configure()
    // never leaves only one of the two halves in place.
    static ConfigureResult Configure(
        const std::string& username,
        const std::string& tty,
        const std::string& root = "/"
    );

    // Removes exactly the getty drop-in Configure() created (if it
    // exists) and exactly the marked block Configure() appended to the
    // profile file (if present), leaving everything else in that
    // profile file completely untouched. Returns true if anything was
    // actually removed.
    static bool Undo(
        const std::string& username,
        const std::string& tty,
        const std::string& root = "/"
    );

    // Advisory helper, not a precondition of anything above: true if
    // `homeDir`/.xinitrc exists and execs `kohiko` directly (with
    // nothing else after it on that line) rather than `kohiko-session` -
    // the exact bypass-of-crash-supervision this class's own header
    // comment and ConfigureResult flag. Never modifies the file; purely
    // informational for the confirmation prompt/result message.
    static bool XinitrcBypassesSessionWrapper(
        const std::string& homeDir,
        const std::string& root = "/"
    );

};

}
