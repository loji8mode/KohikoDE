#include "AutostartPlan.h"

#include "Utils.h"

#include <set>

namespace Kohiko
{

std::string ProgramIdentity(const std::string& command)
{
    for (const std::string& word : Utils::SplitWhitespace(command))
    {
        if (word == "env")
            continue;

        // VAR=value assignments in front of the real command.
        std::size_t eq = word.find('=');
        if (eq != std::string::npos && eq > 0 && word.find('/') > eq)
            continue;

        std::size_t slash = word.find_last_of('/');
        std::string base = (slash == std::string::npos) ? word : word.substr(slash + 1);

        return Utils::Lower(base);
    }

    return std::string();
}

AutostartPlan PlanAutostart(const AutostartInputs& inputs)
{
    AutostartPlan plan;

    // Identities claimed by each more-explicit mechanism.
    std::set<std::string> claimedByWorkspace;
    std::set<std::string> claimedByPrograms;

    for (const auto& [workspace, program] : inputs.workspacePrograms)
    {
        std::string id = ProgramIdentity(program);

        if (!id.empty())
            claimedByWorkspace.insert(id);
    }

    // auto_start_programs: dropped only if a workspace line launches
    // the same program. Duplicates WITHIN the list are all kept.
    for (const std::string& program : inputs.programs)
    {
        AutostartItem item;
        item.source = AutostartItem::Source::ProgramList;
        item.command = program;

        std::string id = ProgramIdentity(program);

        if (!id.empty() && claimedByWorkspace.count(id))
        {
            plan.skipped.push_back({ item,
                "also listed under a workspace<N>= line, which launches it" });
            continue;
        }

        if (!id.empty())
            claimedByPrograms.insert(id);

        plan.launch.push_back(item);
    }

    // XDG entries: dropped if either explicit mechanism launches the
    // same program - the user's own configuration is what they asked
    // for; an entry some package dropped into ~/.config/autostart
    // isn't.
    for (const auto& [desktopId, exec] : inputs.xdgEntries)
    {
        AutostartItem item;
        item.source = AutostartItem::Source::Xdg;
        item.command = exec;
        item.desktopId = desktopId;

        std::string id = ProgramIdentity(exec);

        if (!id.empty() && claimedByWorkspace.count(id))
        {
            plan.skipped.push_back({ item,
                "also listed under a workspace<N>= line, which launches it" });
            continue;
        }

        if (!id.empty() && claimedByPrograms.count(id))
        {
            plan.skipped.push_back({ item,
                "also listed in auto_start_programs, which launches it" });
            continue;
        }

        plan.launch.push_back(item);
    }

    for (const auto& [workspace, program] : inputs.workspacePrograms)
    {
        AutostartItem item;
        item.source = AutostartItem::Source::WorkspaceLine;
        item.command = program;
        item.workspace = workspace;
        plan.launch.push_back(item);
    }

    return plan;
}

}
