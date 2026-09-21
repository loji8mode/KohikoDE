#include "ConsoleAutologinConfigurator.h"

#include "Utils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace Kohiko
{

namespace
{

namespace fs = std::filesystem;

const char* const kBeginMarker = "# >>> kohiko console-autologin >>>";
const char* const kEndMarker   = "# <<< kohiko console-autologin <<<";

// Same normalization as AutologinConfigurator.cpp's own helper of the
// same name, and for the same reason - see there.
std::string NormalizeRoot(const std::string& root)
{
    if (root.empty() || root == "/")
        return "";

    if (root.back() == '/')
        return root.substr(0, root.size() - 1);

    return root;
}

// Every "*.conf" file directly inside `dir`, sorted - empty (not an
// error) if `dir` doesn't exist. Identical in spirit to
// AutologinConfigurator.cpp's ConfDFiles(); duplicated rather than
// shared since both are five-line, file-local helpers and neither
// class depends on the other.
std::vector<std::string> ConfDFiles(const std::string& dir)
{
    std::vector<std::string> files;
    std::error_code error;

    if (!fs::is_directory(dir, error))
        return files;

    for (const auto& entry : fs::directory_iterator(dir, error))
    {
        if (entry.path().extension() == ".conf")
            files.push_back(entry.path().string());
    }

    std::sort(files.begin(), files.end());

    return files;
}

std::string ReadWholeFile(const std::string& path)
{
    std::ifstream file(path);

    if (!file.is_open())
        return "";

    return std::string(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
}

std::string Basename(const std::string& path)
{
    std::size_t slash = path.find_last_of('/');
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

}

ConsoleAutologinConfigurator::DetectionResult ConsoleAutologinConfigurator::DetectSupport(
    const std::string& root)
{
    std::string base = NormalizeRoot(root);

    // The vendor unit template - real infrastructure actually present,
    // not just a directory that happens to exist, mirroring how
    // AutologinConfigurator prefers the systemd display-manager.service
    // symlink over guessing from a binary's mere presence.
    static const char* const kCandidates[] = {
        "/usr/lib/systemd/system/getty@.service",
        "/lib/systemd/system/getty@.service",
    };

    std::error_code error;

    for (const char* candidate : kCandidates)
    {
        if (fs::exists(base + candidate, error))
        {
            return {
                true,
                std::string("systemd's getty@.service template is present (") +
                    base + candidate + ")"
            };
        }
    }

    return {
        false,
        "no systemd getty@.service template found under /usr/lib/systemd/system "
        "or /lib/systemd/system - this machine doesn't appear to use systemd-managed "
        "virtual console logins, so automatic console autologin isn't available here. "
        "See the README's \"Automatic login\" section for how to set this up by hand "
        "instead."
    };
}

ConsoleAutologinConfigurator::PasswdEntry ConsoleAutologinConfigurator::LookupUser(
    const std::string& username,
    const std::string& root)
{
    std::string base = NormalizeRoot(root);
    std::ifstream file(base + "/etc/passwd");

    if (!file.is_open())
        return { false, "", "" };

    std::string line;

    while (std::getline(file, line))
    {
        // name:passwd:uid:gid:gecos:home:shell
        std::vector<std::string> fields;
        std::stringstream stream(line);
        std::string field;

        while (std::getline(stream, field, ':'))
            fields.push_back(field);

        if (fields.size() < 7)
            continue;

        if (fields[0] == username)
            return { true, fields[5], fields[6] };
    }

    return { false, "", "" };
}

ConsoleAutologinConfigurator::LoginShell ConsoleAutologinConfigurator::ClassifyShell(
    const std::string& shellPath)
{
    std::string name = Basename(shellPath);

    if (name == "bash")
        return LoginShell::Bash;

    if (name == "zsh")
        return LoginShell::Zsh;

    if (name == "sh" || name == "dash" || name == "ash")
        return LoginShell::PosixSh;

    return LoginShell::Unknown;
}

std::string ConsoleAutologinConfigurator::ProfilePath(
    LoginShell shell,
    const std::string& homeDir)
{
    switch (shell)
    {
        case LoginShell::Bash:    return homeDir + "/.bash_profile";
        case LoginShell::Zsh:     return homeDir + "/.zprofile";
        case LoginShell::PosixSh: return homeDir + "/.profile";
        case LoginShell::Unknown: return "";
    }

    return "";
}

std::string ConsoleAutologinConfigurator::ProfileBlock(
    const std::string& tty)
{
    return
        std::string(kBeginMarker) + "\n" +
        "# Installed by `kohikoctl configure-console-autologin` - see README.md's\n"
        "# \"Automatic login\" section. Starts Kohiko right after this getty's own\n"
        "# automatic login - only on the physical console this was configured for\n"
        "# (" + tty + "), and only if this shell doesn't already have an X session\n"
        "# on it (a nested su/login, or genuinely already inside X). Logging out of\n"
        "# Kohiko ends this shell too (exec replaces it), so the next boot-time\n"
        "# getty on " + tty + " simply repeats this same automatic cycle.\n"
        "if [ -z \"${DISPLAY:-}\" ] && [ \"$(tty)\" = \"/dev/" + tty + "\" ]; then\n"
        "    exec startx\n"
        "fi\n" +
        kEndMarker + "\n";
}

bool ConsoleAutologinConfigurator::ProfileHasBlock(
    const std::string& profilePath)
{
    return ReadWholeFile(profilePath).find(kBeginMarker) != std::string::npos;
}

std::string ConsoleAutologinConfigurator::GettyDropInPath(
    const std::string& tty,
    const std::string& root)
{
    std::string base = NormalizeRoot(root);
    return base + "/etc/systemd/system/getty@" + tty + ".service.d/60-kohiko-autologin.conf";
}

std::string ConsoleAutologinConfigurator::GettyPreviewContent(
    const std::string& username)
{
    // /usr/bin/agetty - the same binary systemd's own vendor
    // getty@.service already invokes on every mainstream distro with a
    // merged /usr (Arch, current Fedora/Debian/Ubuntu all included;
    // /sbin/agetty is just a symlink to this same binary where /sbin
    // still exists at all) - so this override changes only the flags,
    // never which program actually runs the console.
    return
        "[Service]\n"
        "ExecStart=\n"
        "ExecStart=-/usr/bin/agetty --autologin " + username + " --noclear %I $TERM\n";
}

ConsoleAutologinConfigurator::ExistingCheck ConsoleAutologinConfigurator::CheckExistingGettyAutologin(
    const std::string& tty,
    const std::string& root)
{
    std::string base = NormalizeRoot(root);
    std::string dir = base + "/etc/systemd/system/getty@" + tty + ".service.d";

    for (const std::string& path : ConfDFiles(dir))
    {
        std::ifstream file(path);
        std::string line;
        int lineNo = 0;

        while (std::getline(file, line))
        {
            ++lineNo;

            std::string trimmed = Utils::Trim(line);

            if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';')
                continue;

            const std::string prefix = "ExecStart=";

            if (trimmed.compare(0, prefix.size(), prefix) != 0)
                continue;

            std::string value = trimmed.substr(prefix.size());

            if (value.find("--autologin") != std::string::npos)
            {
                return {
                    true,
                    path + ":" + std::to_string(lineNo) + " (" + trimmed + ")"
                };
            }
        }
    }

    return { false, "" };
}

bool ConsoleAutologinConfigurator::XinitrcBypassesSessionWrapper(
    const std::string& homeDir,
    const std::string& root)
{
    std::string base = NormalizeRoot(root);
    std::string content = ReadWholeFile(base + homeDir + "/.xinitrc");

    if (content.empty())
        return false;

    std::stringstream stream(content);
    std::string line;

    while (std::getline(stream, line))
    {
        std::string trimmed = Utils::Trim(line);

        if (trimmed == "exec /usr/local/bin/kohiko" || trimmed == "exec kohiko")
            return true;
    }

    return false;
}

ConsoleAutologinConfigurator::ConfigureResult ConsoleAutologinConfigurator::Configure(
    const std::string& username,
    const std::string& tty,
    const std::string& root)
{
    std::string base = NormalizeRoot(root);

    DetectionResult support = DetectSupport(root);

    if (!support.supported)
        return { false, support.detail };

    if (username.empty())
        return { false, "no username given" };

    if (tty.empty())
        return { false, "no tty given" };

    PasswdEntry pw = LookupUser(username, root);

    if (!pw.found)
    {
        return {
            false,
            "no such user '" + username + "' found in " + base + "/etc/passwd"
        };
    }

    LoginShell shell = ClassifyShell(pw.shell);

    if (shell == LoginShell::Unknown)
    {
        return {
            false,
            "'" + username + "'s login shell (" + pw.shell + ") isn't one this can "
            "automate a startx line for. Add this line to its login profile by hand "
            "instead:\n"
            "    [ -z \"$DISPLAY\" ] && [ \"$(tty)\" = \"/dev/" + tty + "\" ] && exec startx"
        };
    }

    ExistingCheck existing = CheckExistingGettyAutologin(tty, root);

    if (existing.found)
    {
        return {
            false,
            "an existing getty autologin was already found at " + existing.location +
            " - refusing to overwrite it. Remove or edit it by hand first if you're sure."
        };
    }

    std::string dropInPath = GettyDropInPath(tty, root);
    std::error_code error;

    if (fs::exists(dropInPath, error))
    {
        return {
            false,
            dropInPath + " already exists - refusing to overwrite it. "
            "Remove it by hand first if you're sure, or use --undo."
        };
    }

    std::string profilePath = base + ProfilePath(shell, pw.home);

    if (ProfileHasBlock(profilePath))
    {
        return {
            false,
            "a console-autologin block already exists in " + profilePath +
            " - refusing to add a second one. Remove it by hand first if you're "
            "sure, or use --undo."
        };
    }

    // --- Write the getty drop-in, validating it the same write-then-
    // read-back way AutologinConfigurator::Configure() does. ---

    fs::create_directories(fs::path(dropInPath).parent_path(), error);

    if (error)
        return { false, "could not create " + fs::path(dropInPath).parent_path().string() + ": " + error.message() };

    std::string gettyContent = GettyPreviewContent(username);

    {
        std::ofstream out(dropInPath, std::ios::trunc);

        if (!out.is_open())
        {
            return {
                false,
                "could not open " + dropInPath + " for writing - this needs root "
                "(re-run with sudo)"
            };
        }

        out << gettyContent;
    }

    if (ReadWholeFile(dropInPath) != gettyContent)
    {
        fs::remove(dropInPath, error); // never leave a partial file behind
        return {
            false,
            "wrote " + dropInPath + " but its content didn't verify correctly "
            "afterward - removed it again; no changes were made."
        };
    }

    // --- Append the profile block. Roll back the getty drop-in above
    // if this half doesn't fully succeed - Configure() never leaves
    // only one of the two halves in place. ---

    fs::create_directories(fs::path(profilePath).parent_path(), error);

    std::string profileBlock = ProfileBlock(tty);
    bool profileOk = false;

    if (!error)
    {
        std::string existingProfileContent = ReadWholeFile(profilePath);
        bool needsLeadingNewline =
            !existingProfileContent.empty() && existingProfileContent.back() != '\n';

        std::ofstream out(profilePath, std::ios::app);

        if (out.is_open())
        {
            if (needsLeadingNewline)
                out << "\n";

            out << profileBlock;
            out.close();

            profileOk = ReadWholeFile(profilePath).find(profileBlock) != std::string::npos;
        }
    }

    if (!profileOk)
    {
        fs::remove(dropInPath, error);
        return {
            false,
            "could not append the startx block to " + profilePath + " - this needs "
            "root, or write access to " + username + "'s home directory. No changes "
            "were made (the getty drop-in just created was removed again)."
        };
    }

    std::string message =
        "created " + dropInPath + " and appended a guarded `exec startx` block to " +
        profilePath + " - " + username + " will be logged into a shell on " + tty +
        " automatically on next boot, which immediately starts X (and, via " +
        "~/.xinitrc, kohiko-session) with no password prompt at the console. Run "
        "`kohikoctl configure-console-autologin --undo` to remove both.";

    if (XinitrcBypassesSessionWrapper(pw.home, root))
    {
        message +=
            "\n\nNote: " + pw.home + "/.xinitrc currently execs `kohiko` directly "
            "rather than `kohiko-session`, which bypasses kohiko-session's "
            "crash-restart supervision entirely. Change its `exec kohiko` (or "
            "`exec /usr/local/bin/kohiko`) line to `exec kohiko-session` to get "
            "that back.";
    }

    return { true, message };
}

bool ConsoleAutologinConfigurator::Undo(
    const std::string& username,
    const std::string& tty,
    const std::string& root)
{
    std::string base = NormalizeRoot(root);
    bool removedAny = false;

    std::string dropInPath = GettyDropInPath(tty, root);
    std::error_code error;

    if (fs::exists(dropInPath, error))
    {
        fs::remove(dropInPath, error);
        removedAny = !error;
    }

    PasswdEntry pw = LookupUser(username, root);

    if (!pw.found)
        return removedAny;

    LoginShell shell = ClassifyShell(pw.shell);

    if (shell == LoginShell::Unknown)
        return removedAny;

    std::string profilePath = base + ProfilePath(shell, pw.home);
    std::string content = ReadWholeFile(profilePath);

    std::size_t begin = content.find(kBeginMarker);

    if (begin == std::string::npos)
        return removedAny;

    std::size_t end = content.find(kEndMarker, begin);

    if (end == std::string::npos)
        return removedAny;

    // Extend `end` past the marker line itself (including its
    // trailing newline, if any) so the removed span is exactly what
    // ProfileBlock() added - nothing from before kBeginMarker or after
    // kEndMarker's own line is ever touched.
    end += std::string(kEndMarker).size();

    if (end < content.size() && content[end] == '\n')
        ++end;

    std::string updated = content.substr(0, begin) + content.substr(end);

    std::ofstream out(profilePath, std::ios::trunc);

    if (out.is_open())
    {
        out << updated;
        removedAny = true;
    }

    return removedAny;
}

}
