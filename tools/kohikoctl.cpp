// kohikoctl - the command-line client for Kohiko's IPCServer, in the
// spirit of hyprctl: connect, send one line, print whatever comes
// back, disconnect.
//
//   kohikoctl dispatch workspace 3
//   kohikoctl clients | monitors | activewindow | tree
//   kohikoctl reload
//   kohikoctl reloadlauncher
//   kohikoctl quit
//
// restore-config and configure-autologin are the exceptions - neither
// touches the IPC socket at all, deliberately: restore-config exists
// specifically for the moment Kohiko *isn't* running (stuck in
// RecoveryMode's safe-defaults fallback after a bad config - see
// RecoveryMode.h/ConfigMigration.h), and configure-autologin edits
// system-wide display manager configuration, nothing kohiko itself
// has any part in - so both work as plain filesystem operations
// instead.
//
//   kohikoctl restore-config [path]
//   kohikoctl configure-autologin [--undo]
//   kohikoctl configure-console-autologin [--undo]

#include "AutologinConfigurator.h"
#include "ConsoleAutologinConfigurator.h"
#include "IpcPath.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

namespace
{

void PrintUsage()
{
    std::fprintf(
        stderr,
        "usage: kohikoctl <command> [args...]\n"
        "\n"
        "  kohikoctl dispatch <action>    e.g. kohikoctl dispatch workspace 3\n"
        "  kohikoctl clients              JSON list of every managed window\n"
        "  kohikoctl monitors             JSON list of detected monitors\n"
        "  kohikoctl activewindow         JSON info for the focused window\n"
        "  kohikoctl tree                 JSON dump of the current workspace's BSP tree\n"
        "  kohikoctl notify <text>        show a native toast notification (bottom-right, 2.5s)\n"
        "  kohikoctl reload               re-read the config file\n"
        "  kohikoctl reloadlauncher       re-scan applications/files for the native launcher\n"
        "  kohikoctl quit                 ask kohiko to exit\n"
        "  kohikoctl restore-config [path]\n"
        "                                 restore kohiko.conf from its .bak backup - works\n"
        "                                 even if kohiko isn't running (see RecoveryMode)\n"
        "  kohikoctl configure-autologin [--undo]\n"
        "                                 offer to set up display-manager autologin for a\n"
        "                                 Kohiko session (needs root; always asks first,\n"
        "                                 default answer is No). --undo removes it again.\n"
        "  kohikoctl configure-console-autologin [--undo]\n"
        "                                 the same, but for a plain-console/`startx` setup\n"
        "                                 with no display manager at all - see the README's\n"
        "                                 \"Automatic login\" section. --undo removes it again.\n"
    );
}

// Exactly mirrors main.cpp's own default (no path given on kohiko's
// own command line) - deliberately not going through Xdg::Home() (a
// different fallback, "" rather than ".") so this can never disagree
// with the same default kohiko itself actually started from.
std::string DefaultConfigPath()
{
    const char* home = std::getenv("HOME");
    return (home ? std::string(home) : std::string(".")) + "/.config/kohiko/kohiko.conf";
}

int RestoreConfig(
    const std::string& configPath)
{
    std::string backupPath = configPath + ".bak";
    std::error_code error;

    if (!std::filesystem::is_regular_file(backupPath, error))
    {
        std::fprintf(stderr, "kohikoctl: no backup found at %s\n", backupPath.c_str());
        return 1;
    }

    std::filesystem::copy_file(
        backupPath, configPath,
        std::filesystem::copy_options::overwrite_existing, error);

    if (error)
    {
        std::fprintf(stderr, "kohikoctl: could not restore %s: %s\n",
            configPath.c_str(), error.message().c_str());
        return 1;
    }

    std::printf("kohikoctl: restored %s from %s\n", configPath.c_str(), backupPath.c_str());
    std::printf("Run 'kohikoctl reload' if kohiko is already running, or just restart it.\n");
    return 0;
}

const char* DisplayManagerName(Kohiko::AutologinConfigurator::DisplayManager manager)
{
    using DM = Kohiko::AutologinConfigurator::DisplayManager;

    switch (manager)
    {
        case DM::LightDM:  return "LightDM";
        case DM::Sddm:     return "SDDM";
        case DM::Gdm:      return "GDM";
        case DM::Unknown:  return "an unrecognised display manager";
        case DM::Ambiguous: return "an ambiguous set of display managers";
        case DM::None:     return "no display manager";
    }

    return "an unknown display manager";
}

// Undo is always attempted against every display manager Configure()
// could ever have targeted, regardless of what's currently detected -
// the display manager installed at Configure() time might not even be
// the one installed now, and Undo() is a harmless no-op for any
// manager it finds nothing to remove for.
int UndoAutologin()
{
    using DM = Kohiko::AutologinConfigurator::DisplayManager;

    bool removedAny = false;

    for (DM manager : { DM::LightDM, DM::Sddm })
    {
        std::string path = Kohiko::AutologinConfigurator::DropInPath(manager);

        if (Kohiko::AutologinConfigurator::Undo(manager))
        {
            std::printf("kohikoctl: removed %s\n", path.c_str());
            removedAny = true;
        }
    }

    if (!removedAny)
        std::printf("kohikoctl: no Kohiko-created autologin configuration was found to remove.\n");

    return 0;
}

int ConfigureAutologin(
    bool undo)
{
    if (geteuid() != 0)
    {
        std::fprintf(stderr,
            "kohikoctl: configure-autologin needs root - re-run with sudo\n");
        return 1;
    }

    if (undo)
        return UndoAutologin();

    using Result = Kohiko::AutologinConfigurator;

    Result::DetectionResult detection = Result::DetectDisplayManager();

    std::printf("Detected: %s\n", detection.detail.c_str());

    if (detection.manager != Result::DisplayManager::LightDM &&
        detection.manager != Result::DisplayManager::Sddm)
    {
        std::printf(
            "\nAutomatic autologin configuration isn't available for %s.\n"
            "See the README's autologin section for how to set it up by hand instead -\n"
            "nothing has been changed.\n",
            DisplayManagerName(detection.manager));
        return 1;
    }

    Result::ExistingAutologinCheck existing = Result::CheckExistingAutologin(detection.manager);

    if (existing.found)
    {
        std::printf(
            "\nAn existing autologin configuration was already found at:\n"
            "  %s\n"
            "Kohiko will not modify or overwrite it. Remove or edit it by hand first\n"
            "if you want to reconfigure autologin - nothing has been changed.\n",
            existing.location.c_str());
        return 1;
    }

    // sudo preserves the original invoking user in $SUDO_USER - unlike
    // getuid()/getpwuid(), which (this whole function only having run
    // this far because geteuid() == 0 above) would just report root.
    // Only ever used as a suggested default, never assumed - the
    // prompt below always requires it to be explicitly confirmed or
    // overridden, same as everything else in this flow.
    const char* sudoUser = std::getenv("SUDO_USER");
    std::string defaultUser = sudoUser ? sudoUser : "";

    std::printf("\nThis will set up automatic login into a Kohiko session via %s.\n",
        DisplayManagerName(detection.manager));

    if (defaultUser.empty())
        std::printf("Auto-login user: ");
    else
        std::printf("Auto-login user [%s]: ", defaultUser.c_str());

    std::string username;
    std::getline(std::cin, username);

    // Trim surrounding whitespace by hand rather than pulling in
    // Utils.cpp as a new dependency for this one call - kohikoctl has
    // deliberately stayed a tiny, near-dependency-free binary (see
    // its own build rule in the Makefile/CMakeLists.txt).
    std::size_t start = username.find_first_not_of(" \t\r\n");
    std::size_t end = username.find_last_not_of(" \t\r\n");
    username = (start == std::string::npos) ? "" : username.substr(start, end - start + 1);

    if (username.empty())
        username = defaultUser;

    if (username.empty())
    {
        std::printf("\nNo username given - aborting. Nothing has been changed.\n");
        return 1;
    }

    std::string dropInPath = Result::DropInPath(detection.manager);
    std::string content = Result::PreviewContent(detection.manager, username);

    std::printf(
        "\nThis will create:\n"
        "  %s\n"
        "containing:\n"
        "\n%s\n"
        "This does NOT change %s's password or disable normal login - anyone with\n"
        "physical/console access to this machine will reach a Kohiko desktop\n"
        "without entering a password. Only do this on a machine you trust\n"
        "physically.\n\n",
        dropInPath.c_str(), content.c_str(), username.c_str());

    std::printf("Proceed? [y/N]: ");

    std::string confirm;
    std::getline(std::cin, confirm);

    if (confirm != "y" && confirm != "Y" && confirm != "yes" && confirm != "YES")
    {
        std::printf("Aborted - nothing has been changed.\n");
        return 1;
    }

    Result::ConfigureResult result = Result::Configure(detection.manager, username);

    std::printf("%s\n", result.message.c_str());

    return result.success ? 0 : 1;
}

// Trims surrounding whitespace, same as the inline logic in
// ConfigureAutologin() above and for the same reason (see its own
// comment) - factored out here only because this flow needs it twice
// (username and tty) rather than once.
std::string TrimCliInput(const std::string& value)
{
    std::size_t start = value.find_first_not_of(" \t\r\n");
    std::size_t end = value.find_last_not_of(" \t\r\n");
    return (start == std::string::npos) ? "" : value.substr(start, end - start + 1);
}

int ConfigureConsoleAutologin(
    bool undo)
{
    if (geteuid() != 0)
    {
        std::fprintf(stderr,
            "kohikoctl: configure-console-autologin needs root - re-run with sudo\n");
        return 1;
    }

    using Result = Kohiko::ConsoleAutologinConfigurator;

    Result::DetectionResult detection = Result::DetectSupport();

    std::printf("Detected: %s\n", detection.detail.c_str());

    if (!detection.supported)
    {
        std::printf(
            "\nAutomatic console autologin isn't available on this machine.\n"
            "See the README's \"Automatic login\" section for how to set this up\n"
            "by hand instead - nothing has been changed.\n");
        return 1;
    }

    const char* sudoUser = std::getenv("SUDO_USER");
    std::string defaultUser = sudoUser ? sudoUser : "";

    if (defaultUser.empty())
        std::printf("\nUsername: ");
    else
        std::printf("\nUsername [%s]: ", defaultUser.c_str());

    std::string username;
    std::getline(std::cin, username);
    username = TrimCliInput(username);

    if (username.empty())
        username = defaultUser;

    if (username.empty())
    {
        std::printf("\nNo username given - aborting. Nothing has been changed.\n");
        return 1;
    }

    std::printf("Console tty [tty1]: ");
    std::string tty;
    std::getline(std::cin, tty);
    tty = TrimCliInput(tty);

    if (tty.empty())
        tty = "tty1";

    if (undo)
    {
        bool removed = Result::Undo(username, tty);

        if (removed)
        {
            std::printf(
                "kohikoctl: removed the console autologin configured for %s on %s.\n",
                username.c_str(), tty.c_str());
        }
        else
        {
            std::printf(
                "kohikoctl: no Kohiko-created console autologin was found for %s on %s.\n",
                username.c_str(), tty.c_str());
        }

        return 0;
    }

    Result::ExistingCheck existing = Result::CheckExistingGettyAutologin(tty);

    if (existing.found)
    {
        std::printf(
            "\nAn existing getty autologin was already found at:\n"
            "  %s\n"
            "Kohiko will not modify or overwrite it. Remove or edit it by hand first\n"
            "if you want to reconfigure it - nothing has been changed.\n",
            existing.location.c_str());
        return 1;
    }

    Result::PasswdEntry pw = Result::LookupUser(username);

    if (!pw.found)
    {
        std::printf("\nNo such user '%s' - aborting. Nothing has been changed.\n", username.c_str());
        return 1;
    }

    Result::LoginShell shell = Result::ClassifyShell(pw.shell);

    if (shell == Result::LoginShell::Unknown)
    {
        std::printf(
            "\n%s's login shell (%s) isn't one this can automate a startx line for.\n"
            "Add this line to its login profile by hand instead - nothing has been\n"
            "changed:\n"
            "    [ -z \"$DISPLAY\" ] && [ \"$(tty)\" = \"/dev/%s\" ] && exec startx\n",
            username.c_str(), pw.shell.c_str(), tty.c_str());
        return 1;
    }

    std::string profilePath = Result::ProfilePath(shell, pw.home);

    if (Result::ProfileHasBlock(profilePath))
    {
        std::printf(
            "\nA console-autologin block already exists in:\n"
            "  %s\n"
            "Kohiko will not add a second one. Remove it by hand first if you want to\n"
            "reconfigure it - nothing has been changed.\n",
            profilePath.c_str());
        return 1;
    }

    std::string dropInPath = Result::GettyDropInPath(tty);
    std::string gettyContent = Result::GettyPreviewContent(username);
    std::string profileBlock = Result::ProfileBlock(tty);

    std::printf(
        "\nThis will create:\n"
        "  %s\n"
        "containing:\n"
        "\n%s\n"
        "and append this block to %s:\n"
        "\n%s\n"
        "This does NOT change %s's password or disable normal login - anyone with\n"
        "physical access to this machine's console reaches an X session with no\n"
        "password prompt at all (Kohiko's own lock screen, if `lockscreen.after` is\n"
        "configured to engage at startup, is still there - see the README's \"Native\n"
        "lock screen\" section). Only do this on a machine you trust physically.\n\n",
        dropInPath.c_str(), gettyContent.c_str(), profilePath.c_str(), profileBlock.c_str(),
        username.c_str());

    if (Result::XinitrcBypassesSessionWrapper(pw.home))
    {
        std::printf(
            "Note: %s/.xinitrc currently execs `kohiko` directly rather than\n"
            "`kohiko-session`, which bypasses its crash-restart supervision. Update its\n"
            "`exec kohiko` (or `exec /usr/local/bin/kohiko`) line to `exec kohiko-session`\n"
            "to get that back - this tool won't edit that file for you.\n\n",
            pw.home.c_str());
    }

    std::printf("Proceed? [y/N]: ");

    std::string confirm;
    std::getline(std::cin, confirm);

    if (confirm != "y" && confirm != "Y" && confirm != "yes" && confirm != "YES")
    {
        std::printf("Aborted - nothing has been changed.\n");
        return 1;
    }

    Result::ConfigureResult result = Result::Configure(username, tty);

    std::printf("%s\n", result.message.c_str());

    return result.success ? 0 : 1;
}

}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        PrintUsage();
        return 1;
    }

    if (std::string(argv[1]) == "restore-config")
        return RestoreConfig(argc > 2 ? argv[2] : DefaultConfigPath());

    if (std::string(argv[1]) == "configure-autologin")
        return ConfigureAutologin(argc > 2 && std::string(argv[2]) == "--undo");

    if (std::string(argv[1]) == "configure-console-autologin")
        return ConfigureConsoleAutologin(argc > 2 && std::string(argv[2]) == "--undo");

    std::string request;

    for (int i = 1; i < argc; ++i)
    {
        if (i > 1)
            request += ' ';

        request += argv[i];
    }

    std::string path = Kohiko::IpcSocketPath();

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
    {
        std::perror("kohikoctl: socket");
        return 1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        std::fprintf(stderr, "kohikoctl: could not connect to %s (is kohiko running?)\n", path.c_str());
        close(fd);
        return 1;
    }

    request += '\n';
    ssize_t sent = write(fd, request.data(), request.size());
    (void)sent;

    shutdown(fd, SHUT_WR);

    std::string response;
    char buffer[4096];
    ssize_t received;

    while ((received = read(fd, buffer, sizeof(buffer))) > 0)
        response.append(buffer, static_cast<std::size_t>(received));

    close(fd);

    std::fputs(response.c_str(), stdout);

    return 0;
}
