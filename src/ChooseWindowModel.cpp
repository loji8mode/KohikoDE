#include "ChooseWindowModel.h"

#include "Utils.h"
#include "WindowRuleStore.h"

#include <algorithm>

namespace Kohiko
{

ChooseWindowModel::ChooseWindowModel(
    int workspaceCount,
    const std::optional<WindowRule>& saved)
{
    ScopeChoice only;
    only.label = "All windows";
    only.scope = saved ? saved->Scope() : RuleScope();
    only.saved = saved;

    m_scopes.push_back(only);
    Init(workspaceCount, 0);
}

ChooseWindowModel::ChooseWindowModel(
    int workspaceCount,
    std::vector<ScopeChoice> scopes,
    int initialScope)
    : m_scopes(std::move(scopes))
{
    if (m_scopes.empty())
    {
        ScopeChoice all;
        all.label = "All windows";
        m_scopes.push_back(all);
    }

    Init(workspaceCount, initialScope);
}

void ChooseWindowModel::Init(int workspaceCount, int initialScope)
{
    m_workspaceCount = std::max(1, workspaceCount);
    m_scope = (initialScope >= 0 && initialScope < static_cast<int>(m_scopes.size())) ? initialScope : 0;

    m_choices.assign(m_scopes.size(), Choices{ 0, 0, 0, 0 });

    for (std::size_t i = 0; i < m_scopes.size(); ++i)
    {
        const std::optional<WindowRule>& saved = m_scopes[i].saved;

        if (!saved)
            continue;

        // A rule saved when there were more workspaces than there are
        // now must still show (and keep) its workspace rather than
        // snapping to Default the moment the dialog opens.
        m_workspaceCount = std::max(m_workspaceCount, saved->workspace);

        m_choices[i][RowIndex(Row::Workspace)]  = saved->workspace;
        m_choices[i][RowIndex(Row::Horizontal)] = static_cast<int>(saved->horizontal);
        m_choices[i][RowIndex(Row::Vertical)]   = static_cast<int>(saved->vertical);
        m_choices[i][RowIndex(Row::Mode)]       = static_cast<int>(saved->mode);
    }

    // With a kind to choose, the first question the dialog asks is
    // "which windows is this for" - so that is where it starts, instead
    // of silently defaulting to one answer. (Digits still jump straight
    // to the Workspace row, and Enter still saves from anywhere.)
    m_row = HasScopeRow() ? Row::Scope : Row::Workspace;
}

ChooseWindowModel::ScopeChoices ChooseWindowModel::BuildScopeChoices(
    const std::vector<std::string>& lines,
    const std::string& appKey,
    const std::string& role,
    const std::string& windowType,
    const std::optional<RuleScope>& requested)
{
    ScopeChoices result;

    auto add = [&](const RuleScope& scope)
    {
        ScopeChoice choice;
        choice.scope = scope;
        choice.label = scope.Describe();
        choice.saved = WindowRuleStore::FindAppRule(lines, appKey, scope);
        result.choices.push_back(choice);
    };

    add(RuleScope());

    if (!role.empty())
    {
        RuleScope scope;
        scope.role = role;
        add(scope);
    }

    if (!windowType.empty())
    {
        RuleScope scope;
        scope.windowType = windowType;
        add(scope);
    }

    if (!role.empty() && !windowType.empty())
    {
        RuleScope scope;
        scope.role = role;
        scope.windowType = windowType;
        add(scope);
    }

    auto specificity = [](const RuleScope& scope)
    {
        return static_cast<int>(!scope.role.empty()) + static_cast<int>(!scope.windowType.empty());
    };

    result.defaultIndex = 0;

    if (requested)
    {
        for (std::size_t i = 0; i < result.choices.size(); ++i)
        {
            if (result.choices[i].scope == *requested)
            {
                result.defaultIndex = static_cast<int>(i);
                return result;
            }
        }

        return result; // not offered: the caller is expected to have refused it already
    }

    // The most specific role/type kind that already has a rule: the one
    // in force for this window. (A saved application-wide rule does not
    // count here - see below: it is what a window of ANY kind falls back
    // to, so opening on it from a window that has a rule of its own kind
    // available would edit the rule that also governs the application's
    // other windows.)
    int bestSpecificity = 0;

    for (std::size_t i = 0; i < result.choices.size(); ++i)
    {
        if (!result.choices[i].saved)
            continue;

        int s = specificity(result.choices[i].scope);

        if (s > bestSpecificity)
        {
            bestSpecificity = s;
            result.defaultIndex = static_cast<int>(i);
        }
    }

    if (bestSpecificity > 0)
        return result;

    // No role/type rule for this window's kinds yet. If the application
    // already has rules scoped to some other role/type, the person is
    // using per-kind rules for it (a generic rule alongside is just the
    // fallback): open on this window's own role, so a new kind gets its
    // own rule instead of rewriting the generic one. Otherwise - nothing
    // saved, or only an application-wide rule - open on "All windows",
    // as the dialog always has.
    if (!role.empty())
    {
        for (const std::string& line : lines)
        {
            WindowRule rule;

            if (WindowRule::Parse(line, rule) && rule.IsScopedAppRule() &&
                !rule.Scope().IsApplication() &&
                Utils::Lower(rule.appKey) == Utils::Lower(appKey))
            {
                for (std::size_t i = 0; i < result.choices.size(); ++i)
                {
                    if (!result.choices[i].scope.role.empty() && result.choices[i].scope.windowType.empty())
                        result.defaultIndex = static_cast<int>(i);
                }

                break;
            }
        }
    }

    return result;
}

std::vector<ChooseWindowModel::Row> ChooseWindowModel::Rows() const
{
    std::vector<Row> rows;

    if (HasScopeRow())
        rows.push_back(Row::Scope);

    rows.push_back(Row::Workspace);
    rows.push_back(Row::Horizontal);
    rows.push_back(Row::Vertical);
    rows.push_back(Row::Mode);

    return rows;
}

int ChooseWindowModel::OptionCount(Row row) const
{
    switch (row)
    {
        case Row::Workspace:  return m_workspaceCount + 1; // Default + 1..N
        case Row::Horizontal: return 3;
        case Row::Vertical:   return 3;
        case Row::Mode:       return 4;
        case Row::Scope:      return static_cast<int>(m_scopes.size());
    }

    return 0;
}

std::string ChooseWindowModel::OptionLabel(Row row, int index) const
{
    static const char* kHorizontal[] = { "Default", "Left", "Right" };
    static const char* kVertical[] = { "Default", "Top", "Bottom" };
    static const char* kMode[] = { "Default", "Tiled", "Floating", "Fullscreen" };

    if (index < 0 || index >= OptionCount(row))
        return std::string();

    switch (row)
    {
        case Row::Workspace:  return index == 0 ? "Default" : std::to_string(index);
        case Row::Horizontal: return kHorizontal[index];
        case Row::Vertical:   return kVertical[index];
        case Row::Mode:       return kMode[index];
        case Row::Scope:      return m_scopes[static_cast<std::size_t>(index)].label;
    }

    return std::string();
}

std::string ChooseWindowModel::RowLabel(Row row)
{
    switch (row)
    {
        case Row::Workspace:  return "Workspace";
        case Row::Horizontal: return "Horizontal";
        case Row::Vertical:   return "Vertical";
        case Row::Mode:       return "Mode";
        case Row::Scope:      return "Applies to";
    }

    return std::string();
}

int ChooseWindowModel::Selected(Row row) const
{
    if (row == Row::Scope)
        return m_scope;

    return Current()[static_cast<std::size_t>(RowIndex(row))];
}

void ChooseWindowModel::Select(Row row, int index)
{
    if (index < 0 || index >= OptionCount(row))
        return;

    if (row == Row::Scope)
        m_scope = index;
    else
        Current()[static_cast<std::size_t>(RowIndex(row))] = index;
}

ChooseWindowModel::Action ChooseWindowModel::HandleKey(const Key& key)
{
    std::vector<Row> rows = Rows();
    const int rowCount = static_cast<int>(rows.size());

    auto positionOfCurrent = [&]() -> int
    {
        auto it = std::find(rows.begin(), rows.end(), m_row);
        return it == rows.end() ? 0 : static_cast<int>(it - rows.begin());
    };

    auto stepRow = [&](int delta, bool wrap)
    {
        int next = positionOfCurrent() + delta;

        if (wrap)
            next = (next % rowCount + rowCount) % rowCount;
        else
            next = std::clamp(next, 0, rowCount - 1);

        m_row = rows[static_cast<std::size_t>(next)];
    };

    auto stepChoice = [&](int delta)
    {
        int count = OptionCount(m_row);
        int selected = Selected(m_row);
        Select(m_row, ((selected + delta) % count + count) % count);
    };

    switch (key.kind)
    {
        case Key::Kind::Up:      stepRow(-1, false); break;
        case Key::Kind::Down:    stepRow(+1, false); break;
        case Key::Kind::Tab:     stepRow(+1, true);  break;
        case Key::Kind::BackTab: stepRow(-1, true);  break;
        case Key::Kind::Left:    stepChoice(-1);     break;
        case Key::Kind::Right:   stepChoice(+1);     break;

        case Key::Kind::Digit:
            if (key.digit >= 0 && key.digit <= m_workspaceCount)
            {
                m_row = Row::Workspace;
                Select(Row::Workspace, key.digit);
            }
            break;

        case Key::Kind::Enter:  return Action::Save;
        case Key::Kind::Escape: return Action::Cancel;
        case Key::Kind::Delete: return Action::Reset;
    }

    return Action::Continue;
}

WindowRule ChooseWindowModel::ToRule(const std::string& appKey) const
{
    return ToRule(appKey, CurrentRuleScope());
}

WindowRule ChooseWindowModel::ToRule(const std::string& appKey, const RuleScope& scope) const
{
    WindowRule rule;
    rule.appKey = appKey;
    rule.role = scope.role;
    rule.windowType = scope.windowType;
    rule.workspace = Current()[static_cast<std::size_t>(RowIndex(Row::Workspace))];
    rule.horizontal = static_cast<WindowRule::Horizontal>(Current()[static_cast<std::size_t>(RowIndex(Row::Horizontal))]);
    rule.vertical = static_cast<WindowRule::Vertical>(Current()[static_cast<std::size_t>(RowIndex(Row::Vertical))]);
    rule.mode = static_cast<WindowRule::Mode>(Current()[static_cast<std::size_t>(RowIndex(Row::Mode))]);

    const std::optional<WindowRule>& saved = m_scopes[static_cast<std::size_t>(m_scope)].saved;

    if (saved)
        rule.denyFullscreen = saved->denyFullscreen;

    return rule;
}

std::string ChooseWindowModel::SelectorText(const std::string& appKey) const
{
    const RuleScope& scope = CurrentRuleScope();

    std::string text = "app:" + WindowRule::EncodeValue(appKey);

    if (!scope.role.empty())
        text += " role:" + WindowRule::EncodeValue(scope.role);

    if (!scope.windowType.empty())
        text += " type:" + scope.windowType;

    return text;
}

bool ChooseWindowModel::AllDefault() const
{
    for (int value : Current())
    {
        if (value != 0)
            return false;
    }

    return true;
}

}
