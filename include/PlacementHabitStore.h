#pragma once

#include "Types.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Learns, per application class, which workspace and which side of
// the tiling layout the user has repeatedly moved its windows to -
// the "Adaptive placement" half of Session Restore (see README).
// Two independent habits, scored completely separately - an app can
// have a strong workspace habit and no position habit at all, or vice
// versa, or both, or neither.
//
// ---------------------------------------------------------------
// What counts as evidence (the learning events)
// ---------------------------------------------------------------
//
// The store never guesses intent. WindowManager tells it, explicitly,
// about exactly two kinds of event:
//
//   1. A USER-DIRECTED PLACEMENT (a "vote"):
//        - RecordWorkspaceMove(): the user sent a window to another
//          workspace by hand (Super+Shift+<N>, `kohikoctl
//          movetoworkspace`).
//        - RecordPositionMove(): the user repositioned a tiled window
//          by hand (a Super+LMB drag, or Super+Shift+h/j/k/l). Only
//          the window the USER moved is recorded. The window it
//          traded places with is passive: it was moved as a side
//          effect, not by choice, so it receives no vote.
//      Kohiko's own placement (a habit being applied to a new
//      window, session restore, a window rule, autostart) never
//      calls either - there is no path from "Kohiko placed it" to
//      "Kohiko learned from it".
//
//   2. AN OPPORTUNITY TO LEARN (an "opening"):
//        - RecordWorkspaceOpening() / RecordPositionOpening(): a new
//          window of this class appeared and NOTHING had already
//          decided where it goes - not a session record, not a
//          window rule, not an autostart placement, not its transient
//          parent, and not an existing habit. Only then did the user
//          genuinely have the chance to express a preference by
//          moving it (or not). See PlacementOrigin below.
//
// A window that an existing habit already placed is deliberately NOT
// an opening: counting it as "the user didn't move it" would make a
// working habit erode itself just by being used.
//
// ---------------------------------------------------------------
// Confidence (the same bar for both habits, unchanged baseline +
// one new requirement)
// ---------------------------------------------------------------
//
// A habit is confident (and only then predicts anything) when ALL of:
//   (a) at least kMinObservations votes             (3, unchanged)
//   (b) the leading choice holds >= kMinShare of
//       those votes                                  (70%, unchanged)
//   (c) leading votes / openings >= kMinExposureShare (50%, NEW)
//       with at least one opening on record.
//
// (c) is the missing denominator: "moved it 3 times" is very
// different from "opened it 30 times and moved it 3 times". The
// position habit applies (c) per axis with its own opening counter.
//
// ---------------------------------------------------------------
// One vote per window instance ("last placement wins")
// ---------------------------------------------------------------
//
// Callers may pass the window's id. Within one window's lifetime,
// each kind of vote is replaced rather than accumulated: moving a
// window to workspace 5 "just for now" and then back to workspace 3
// leaves exactly one vote, for 3, not one for 5 and one for 3. The
// instance table is in-memory only (never persisted) and is cleared
// per window by ForgetInstance().
//
// There is deliberately no time decay or expiry: counters are plain
// integers, the whole model stays deterministic, and a stale habit
// is displaced the same way it was formed - by the user's own later
// placements and by the openings those windows produce.
//
// ---------------------------------------------------------------
// Persistence (versioned; see Load()/Save() in the .cpp)
// ---------------------------------------------------------------
//
// One small flat file under Xdg::DataDir() ("placement_habits"),
// first line `# kohiko-placement-habits v2`. A file with no such
// header is the 0.21.4-and-earlier format; it is migrated on load
// (each legacy vote is taken to have come from one opening, so a
// legacy habit predicts exactly as it did before and is never
// MORE confident than 0.21.4 made it), the original is kept as
// `placement_habits.v1.bak`, and the file is rewritten as v2. A file
// from a NEWER format version is left untouched and not used.
namespace Kohiko
{

// Where a new window's placement came from, as decided by
// WindowManager::Manage()'s priority chain. Only Default - nothing
// else decided - is a genuine opportunity to learn.
enum class PlacementOrigin
{
    Default,         // nothing decided; fell through to "current workspace"
    SessionRestore,  // session.restore_priority / saved session record
    WindowRule,      // windowrule= forced workspace
    Autostart,       // launched by Kohiko's own startup plan
    TransientParent, // dialog/transient follows its parent
    AdaptiveHabit    // an existing learned habit decided it
};

constexpr bool CountsAsOpportunity(PlacementOrigin origin)
{
    return origin == PlacementOrigin::Default;
}

// Which side of an axis a window sits on. Neutral = no meaningful side
// (the window spans the axis, or its center is indistinguishable
// from the work-area center), which is NOT evidence for either side.
enum class AxisSide { Neutral, Low, High }; // Low = left/top, High = right/bottom

struct PositionSides
{
    AxisSide horizontal = AxisSide::Neutral;
    AxisSide vertical = AxisSide::Neutral;
};

struct PlacementHabit
{
    // Workspace -> number of user placements that landed a window of
    // this class there. 1-based; an absent entry means 0 votes.
    std::unordered_map<int, int> workspaceVotes;

