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

bool ConfigMigration::MigrateIfNeeded(
    const std::string& configPath)
{
    ConfigWriter writer;

    if (!writer.Load(configPath))
        return false; // no file (or unreadable) - see this class's header comment

    bool changed = false;

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
