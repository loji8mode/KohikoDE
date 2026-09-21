// Regression test for ConsoleAutologinConfigurator - no X11 needed.
// Every scenario is exercised against a real, throwaway fake
// filesystem tree (this class's `root` parameter exists specifically
// to make this possible - see AutologinConfigurator.h's own header
// comment, which this class's applies to identically), not mocked.
//
// Build & run: see the "test-consoleautologinconfigurator" target in
// the Makefile.

#include "ConsoleAutologinConfigurator.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace Kohiko;
namespace fs = std::filesystem;

namespace
{

int g_pass = 0;

void Check(bool condition, const char* what)
{
    if (condition)
    {
        ++g_pass;
        std::printf("  PASS: %s\n", what);
    }
    else
    {
        std::printf("  FAIL: %s\n", what);
        std::exit(1);
    }
}

void WriteFile(const fs::path& path, const std::string& content)
{
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::trunc);
    file << content;
}

std::string ReadFile(const fs::path& path)
{
    std::ifstream file(path);
    return std::string(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
}

// A minimal but complete /etc/passwd - every scenario that needs a
// real user starts from this so Configure()/Undo() have somewhere
// consistent to look.
void WriteBasicPasswd(const fs::path& root)
{
    WriteFile(root / "etc/passwd",
        "root:x:0:0:root:/root:/bin/bash\n"
        "alice:x:1000:1000:Alice:/home/alice:/bin/bash\n"
        "zed:x:1001:1001:Zed:/home/zed:/usr/bin/zsh\n"
        "dana:x:1002:1002:Dana:/home/dana:/bin/dash\n"
        "fin:x:1003:1003:Fin:/home/fin:/usr/bin/fish\n");
}

void WriteGettyTemplate(const fs::path& root)
{
    WriteFile(root / "usr/lib/systemd/system/getty@.service",
        "[Unit]\nDescription=Getty on %I\n");
}

}

