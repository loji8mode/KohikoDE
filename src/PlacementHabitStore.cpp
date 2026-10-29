#include "PlacementHabitStore.h"

#include "Xdg.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace Kohiko
{

namespace
{

namespace fs = std::filesystem;

fs::path HabitFilePath()
{
    return Xdg::DataDir() / "placement_habits";
}

// Current on-disk format. v1 (0.21.4 and earlier) had no header at
// all. Bump this only together with a real Load() migration branch.
constexpr int kFormatVersion = 2;
constexpr const char* kHeaderPrefix = "# kohiko-placement-habits v";

// ---- Confidence --------------------------------------------------
//
// Baseline (unchanged since 0.21.4): neither habit starts influencing
// anything until there's a genuinely repeated pattern behind it
// (kMinObservations), and even then only once it's clearly lopsided
// (kMinShare) - a single contrary move is expected and shouldn't
// flip a prediction back and forth.
constexpr int kMinObservations = 3;
constexpr double kMinShare = 0.7;

// New: leading votes / openings must also reach this. 0.5 means "on
// at least half the occasions where the user had a real chance to
// place this app, they actually placed it here". This is an initial,
// tested value - tests/test_placementhabits.cpp pins both sides of
// the boundary so any later change is a deliberate, visible one.
constexpr double kMinExposureShare = 0.5;

// Layout slots are integer-rounded, so a window that visually spans
// an axis can still have a center up to a pixel off the work-area
// center. Anything within this many pixels is "no side".
constexpr int kCenterTolerancePx = 1;

bool PassesExposure(int leadingVotes, int openings)
{
    if (openings <= 0)
        return false; // no recorded opportunities at all -> no denominator -> no confidence

    return static_cast<double>(leadingVotes) / static_cast<double>(openings) >= kMinExposureShare;
}

AxisSide ClassifyAxis(int start, int size, int border, int areaStart, int areaSize)
{
    // Work in doubled coordinates so no center is ever rounded.
    // The window's outer extent is its content plus the border on
    // BOTH sides, starting at `start` (content x/y is the slot's
    // origin; the border is drawn outside the content rect).
    long windowCenter2 = 2L * start + size + 2L * border;
    long areaCenter2 = 2L * areaStart + areaSize;
    long diff2 = windowCenter2 - areaCenter2;

    if (std::labs(diff2) <= 2L * kCenterTolerancePx)
        return AxisSide::Neutral;

    return diff2 < 0 ? AxisSide::Low : AxisSide::High;
}

void AdjustHorizontal(PlacementHabit& habit, AxisSide side, int delta)
{
    int* counter = side == AxisSide::Low ? &habit.leftVotes :
                   side == AxisSide::High ? &habit.rightVotes : nullptr;

    if (counter)
        *counter = std::max(0, *counter + delta);
}

void AdjustVertical(PlacementHabit& habit, AxisSide side, int delta)
{
    int* counter = side == AxisSide::Low ? &habit.topVotes :
                   side == AxisSide::High ? &habit.bottomVotes : nullptr;

    if (counter)
        *counter = std::max(0, *counter + delta);
}

void DecrementWorkspace(PlacementHabit& habit, int workspace)
{
    auto it = habit.workspaceVotes.find(workspace);

    if (it == habit.workspaceVotes.end())
        return;

    if (--it->second <= 0)
        habit.workspaceVotes.erase(it);
}

// Splits a line into whitespace-separated non-negative integers
// after the leading class token. Returns false on any malformed
// token so a corrupt line is rejected whole rather than half-read.
bool ParseInts(std::istringstream& in, std::vector<long>& out)
{
    std::string token;

    while (in >> token)
    {
        char* end = nullptr;
        long value = std::strtol(token.c_str(), &end, 10);

        if (end == token.c_str() || *end != '\0' || value < 0 || value > 1000000000L)
            return false;

        out.push_back(value);
    }

    return true;
}

}

PlacementHabitStore::PlacementHabitStore()
{
    Load();
}

PositionSides PlacementHabitStore::ClassifyPosition(
    const Rect& finalGeometry,
    const Rect& monitorWorkArea,
    int borderWidth)
{
    PositionSides sides;

    if (finalGeometry.width <= 0 || finalGeometry.height <= 0 ||
        monitorWorkArea.width <= 0 || monitorWorkArea.height <= 0)
        return sides;

    int border = std::max(0, borderWidth);

    sides.horizontal = ClassifyAxis(
        finalGeometry.x, finalGeometry.width, border,
        monitorWorkArea.x, monitorWorkArea.width);

    sides.vertical = ClassifyAxis(
        finalGeometry.y, finalGeometry.height, border,
        monitorWorkArea.y, monitorWorkArea.height);

    return sides;
}

// ---------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------
//
// v2 line:  class leftV rightV topV bottomV hOpen vOpen wsOpen n [ws votes]...
// v1 line:  class leftV rightV topV bottomV n [ws votes]...        (no header)
//
// Migration v1 -> v2 (deterministic, documented in the CHANGELOG):
//   workspaceOpenings  = sum of workspace votes
//   horizontalOpenings = leftVotes + rightVotes
//   verticalOpenings   = topVotes + bottomVotes
// i.e. each legacy vote is assumed to correspond to one opening, so
// every legacy habit gets exactly the confidence 0.21.4 gave it -
// never more - and genuine openings dilute it honestly from then on.

void PlacementHabitStore::Load()
{
    m_habits.clear();
    m_persistDisabled = false;

    std::ifstream file(HabitFilePath());

    if (!file)
        return;

    std::vector<std::string> lines;
    std::string line;

    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (line.find_first_not_of(" \t") == std::string::npos)
            continue;

        lines.push_back(line);
    }

    if (lines.empty())
        return;

    int version = 1;
    std::size_t firstData = 0;

    if (lines[0].rfind(kHeaderPrefix, 0) == 0)
    {
        version = std::atoi(lines[0].c_str() + std::string(kHeaderPrefix).size());
        firstData = 1;

        if (version > kFormatVersion || version < 2)
        {
            // Written by a newer Kohiko (or garbage): don't guess at
            // its meaning and don't overwrite it on the next save.
            m_persistDisabled = true;
            return;
        }
    }

    for (std::size_t i = firstData; i < lines.size(); ++i)
    {
        if (lines[i][0] == '#')
            continue; // comment

        std::istringstream in(lines[i]);
        std::string appClass;

        if (!(in >> appClass))
            continue;

        std::vector<long> v;

        if (!ParseInts(in, v))
            continue;

        PlacementHabit habit;
        std::size_t wsStart;

        if (version == 1)
        {
            if (v.size() < 5)
                continue;

            habit.leftVotes = static_cast<int>(v[0]);
            habit.rightVotes = static_cast<int>(v[1]);
            habit.topVotes = static_cast<int>(v[2]);
            habit.bottomVotes = static_cast<int>(v[3]);
            wsStart = 5;

            if (v[4] != static_cast<long>((v.size() - wsStart) / 2) || (v.size() - wsStart) % 2 != 0)
                continue;
        }
        else
        {
            if (v.size() < 8)
                continue;

            habit.leftVotes = static_cast<int>(v[0]);
            habit.rightVotes = static_cast<int>(v[1]);
            habit.topVotes = static_cast<int>(v[2]);
            habit.bottomVotes = static_cast<int>(v[3]);
            habit.horizontalOpenings = static_cast<int>(v[4]);
            habit.verticalOpenings = static_cast<int>(v[5]);
            habit.workspaceOpenings = static_cast<int>(v[6]);
            wsStart = 8;

            if (v[7] != static_cast<long>((v.size() - wsStart) / 2) || (v.size() - wsStart) % 2 != 0)
                continue;
        }

        bool ok = true;

        for (std::size_t k = wsStart; k + 1 < v.size(); k += 2)
        {
            if (v[k] < 1) { ok = false; break; }

            if (v[k + 1] > 0)
                habit.workspaceVotes[static_cast<int>(v[k])] = static_cast<int>(v[k + 1]);
        }

        if (!ok)
            continue;

        if (version == 1)
        {
            int workspaceTotal = 0;

            for (const auto& [workspace, votes] : habit.workspaceVotes)
                workspaceTotal += votes;

            habit.workspaceOpenings = workspaceTotal;
            habit.horizontalOpenings = habit.leftVotes + habit.rightVotes;
            habit.verticalOpenings = habit.topVotes + habit.bottomVotes;
        }

        m_habits[appClass] = std::move(habit);
    }

    if (version == 1)
    {
        // Keep the user's original file once, then persist v2.
        std::error_code ec;
        fs::path backup = HabitFilePath().string() + ".v1.bak";

        if (!fs::exists(backup, ec))
            fs::copy_file(HabitFilePath(), backup, ec);

        Save();
    }
}

