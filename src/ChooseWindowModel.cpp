#include "ChooseWindowModel.h"

#include <algorithm>

namespace Kohiko
{

namespace
{

int RowIndex(ChooseWindowModel::Row row)
{
    return static_cast<int>(row);
}

}

ChooseWindowModel::ChooseWindowModel(
    int workspaceCount,
    const std::optional<WindowRule>& saved)
    : m_workspaceCount(std::max(1, workspaceCount)),
      m_saved(saved)
{
    if (!saved)
        return;

    // A rule saved when there were more workspaces than there are now
    // must still show (and keep) its workspace rather than snapping to
    // Default the moment the dialog opens.
    m_workspaceCount = std::max(m_workspaceCount, saved->workspace);

    m_selected[RowIndex(Row::Workspace)] = saved->workspace;
    m_selected[RowIndex(Row::Horizontal)] = static_cast<int>(saved->horizontal);
    m_selected[RowIndex(Row::Vertical)] = static_cast<int>(saved->vertical);
    m_selected[RowIndex(Row::Mode)] = static_cast<int>(saved->mode);
}

int ChooseWindowModel::OptionCount(Row row) const
{
    switch (row)
    {
        case Row::Workspace:  return m_workspaceCount + 1; // Default + 1..N
        case Row::Horizontal: return 3;
        case Row::Vertical:   return 3;
        case Row::Mode:       return 4;
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
    }

    return std::string();
}

int ChooseWindowModel::Selected(Row row) const
{
    return m_selected[RowIndex(row)];
}

void ChooseWindowModel::Select(Row row, int index)
{
    if (index >= 0 && index < OptionCount(row))
        m_selected[RowIndex(row)] = index;
}

ChooseWindowModel::Action ChooseWindowModel::HandleKey(const Key& key)
{
    auto stepRow = [&](int delta, bool wrap)
    {
        int next = RowIndex(m_row) + delta;

        if (wrap)
            next = (next % kRowCount + kRowCount) % kRowCount;
        else
            next = std::clamp(next, 0, kRowCount - 1);

        m_row = static_cast<Row>(next);
    };

    auto stepChoice = [&](int delta)
    {
        int count = OptionCount(m_row);
        int& selected = m_selected[RowIndex(m_row)];
        selected = ((selected + delta) % count + count) % count;
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
                m_selected[RowIndex(Row::Workspace)] = key.digit;
            }
            break;

        case Key::Kind::Enter:  return Action::Save;
        case Key::Kind::Escape: return Action::Cancel;
        case Key::Kind::Delete: return Action::Reset;
    }

    return Action::Continue;
}

WindowRule ChooseWindowModel::ToRule(const std::string& appKey, const RuleScope& scope) const
{
    WindowRule rule;
    rule.appKey = appKey;
    rule.role = scope.role;
    rule.windowType = scope.windowType;
    rule.workspace = m_selected[RowIndex(Row::Workspace)];
    rule.horizontal = static_cast<WindowRule::Horizontal>(m_selected[RowIndex(Row::Horizontal)]);
    rule.vertical = static_cast<WindowRule::Vertical>(m_selected[RowIndex(Row::Vertical)]);
    rule.mode = static_cast<WindowRule::Mode>(m_selected[RowIndex(Row::Mode)]);

    if (m_saved)
        rule.denyFullscreen = m_saved->denyFullscreen;

    return rule;
}

bool ChooseWindowModel::AllDefault() const
{
    for (int i = 0; i < kRowCount; ++i)
    {
        if (m_selected[i] != 0)
            return false;
    }

    return true;
}

}
