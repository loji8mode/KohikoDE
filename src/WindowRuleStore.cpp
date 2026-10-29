#include "WindowRuleStore.h"

#include "ConfigWriter.h"
#include "Utils.h"

namespace Kohiko
{

namespace
{

// True (and fills `out`) when `line` is the app rule for `appKey` in
// exactly this scope.
bool IsAppRuleFor(
    const std::string& line,
    const std::string& appKey,
    const RuleScope& scope,
    WindowRule& out)
{
    WindowRule rule;

    if (!WindowRule::Parse(line, rule))
        return false;

    if (!rule.IsScopedAppRule() ||
        Utils::Lower(rule.appKey) != Utils::Lower(appKey) ||
        !(rule.Scope() == scope))
        return false;

    out = rule;
    return true;
}

}

std::optional<WindowRule> WindowRuleStore::FindAppRule(
    const std::vector<std::string>& lines,
    const std::string& appKey,
    const RuleScope& scope)
{
    std::optional<WindowRule> found;

    for (const std::string& line : lines)
    {
        WindowRule rule;

        // Last one wins, same as the resolver would treat equally
        // specific rules - so what the UI shows is what applies.
        if (IsAppRuleFor(line, appKey, scope, rule))
            found = rule;
    }

    return found;
}

std::vector<std::string> WindowRuleStore::UpsertAppRule(
    std::vector<std::string> lines,
    const WindowRule& rule)
{
    if (rule.appKey.empty())
        return lines;

    WindowRule scratch;
    std::vector<std::string> result;
    bool placed = false;

    // The rule exactly as it should be stored: the app selector and its
    // scope (role/type), nothing else.
    WindowRule stored;
    stored.appKey = rule.appKey;
    stored.role = rule.role;
    stored.windowType = rule.windowType;
    stored.workspace = rule.workspace;
    stored.horizontal = rule.horizontal;
    stored.vertical = rule.vertical;
    stored.mode = rule.mode;
    stored.denyFullscreen = rule.denyFullscreen;

    for (const std::string& line : lines)
    {
        if (!IsAppRuleFor(line, rule.appKey, rule.Scope(), scratch))
        {
            result.push_back(line);
            continue;
        }

        if (!placed && stored.HasProperty())
        {
            result.push_back(stored.Serialize()); // in place - keeps its rank among equals
            placed = true;
        }
        // else: a duplicate (or the whole rule is being reset) - dropped.
    }

    if (!placed && stored.HasProperty())
        result.push_back(stored.Serialize());

    return result;
}

std::vector<std::string> WindowRuleStore::RemoveAppRule(
    std::vector<std::string> lines,
    const std::string& appKey,
    const RuleScope& scope)
{
    WindowRule none;
    none.appKey = appKey; // no properties => UpsertAppRule removes
    none.role = scope.role;
    none.windowType = scope.windowType;
    return UpsertAppRule(std::move(lines), none);
}

std::vector<std::string> WindowRuleStore::OtherMatchingRules(
    const std::vector<std::string>& lines,
    const std::string& className,
    const std::string& instanceName,
    const std::string& title)
{
    WindowIdentity window;
    window.className = className;
    window.instanceName = instanceName;
    window.title = title;

    return OtherMatchingRules(lines, window, RuleScope());
}

std::vector<std::string> WindowRuleStore::OtherMatchingRules(
    const std::vector<std::string>& lines,
    const WindowIdentity& window,
    const RuleScope& ownScope)
{
    std::vector<std::string> others;
    std::string ownKey = WindowRule::AppKeyFor(window.className, window.instanceName);

    for (const std::string& line : lines)
    {
        WindowRule rule;

        if (!WindowRule::Parse(line, rule) || !rule.Matches(window))
            continue;

        if (rule.IsScopedAppRule() &&
            Utils::Lower(rule.appKey) == Utils::Lower(ownKey) &&
            rule.Scope() == ownScope)
            continue;

        others.push_back(line);
    }

    return others;
}

bool WindowRuleStore::LoadLines(
    const std::string& configPath,
    std::vector<std::string>& lines)
{
    ConfigWriter writer;

    if (!writer.Load(configPath))
        return false;

    lines = writer.GetRawBlock(kPrefix);
    return true;
}

bool WindowRuleStore::SaveLines(
    const std::string& configPath,
    const std::vector<std::string>& lines)
{
    ConfigWriter writer;

    if (!writer.Load(configPath))
        return false;

    // Nothing to write and nothing already there: leave the file
    // byte-for-byte alone rather than adding an empty marker comment.
    if (lines.empty() && writer.GetRawBlock(kPrefix).empty())
        return true;

    writer.ReplaceRawBlock(kPrefix, lines);
    return writer.Save();
}

}
