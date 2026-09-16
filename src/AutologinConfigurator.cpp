#include "AutologinConfigurator.h"

#include "Utils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace Kohiko
{

namespace
{

namespace fs = std::filesystem;

// "/" -> "" so `NormalizeRoot(root) + "/etc/..."` never produces a
// leading "//" for the common (real, non-test) case - purely
// cosmetic (POSIX treats "//etc" the same as "/etc" in practice), but
// keeps every path this class reports to the user looking normal.
std::string NormalizeRoot(const std::string& root)
{
    if (root.empty() || root == "/")
        return "";

    if (root.back() == '/')
        return root.substr(0, root.size() - 1);

    return root;
}

// Scans `path` (an INI-style file - lines are `key=value`, `#`/`;`
// start a comment, `[Section]` changes the current section) for an
// *active* `key=<non-empty value>` line. If `requiredSectionPrefix` is
// non-empty, only a line inside a section whose name starts with
// `requiredSection` counts (so "Seat:" matches both `[Seat:*]` and
// `[Seat:0]` for LightDM; SDDM's "Autologin" only ever matches itself
// exactly, which starts-with still handles correctly). Appends
// "path:lineNo (key=value)" to `outLocation` and returns true on the
// first match found.
bool FileHasActiveKey(
    const std::string& path,
    const std::string& key,
    const std::string& requiredSectionPrefix,
    std::string& outLocation)
{
    std::ifstream file(path);

    if (!file.is_open())
        return false;

    std::string line;
    std::string currentSection;
    int lineNo = 0;

    while (std::getline(file, line))
    {
        ++lineNo;

        std::string trimmed = Utils::Trim(line);

        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';')
            continue;

        if (trimmed.front() == '[' && trimmed.back() == ']')
        {
            currentSection = trimmed.substr(1, trimmed.size() - 2);
            continue;
        }

        if (!requiredSectionPrefix.empty() &&
            currentSection.compare(0, requiredSectionPrefix.size(), requiredSectionPrefix) != 0)
            continue;

        std::size_t eq = trimmed.find('=');

        if (eq == std::string::npos)
            continue;

        std::string foundKey = Utils::Trim(trimmed.substr(0, eq));
        std::string foundValue = Utils::Trim(trimmed.substr(eq + 1));

        if (foundKey == key && !foundValue.empty())
        {
            outLocation = path + ":" + std::to_string(lineNo) + " (" + foundKey + "=" + foundValue + ")";
            return true;
        }
    }

    return false;
}

// Every `*.conf` file directly inside `dir`, sorted - empty (not an
// error) if `dir` doesn't exist, which is the common case on any
// system that doesn't use drop-in config directories at all.
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

}

AutologinConfigurator::DetectionResult AutologinConfigurator::DetectDisplayManager(
    const std::string& root)
{
    std::string base = NormalizeRoot(root);
    std::string symlinkPath = base + "/etc/systemd/system/display-manager.service";

    std::error_code error;

    if (fs::is_symlink(symlinkPath, error))
    {
        fs::path target = fs::read_symlink(symlinkPath, error);

        if (!error)
        {
            std::string serviceName = target.filename().string();
            const std::string suffix = ".service";

            if (serviceName.size() > suffix.size() &&
                serviceName.compare(serviceName.size() - suffix.size(), suffix.size(), suffix) == 0)
                serviceName = serviceName.substr(0, serviceName.size() - suffix.size());

            DisplayManager manager = DisplayManager::Unknown;

            if (serviceName == "lightdm")
                manager = DisplayManager::LightDM;
            else if (serviceName == "sddm")
                manager = DisplayManager::Sddm;
            else if (serviceName == "gdm" || serviceName == "gdm3")
                manager = DisplayManager::Gdm;

            std::string detail = "systemd's display-manager.service points to " + serviceName;

            if (manager == DisplayManager::Unknown)
                detail += " - not a display manager this supports automatic configuration for";

            return { manager, detail };
        }
    }

    // No systemd symlink (a non-systemd init, or nothing enabled that
    // way) - fall back to checking for known binaries.
    struct Candidate { const char* path; DisplayManager manager; const char* name; };

    static const Candidate kCandidates[] = {
        { "/usr/bin/lightdm", DisplayManager::LightDM, "LightDM" },
        { "/usr/sbin/lightdm", DisplayManager::LightDM, "LightDM" },
        { "/usr/bin/sddm", DisplayManager::Sddm, "SDDM" },
        { "/usr/sbin/sddm", DisplayManager::Sddm, "SDDM" },
        { "/usr/bin/gdm", DisplayManager::Gdm, "GDM" },
        { "/usr/bin/gdm3", DisplayManager::Gdm, "GDM" },
        { "/usr/sbin/gdm", DisplayManager::Gdm, "GDM" },
        { "/usr/sbin/gdm3", DisplayManager::Gdm, "GDM" },
    };

    std::vector<DisplayManager> found;
    std::vector<std::string> foundNames;

    for (const Candidate& candidate : kCandidates)
    {
        if (fs::exists(base + candidate.path, error) &&
            std::find(found.begin(), found.end(), candidate.manager) == found.end())
        {
            found.push_back(candidate.manager);
            foundNames.push_back(candidate.name);
        }
    }

    if (found.empty())
    {
        return {
            DisplayManager::None,
            "no supported display manager binary found, and no systemd "
            "display-manager.service is configured"
        };
    }

    if (found.size() > 1)
    {
        std::string names;

        for (std::size_t i = 0; i < foundNames.size(); ++i)
        {
            if (i > 0)
                names += ", ";

            names += foundNames[i];
        }

        return {
            DisplayManager::Ambiguous,
            "multiple display managers appear installed (" + names +
            ") and none is set as the active one via systemd - can't tell which to configure"
        };
    }

    return {
        found[0],
        std::string(foundNames[0]) +
        " binary found (no systemd display-manager.service symlink to confirm it's the active one)"
    };
}

