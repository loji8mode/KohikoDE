// Standalone correctness test for ConfigMigration - no X11 needed.
// Build & run: see the "test-configmigration" target in the Makefile.

#include "ConfigMigration.h"
#include "ConfigSchema.h"
#include "ConfigWriter.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <set>
#include <sstream>
#include <string>
#include <vector>

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

bool SchemaHas(const std::string& key)
{
    for (const ConfigOption& option : ConfigSchema::All())
    {
        if (option.key == key)
            return true;
    }

    return false;
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

    std::printf("\n-- Retired settings (general.adaptive_placement, removed in 0.22.0) --\n");
    {
        Check(!SchemaHas("general.adaptive_placement"),
              "the schema no longer knows general.adaptive_placement (so Settings never offers it, and migration never re-adds it)");

        for (const std::string& retired : ConfigMigration::RetiredKeys())
            Check(!SchemaHas(retired), ("retired key is really absent from the schema: " + retired).c_str());

        const std::string original =
            "# my config\n"
            "general.gap=6\n"
            "general.adaptive_placement=true\n"
            "bind=SUPER+Q,close\n"
            "windowrule=tile class:tlauncher\n";
        WriteFile(configPath, original);

        std::vector<std::string> retired;
        bool changed = ConfigMigration::MigrateIfNeeded(configPath.string(), &retired);

        std::string after = ReadFile(configPath);
        Check(changed, "a config still carrying the retired key is reported as changed");
        Check(retired.size() == 1 && retired[0] == "general.adaptive_placement", "...and the removed key is reported (for the log line)");
        Check(after.find("adaptive_placement") == std::string::npos, "the line is gone from the file");
        Check(after.find("general.gap=6") != std::string::npos &&
              after.find("bind=SUPER+Q,close") != std::string::npos &&
              after.find("windowrule=tile class:tlauncher") != std::string::npos &&
              after.find("# my config") != std::string::npos,
              "...and everything else the user wrote (comment, setting, bind, rule) is kept");
        Check(ReadFile(configPath.string() + ".bak") == original, "the previous contents were saved to .bak first");

        // Removal must never invent a rule from old data.
        Check(after.find("app:") == std::string::npos, "migration did not create any app rule");

        // Idempotent.
        std::vector<std::string> again;
        ConfigMigration::MigrateIfNeeded(configPath.string(), &again);
        Check(again.empty(), "a second run has nothing left to retire");

        // A commented-out occurrence is the user's own text and stays.
        WriteFile(configPath, "#general.adaptive_placement=false\n");
        std::vector<std::string> none;
        ConfigMigration::MigrateIfNeeded(configPath.string(), &none);
        Check(none.empty() && ReadFile(configPath).find("#general.adaptive_placement=false") != std::string::npos,
              "a commented-out mention is left exactly as the user wrote it");
    }

    std::printf("\n-- Obsolete data files (placement_habits) --\n");
    {
        std::filesystem::path dataDir = tempDir / "data";
        std::filesystem::create_directories(dataDir);

        WriteFile(dataDir / "placement_habits", "firefox 0 0 0 0 1 3 5\n");
        WriteFile(dataDir / "session", "unrelated session data\n");
        WriteFile(dataDir / "history", "unrelated launcher history\n");

        std::vector<std::string> removed = ConfigMigration::RetireObsoleteData(dataDir);

        Check(removed.size() == 1 && removed[0] == "placement_habits", "the obsolete habits file is removed and reported");
        Check(!std::filesystem::exists(dataDir / "placement_habits"), "...and is really gone from disk");
        Check(std::filesystem::exists(dataDir / "session") && std::filesystem::exists(dataDir / "history"),
              "...while unrelated data files are never touched");

        // The old votes must not have turned into a rule anywhere.
        std::filesystem::path conf = tempDir / "kohiko2.conf";
        WriteFile(conf, "general.gap=6\n");
        ConfigMigration::MigrateIfNeeded(conf.string());
        Check(ReadFile(conf).find("windowrule") == std::string::npos,
              "old adaptive data is never converted into explicit window rules");

        Check(ConfigMigration::RetireObsoleteData(dataDir).empty(), "running it again is a no-op");
        Check(ConfigMigration::RetireObsoleteData(tempDir / "does-not-exist").empty(), "a missing data directory is fine");
    }

    std::printf("\n-- Shipped defaults must not conflict with each other (autostart) --\n");
    {
        // The real reproducer for duplicate launches: ConfigMigration
        // appends a missing key's SCHEMA default to an existing config,
        // so if that default names programs that the shipped config
        // already launches from workspace<N>= lines, every migrated
        // config launched them twice. Compared against the actual
        // shipped config/default.conf, so the two can't drift apart again.
        std::string defaultsPath = "config/default.conf";
        std::ifstream shipped(defaultsPath);

        if (!shipped)
            std::printf("  (skipped: %s not found - run from the repository root)\n", defaultsPath.c_str());
        else
        {
            std::set<std::string> workspaceLaunched;
            std::string line;

            while (std::getline(shipped, line))
            {
                if (line.rfind("workspace", 0) != 0 || line.find('=') == std::string::npos)
                    continue;

                std::string key = line.substr(0, line.find('='));
                if (key.size() < 10 || !std::isdigit(static_cast<unsigned char>(key[9])))
                    continue;

                std::istringstream programs(line.substr(line.find('=') + 1));
                std::string program;
                while (programs >> program)
                    workspaceLaunched.insert(program);
            }

            std::string schemaDefault;
            for (const ConfigOption& option : ConfigSchema::All())
                if (option.key == "auto_start_programs")
                    schemaDefault = option.defaultValue;

            std::istringstream defaults(schemaDefault);
            std::string program;
            std::string overlap;
            while (defaults >> program)
                if (workspaceLaunched.count(program))
                    overlap += program + " ";

            Check(!workspaceLaunched.empty(), "(sanity) the shipped config does launch programs from workspace<N>= lines");
            Check(overlap.empty(),
                  ("auto_start_programs' schema default shares no program with the shipped workspace<N>= lines (overlap: [" + overlap + "])").c_str());
        }
    }

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    std::filesystem::remove_all(tempDir);
    return 0;
}