int main()
{
    fs::path root = fs::temp_directory_path() / "kohiko-test-consoleautologinconfigurator";

    auto reset = [&]() { fs::remove_all(root); fs::create_directories(root); };

    // --- DetectSupport() -----------------------------------------------------

    std::printf("-- DetectSupport() --\n");
    {
        reset();
        WriteGettyTemplate(root);

        auto result = ConsoleAutologinConfigurator::DetectSupport(root.string());
        Check(result.supported, "a getty@.service template under /usr/lib/systemd/system -> supported");
    }
    {
        reset();
        WriteFile(root / "lib/systemd/system/getty@.service", "[Unit]\n");

        auto result = ConsoleAutologinConfigurator::DetectSupport(root.string());
        Check(result.supported, "a getty@.service template under the non-merged /lib path -> supported too");
    }
    {
        reset();

        auto result = ConsoleAutologinConfigurator::DetectSupport(root.string());
        Check(!result.supported, "no getty@.service template anywhere -> not supported, not a guess");
    }

    // --- LookupUser() ----------------------------------------------------------

    std::printf("\n-- LookupUser() --\n");
    {
        reset();
        WriteBasicPasswd(root);

        auto pw = ConsoleAutologinConfigurator::LookupUser("alice", root.string());
        Check(pw.found, "an existing user is found");
        Check(pw.home == "/home/alice", "...with the correct home directory");
        Check(pw.shell == "/bin/bash", "...and the correct shell");
    }
    {
        reset();
        WriteBasicPasswd(root);

        auto pw = ConsoleAutologinConfigurator::LookupUser("nobody-such-user", root.string());
        Check(!pw.found, "a nonexistent user correctly reports not found");
    }
    {
        reset();
        WriteFile(root / "etc/passwd", "alice:x:1000:1000\n"); // too few fields
        auto pw = ConsoleAutologinConfigurator::LookupUser("alice", root.string());
        Check(!pw.found, "a malformed (too-short) passwd line is skipped, not crashed on");
    }

    // --- ClassifyShell() / ProfilePath() ----------------------------------------

    std::printf("\n-- ClassifyShell() / ProfilePath() --\n");
    {
        using Shell = ConsoleAutologinConfigurator::LoginShell;

        Check(ConsoleAutologinConfigurator::ClassifyShell("/bin/bash") == Shell::Bash, "/bin/bash -> Bash");
        Check(ConsoleAutologinConfigurator::ClassifyShell("/usr/bin/zsh") == Shell::Zsh, "/usr/bin/zsh -> Zsh");
        Check(ConsoleAutologinConfigurator::ClassifyShell("/bin/dash") == Shell::PosixSh, "/bin/dash -> PosixSh");
        Check(ConsoleAutologinConfigurator::ClassifyShell("/bin/sh") == Shell::PosixSh, "/bin/sh -> PosixSh");
        Check(ConsoleAutologinConfigurator::ClassifyShell("/usr/bin/fish") == Shell::Unknown, "/usr/bin/fish -> Unknown, not guessed at");

        Check(ConsoleAutologinConfigurator::ProfilePath(Shell::Bash, "/home/alice") == "/home/alice/.bash_profile",
              "Bash -> ~/.bash_profile");
        Check(ConsoleAutologinConfigurator::ProfilePath(Shell::Zsh, "/home/zed") == "/home/zed/.zprofile",
              "Zsh -> ~/.zprofile");
        Check(ConsoleAutologinConfigurator::ProfilePath(Shell::PosixSh, "/home/dana") == "/home/dana/.profile",
              "PosixSh -> ~/.profile");
        Check(ConsoleAutologinConfigurator::ProfilePath(Shell::Unknown, "/home/fin").empty(),
              "Unknown -> no profile path at all");
    }

    // --- LooksLikeConsoleTty() --------------------------------------------------

    std::printf("\n-- LooksLikeConsoleTty() --\n");
    {
        Check(ConsoleAutologinConfigurator::LooksLikeConsoleTty("tty1"), "'tty1' is accepted");
        Check(ConsoleAutologinConfigurator::LooksLikeConsoleTty("tty12"), "'tty12' is accepted");
        Check(ConsoleAutologinConfigurator::LooksLikeConsoleTty("tty0"), "'tty0' is accepted");
        Check(!ConsoleAutologinConfigurator::LooksLikeConsoleTty(""), "an empty string is rejected");
        Check(!ConsoleAutologinConfigurator::LooksLikeConsoleTty("tty"), "'tty' with no digits at all is rejected");
        Check(!ConsoleAutologinConfigurator::LooksLikeConsoleTty("ttyabc"), "'ttyabc' is rejected");
        Check(!ConsoleAutologinConfigurator::LooksLikeConsoleTty("tty1a"), "'tty1a' (trailing junk) is rejected");
        Check(!ConsoleAutologinConfigurator::LooksLikeConsoleTty("TTY1"), "uppercase 'TTY1' is rejected");
        Check(!ConsoleAutologinConfigurator::LooksLikeConsoleTty("tty 1"), "'tty 1' (embedded space) is rejected");
        // The real incident this exists for: something typed into the
        // tty prompt by mistake (a password, in the actual report) that
        // happens to contain no spaces, still correctly rejected for
        // not being all-digits-after-"tty".
        Check(!ConsoleAutologinConfigurator::LooksLikeConsoleTty("NotAConsole123!"),
              "an arbitrary mistyped string (e.g. a password typed into the wrong prompt) is rejected");
    }

    // --- ProfileBlock() / ProfileHasBlock() -------------------------------------

    std::printf("\n-- ProfileBlock() / ProfileHasBlock() --\n");
    {
        reset();
        std::string block = ConsoleAutologinConfigurator::ProfileBlock("tty1");
        Check(block.find("exec startx") != std::string::npos, "the block actually execs startx");
        Check(block.find("/dev/tty1") != std::string::npos, "...gated on the specific tty passed in");
        Check(block.find("DISPLAY") != std::string::npos, "...and gated on DISPLAY being unset");

        WriteFile(root / "profile-without-block", "# just a normal profile\nexport PATH=$PATH:/opt/bin\n");
        Check(!ConsoleAutologinConfigurator::ProfileHasBlock((root / "profile-without-block").string()),
              "a normal profile with no block reports false");

        WriteFile(root / "profile-with-block", "export PATH=$PATH:/opt/bin\n" + block);
        Check(ConsoleAutologinConfigurator::ProfileHasBlock((root / "profile-with-block").string()),
              "a profile containing the block reports true");
    }

    // --- ProfileAlreadyAutostartsX() --------------------------------------------

    std::printf("\n-- ProfileAlreadyAutostartsX() --\n");
    {
        reset();
        WriteFile(root / "profile-plain", "# nothing special here\nexport PATH=$PATH:/opt/bin\n");
        Check(!ConsoleAutologinConfigurator::ProfileAlreadyAutostartsX((root / "profile-plain").string()),
              "a profile with no startx line at all reports false");

        WriteFile(root / "profile-handwritten",
            "if [ -z \"$DISPLAY\" ] && [ \"$(tty)\" = \"/dev/tty1\" ]; then\n    exec startx\nfi\n");
        Check(ConsoleAutologinConfigurator::ProfileAlreadyAutostartsX((root / "profile-handwritten").string()),
              "a pre-existing, hand-written (unmarked) startx guard is detected too");
    }

    // --- GettyDropInPath() / GettyPreviewContent() ------------------------------

    std::printf("\n-- GettyDropInPath() / GettyPreviewContent() --\n");
    {
        std::string path = ConsoleAutologinConfigurator::GettyDropInPath("tty1", root.string());
        Check(path == (root / "etc/systemd/system/getty@tty1.service.d/60-kohiko-autologin.conf").string(),
              "the drop-in path is exactly getty@<tty>.service.d/60-kohiko-autologin.conf");

        std::string content = ConsoleAutologinConfigurator::GettyPreviewContent("alice");
        Check(content.find("--autologin alice") != std::string::npos,
              "the preview content passes --autologin with the correct username");
        Check(content.find("ExecStart=\n") != std::string::npos,
              "...and clears the vendor ExecStart= first, the standard systemd override pattern");
    }

    // --- CheckExistingGettyAutologin() ------------------------------------------

    std::printf("\n-- CheckExistingGettyAutologin() --\n");
    {
        reset();
        WriteFile(root / "etc/systemd/system/getty@tty1.service.d/50-other.conf",
            "[Service]\nExecStart=\nExecStart=-/usr/bin/agetty --autologin someone --noclear %I $TERM\n");

        auto check = ConsoleAutologinConfigurator::CheckExistingGettyAutologin("tty1", root.string());
        Check(check.found, "an active --autologin ExecStart= line is found, from anyone");
    }
    {
        reset();
        WriteFile(root / "etc/systemd/system/getty@tty1.service.d/50-other.conf",
            "[Service]\n#ExecStart=-/usr/bin/agetty --autologin someone --noclear %I $TERM\n");

        auto check = ConsoleAutologinConfigurator::CheckExistingGettyAutologin("tty1", root.string());
        Check(!check.found, "a commented-out ExecStart= line is correctly ignored");
    }
    {
        reset();
        WriteFile(root / "etc/systemd/system/getty@tty1.service.d/50-other.conf",
            "[Service]\nExecStart=\nExecStart=-/usr/bin/agetty --noclear %I $TERM\n");

        auto check = ConsoleAutologinConfigurator::CheckExistingGettyAutologin("tty1", root.string());
        Check(!check.found, "an ExecStart= line with no --autologin flag doesn't count");
    }
    {
        reset();
        auto check = ConsoleAutologinConfigurator::CheckExistingGettyAutologin("tty1", root.string());
        Check(!check.found, "no drop-in directory at all reports nothing found");
    }

    // --- XinitrcBypassesSessionWrapper() ----------------------------------------

    std::printf("\n-- XinitrcBypassesSessionWrapper() --\n");
    {
        reset();
        WriteFile(root / "home/alice/.xinitrc", "#!/bin/sh\nnumlockx on\nexec /usr/local/bin/kohiko\n");
        Check(ConsoleAutologinConfigurator::XinitrcBypassesSessionWrapper("/home/alice", root.string()),
              "'exec /usr/local/bin/kohiko' (bypassing kohiko-session) is detected");
    }
    {
        reset();
        WriteFile(root / "home/alice/.xinitrc", "#!/bin/sh\nexec kohiko\n");
        Check(ConsoleAutologinConfigurator::XinitrcBypassesSessionWrapper("/home/alice", root.string()),
              "the bare 'exec kohiko' form is detected too");
    }
    {
        reset();
        WriteFile(root / "home/alice/.xinitrc", "#!/bin/sh\nexec kohiko-session\n");
        Check(!ConsoleAutologinConfigurator::XinitrcBypassesSessionWrapper("/home/alice", root.string()),
              "'exec kohiko-session' is correctly NOT flagged as a bypass");
    }
    {
        reset();
        Check(!ConsoleAutologinConfigurator::XinitrcBypassesSessionWrapper("/home/alice", root.string()),
              "no ~/.xinitrc at all reports false, not an error");
    }

    // --- Configure() -----------------------------------------------------------

    std::printf("\n-- Configure() succeeds cleanly with nothing pre-existing --\n");
    {
        reset();
        WriteGettyTemplate(root);
        WriteBasicPasswd(root);

        auto result = ConsoleAutologinConfigurator::Configure("alice", "tty1", root.string());
        Check(result.success, "Configure() reports success");

        fs::path dropIn = root / "etc/systemd/system/getty@tty1.service.d/60-kohiko-autologin.conf";
        Check(fs::exists(dropIn), "the getty drop-in now exists");
        Check(ReadFile(dropIn).find("--autologin alice") != std::string::npos,
              "...with the correct username");

        fs::path profile = root / "home/alice/.bash_profile";
        Check(fs::exists(profile), "alice's .bash_profile now exists");
        Check(ConsoleAutologinConfigurator::ProfileHasBlock(profile.string()),
              "...and contains the console-autologin block");
        Check(result.message.find("bypasses") == std::string::npos,
              "no .xinitrc-bypass note when alice has no ~/.xinitrc at all");
    }

    std::printf("\n-- Configure() preserves the rest of an existing profile file --\n");
    {
        reset();
        WriteGettyTemplate(root);
        WriteBasicPasswd(root);
        WriteFile(root / "home/alice/.bash_profile", "# my own stuff\nexport PATH=$PATH:/opt/bin\n");

        auto result = ConsoleAutologinConfigurator::Configure("alice", "tty1", root.string());
        Check(result.success, "Configure() reports success");

        std::string content = ReadFile(root / "home/alice/.bash_profile");
        Check(content.find("export PATH=$PATH:/opt/bin") != std::string::npos,
              "the user's own existing profile content is still there");
        Check(content.find("exec startx") != std::string::npos,
              "...with the new block appended after it");
    }

    std::printf("\n-- Configure() warns (but still succeeds) when ~/.xinitrc bypasses kohiko-session --\n");
    {
        reset();
        WriteGettyTemplate(root);
        WriteBasicPasswd(root);
        WriteFile(root / "home/alice/.xinitrc", "#!/bin/sh\nexec /usr/local/bin/kohiko\n");

        auto result = ConsoleAutologinConfigurator::Configure("alice", "tty1", root.string());
        Check(result.success, "Configure() still succeeds - this is advisory, not fatal");
        Check(result.message.find("bypasses") != std::string::npos,
              "...but the message flags the .xinitrc bypass");
    }

    std::printf("\n-- Configure() refuses when unsupported (no getty@.service template) --\n");
    {
        reset();
        WriteBasicPasswd(root);

        auto result = ConsoleAutologinConfigurator::Configure("alice", "tty1", root.string());
        Check(!result.success, "Configure() refuses outright");
    }

    std::printf("\n-- Configure() refuses for a nonexistent user --\n");
    {
        reset();
        WriteGettyTemplate(root);
        WriteBasicPasswd(root);

        auto result = ConsoleAutologinConfigurator::Configure("nobody-such-user", "tty1", root.string());
        Check(!result.success, "Configure() refuses outright");
    }

    std::printf("\n-- Configure() refuses for a user with an unsupported login shell --\n");
    {
        reset();
        WriteGettyTemplate(root);
        WriteBasicPasswd(root);

        auto result = ConsoleAutologinConfigurator::Configure("fin", "tty1", root.string());
        Check(!result.success, "Configure() refuses rather than guessing at fish syntax");
        Check(!fs::exists(root / "etc/systemd/system/getty@tty1.service.d/60-kohiko-autologin.conf"),
              "...and creates no getty drop-in at all");
    }

    std::printf("\n-- Configure() refuses when an existing getty autologin is already there --\n");
    {
        reset();
        WriteGettyTemplate(root);
        WriteBasicPasswd(root);
        WriteFile(root / "etc/systemd/system/getty@tty1.service.d/50-other.conf",
            "[Service]\nExecStart=\nExecStart=-/usr/bin/agetty --autologin someone --noclear %I $TERM\n");

        auto result = ConsoleAutologinConfigurator::Configure("alice", "tty1", root.string());
        Check(!result.success, "Configure() refuses outright");
        Check(!fs::exists(root / "etc/systemd/system/getty@tty1.service.d/60-kohiko-autologin.conf"),
              "...and creates no drop-in of its own");
    }

    std::printf("\n-- Configure() refuses when its own drop-in file already exists --\n");
    {
        reset();
        WriteGettyTemplate(root);
        WriteBasicPasswd(root);
        WriteFile(root / "etc/systemd/system/getty@tty1.service.d/60-kohiko-autologin.conf", "already here\n");

        auto result = ConsoleAutologinConfigurator::Configure("alice", "tty1", root.string());
        Check(!result.success, "Configure() refuses rather than overwriting it");
        Check(ReadFile(root / "etc/systemd/system/getty@tty1.service.d/60-kohiko-autologin.conf") == "already here\n",
              "...and the existing file's content is completely untouched");
    }

    std::printf("\n-- Configure() skips the profile step (but still adds getty autologin) when the profile already auto-starts X another way --\n");
    {
        reset();
        WriteGettyTemplate(root);
        WriteBasicPasswd(root);
        const std::string existingContent =
            "if [ -z \"$DISPLAY\" ] && [ \"$(tty)\" = \"/dev/tty1\" ]; then\n    exec startx\nfi\n";
        WriteFile(root / "home/alice/.bash_profile", existingContent);

        auto result = ConsoleAutologinConfigurator::Configure("alice", "tty1", root.string());
        Check(result.success, "Configure() still succeeds overall - the getty half is genuinely new");
        Check(fs::exists(root / "etc/systemd/system/getty@tty1.service.d/60-kohiko-autologin.conf"),
              "...and the getty drop-in really was created");

        std::string content = ReadFile(root / "home/alice/.bash_profile");
        Check(content == existingContent,
              "...but the user's existing profile is completely unmodified, byte for byte - no duplicate added");
        Check(!ConsoleAutologinConfigurator::ProfileHasBlock((root / "home/alice/.bash_profile").string()),
              "...specifically, no Kohiko marker block was added at all");
    }

    std::printf("\n-- Undo() only ever removes a block Kohiko itself added, never a pre-existing user block --\n");
    {
        reset();
        WriteGettyTemplate(root);
        WriteBasicPasswd(root);
        const std::string existingContent =
            "if [ -z \"$DISPLAY\" ] && [ \"$(tty)\" = \"/dev/tty1\" ]; then\n    exec startx\nfi\n";
        WriteFile(root / "home/alice/.bash_profile", existingContent);

        ConsoleAutologinConfigurator::Configure("alice", "tty1", root.string()); // skips the profile step, per above

        bool removed = ConsoleAutologinConfigurator::Undo("alice", "tty1", root.string());
        Check(removed, "Undo() reports it removed something (the getty drop-in)");
        Check(!fs::exists(root / "etc/systemd/system/getty@tty1.service.d/60-kohiko-autologin.conf"),
              "...and the getty drop-in really is gone");

        std::string content = ReadFile(root / "home/alice/.bash_profile");
        Check(content == existingContent,
              "...while the user's own pre-existing block - never marked, never Kohiko's - is completely untouched");
    }

    std::printf("\n-- Configure() refuses when the profile already has a block --\n");
    {
        reset();
        WriteGettyTemplate(root);
        WriteBasicPasswd(root);
        WriteFile(root / "home/alice/.bash_profile", ConsoleAutologinConfigurator::ProfileBlock("tty1"));

        auto result = ConsoleAutologinConfigurator::Configure("alice", "tty1", root.string());
        Check(!result.success, "Configure() refuses outright");
        Check(!fs::exists(root / "etc/systemd/system/getty@tty1.service.d/60-kohiko-autologin.conf"),
              "...and creates no getty drop-in either, since it fails before writing anything");
    }

    std::printf("\n-- Configure() refuses a mistyped/non-tty value before writing anything --\n");
    {
        reset();
        WriteGettyTemplate(root);
        WriteBasicPasswd(root);

        // The actual real-world case this guards against: something
        // that isn't a console name at all (a password typed into the
        // wrong prompt, in the real report this came from) - deliberately
        // not reusing any real credential, just an arbitrary string
        // shaped like one.
        auto result = ConsoleAutologinConfigurator::Configure("alice", "NotAConsole123!", root.string());
        Check(!result.success, "Configure() refuses outright");
        Check(!fs::exists(root / "etc/systemd/system/getty@NotAConsole123!.service.d"),
              "...and never creates a getty drop-in directory named after the bad value");
        Check(!ConsoleAutologinConfigurator::ProfileHasBlock((root / "home/alice/.bash_profile").string()),
              "...and alice's profile is never touched at all");
    }

    std::printf("\n-- Configure() rolls back the getty drop-in if the profile half fails --\n");
    {
        reset();
        WriteGettyTemplate(root);
        WriteBasicPasswd(root);
        // A regular file where a directory needs to be created blocks
        // fs::create_directories() for alice's profile path, simulating
        // a permissions/environment problem partway through.
        WriteFile(root / "home", "not a directory");

        auto result = ConsoleAutologinConfigurator::Configure("alice", "tty1", root.string());
        Check(!result.success, "Configure() reports failure rather than a partial success");
        Check(!fs::exists(root / "etc/systemd/system/getty@tty1.service.d/60-kohiko-autologin.conf"),
              "...and the getty drop-in it wrote a moment earlier was rolled back again");
    }

    // --- Undo() ------------------------------------------------------------------

    std::printf("\n-- Undo() removes both halves, leaving the rest of the profile intact --\n");
    {
        reset();
        WriteGettyTemplate(root);
        WriteBasicPasswd(root);
        WriteFile(root / "home/alice/.bash_profile", "# before\nexport PATH=$PATH:/opt/bin\n# after (kept)\n");

        ConsoleAutologinConfigurator::Configure("alice", "tty1", root.string());

        fs::path dropIn = root / "etc/systemd/system/getty@tty1.service.d/60-kohiko-autologin.conf";
        Check(fs::exists(dropIn), "sanity check: the drop-in exists before Undo()");

        bool removed = ConsoleAutologinConfigurator::Undo("alice", "tty1", root.string());
        Check(removed, "Undo() reports it removed something");
        Check(!fs::exists(dropIn), "...and the getty drop-in is actually gone");

        std::string content = ReadFile(root / "home/alice/.bash_profile");
        Check(!ConsoleAutologinConfigurator::ProfileHasBlock((root / "home/alice/.bash_profile").string()),
              "...the block itself is gone from the profile");
        Check(content.find("# before") != std::string::npos &&
              content.find("export PATH=$PATH:/opt/bin") != std::string::npos &&
              content.find("# after (kept)") != std::string::npos,
              "...but every line the user had before and after it survives untouched");
    }

    std::printf("\n-- Undo() still removes the getty drop-in even if the user no longer exists --\n");
    {
        reset();
        WriteGettyTemplate(root);
        WriteBasicPasswd(root);
        ConsoleAutologinConfigurator::Configure("alice", "tty1", root.string());

        // Simulate the account having been removed since Configure().
        WriteBasicPasswd(root); // (re-written identically here; the getty half never depended on it anyway)
        fs::remove(root / "etc/passwd");

        bool removed = ConsoleAutologinConfigurator::Undo("alice", "tty1", root.string());
        Check(removed, "Undo() still reports success for the half it *can* do");
        Check(!fs::exists(root / "etc/systemd/system/getty@tty1.service.d/60-kohiko-autologin.conf"),
              "...and the getty drop-in is gone regardless");
    }

    std::printf("\n-- Undo() is a harmless no-op when there's nothing to undo --\n");
    {
        reset();
        WriteBasicPasswd(root);
        bool removed = ConsoleAutologinConfigurator::Undo("alice", "tty1", root.string());
        Check(!removed, "Undo() correctly reports nothing was removed");
    }

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    fs::remove_all(root);
    return 0;
}