AutologinConfigurator::ExistingAutologinCheck AutologinConfigurator::CheckExistingAutologin(
    DisplayManager manager,
    const std::string& root)
{
    std::string base = NormalizeRoot(root);
    std::string location;

    if (manager == DisplayManager::LightDM)
    {
        std::vector<std::string> files = { base + "/etc/lightdm/lightdm.conf" };
        std::vector<std::string> dropIns = ConfDFiles(base + "/etc/lightdm/lightdm.conf.d");
        files.insert(files.end(), dropIns.begin(), dropIns.end());

        for (const std::string& file : files)
        {
            if (FileHasActiveKey(file, "autologin-user", "Seat:", location))
                return { true, location };
        }
    }
    else if (manager == DisplayManager::Sddm)
    {
        std::vector<std::string> files = { base + "/etc/sddm.conf" };
        std::vector<std::string> dropIns = ConfDFiles(base + "/etc/sddm.conf.d");
        files.insert(files.end(), dropIns.begin(), dropIns.end());

        for (const std::string& file : files)
        {
            if (FileHasActiveKey(file, "User", "Autologin", location))
                return { true, location };
        }
    }

    return { false, "" };
}

std::string AutologinConfigurator::DropInPath(
    DisplayManager manager,
    const std::string& root)
{
    std::string base = NormalizeRoot(root);

    if (manager == DisplayManager::LightDM)
        return base + "/etc/lightdm/lightdm.conf.d/60-kohiko-autologin.conf";

    if (manager == DisplayManager::Sddm)
        return base + "/etc/sddm.conf.d/60-kohiko-autologin.conf";

    return "";
}

std::string AutologinConfigurator::PreviewContent(
    DisplayManager manager,
    const std::string& username)
{
    if (manager == DisplayManager::LightDM)
    {
        return
            "[Seat:*]\n"
            "autologin-user=" + username + "\n"
            "autologin-user-timeout=0\n"
            "autologin-session=kohiko\n";
    }

    if (manager == DisplayManager::Sddm)
    {
        return
            "[Autologin]\n"
            "User=" + username + "\n"
            "Session=kohiko\n";
    }

    return "";
}

AutologinConfigurator::ConfigureResult AutologinConfigurator::Configure(
    DisplayManager manager,
    const std::string& username,
    const std::string& root)
{
    if (manager != DisplayManager::LightDM && manager != DisplayManager::Sddm)
    {
        return {
            false,
            "automatic configuration isn't supported for this display manager - "
            "see the README's autologin section for how to set it up by hand instead"
        };
    }

    if (username.empty())
        return { false, "no username given" };

    ExistingAutologinCheck existing = CheckExistingAutologin(manager, root);

    if (existing.found)
    {
        return {
            false,
            "an existing autologin configuration was found at " + existing.location +
            " - refusing to overwrite it. Remove or edit it by hand first if you're sure."
        };
    }

    std::string dropInPath = DropInPath(manager, root);

    if (dropInPath.empty())
        return { false, "internal error: no drop-in path known for this display manager" };

    std::error_code error;

    if (fs::exists(dropInPath, error))
    {
        return {
            false,
            dropInPath + " already exists - refusing to overwrite it. "
            "Remove it by hand first if you're sure, or use --undo."
        };
    }

    fs::path dropInDir = fs::path(dropInPath).parent_path();
    fs::create_directories(dropInDir, error);

    if (error)
        return { false, "could not create " + dropInDir.string() + ": " + error.message() };

    std::string content = PreviewContent(manager, username);

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

        out << content;
    }

    // Validate: re-read exactly what was written, rather than trusting
    // that the write syscalls above actually landed correctly - see
    // this class's own header comment on Configure().
    std::ifstream verify(dropInPath);
    std::string actual(
        (std::istreambuf_iterator<char>(verify)),
        std::istreambuf_iterator<char>());

    if (actual != content)
    {
        fs::remove(dropInPath, error); // never leave a partial/corrupt file behind
        return {
            false,
            "wrote " + dropInPath + " but its content didn't verify correctly afterward - "
            "removed it again; no changes were made. This may indicate a disk or "
            "permissions problem."
        };
    }

    return {
        true,
        "created " + dropInPath + " - " + username +
        " will be logged in automatically into a Kohiko session on next boot. "
        "Run `kohikoctl configure-autologin --undo` to remove this."
    };
}

bool AutologinConfigurator::Undo(
    DisplayManager manager,
    const std::string& root)
{
    std::string path = DropInPath(manager, root);

    if (path.empty())
        return false;

    std::error_code error;

    if (!fs::exists(path, error))
        return false;

    fs::remove(path, error);

    return !error;
}

}
