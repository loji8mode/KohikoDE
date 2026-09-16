// Regression test for AutologinConfigurator - no X11 needed. Every
// scenario is exercised against a real, throwaway fake filesystem
// tree (this class's `root` parameter exists specifically to make
// this possible - see its own header comment), not mocked.
//
// Build & run: see the "test-autologinconfigurator" target in the
// Makefile.

#include "AutologinConfigurator.h"

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

void Symlink(const fs::path& link, const fs::path& target)
{
    fs::create_directories(link.parent_path());
    fs::remove(link);
    fs::create_symlink(target, link);
}

std::string ReadFile(const fs::path& path)
{
    std::ifstream file(path);
    return std::string(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
}

}

int main()
{
    fs::path root = fs::temp_directory_path() / "kohiko-test-autologinconfigurator";

    auto reset = [&]() { fs::remove_all(root); fs::create_directories(root); };

    // --- DetectDisplayManager() -------------------------------------------

    std::printf("-- Detection via the systemd display-manager.service symlink --\n");
    {
        reset();
        Symlink(root / "etc/systemd/system/display-manager.service",
                "/usr/lib/systemd/system/lightdm.service");

        auto result = AutologinConfigurator::DetectDisplayManager(root.string());
        Check(result.manager == AutologinConfigurator::DisplayManager::LightDM,
              "lightdm.service symlink -> DisplayManager::LightDM");
    }
    {
        reset();
        Symlink(root / "etc/systemd/system/display-manager.service",
                "/usr/lib/systemd/system/sddm.service");

        auto result = AutologinConfigurator::DetectDisplayManager(root.string());
        Check(result.manager == AutologinConfigurator::DisplayManager::Sddm,
              "sddm.service symlink -> DisplayManager::Sddm");
    }
    {
        reset();
        Symlink(root / "etc/systemd/system/display-manager.service",
                "/usr/lib/systemd/system/gdm.service");

        auto result = AutologinConfigurator::DetectDisplayManager(root.string());
        Check(result.manager == AutologinConfigurator::DisplayManager::Gdm,
              "gdm.service symlink -> DisplayManager::Gdm");
    }
    {
        reset();
        Symlink(root / "etc/systemd/system/display-manager.service",
                "/usr/lib/systemd/system/some-exotic-dm.service");

        auto result = AutologinConfigurator::DetectDisplayManager(root.string());
        Check(result.manager == AutologinConfigurator::DisplayManager::Unknown,
              "an unrecognised .service name -> DisplayManager::Unknown, not a guess");
    }

    std::printf("\n-- Fallback to binary detection with no systemd symlink --\n");
    {
        reset();
        WriteFile(root / "usr/bin/sddm", "");

        auto result = AutologinConfigurator::DetectDisplayManager(root.string());
        Check(result.manager == AutologinConfigurator::DisplayManager::Sddm,
              "a lone sddm binary with no systemd symlink -> DisplayManager::Sddm");
    }
    {
        reset();
        WriteFile(root / "usr/bin/lightdm", "");
        WriteFile(root / "usr/bin/sddm", "");

        auto result = AutologinConfigurator::DetectDisplayManager(root.string());
        Check(result.manager == AutologinConfigurator::DisplayManager::Ambiguous,
              "two installed display managers with no active one -> Ambiguous");
    }
    {
        reset();

        auto result = AutologinConfigurator::DetectDisplayManager(root.string());
        Check(result.manager == AutologinConfigurator::DisplayManager::None,
              "nothing at all found -> DisplayManager::None");
    }

    // --- CheckExistingAutologin() -------------------------------------------

    std::printf("\n-- Existing LightDM autologin in the main conf file --\n");
    {
        reset();
        WriteFile(root / "etc/lightdm/lightdm.conf",
            "[Seat:*]\n"
            "autologin-user=alice\n"
            "greeter-session=lightdm-gtk-greeter\n");

        auto check = AutologinConfigurator::CheckExistingAutologin(
            AutologinConfigurator::DisplayManager::LightDM, root.string());
        Check(check.found, "an active autologin-user= line under [Seat:*] is found");
    }

    std::printf("\n-- Existing LightDM autologin only in a conf.d/ drop-in --\n");
    {
        reset();
        WriteFile(root / "etc/lightdm/lightdm.conf", "[Seat:*]\ngreeter-session=lightdm-gtk-greeter\n");
        WriteFile(root / "etc/lightdm/lightdm.conf.d/50-existing.conf",
            "[Seat:0]\nautologin-user=bob\n");

        auto check = AutologinConfigurator::CheckExistingAutologin(
            AutologinConfigurator::DisplayManager::LightDM, root.string());
        Check(check.found, "found even though it's only in a drop-in file, under [Seat:0] not [Seat:*]");
    }

    std::printf("\n-- A commented-out autologin line doesn't count --\n");
    {
        reset();
        WriteFile(root / "etc/lightdm/lightdm.conf",
            "[Seat:*]\n#autologin-user=alice\n");

        auto check = AutologinConfigurator::CheckExistingAutologin(
            AutologinConfigurator::DisplayManager::LightDM, root.string());
        Check(!check.found, "a commented-out autologin-user= line is correctly ignored");
    }

    std::printf("\n-- An autologin-user= key in the wrong section doesn't count --\n");
    {
        reset();
        // Not realistic LightDM syntax, but tests that section-scoping
        // is actually enforced, not just "does this key ever appear
        // anywhere in the file".
        WriteFile(root / "etc/lightdm/lightdm.conf",
            "[SomeOtherSection]\nautologin-user=alice\n");

        auto check = AutologinConfigurator::CheckExistingAutologin(
            AutologinConfigurator::DisplayManager::LightDM, root.string());
        Check(!check.found, "a key outside any [Seat:...] section is correctly ignored");
    }

    std::printf("\n-- No existing LightDM autologin at all --\n");
    {
        reset();
        WriteFile(root / "etc/lightdm/lightdm.conf", "[Seat:*]\ngreeter-session=lightdm-gtk-greeter\n");

        auto check = AutologinConfigurator::CheckExistingAutologin(
            AutologinConfigurator::DisplayManager::LightDM, root.string());
        Check(!check.found, "a normal config with no autologin at all reports nothing found");
    }

    std::printf("\n-- Existing SDDM autologin --\n");
    {
        reset();
        WriteFile(root / "etc/sddm.conf", "[Autologin]\nUser=carol\nSession=plasma\n");

        auto check = AutologinConfigurator::CheckExistingAutologin(
            AutologinConfigurator::DisplayManager::Sddm, root.string());
        Check(check.found, "an active User= line under [Autologin] is found");
    }

    // --- Configure() ---------------------------------------------------------

    std::printf("\n-- Configure() succeeds cleanly for LightDM with nothing pre-existing --\n");
    {
        reset();
        WriteFile(root / "etc/lightdm/lightdm.conf", "[Seat:*]\n");

        auto result = AutologinConfigurator::Configure(
            AutologinConfigurator::DisplayManager::LightDM, "alice", root.string());

        Check(result.success, "Configure() reports success");

        fs::path expected = root / "etc/lightdm/lightdm.conf.d/60-kohiko-autologin.conf";
        Check(fs::exists(expected), "the expected drop-in file now exists");

        std::string content = ReadFile(expected);
        Check(content.find("autologin-user=alice") != std::string::npos,
              "the drop-in contains the correct username");
        Check(content.find("autologin-session=kohiko") != std::string::npos,
              "the drop-in pins the session to kohiko specifically");

        // The main lightdm.conf must be completely untouched.
        Check(ReadFile(root / "etc/lightdm/lightdm.conf") == "[Seat:*]\n",
              "the original lightdm.conf was never modified at all");
    }

    std::printf("\n-- Configure() refuses when an autologin config already exists --\n");
    {
        reset();
        WriteFile(root / "etc/lightdm/lightdm.conf", "[Seat:*]\nautologin-user=someone-else\n");

        auto result = AutologinConfigurator::Configure(
            AutologinConfigurator::DisplayManager::LightDM, "alice", root.string());

        Check(!result.success, "Configure() refuses outright");
        Check(!fs::exists(root / "etc/lightdm/lightdm.conf.d/60-kohiko-autologin.conf"),
              "...and creates no drop-in file at all");
    }

    std::printf("\n-- Configure() refuses when its own drop-in file already exists --\n");
    {
        reset();
        WriteFile(root / "etc/lightdm/lightdm.conf", "[Seat:*]\n");
        WriteFile(root / "etc/lightdm/lightdm.conf.d/60-kohiko-autologin.conf", "already here\n");

        auto result = AutologinConfigurator::Configure(
            AutologinConfigurator::DisplayManager::LightDM, "alice", root.string());

        Check(!result.success, "Configure() refuses rather than overwriting it");
        Check(ReadFile(root / "etc/lightdm/lightdm.conf.d/60-kohiko-autologin.conf") == "already here\n",
              "...and the existing file's content is completely untouched");
    }

    std::printf("\n-- Configure() is unsupported (docs-only) for GDM --\n");
    {
        reset();
        auto result = AutologinConfigurator::Configure(
            AutologinConfigurator::DisplayManager::Gdm, "alice", root.string());
        Check(!result.success, "Configure() always refuses for GDM, by design");
    }

    std::printf("\n-- Configure() succeeds cleanly for SDDM too --\n");
    {
        reset();

        auto result = AutologinConfigurator::Configure(
            AutologinConfigurator::DisplayManager::Sddm, "dave", root.string());

        Check(result.success, "Configure() reports success");

        fs::path expected = root / "etc/sddm.conf.d/60-kohiko-autologin.conf";
        std::string content = ReadFile(expected);
        Check(content.find("User=dave") != std::string::npos, "the drop-in contains the correct username");
        Check(content.find("Session=kohiko") != std::string::npos, "the drop-in pins the session to kohiko");
    }

    // --- Undo() ----------------------------------------------------------------

    std::printf("\n-- Undo() removes exactly the drop-in Configure() created --\n");
    {
        reset();
        AutologinConfigurator::Configure(
            AutologinConfigurator::DisplayManager::LightDM, "alice", root.string());

        fs::path dropIn = root / "etc/lightdm/lightdm.conf.d/60-kohiko-autologin.conf";
        Check(fs::exists(dropIn), "sanity check: the drop-in exists before Undo()");

        bool removed = AutologinConfigurator::Undo(
            AutologinConfigurator::DisplayManager::LightDM, root.string());

        Check(removed, "Undo() reports it removed something");
        Check(!fs::exists(dropIn), "...and the drop-in file is actually gone");
    }

    std::printf("\n-- Undo() is a harmless no-op when there's nothing to undo --\n");
    {
        reset();
        bool removed = AutologinConfigurator::Undo(
            AutologinConfigurator::DisplayManager::LightDM, root.string());
        Check(!removed, "Undo() correctly reports nothing was removed");
    }

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    fs::remove_all(root);
    return 0;
}
