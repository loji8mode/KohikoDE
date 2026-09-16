#pragma once

#include "Types.h"

#include <string>
#include <unordered_map>
#include <vector>

// Learns, per application class, which workspace and which side of
// the tiling layout the user has repeatedly moved its windows to -
// the "Adaptive placement" half of Session Restore (see README).
// Two independent habits, scored completely separately - an app can
// have a strong workspace habit and no position habit at all, or vice
// versa, or both, or neither:
//
//   - Workspace habit: a vote is cast every time
//     WindowManager::MoveFocusedToWorkspace() sends a window of this
//     class somewhere else by hand (Super+Shift+<N>, or `kohikoctl
//     move-to-workspace`). Once one workspace holds a clear, durable
//     majority of the votes, PredictWorkspace() starts recommending
//     it for brand new windows of that class - the lowest-priority
//     link in the same workspace-decision chain windowrule=workspace:
//     and session restore already sit at the top of, in
//     WindowManager::Manage().
//
//   - Position habit: a vote is cast every time
//     WindowManager::SwapWindows() moves a tiled window of this class
//     by hand (a Super+LMB drag, or Super+Shift+h/j/k/l) and records
//     which half of its monitor's work area (left/right,
//     independently top/bottom) it ended up in. Once an axis holds a
//     clear majority, PredictPositions() starts recommending nudging
//     brand new windows of that class toward that edge - see
//     WindowManager::ApplyAdaptivePlacementNudge().
//
// Both habits use the exact same "clear majority" bar
// (kMinObservations/kMinShare in the .cpp) precisely so a single
// one-off manual move never immediately hard-codes a permanent habit:
// it takes a genuinely repeated pattern before Kohiko starts
// predicting anything, and a prediction is silently withdrawn the
// moment the user's own votes stop actually agreeing with each other
// that strongly - there's no decay or expiry beyond that; a habit
// that was once strong and has since been diluted by enough
// contrary votes simply stops being confident, the same as it never
// having formed.
//
// This is deliberately a general, ongoing habit, not something that
// only kicks in after a restart - every RecordX() call updates the
// live in-memory table immediately, so a habit formed five minutes
// ago already applies to the very next window opened this same
// session; disk persistence (one small flat file under
// Xdg::DataDir(), same approach as HistoryStore/SessionStore) is only
// what carries it across a restart, not what makes it apply at all.
namespace Kohiko
{

struct PlacementHabit
{
    // Workspace -> number of times a manual move landed a window of
    // this class there. 1-based workspace numbers; an absent entry
    // means 0 votes.
    std::unordered_map<int, int> workspaceVotes;

    // Independent left/right and top/bottom tallies - see the class
    // comment's "Position habit" paragraph. A window that ends up
    // left-of-center after a manual move bumps leftVotes; right-of-
    // center bumps rightVotes; likewise topVotes/bottomVotes against
    // vertical center - every manual move counted exactly once on
    // each axis (a window can be, and often is, both "left" and
    // "top" from the same single move).
    int leftVotes = 0;
    int rightVotes = 0;
    int topVotes = 0;
    int bottomVotes = 0;
};

class PlacementHabitStore
{
public:

    // Loads from disk immediately - cheap (one small flat file), same
    // reasoning HistoryStore uses.
    PlacementHabitStore();

    // Records one manual "moved to workspace `workspace`" observation
    // for `appClass` and saves immediately - a manual move is rare
    // enough (nowhere near a hot path) that synchronous, immediate
    // persistence isn't worth avoiding, exactly like
    // HistoryStore::RecordLaunch().
    void RecordWorkspaceMove(
        const std::string& appClass,
        int workspace
    );

    // Records one manual reposition observation for `appClass`, given
    // the window's final geometry and the work area of the monitor it
    // landed on (used only to find each axis's center - see the class
    // comment for exactly what gets counted). A geometry/work area
    // with no area (width or height <= 0) is silently ignored - there
    // is no meaningful "which half" answer for either.
    void RecordPositionMove(
        const std::string& appClass,
        const Rect& finalGeometry,
        const Rect& monitorWorkArea
    );

    // 0 if `appClass` has no confident workspace habit yet - too few
    // observations, or too evenly split to call a genuine preference.
    int PredictWorkspace(
        const std::string& appClass
    ) const;

    // One entry per axis with a confident habit, strongest first -
    // empty if `appClass` has no confident position habit on either
    // axis. A caller walking this list in order and nudging one step
    // at a time per entry (see ApplyAdaptivePlacementNudge()) ends up
    // approximating a corner (e.g. Left then Up for "always dragged
    // to the top-left") when both axes are confident, without this
    // class needing any notion of "corner" itself.
    std::vector<Direction> PredictPositions(
        const std::string& appClass
    ) const;

private:

    void Load();
    void Save() const;

    std::unordered_map<std::string, PlacementHabit> m_habits;

};

}
