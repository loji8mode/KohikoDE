#include "PlacementHabitStore.h"

#include "Xdg.h"

#include <algorithm>
#include <fstream>

namespace Kohiko
{

namespace
{

std::filesystem::path HabitFilePath()
{
    return Xdg::DataDir() / "placement_habits";
}

// See PlacementHabitStore's header comment: neither habit starts
// influencing anything until there's a genuinely repeated pattern
// behind it (kMinObservations), and even then only once it's clearly
// lopsided rather than a near-even split (kMinShare) - a single
// contrary move here or there is expected and shouldn't visibly flip
// a prediction back and forth.
constexpr int kMinObservations = 3;
constexpr double kMinShare = 0.7;

}

PlacementHabitStore::PlacementHabitStore()
{
    Load();
}

void PlacementHabitStore::Load()
{
    m_habits.clear();

    std::ifstream file(HabitFilePath());

    std::string appClass;
    int leftVotes;
    int rightVotes;
    int topVotes;
    int bottomVotes;
    int workspaceEntryCount;

    // Application classes (WM_CLASS's class string) never contain
    // whitespace in practice, so a plain whitespace-delimited format
    // is fine here - the same assumption HistoryStore's desktopId
    // format already relies on.
    while (file >> appClass >> leftVotes >> rightVotes >> topVotes >> bottomVotes >> workspaceEntryCount)
    {
        PlacementHabit habit;
        habit.leftVotes = leftVotes;
        habit.rightVotes = rightVotes;
        habit.topVotes = topVotes;
        habit.bottomVotes = bottomVotes;

        bool ok = true;

        for (int i = 0; i < workspaceEntryCount; ++i)
        {
            int workspace;
            int votes;

            if (!(file >> workspace >> votes))
            {
                ok = false;
                break;
            }

            habit.workspaceVotes[workspace] = votes;
        }

        if (!ok)
            break; // truncated/corrupt tail - stop rather than risk misparsing the rest

        m_habits[appClass] = std::move(habit);
    }
}

void PlacementHabitStore::Save() const
{
    std::ofstream file(HabitFilePath(), std::ios::trunc);

    for (const auto& [appClass, habit] : m_habits)
    {
        file << appClass << ' '
             << habit.leftVotes << ' ' << habit.rightVotes << ' '
             << habit.topVotes << ' ' << habit.bottomVotes << ' '
             << habit.workspaceVotes.size();

        for (const auto& [workspace, votes] : habit.workspaceVotes)
            file << ' ' << workspace << ' ' << votes;

        file << '\n';
    }
}

void PlacementHabitStore::RecordWorkspaceMove(
    const std::string& appClass,
    int workspace)
{
    if (appClass.empty() || workspace < 1)
        return;

    m_habits[appClass].workspaceVotes[workspace] += 1; // default-constructed habit/entry if this is the first vote
    Save();
}

void PlacementHabitStore::RecordPositionMove(
    const std::string& appClass,
    const Rect& finalGeometry,
    const Rect& monitorWorkArea)
{
    if (appClass.empty())
        return;

    if (finalGeometry.width <= 0 || finalGeometry.height <= 0 ||
        monitorWorkArea.width <= 0 || monitorWorkArea.height <= 0)
        return;

    PlacementHabit& habit = m_habits[appClass];

    int windowCenterX = finalGeometry.x + finalGeometry.width / 2;
    int windowCenterY = finalGeometry.y + finalGeometry.height / 2;
    int monitorCenterX = monitorWorkArea.x + monitorWorkArea.width / 2;
    int monitorCenterY = monitorWorkArea.y + monitorWorkArea.height / 2;

    if (windowCenterX < monitorCenterX)
        habit.leftVotes += 1;
    else
        habit.rightVotes += 1;

    if (windowCenterY < monitorCenterY)
        habit.topVotes += 1;
    else
        habit.bottomVotes += 1;

    Save();
}

int PlacementHabitStore::PredictWorkspace(
    const std::string& appClass) const
{
    auto it = m_habits.find(appClass);

    if (it == m_habits.end())
        return 0;

    const PlacementHabit& habit = it->second;

    int total = 0;
    int bestWorkspace = 0;
    int bestVotes = 0;

    for (const auto& [workspace, votes] : habit.workspaceVotes)
    {
        total += votes;

        if (votes > bestVotes)
        {
            bestVotes = votes;
            bestWorkspace = workspace;
        }
    }

    if (total < kMinObservations || bestWorkspace == 0)
        return 0;

    if (static_cast<double>(bestVotes) / static_cast<double>(total) < kMinShare)
        return 0;

    return bestWorkspace;
}

std::vector<Direction> PlacementHabitStore::PredictPositions(
    const std::string& appClass) const
{
    std::vector<Direction> result;

    auto it = m_habits.find(appClass);

    if (it == m_habits.end())
        return result;

    const PlacementHabit& habit = it->second;

    struct Candidate
    {
        Direction direction;
        double share;
    };

    std::vector<Candidate> candidates;

    int horizontalTotal = habit.leftVotes + habit.rightVotes;

    if (horizontalTotal >= kMinObservations)
    {
        double leftShare = static_cast<double>(habit.leftVotes) / horizontalTotal;
        double rightShare = 1.0 - leftShare;

        if (leftShare >= kMinShare)
            candidates.push_back({Direction::Left, leftShare});
        else if (rightShare >= kMinShare)
            candidates.push_back({Direction::Right, rightShare});
    }

    int verticalTotal = habit.topVotes + habit.bottomVotes;

    if (verticalTotal >= kMinObservations)
    {
        double topShare = static_cast<double>(habit.topVotes) / verticalTotal;
        double bottomShare = 1.0 - topShare;

        if (topShare >= kMinShare)
            candidates.push_back({Direction::Up, topShare});
        else if (bottomShare >= kMinShare)
            candidates.push_back({Direction::Down, bottomShare});
    }

    // Strongest habit first - see PredictPositions()'s own header
    // comment for why the caller cares about this order.
    std::sort(
        candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) { return a.share > b.share; });

    for (const Candidate& candidate : candidates)
        result.push_back(candidate.direction);

    return result;
}

}