    // Opportunities where nothing else decided the workspace.
    int workspaceOpenings = 0;

    // Independent left/right and top/bottom tallies. A window whose
    // center is not clearly on one side of an axis casts no vote on
    // that axis (see ClassifyPosition()).
    int leftVotes = 0;
    int rightVotes = 0;
    int topVotes = 0;
    int bottomVotes = 0;

    // Per-axis opportunities (a tiled window opened, a swap was
    // possible, and no position habit/restore had already placed it
    // on that axis).
    int horizontalOpenings = 0;
    int verticalOpenings = 0;
};

class PlacementHabitStore
{
public:

    // Identifies one window for the "last placement wins" rule. 0
    // means "unknown/untracked": the vote is simply added.
    using InstanceId = unsigned long;

    // Loads (and, for a legacy file, migrates) from disk immediately.
    PlacementHabitStore();

    // ---- User-directed placement (votes) --------------------------

    void RecordWorkspaceMove(
        const std::string& appClass,
        int workspace,
        InstanceId instance = 0
    );

    // `finalGeometry` is the window's content rect as Kohiko tracks
    // it; `borderWidth` is the X11 border drawn OUTSIDE that rect.
    // `monitorWorkArea` is the area whose center defines the axes.
    // A geometry/work area with no area is ignored.
    void RecordPositionMove(
        const std::string& appClass,
        const Rect& finalGeometry,
        const Rect& monitorWorkArea,
        int borderWidth = 0,
        InstanceId instance = 0
    );

    // ---- Opportunities (openings) ---------------------------------

    void RecordWorkspaceOpening(const std::string& appClass);

    // Per-axis: pass true for each axis on which this window had a
    // genuine opportunity (not already positioned on that axis).
    void RecordPositionOpening(
        const std::string& appClass,
        bool horizontal,
        bool vertical
    );

    // Drops the per-window "last placement" memory (window closed).
    void ForgetInstance(InstanceId instance);

    // ---- Prediction -------------------------------------------------

    // 0 if `appClass` has no confident workspace habit.
    int PredictWorkspace(const std::string& appClass) const;

    // One entry per confident axis, strongest first; empty if none.
    std::vector<Direction> PredictPositions(const std::string& appClass) const;

    // ---- Introspection (tests, diagnostics) -------------------------

    std::optional<PlacementHabit> Inspect(const std::string& appClass) const;

    // The exact classification RecordPositionMove() uses. Windows are
    // judged by their OUTER rect (content + border on each side),
    // which is the layout slot the window actually occupies, with a
    // 1 px tolerance for integer rounding in the layout. Anything
    // within that tolerance of the work-area center on an axis is
    // AxisSide::Neutral for that axis.
    static PositionSides ClassifyPosition(
        const Rect& finalGeometry,
        const Rect& monitorWorkArea,
        int borderWidth
    );

private:

    struct InstanceVote
    {
        std::string appClass;
        int workspace = 0;
        AxisSide horizontal = AxisSide::Neutral;
        AxisSide vertical = AxisSide::Neutral;
    };

    void Load();
    void Save() const;

    std::unordered_map<std::string, PlacementHabit> m_habits;
    std::unordered_map<InstanceId, InstanceVote> m_instances;

    // True when the on-disk file is from a newer format than this
    // build understands: it is neither used nor overwritten.
    bool m_persistDisabled = false;
};

}