void PlacementHabitStore::Save() const
{
    if (m_persistDisabled)
        return;

    // Deterministic output: sorted classes, sorted workspaces.
    std::vector<const std::pair<const std::string, PlacementHabit>*> ordered;

    for (const auto& entry : m_habits)
        ordered.push_back(&entry);

    std::sort(ordered.begin(), ordered.end(),
              [](const auto* a, const auto* b) { return a->first < b->first; });

    // Write to a temp file and rename so a crash mid-write can never
    // leave a half-written habit file behind - this now runs on every
    // opening as well as every move.
    fs::path path = HabitFilePath();
    fs::path temp = path.string() + ".tmp";

    {
        std::ofstream file(temp, std::ios::trunc);

        if (!file)
            return;

        file << kHeaderPrefix << kFormatVersion << '\n';

        for (const auto* entry : ordered)
        {
            const PlacementHabit& habit = entry->second;

            std::vector<std::pair<int, int>> workspaces(
                habit.workspaceVotes.begin(), habit.workspaceVotes.end());
            std::sort(workspaces.begin(), workspaces.end());

            file << entry->first << ' '
                 << habit.leftVotes << ' ' << habit.rightVotes << ' '
                 << habit.topVotes << ' ' << habit.bottomVotes << ' '
                 << habit.horizontalOpenings << ' ' << habit.verticalOpenings << ' '
                 << habit.workspaceOpenings << ' '
                 << workspaces.size();

            for (const auto& [workspace, votes] : workspaces)
                file << ' ' << workspace << ' ' << votes;

            file << '\n';
        }

        if (!file)
            return;
    }

    std::error_code ec;
    fs::rename(temp, path, ec);

    if (ec)
        fs::remove(temp, ec);
}

