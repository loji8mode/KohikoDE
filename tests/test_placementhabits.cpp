// Standalone correctness test for adaptive placement's learning
// engine - no X11 display needed, and no real XDG_DATA_HOME either:
// every case below redirects XDG_DATA_HOME to its own throwaway temp
// directory before touching PlacementHabitStore, so running this
// test never reads or overwrites a real user's actual learned habits
// and no case can leak state into another (the 0.21.4 version of
// this file shared one directory across all its blocks).
//
// What this pins down (see PlacementHabitStore.h for the model):
//   - the unchanged 0.21.4 baseline (>=3 votes, >=70% share);
//   - the new exposure requirement (votes / openings >= 50%), with
//     both sides of every threshold boundary;
//   - what is and isn't a learning opportunity (PlacementOrigin);
//   - "last placement wins" per window instance;
//   - position classification, driven by the REAL LayoutEngine, so
//     the center-line artifact the audit found can't silently return;
//   - the versioned file format: v2 round-trip, legacy v1 migration,
//     corrupt input, and a newer-than-known version being left alone.
//
// The WindowManager side (the passive swap target gets no vote,
// Kohiko's own placement never becomes a vote, openings are recorded
// only when nothing else decided placement) needs a real window
// manager and lives in tests/test_adaptive_live.sh.
//
// Build & run: see the "test-placementhabits" target in the Makefile.

#include "BSPTree.h"
#include "LayoutEngine.h"
#include "ManagedWindow.h"
#include "PlacementHabitStore.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace Kohiko;
namespace fs = std::filesystem;

