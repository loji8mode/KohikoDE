#include "ConfigMigration.h"

#include "ConfigSchema.h"
#include "ConfigWriter.h"

#include <filesystem>
#include <system_error>

namespace Kohiko
{

namespace
{

// Distinct from ConfigWriter::kDefaultMarker (Kohiko Settings' own
// section) on purpose - see ConfigMigration.h's header comment.
const char* const kMigrationMarker =
    "# --- Added automatically by Kohiko (config migration - see CHANGELOG.md) ---";

}

bool ConfigMigration::BackupConfig(
    const std::string& configPath)
{
    std::error_code error;

    if (!std::filesystem::is_regular_file(configPath, error))
        return false; // nothing to back up - not an error, just a no-op

    std::filesystem::copy_file(
        configPath, configPath + ".bak",
        std::filesystem::copy_options::overwrite_existing, error);

    return !error;
}

const std::vector<std::string>& ConfigMigration::RetiredKeys()
{
    static const std::vector<std::string> kRetired = {
        "general.adaptive_placement",
    };

    return kRetired;
}

std::vector<std::string> ConfigMigration::RetireObsoleteData(
    const std::filesystem::path& dataDir)
{
    static const char* const kObsoleteFiles[] = {
        "placement_habits",
    };

    std::vector<std::string> removed;

    for (const char* name : kObsoleteFiles)
    {
        std::error_code error;
        std::filesystem::path path = dataDir / name;

        if (std::filesystem::is_regular_file(path, error) &&
            std::filesystem::remove(path, error) && !error)
        {
            removed.push_back(name);
        }
    }

    return removed;
}

bool ConfigMigration::MigrateIfNeeded(
    const std::string& configPath,
    std::vector<std::string>* retiredKeys)
{
    ConfigWriter writer;

    if (!writer.Load(configPath))
        return false; // no file (or unreadable) - see this class's header comment

    bool changed = false;

    for (const std::string& key : RetiredKeys())
    {
        if (!writer.Contains(key))
            continue;

        if (!changed)
        {
            BackupConfig(configPath);
            changed = true;
        }

        writer.RemoveScalar(key);

        if (retiredKeys)
            retiredKeys->push_back(key);
    }

    for (const ConfigOption& option : ConfigSchema::All())
    {
        // Contains(), not GetScalar().empty() - a key the user
        // deliberately left blank (several schema defaults are
        // themselves "", e.g. lockscreen.background_image) must never
        // be touched, and only Contains() tells "never set" apart
        // from "set to empty on purpose". See ConfigWriter.h.
        if (writer.Contains(option.key))
            continue;

        if (!changed)
        {
            // Only touch the file at all - starting with this backup -
            // once there's actually something to migrate.
            BackupConfig(configPath);
            changed = true;
        }

        writer.SetScalar(option.key, option.defaultValue, kMigrationMarker);
    }

    return changed && writer.Save();
}

}