// ---------------------------------------------------------------
// Events
// ---------------------------------------------------------------

void PlacementHabitStore::RecordWorkspaceMove(
    const std::string& appClass,
    int workspace,
    InstanceId instance)
{
    if (appClass.empty() || workspace < 1)
        return;

    if (instance != 0)
    {
        InstanceVote& last = m_instances[instance];

        if (last.appClass != appClass)
        {
            last = InstanceVote{};
            last.appClass = appClass;
        }

        if (last.workspace == workspace)
            return; // this window's current placement is already counted

        if (last.workspace != 0)
            DecrementWorkspace(m_habits[appClass], last.workspace);

        last.workspace = workspace;
    }

    m_habits[appClass].workspaceVotes[workspace] += 1;
    Save();
}

void PlacementHabitStore::RecordPositionMove(
    const std::string& appClass,
    const Rect& finalGeometry,
    const Rect& monitorWorkArea,
    int borderWidth,
    InstanceId instance)
{
    if (appClass.empty())
        return;

    PositionSides sides = ClassifyPosition(finalGeometry, monitorWorkArea, borderWidth);

    if (instance == 0)
    {
        if (sides.horizontal == AxisSide::Neutral && sides.vertical == AxisSide::Neutral)
            return; // nothing distinguishable on either axis: no evidence at all

        PlacementHabit& habit = m_habits[appClass];
        AdjustHorizontal(habit, sides.horizontal, +1);
        AdjustVertical(habit, sides.vertical, +1);
        Save();
        return;
    }

    InstanceVote& last = m_instances[instance];

    if (last.appClass != appClass)
    {
        last = InstanceVote{};
        last.appClass = appClass;
    }

    if (sides.horizontal == last.horizontal && sides.vertical == last.vertical)
        return; // identical to what this window already contributed

    PlacementHabit& habit = m_habits[appClass];

    // Last placement wins: retract what this window contributed
    // before, then add what it contributes now.
    AdjustHorizontal(habit, last.horizontal, -1);
    AdjustVertical(habit, last.vertical, -1);
    AdjustHorizontal(habit, sides.horizontal, +1);
    AdjustVertical(habit, sides.vertical, +1);

    last.horizontal = sides.horizontal;
    last.vertical = sides.vertical;

    Save();
}