namespace
{

int g_pass = 0;

void Check(bool condition, const std::string& what)
{
    if (condition)
    {
        ++g_pass;
        std::printf("  PASS: %s\n", what.c_str());
    }
    else
    {
        std::printf("  FAIL: %s\n", what.c_str());
        std::exit(1);
    }
}

// Each case gets a brand new, empty data directory.
fs::path g_dir;

void FreshDataDir(const char* name)
{
    g_dir = fs::temp_directory_path() / (std::string("kohiko-test-placementhabits-") + name);
    fs::remove_all(g_dir);
    fs::create_directories(g_dir);
    setenv("XDG_DATA_HOME", g_dir.string().c_str(), 1);
}

fs::path HabitFile()
{
    return g_dir / "kohiko" / "placement_habits";
}

void WriteHabitFile(const std::string& contents)
{
    fs::create_directories(HabitFile().parent_path());
    std::ofstream(HabitFile(), std::ios::trunc) << contents;
}

std::string ReadFile(const fs::path& path)
{
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string ReadHabitFile()
{
    return ReadFile(HabitFile());
}

// ---- Event helpers: what WindowManager does, in store terms ----------

// n new windows opened with nothing deciding their workspace.
void OpenWorkspace(PlacementHabitStore& s, const std::string& cls, int n = 1)
{
    for (int i = 0; i < n; ++i)
        s.RecordWorkspaceOpening(cls);
}

// The user sent windows to `ws` by hand (no matching opening: e.g. the
// window was already habit-placed elsewhere, and the user overrode it).
void UserMovesToWorkspace(PlacementHabitStore& s, const std::string& cls, int ws, int n = 1)
{
    for (int i = 0; i < n; ++i)
        s.RecordWorkspaceMove(cls, ws);
}

// n windows opened undecided and each was then moved to `ws`.
void OpenedAndMoved(PlacementHabitStore& s, const std::string& cls, int ws, int n)
{
    for (int i = 0; i < n; ++i)
    {
        s.RecordWorkspaceOpening(cls);
        s.RecordWorkspaceMove(cls, ws);
    }
}

const Rect kWorkArea{0, 0, 1920, 1080};

// Tiled windows clearly in the left / right half and in quadrants.
const Rect kLeft{8, 8, 940, 1060};
const Rect kRight{970, 8, 940, 1060};
const Rect kTopLeft{8, 8, 940, 520};
const Rect kBottomRight{970, 540, 940, 520};

}

int main()
{
    // ======================================================================
    // 1. The unchanged 0.21.4 baseline (now with exposure carried along)
    // ======================================================================

    std::printf("-- Baseline: fewer than the minimum evidence -> no prediction --\n");
    {
        FreshDataDir("baseline-min");
        PlacementHabitStore store;

        Check(store.PredictWorkspace("firefox") == 0, "a class with zero observations has no prediction");

        OpenedAndMoved(store, "firefox", 3, 1);
        Check(store.PredictWorkspace("firefox") == 0, "1 genuine observation is not yet a pattern");

        OpenedAndMoved(store, "firefox", 3, 1);
        Check(store.PredictWorkspace("firefox") == 0, "2 genuine observations are still not a pattern (min is 3)");

        OpenedAndMoved(store, "firefox", 3, 1);
        Check(store.PredictWorkspace("firefox") == 3, "3 of 3 consistent genuine observations form the habit");
    }

    std::printf("-- Baseline: a weak, mixed preference never predicts --\n");
    {
        FreshDataDir("baseline-mixed");
        PlacementHabitStore store;

        OpenedAndMoved(store, "mixed", 3, 3);
        OpenedAndMoved(store, "mixed", 5, 2);
        Check(store.PredictWorkspace("mixed") == 0, "3 vs 2 votes (60%) is below the 70% share bar");
        Check(store.Inspect("mixed")->workspaceVotes.at(3) == 3, "the votes themselves were still recorded");

        OpenedAndMoved(store, "mixed", 3, 2);
        Check(store.PredictWorkspace("mixed") == 3, "5 vs 2 (71%) clears the bar once the pattern is clearer");
    }

    std::printf("-- Baseline: contrary actions can eventually change the preference --\n");
    {
        FreshDataDir("baseline-flip");
        PlacementHabitStore store;

        OpenedAndMoved(store, "browser", 3, 3);
        Check(store.PredictWorkspace("browser") == 3, "habit starts on workspace 3");

        // Once the habit predicts, windows are habit-placed (NOT openings);
        // each time the user overrides it that is a pure contrary vote.
        UserMovesToWorkspace(store, "browser", 5, 4);
        Check(store.PredictWorkspace("browser") == 0, "4 contrary votes against 3 withdraw confidence (3/7)");

        UserMovesToWorkspace(store, "browser", 5, 6);
        Check(store.PredictWorkspace("browser") == 5, "enough sustained contrary votes flip the habit to workspace 5");
    }

    std::printf("-- Baseline: persistence round-trips (v2) --\n");
    {
        FreshDataDir("baseline-persist");
        {
            PlacementHabitStore store;
            OpenedAndMoved(store, "persisted", 4, 3);
            store.RecordPositionOpening("persisted", true, true);
            store.RecordPositionMove("persisted", kLeft, kWorkArea, 2);
        }

        PlacementHabitStore reloaded;
        Check(reloaded.PredictWorkspace("persisted") == 4, "the learned workspace survives a restart");

        auto habit = reloaded.Inspect("persisted");
        Check(habit && habit->workspaceOpenings == 3, "openings survive a restart");
        Check(habit && habit->leftVotes == 1 && habit->horizontalOpenings == 1, "position votes+openings survive a restart");
    }

    // ======================================================================
    // 2. Exposure-aware confidence
    // ======================================================================

    std::printf("-- Exposure: an opening with no move contributes exposure only --\n");
    {
        FreshDataDir("exposure-opening");
        PlacementHabitStore store;

        OpenWorkspace(store, "kate");
        auto habit = store.Inspect("kate");
        Check(habit && habit->workspaceOpenings == 1, "the opening was counted");
        Check(habit && habit->workspaceVotes.empty(), "an opening casts no placement vote");
        Check(store.PredictWorkspace("kate") == 0, "and certainly predicts nothing");
    }

    std::printf("-- Exposure: a user move contributes a vote --\n");
    {
        FreshDataDir("exposure-vote");
        PlacementHabitStore store;

        UserMovesToWorkspace(store, "kate", 2);
        auto habit = store.Inspect("kate");
        Check(habit && habit->workspaceVotes.at(2) == 1, "the move was counted as a vote");
        Check(habit && habit->workspaceOpenings == 0, "a vote alone is not an opening");
    }

    std::printf("-- Exposure: 3 moves among many launches do NOT make a habit --\n");
    {
        FreshDataDir("exposure-diluted");
        PlacementHabitStore store;

        OpenWorkspace(store, "chat", 17);       // 17 launches left where they landed
        OpenedAndMoved(store, "chat", 3, 3);    // 3 launches moved to ws 3
        Check(store.Inspect("chat")->workspaceOpenings == 20, "20 launches recorded");
        Check(store.PredictWorkspace("chat") == 0,
              "3 consistent moves in 20 launches (15%) is not a habit - 0.21.4 would have predicted ws 3 here");

        PlacementHabitStore reloaded;
        Check(reloaded.PredictWorkspace("chat") == 0, "and that stays true after a restart");
    }

    std::printf("-- Exposure: moving it again and again eventually does make a habit --\n");
    {
        FreshDataDir("exposure-repeated");
        PlacementHabitStore store;

        OpenWorkspace(store, "editor", 7);      // early on the user left it alone
        OpenedAndMoved(store, "editor", 2, 3);  // then started moving it
        Check(store.PredictWorkspace("editor") == 0, "3 moves in 10 launches: not yet");

        OpenedAndMoved(store, "editor", 2, 7);  // and kept doing so
        Check(store.Inspect("editor")->workspaceOpenings == 17, "17 launches");
        Check(store.PredictWorkspace("editor") == 2, "10 moves in 17 launches (59%) is a real pattern");
    }

    std::printf("-- Exposure: with no opening on record there is no denominator -> no habit --\n");
    {
        FreshDataDir("exposure-none");
        PlacementHabitStore store;

        UserMovesToWorkspace(store, "orphan", 4, 5);
        Check(store.PredictWorkspace("orphan") == 0,
              "5 consistent moves with zero recorded openings is not confidence (nothing to divide by)");
    }

    std::printf("-- Boundaries: min observations (3), share (70%%), exposure (50%%) --\n");
    {
        FreshDataDir("boundaries");
        PlacementHabitStore store;

        // min observations: exactly 2 vs exactly 3 (exposure and share fine)
        OpenedAndMoved(store, "b-min2", 1, 2);
        OpenedAndMoved(store, "b-min3", 1, 3);
        Check(store.PredictWorkspace("b-min2") == 0, "2 votes / 2 openings, 100% share: below minimum");
        Check(store.PredictWorkspace("b-min3") == 1, "3 votes / 3 openings, 100% share: at minimum");

        // share: 7 of 10 votes is exactly 70% (passes); 6 of 10 does not
        OpenedAndMoved(store, "b-share7", 1, 7);
        OpenedAndMoved(store, "b-share7", 2, 3);
        OpenedAndMoved(store, "b-share6", 1, 6);
        OpenedAndMoved(store, "b-share6", 2, 4);
        Check(store.PredictWorkspace("b-share7") == 1, "exactly 70% share predicts");
        Check(store.PredictWorkspace("b-share6") == 0, "60% share does not");

        // exposure: 3 votes / 6 openings is exactly 50% (passes); 3 / 7 does not
        UserMovesToWorkspace(store, "b-exp6", 1, 3);
        OpenWorkspace(store, "b-exp6", 6);
        UserMovesToWorkspace(store, "b-exp7", 1, 3);
        OpenWorkspace(store, "b-exp7", 7);
        Check(store.PredictWorkspace("b-exp6") == 1, "exactly 50% of openings predicts (3/6)");
        Check(store.PredictWorkspace("b-exp7") == 0, "just under 50% of openings does not (3/7)");

        // and one more undecided opening takes a confident habit back under the bar
        OpenWorkspace(store, "b-exp6");
        Check(store.PredictWorkspace("b-exp6") == 0, "a 7th undecided opening dilutes 3/6 to 3/7 and withdraws it");
    }

    // ======================================================================
    // 3. What is (not) a learning opportunity
    // ======================================================================

    std::printf("-- Origins: only 'nothing decided' is an opportunity to learn --\n");
    {
        Check(CountsAsOpportunity(PlacementOrigin::Default), "default placement is an opportunity");
        Check(!CountsAsOpportunity(PlacementOrigin::SessionRestore), "session restore is NOT (must not dilute a habit)");
        Check(!CountsAsOpportunity(PlacementOrigin::WindowRule), "an explicit window rule is NOT");
        Check(!CountsAsOpportunity(PlacementOrigin::Autostart), "autostart placement is NOT");
        Check(!CountsAsOpportunity(PlacementOrigin::TransientParent), "following a transient parent is NOT");
        Check(!CountsAsOpportunity(PlacementOrigin::AdaptiveHabit),
              "a window the habit itself placed is NOT (or a working habit would erode itself)");
    }

    std::printf("-- A working habit does not erode itself just by being applied --\n");
    {
        FreshDataDir("no-self-erosion");
        PlacementHabitStore store;

        OpenedAndMoved(store, "steady", 3, 3);
        Check(store.PredictWorkspace("steady") == 3, "habit formed");

        // 200 later launches are all placed by the habit. WindowManager
        // records nothing for them (origin == AdaptiveHabit); predicting
        // is a pure read.
        int last = 0;
        for (int i = 0; i < 200; ++i)
            last = store.PredictWorkspace("steady");

        Check(last == 3, "200 habit-placed launches later it still predicts workspace 3");
        auto habit = store.Inspect("steady");
        Check(habit->workspaceOpenings == 3 && habit->workspaceVotes.at(3) == 3,
              "and neither votes nor openings changed: Kohiko's own placement never becomes evidence");
    }

    // ======================================================================
    // 4. One vote per window instance - temporary actions are less dangerous
    // ======================================================================

    std::printf("-- Last placement wins: a window moved away and back counts once --\n");
    {
        FreshDataDir("instance-excursion");
        PlacementHabitStore store;

        OpenedAndMoved(store, "dev", 3, 3);
        Check(store.PredictWorkspace("dev") == 3, "established habit: workspace 3");

        // One habit-placed window (id 100): sent to ws 5 'for now', then back to 3.
        store.RecordWorkspaceMove("dev", 5, 100);
        Check(store.Inspect("dev")->workspaceVotes.at(5) == 1, "the excursion is a vote while it lasts");
        Check(store.PredictWorkspace("dev") == 3, "and one excursion does not displace a 3-vote habit");

        store.RecordWorkspaceMove("dev", 3, 100);
        auto habit = store.Inspect("dev");
        Check(habit->workspaceVotes.count(5) == 0, "moving it back RETRACTS the temporary vote for ws 5");
        Check(habit->workspaceVotes.at(3) == 4, "and counts once for ws 3");
        Check(store.PredictWorkspace("dev") == 3, "habit intact");
    }

    std::printf("-- Last placement wins: the audit's 'move away, then to ws 1' scenario --\n");
    {
        FreshDataDir("instance-audit-b");
        PlacementHabitStore store;

        OpenedAndMoved(store, "dev", 3, 3);

        store.RecordWorkspaceMove("dev", 5, 200); // temporary task window...
        store.RecordWorkspaceMove("dev", 1, 200); // ...finally parked on ws 1

        auto habit = store.Inspect("dev");
        Check(habit->workspaceVotes.count(5) == 0 && habit->workspaceVotes.at(1) == 1,
              "that window contributes exactly one vote (its final workspace)");
        Check(store.PredictWorkspace("dev") == 3,
              "3/4 = 75% - the habit survives (0.21.4: 3/5 = 60%, silently withdrawn)");
    }

    std::printf("-- Last placement wins: repeating the same move does not stack --\n");
    {
        FreshDataDir("instance-repeat");
        PlacementHabitStore store;

        for (int i = 0; i < 5; ++i)
            store.RecordWorkspaceMove("once", 2, 300);

        Check(store.Inspect("once")->workspaceVotes.at(2) == 1, "5 identical moves of ONE window are one vote");
    }

    std::printf("-- Last placement wins: different windows each contribute --\n");
    {
        FreshDataDir("instance-distinct");
        PlacementHabitStore store;

        for (unsigned long id = 1; id <= 3; ++id)
        {
            store.RecordWorkspaceOpening("multi");
            store.RecordWorkspaceMove("multi", 2, id);
        }
        Check(store.PredictWorkspace("multi") == 2, "3 different windows, 3 votes, 3 openings -> habit");

        store.ForgetInstance(1);
        store.RecordWorkspaceMove("multi", 2, 1); // id reused by a later, different window
        Check(store.Inspect("multi")->workspaceVotes.at(2) == 4, "a forgotten (closed) window's id starts fresh");
    }

    std::printf("-- A brief temporary move cannot dominate just because it is the only recent event --\n");
    {
        FreshDataDir("temp-dominate");
        PlacementHabitStore store;

        OpenWorkspace(store, "slack", 12);       // 12 undecided launches, no moves at all
        store.RecordWorkspaceMove("slack", 4, 1);
        store.RecordWorkspaceMove("slack", 4, 2);
        store.RecordWorkspaceMove("slack", 4, 3); // three quick moves in one sitting
        Check(store.Inspect("slack")->workspaceVotes.at(4) == 3, "3 moves recorded");
        Check(store.PredictWorkspace("slack") == 0,
              "3 moves against 12 launches (25%) does not become a habit, however recent");
    }

    // ======================================================================
    // 5. Position correctness
    // ======================================================================

    std::printf("-- Position: clearly left/right/top/bottom still classify and learn --\n");
    {
        PositionSides left = PlacementHabitStore::ClassifyPosition(kLeft, kWorkArea, 2);
        PositionSides right = PlacementHabitStore::ClassifyPosition(kRight, kWorkArea, 2);
        PositionSides topLeft = PlacementHabitStore::ClassifyPosition(kTopLeft, kWorkArea, 2);
        PositionSides bottomRight = PlacementHabitStore::ClassifyPosition(kBottomRight, kWorkArea, 2);

        Check(left.horizontal == AxisSide::Low, "left half -> Low (left)");
        Check(right.horizontal == AxisSide::High, "right half -> High (right)");
        Check(topLeft.vertical == AxisSide::Low && topLeft.horizontal == AxisSide::Low, "top-left quadrant -> top + left");
        Check(bottomRight.vertical == AxisSide::High && bottomRight.horizontal == AxisSide::High, "bottom-right quadrant -> bottom + right");

        FreshDataDir("position-clear");
        PlacementHabitStore store;
        for (int i = 0; i < 3; ++i)
            store.RecordPositionOpening("term", true, true);
        for (int i = 0; i < 3; ++i)
            store.RecordPositionMove("term", kTopLeft, kWorkArea, 2);

        std::vector<Direction> predicted = store.PredictPositions("term");
        Check(predicted.size() == 2, "3 of 3 top-left placements -> a confident habit on both axes");
        Check(predicted.size() == 2 &&
              (predicted[0] == Direction::Left || predicted[0] == Direction::Up) &&
              (predicted[1] == Direction::Left || predicted[1] == Direction::Up) &&
              predicted[0] != predicted[1], "toward Left and Up");
    }

    std::printf("-- Position: exact center produces NO vote on that axis --\n");
    {
        Rect fullScreen{0, 0, 1920, 1080};
        PositionSides sides = PlacementHabitStore::ClassifyPosition(fullScreen, kWorkArea, 0);
        Check(sides.horizontal == AxisSide::Neutral && sides.vertical == AxisSide::Neutral,
              "a window spanning the whole work area is neither left/right nor top/bottom");

        Rect fullHeightLeftHalf{0, 0, 960, 1080};
        sides = PlacementHabitStore::ClassifyPosition(fullHeightLeftHalf, kWorkArea, 0);
        Check(sides.horizontal == AxisSide::Low && sides.vertical == AxisSide::Neutral,
              "full-height left half: left on x, NO vote on y");

        FreshDataDir("position-center");
        PlacementHabitStore store;
        store.RecordPositionMove("c", fullScreen, kWorkArea, 0);
        Check(!store.Inspect("c").has_value(), "a fully-centered window records nothing at all");

        store.RecordPositionMove("c", fullHeightLeftHalf, kWorkArea, 0);
        auto habit = store.Inspect("c");
        Check(habit && habit->leftVotes == 1 && habit->topVotes == 0 && habit->bottomVotes == 0,
              "full-height left half records a left vote and NO top/bottom vote");
    }

    std::printf("-- Position: near-center is deterministic (1 px tolerance, then decided) --\n");
    {
        auto horizontalAt = [](int offsetPx)
        {
            Rect r{960 - 100 + offsetPx, 0, 200, 1080}; // 200 wide: center at 960 + offset
            return PlacementHabitStore::ClassifyPosition(r, kWorkArea, 0).horizontal;
        };

        Check(horizontalAt(0) == AxisSide::Neutral, "0 px off center: neutral");
        Check(horizontalAt(-1) == AxisSide::Neutral, "1 px left of center: neutral (rounding tolerance)");
        Check(horizontalAt(1) == AxisSide::Neutral, "1 px right of center: neutral (rounding tolerance)");
        Check(horizontalAt(-2) == AxisSide::Low, "2 px left of center: decided -> left");
        Check(horizontalAt(2) == AxisSide::High, "2 px right of center: decided -> right");

        // An odd-sized area has a half-pixel center; must not flip-flop.
        Rect oddArea{0, 0, 1921, 1081};
        PositionSides odd = PlacementHabitStore::ClassifyPosition(oddArea, oddArea, 0);
        Check(odd.horizontal == AxisSide::Neutral && odd.vertical == AxisSide::Neutral,
              "an odd-sized, fully-spanning window has no integer-rounding bias either");

        // Same input -> same answer, every time.
        bool stable = true;
        for (int i = 0; i < 100; ++i)
            stable = stable && horizontalAt(-1) == AxisSide::Neutral && horizontalAt(2) == AxisSide::High;
        Check(stable, "repeated classification of the same near-center input never changes");
    }

    std::printf("-- Position: border width never creates a fake side (real LayoutEngine) --\n");
    {
        for (int border : {0, 1, 2, 3, 5})
        {
            std::vector<std::unique_ptr<ManagedWindow>> windows;
            windows.push_back(std::make_unique<ManagedWindow>(1));
            windows.push_back(std::make_unique<ManagedWindow>(2));
            ManagedWindow* a = windows[0].get();
            ManagedWindow* b = windows[1].get();

            BSPTree tree;
            LayoutEngine layout;
            LayoutEngine::Params params;
            params.innerGap = 6;
            params.outerGap = 8;
            params.borderWidth = border;
            params.smartGaps = false;
            params.smartBorders = false;

            tree.Insert(a);
            tree.Focus(a);
            layout.Apply(tree.Root(), kWorkArea, params);
            tree.Insert(b);
            tree.Focus(b);
            layout.Apply(tree.Root(), kWorkArea, params);

            std::string label = "border=" + std::to_string(border) + ": ";

            // The OLD classifier (content center vs work-area center,
            // `<` = top/left, else bottom/right), reproduced verbatim.
            auto oldVoteTop = [&](ManagedWindow* w)
            {
                int cy = w->Geometry().y + w->Geometry().height / 2;
                int my = kWorkArea.y + kWorkArea.height / 2;
                return cy < my;
            };

            // Two windows on a landscape monitor sit side by side: each spans
            // the full height, so vertical position is NOT a side.
            for (ManagedWindow* w : {a, b})
            {
                PositionSides s = PlacementHabitStore::ClassifyPosition(w->Geometry(), kWorkArea, border);
                Check(s.vertical == AxisSide::Neutral, label + "a full-height side-by-side window has no top/bottom side");
                Check(s.horizontal != AxisSide::Neutral, label + "but its left/right side is still found");
            }

            PositionSides sa = PlacementHabitStore::ClassifyPosition(a->Geometry(), kWorkArea, border);
            PositionSides sb = PlacementHabitStore::ClassifyPosition(b->Geometry(), kWorkArea, border);
            Check(sa.horizontal != sb.horizontal, label + "the two windows land on opposite sides");

            if (border > 0)
                Check(oldVoteTop(a) && oldVoteTop(b),
                      label + "REGRESSION PROOF: the old classifier voted 'top' for BOTH windows (a fake signal)");
            else
                Check(!oldVoteTop(a) && !oldVoteTop(b),
                      label + "REGRESSION PROOF: with no border the old `<` test tied and voted 'bottom' for BOTH windows");
        }
    }

    std::printf("-- Position: stacked windows have no left/right side either --\n");
    {
        Rect stackedTop{8, 8, 1904, 520};
        Rect stackedBottom{8, 540, 1904, 520};
        PositionSides t = PlacementHabitStore::ClassifyPosition(stackedTop, kWorkArea, 0);
        PositionSides b = PlacementHabitStore::ClassifyPosition(stackedBottom, kWorkArea, 0);
        Check(t.horizontal == AxisSide::Neutral && b.horizontal == AxisSide::Neutral,
              "full-width stacked windows: no left/right vote");
        Check(t.vertical == AxisSide::Low && b.vertical == AxisSide::High, "but top/bottom is found");
    }

    std::printf("-- Position: three fake side-by-side swaps no longer manufacture an 'Up' habit --\n");
    {
        // The audit's headline consequence: 3 swaps of two side-by-side windows
        // produced a 100% 'Up' habit from no real signal. Replay with the
        // real layout geometry.
        std::vector<std::unique_ptr<ManagedWindow>> windows;
        windows.push_back(std::make_unique<ManagedWindow>(1));
        windows.push_back(std::make_unique<ManagedWindow>(2));
        BSPTree tree;
        LayoutEngine layout;
        LayoutEngine::Params params;
        params.innerGap = 6; params.outerGap = 8; params.borderWidth = 2;
        params.smartGaps = false; params.smartBorders = false;
        tree.Insert(windows[0].get()); tree.Focus(windows[0].get());
        layout.Apply(tree.Root(), kWorkArea, params);
        tree.Insert(windows[1].get()); tree.Focus(windows[1].get());
        layout.Apply(tree.Root(), kWorkArea, params);

        FreshDataDir("fake-up-habit");
        PlacementHabitStore store;
        for (int i = 0; i < 3; ++i)
        {
            store.RecordPositionOpening("pair", true, true);
            store.RecordPositionMove("pair", windows[0]->Geometry(), kWorkArea, 2);
        }

        std::vector<Direction> predicted = store.PredictPositions("pair");
        bool hasVertical = false;
        for (Direction d : predicted)
            hasVertical = hasVertical || d == Direction::Up || d == Direction::Down;
        Check(!hasVertical, "no vertical (Up/Down) habit is ever formed from a layout where nothing is above/below");
        Check(store.Inspect("pair")->topVotes == 0 && store.Inspect("pair")->bottomVotes == 0, "zero top/bottom votes were recorded");
    }

    std::printf("-- Position: exposure applies per axis --\n");
    {
        FreshDataDir("position-exposure");
        PlacementHabitStore store;

        for (int i = 0; i < 12; ++i)
            store.RecordPositionOpening("pos", true, false);
        for (int i = 0; i < 3; ++i)
            store.RecordPositionMove("pos", kLeft, kWorkArea, 2);
        Check(store.PredictPositions("pos").empty(), "3 left moves among 12 openings is not a position habit");

        for (int i = 0; i < 3; ++i)
        {
            store.RecordPositionOpening("pos", true, false);
            store.RecordPositionMove("pos", kLeft, kWorkArea, 2);
        }
        Check(store.PredictPositions("pos").empty(), "6 left moves among 15 openings (40%): still no");

        for (int i = 0; i < 3; ++i)
            store.RecordPositionMove("pos", kLeft, kWorkArea, 2); // user keeps doing it
        std::vector<Direction> predicted = store.PredictPositions("pos");
        Check(predicted.size() == 1 && predicted[0] == Direction::Left, "9 of 15 (60%): a confident left habit");

        auto habit = store.Inspect("pos");
        Check(habit->verticalOpenings == 0 && habit->topVotes + habit->bottomVotes == 0,
              "the untouched vertical axis has no votes and no openings");
    }

    std::printf("-- Position: last placement wins for one window --\n");
    {
        FreshDataDir("position-instance");
        PlacementHabitStore store;

        store.RecordPositionMove("w", kLeft, kWorkArea, 2, 7);
        store.RecordPositionMove("w", kRight, kWorkArea, 2, 7); // same window, moved again
        auto habit = store.Inspect("w");
        Check(habit->leftVotes == 0 && habit->rightVotes == 1, "one window: only its final side counts");

        store.RecordPositionMove("w", kRight, kWorkArea, 2, 7);
        Check(store.Inspect("w")->rightVotes == 1, "repeating the same placement does not stack");
    }

    // ======================================================================
    // 6. Persistence / migration
    // ======================================================================

    std::printf("-- Persistence: new files are v2 and deterministic --\n");
    {
        FreshDataDir("persist-v2");
        {
            PlacementHabitStore store;
            OpenedAndMoved(store, "zeta", 2, 1);
            OpenedAndMoved(store, "alpha", 3, 1);
        }

        std::string text = ReadHabitFile();
        Check(text.rfind("# kohiko-placement-habits v2\n", 0) == 0, "the file starts with the v2 header");
        Check(text.find("alpha") < text.find("zeta"), "entries are sorted by class (deterministic output)");
        Check(!fs::exists(HabitFile().string() + ".tmp"), "no temp file left behind after a save");
    }

    std::printf("-- Migration: a 0.21.4 (headerless) file loads, keeps working, and is upgraded --\n");
    {
        FreshDataDir("migrate-v1");
        // 0.21.4 line format:  class left right top bottom nWs [ws votes]...
        WriteHabitFile(
            "firefox 0 0 0 0 1 3 5\n"      // 5 votes for ws 3  -> was confident under 0.21.4
            "weak 0 0 0 0 2 1 2 4 1\n"     // 2 vs 1             -> was NOT confident
            "term 4 0 3 1 0\n");           // position-only habit

        const std::string original = ReadHabitFile();
        PlacementHabitStore store;

        Check(store.PredictWorkspace("firefox") == 3,
              "a legacy habit that predicted under 0.21.4 still predicts (existing habits stay usable)");
        Check(store.PredictWorkspace("weak") == 0, "a legacy non-habit does not become one");

        auto firefox = store.Inspect("firefox");
        Check(firefox && firefox->workspaceOpenings == 5,
              "migration: each legacy vote is assumed to come from one opening (never MORE confident than before)");

        auto term = store.Inspect("term");
        Check(term && term->horizontalOpenings == 4 && term->verticalOpenings == 4,
              "migration: position openings = left+right and top+bottom");
        Check(store.PredictPositions("term").size() == 2, "legacy position habit (4:0 and 3:1) keeps predicting on both axes");

        Check(ReadHabitFile().rfind("# kohiko-placement-habits v2\n", 0) == 0, "the file was rewritten as v2");
        Check(fs::exists(HabitFile().string() + ".v1.bak"), "the original file was kept as placement_habits.v1.bak");
        Check(ReadFile(HabitFile().string() + ".v1.bak") == original, "the backup is byte-identical to the original");

        // New evidence now dilutes honestly: 5 votes / 5 openings + 11 undecided openings.
        for (int i = 0; i < 11; ++i)
            store.RecordWorkspaceOpening("firefox");
        Check(store.PredictWorkspace("firefox") == 0, "after 11 more undecided launches the legacy habit is diluted (5/16)");

        PlacementHabitStore again;
        Check(again.PredictWorkspace("firefox") == 0 && again.Inspect("firefox")->workspaceOpenings == 16,
              "the migrated+updated data reloads identically (migration happens exactly once)");
        Check(ReadFile(HabitFile().string() + ".v1.bak") == original, "and the backup is never overwritten");
    }

    std::printf("-- Migration: identical legacy input always migrates to identical output --\n");
    {
        FreshDataDir("migrate-det-a");
        WriteHabitFile("app 2 5 1 6 2 1 4 7 9\n");
        PlacementHabitStore first;
        std::string v2a = ReadHabitFile();

        FreshDataDir("migrate-det-b");
        WriteHabitFile("app 2 5 1 6 2 1 4 7 9\n");
        PlacementHabitStore second;
        Check(v2a == ReadHabitFile(), "byte-identical v2 output from byte-identical v1 input");
    }

    std::printf("-- Robustness: corrupt / hostile files never crash or poison good data --\n");
    {
        FreshDataDir("corrupt");
        WriteHabitFile(
            "# kohiko-placement-habits v2\n"
            "good 0 0 0 0 0 0 3 1 2 3\n"
            "garbage line with words\n"
            "negative 0 0 0 0 0 0 -5 0\n"
            "truncated 0 0 0 0 0 0 3 2 1 2\n"
            "\n"
            "after 0 0 0 0 0 0 3 1 4 3\n");
        PlacementHabitStore store;

        Check(store.Inspect("good") && store.Inspect("good")->workspaceVotes.at(2) == 3, "a valid line loads");
        Check(!store.Inspect("garbage"), "a malformed line is skipped");
        Check(!store.Inspect("negative"), "a negative count is rejected");
        Check(!store.Inspect("truncated"), "a line whose entry count doesn't match is rejected");
        Check(store.Inspect("after") && store.PredictWorkspace("after") == 4,
              "and a bad line does not stop the lines after it from loading");
    }

    std::printf("-- Robustness: a file from a NEWER format is neither used nor overwritten --\n");
    {
        FreshDataDir("future");
        const std::string future = "# kohiko-placement-habits v9\nsomething from the future\n";
        WriteHabitFile(future);

        PlacementHabitStore store;
        Check(!store.Inspect("something"), "unknown-version data is not interpreted");

        OpenedAndMoved(store, "newclass", 1, 3);
        Check(store.PredictWorkspace("newclass") == 1, "learning still works in memory this session");
        Check(ReadHabitFile() == future, "but the newer file on disk is left untouched (downgrade-safe)");
    }

    std::printf("-- Robustness: empty / missing file --\n");
    {
        FreshDataDir("empty");
        PlacementHabitStore store; // no file at all
        Check(store.PredictWorkspace("x") == 0 && store.PredictPositions("x").empty(), "no file -> no opinions");

        WriteHabitFile("");
        PlacementHabitStore store2;
        Check(store2.PredictWorkspace("x") == 0, "empty file -> no opinions");
    }

    std::printf("-- Empty class is always a no-op --\n");
    {
        FreshDataDir("emptyclass");
        PlacementHabitStore store;
        store.RecordWorkspaceMove("", 3);
        store.RecordWorkspaceOpening("");
        store.RecordPositionOpening("", true, true);
        store.RecordPositionMove("", kLeft, kWorkArea, 2);
        Check(!store.Inspect(""), "nothing is recorded for an empty class");
    }

    std::printf("\nAll %d checks passed.\n", g_pass);
    return 0;
}
