#include "StartupPlan.h"

#include <algorithm>
#include <map>
#include <sstream>

namespace Kohiko
{

namespace
{

std::vector<std::string> Words(const std::string& command)
{
    std::vector<std::string> words;
    std::istringstream in(command);
    std::string word;

    while (in >> word)
        words.push_back(word);

    return words;
}

std::string Basename(const std::string& path)
{
    std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}

const char* StartupSourceName(StartupSource source)
{
    switch (source)
    {
        case StartupSource::WorkspaceLine: return "workspace<N>";
        case StartupSource::AutoStartList: return "auto_start_programs";
        case StartupSource::XdgEntry:      return "XDG autostart";
    }

    return "?";
}

std::string StartupProgramKey(const std::string& command)
{
    std::vector<std::string> words = Words(command);

    if (words.empty())
        return std::string();

    std::string key = Basename(words[0]);

    for (std::size_t i = 1; i < words.size(); ++i)
        key += ' ' + words[i];

    return key;
}

StartupPlan BuildStartupPlan(const StartupPlanInput& input)
{
    // The three sources as flat, ordered entry lists.
    std::vector<StartupEntry> workspaceEntries;

    for (std::size_t i = 0; i < input.workspacePrograms.size(); ++i)
    {
        for (const std::string& command : input.workspacePrograms[i])
            workspaceEntries.push_back({command, static_cast<int>(i) + 1, StartupSource::WorkspaceLine});
    }

    std::vector<StartupEntry> autoEntries;
    for (const std::string& command : input.autoStartPrograms)
        autoEntries.push_back({command, 0, StartupSource::AutoStartList});

    std::vector<StartupEntry> xdgEntries;
    for (const std::string& command : input.xdgCommands)
        xdgEntries.push_back({command, 0, StartupSource::XdgEntry});

    StartupPlan plan;

    // key -> how many entries every HIGHER-priority source declared,
    // and which source declared the first of them (for reporting).
    std::map<std::string, int> declared;
    std::map<std::string, StartupSource> firstDeclaredBy;

    std::vector<StartupEntry> keptWorkspace;
    std::vector<StartupEntry> keptAuto;
    std::vector<StartupEntry> keptXdg;

    auto process = [&](const std::vector<StartupEntry>& source, std::vector<StartupEntry>& kept)
    {
        std::map<std::string, int> seenInThisSource;

        for (const StartupEntry& entry : source)
        {
            std::string key = StartupProgramKey(entry.command);

            if (key.empty())
                continue; // blank command: nothing to launch

            int claimed = declared.count(key) ? declared[key] : 0;
            int nth = ++seenInThisSource[key];

            if (nth <= claimed)
                plan.skipped.push_back({entry, firstDeclaredBy[key]});
            else
                kept.push_back(entry);
        }

        // Only now do this source's entries become claims on the
        // sources below it - never on siblings within itself.
        for (const auto& [key, count] : seenInThisSource)
        {
            if (!declared.count(key))
                firstDeclaredBy[key] = source.front().source;

            declared[key] += count;
        }
    };

    process(workspaceEntries, keptWorkspace);
    process(autoEntries, keptAuto);
    process(xdgEntries, keptXdg);

    // Historical launch order.
    plan.launches.insert(plan.launches.end(), keptAuto.begin(), keptAuto.end());
    plan.launches.insert(plan.launches.end(), keptXdg.begin(), keptXdg.end());
    plan.launches.insert(plan.launches.end(), keptWorkspace.begin(), keptWorkspace.end());

    return plan;
}

}