void PlacementHabitStore::RecordWorkspaceOpening(const std::string& appClass)
{
    if (appClass.empty())
        return;

    m_habits[appClass].workspaceOpenings += 1;
    Save();
}

void PlacementHabitStore::RecordPositionOpening(
    const std::string& appClass,
    bool horizontal,
    bool vertical)
{
    if (appClass.empty() || (!horizontal && !vertical))
        return;

    PlacementHabit& habit = m_habits[appClass];

    if (horizontal)
        habit.horizontalOpenings += 1;

    if (vertical)
        habit.verticalOpenings += 1;

    Save();
}

void PlacementHabitStore::ForgetInstance(InstanceId instance)
{
    if (instance != 0)
        m_instances.erase(instance);
}

// ---------------------------------------------------------------
// Prediction
// ---------------------------------------------------------------

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

        // Lower workspace number wins an exact tie so the result
        // never depends on hash-map iteration order. (A tie can't
        // pass kMinShare anyway; this just keeps it deterministic.)
        if (votes > bestVotes || (votes == bestVotes && workspace < bestWorkspace))
        {
            bestVotes = votes;
            bestWorkspace = workspace;
        }
    }

    if (total < kMinObservations || bestWorkspace == 0)
        return 0;

    if (static_cast<double>(bestVotes) / static_cast<double>(total) < kMinShare)
        return 0;

    if (!PassesExposure(bestVotes, habit.workspaceOpenings))
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

        if (leftShare >= kMinShare && PassesExposure(habit.leftVotes, habit.horizontalOpenings))
            candidates.push_back({Direction::Left, leftShare});
        else if (rightShare >= kMinShare && PassesExposure(habit.rightVotes, habit.horizontalOpenings))
            candidates.push_back({Direction::Right, rightShare});
    }

    int verticalTotal = habit.topVotes + habit.bottomVotes;

    if (verticalTotal >= kMinObservations)
    {
        double topShare = static_cast<double>(habit.topVotes) / verticalTotal;
        double bottomShare = 1.0 - topShare;

        if (topShare >= kMinShare && PassesExposure(habit.topVotes, habit.verticalOpenings))
            candidates.push_back({Direction::Up, topShare});
        else if (bottomShare >= kMinShare && PassesExposure(habit.bottomVotes, habit.verticalOpenings))
            candidates.push_back({Direction::Down, bottomShare});
    }

    // Strongest habit first - the caller walks this list in order
    // (see ApplyAdaptivePlacementNudge()). Stable so equal shares
    // keep horizontal-before-vertical, deterministically.
    std::stable_sort(
        candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) { return a.share > b.share; });

    for (const Candidate& candidate : candidates)
        result.push_back(candidate.direction);

    return result;
}

std::optional<PlacementHabit> PlacementHabitStore::Inspect(
    const std::string& appClass) const
{
    auto it = m_habits.find(appClass);

    if (it == m_habits.end())
        return std::nullopt;

    return it->second;
}

}
