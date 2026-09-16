// Standalone correctness test for ConfigMigration - no X11 needed.
// Build & run: see the "test-configmigration" target in the Makefile.

#include "ConfigMigration.h"
#include "ConfigWriter.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace Kohiko;

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

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream file(path);
    std::string content(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
    return content;
}

void WriteFile(const std::filesystem::path& path, const std::string& body)
{
    std::ofstream file(path, std::ios::trunc);
    file << body;
}

}

int main()
{
    std::filesystem::path tempDir =
        std::filesystem::temp_directory_path() / "kohiko-test-configmigration";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directories(tempDir);

    std::filesystem::path configPath = tempDir / "kohiko.conf";

    std::printf("-- No config file at all --\n");
    {
        std::filesystem::remove(configPath);
        bool changed = ConfigMigration::MigrateIfNeeded(configPath.string());
        Check(!changed, "MigrateIfNeeded() is a no-op with nothing to migrate");
        Check(!std::filesystem::exists(configPath),
              "...and never creates a config file that didn't exist");
    }

    std::printf("\n-- A config missing several schema keys --\n");
    {
        std::string original =
            "# my config\n"
            "general.focus_follows_mouse=false\n"
            "\n"
            "bind=SUPER,Return,exec,kitty\n";
        WriteFile(configPath, original);

        bool changed = ConfigMigration::MigrateIfNeeded(configPath.string());
        Check(changed, "reports the file was modified");

        ConfigWriter after;
        after.Load(configPath.string());

        Check(after.GetScalar("general.focus_follows_mouse") == "false",
              "the user's existing value is completely untouched");
        Check(after.Contains("general.min_tile_width"),
              "a genuinely-missing schema key got appended");
        Check(after.GetScalar("general.min_tile_width") == "100",
              "...with exactly the schema's documented default");
        Check(after.GetRawBlock("bind=").size() == 1 &&
              after.GetRawBlock("bind=")[0] == "SUPER,Return,exec,kitty",
              "an unrelated repeatable directive survives untouched");

        Check(std::filesystem::exists(configPath.string() + ".bak"),
              "a backup was created before any change");
        Check(ReadFile(configPath.string() + ".bak") == original,
              "...and the backup is byte-for-byte the pre-migration file");
    }

    std::printf("\n-- A key deliberately set to an empty value is never \"fixed\" --\n");
    {
        // general.font's schema default is non-empty
        // ("monospace:pixelsize=14") - a user who explicitly blanked
        // it out is opting out of that default on purpose, and
        // migration must never silently restore it.
        WriteFile(configPath, "general.font=\n");

        ConfigMigration::MigrateIfNeeded(configPath.string());

        ConfigWriter after;
        after.Load(configPath.string());

        Check(after.Contains("general.font"), "the key is still present");
        Check(after.GetScalar("general.font").empty(),
              "...and its deliberately-blank value was never overwritten "
              "with the non-empty schema default");
    }

    std::printf("\n-- Nothing missing: file is left completely untouched --\n");
    {
        // Migrate an empty-ish file once to get a fully-populated
        // baseline, then migrate *that* result again and confirm
        // nothing changes the second time.
        WriteFile(configPath, "# baseline\n");
        ConfigMigration::MigrateIfNeeded(configPath.string());

        std::string beforeSecondPass = ReadFile(configPath);
        auto mtimeBefore = std::filesystem::last_write_time(configPath);

        bool changedAgain = ConfigMigration::MigrateIfNeeded(configPath.string());

        Check(!changedAgain, "a second run over an already-migrated file reports no change");
        Check(ReadFile(configPath) == beforeSecondPass,
              "...and the file's content is byte-for-byte identical");
        Check(std::filesystem::last_write_time(configPath) == mtimeBefore,
              "...and its mtime wasn't touched (no unnecessary write)");
    }

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    std::filesystem::remove_all(tempDir);
    return 0;
}
