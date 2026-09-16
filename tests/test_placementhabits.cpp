// Standalone correctness test for adaptive placement's learning
// engine - no X11 display needed, and no real XDG_DATA_HOME either:
// this redirects XDG_DATA_HOME to a throwaway temp directory before
// touching PlacementHabitStore, so running this test never reads or
// overwrites a real user's actual learned habits.
//
// Build & run: see the "test-placementhabits" target in the Makefile.

#include "PlacementHabitStore.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace Kohiko;

namespace
{

int g_pass = 0;

void Check(bool condition, const char* what)
{
    if (condition)
    {
        ++g_pass;
        std::printf("  PASS: %s\n", what);
    }
    else
    {
        std::printf("  FAIL: %s\n", what);
        std::exit(1);
    }
}

}

int main()
{
    // Isolate this run's persisted state from any real user data.
    std::filesystem::path tempDir =
        std::filesystem::temp_directory_path() / "kohiko-test-placementhabits";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directories(tempDir);
    setenv("XDG_DATA_HOME", tempDir.string().c_str(), 1);

    std::printf("-- Workspace habit: no opinion before a genuine pattern forms --\n");
    {
        PlacementHabitStore store;
        Check(store.PredictWorkspace("firefox") == 0,
              "a class with zero observations has no workspace prediction");

        store.RecordWorkspaceMove("firefox", 3);
        Check(store.PredictWorkspace("firefox") == 0,
              "a single observation still isn't a 'clear, consistent' pattern yet");

        store.RecordWorkspaceMove("firefox", 3);
        Check(store.PredictWorkspace("firefox") == 0,
              "two observations still isn't enough (kMinObservations)");
    }

    std::printf("\n-- Workspace habit: a clear, repeated pattern predicts correctly --\n");
    {
        PlacementHabitStore store;
        store.RecordWorkspaceMove("firefox", 3);
        store.RecordWorkspaceMove("firefox", 3);
        store.RecordWorkspaceMove("firefox", 3);

        Check(store.PredictWorkspace("firefox") == 3,
              "three straight votes for workspace 3 -> predicts workspace 3");

        Check(store.PredictWorkspace("Alacritty") == 0,
              "an unrelated app class has no prediction of its own");
    }

    std::printf("\n-- Workspace habit: a near-even split never predicts anything --\n");
    {
        PlacementHabitStore store;
        // 3 votes for workspace 1, 2 votes for workspace 2 - 60%,
        // below kMinShare (0.7) - should NOT produce a confident
        // prediction, since it isn't a clear majority.
        store.RecordWorkspaceMove("code", 1);
        store.RecordWorkspaceMove("code", 1);
        store.RecordWorkspaceMove("code", 1);
        store.RecordWorkspaceMove("code", 2);
        store.RecordWorkspaceMove("code", 2);

        Check(store.PredictWorkspace("code") == 0,
              "a 3-vs-2 (60%) split is too close to call - no prediction");
    }

    std::printf("\n-- Workspace habit: a strong contrary run can outweigh and flip an old habit --\n");
    {
        PlacementHabitStore store;
        for (int i = 0; i < 3; ++i)
            store.RecordWorkspaceMove("terminal", 1);
        Check(store.PredictWorkspace("terminal") == 1, "workspace 1 predicted after 3 votes");

        // The user changes their mind and moves it to workspace 4
        // often enough to swing the majority decisively (10 vs 3,
        // ~77% - comfortably past kMinShare).
        for (int i = 0; i < 10; ++i)
            store.RecordWorkspaceMove("terminal", 4);

        Check(store.PredictWorkspace("terminal") == 4,
              "a decisive contrary run (10 vs 3, ~77%) flips the prediction to workspace 4");
    }

    std::printf("\n-- Position habit: no opinion before a pattern forms, then a clear left habit --\n");
    {
        PlacementHabitStore store;
        Rect monitor{0, 0, 1920, 1080}; // center at (960, 540)

        Rect leftTop{0, 0, 900, 500};        // center (450, 250) -> left, top
        Rect leftBottom{0, 600, 900, 480};   // center (450, 840) -> left, bottom

        Check(store.PredictPositions("kitty").empty(),
              "no position habit before any observation");

        store.RecordPositionMove("kitty", leftTop, monitor);
        store.RecordPositionMove("kitty", leftTop, monitor);
        Check(store.PredictPositions("kitty").empty(),
              "two observations still isn't enough for a confident axis prediction");

        // Third move lands left again but on the *other* vertical
        // half, so only the horizontal axis (left, 3/3) ends up with
        // a clear majority - the vertical axis (2 top, 1 bottom) is
        // below kMinShare and must stay silent.
        store.RecordPositionMove("kitty", leftBottom, monitor);

        auto positions = store.PredictPositions("kitty");
        Check(positions.size() == 1 && positions[0] == Direction::Left,
              "three straight left-side moves -> predicts Direction::Left only "
              "(the vertical axis split 2/1 and stays below the confidence bar)");
    }

    std::printf("\n-- Position habit: both axes confident predicts a corner, strongest first --\n");
    {
        PlacementHabitStore store;
        Rect monitor{0, 0, 1920, 1080};

        // Top-left quadrant, every single time.
        Rect topLeft{0, 0, 900, 400};

        for (int i = 0; i < 4; ++i)
            store.RecordPositionMove("mail", topLeft, monitor);

        auto positions = store.PredictPositions("mail");
        Check(positions.size() == 2, "both axes confident -> two directions returned");
        Check(positions[0] == Direction::Left || positions[0] == Direction::Up,
              "first entry is one of the two confident axes");
        Check(positions[1] == Direction::Left || positions[1] == Direction::Up,
              "second entry is the other confident axis");
        Check(positions[0] != positions[1], "the two entries are actually different axes");
    }

    std::printf("\n-- Position habit: empty/degenerate geometry is silently ignored --\n");
    {
        PlacementHabitStore store;
        Rect monitor{0, 0, 1920, 1080};
        Rect zeroWidth{100, 100, 0, 400};
        Rect zeroMonitor{0, 0, 0, 0};

        store.RecordPositionMove("weird", zeroWidth, monitor);
        store.RecordPositionMove("weird", Rect{0, 0, 800, 600}, zeroMonitor);

        Check(store.PredictPositions("weird").empty(),
              "degenerate geometry never contributes a vote");
    }

    std::printf("\n-- Persistence: habits survive across a fresh PlacementHabitStore instance --\n");
    {
        {
            PlacementHabitStore store;
            store.RecordWorkspaceMove("spotify", 5);
            store.RecordWorkspaceMove("spotify", 5);
            store.RecordWorkspaceMove("spotify", 5);
        }

        // A brand new instance - simulating Kohiko restarting - should
        // load the same votes straight back off disk.
        PlacementHabitStore reloaded;
        Check(reloaded.PredictWorkspace("spotify") == 5,
              "a fresh instance reloads previously-recorded votes from disk "
              "and predicts identically to the instance that recorded them");
    }

    std::printf("\n-- Empty app class is always a no-op --\n");
    {
        PlacementHabitStore store;
        store.RecordWorkspaceMove("", 2);
        store.RecordPositionMove("", Rect{0, 0, 800, 600}, Rect{0, 0, 1920, 1080});
        Check(store.PredictWorkspace("") == 0, "an empty class string never predicts a workspace");
        Check(store.PredictPositions("").empty(), "an empty class string never predicts a position");
    }

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    std::filesystem::remove_all(tempDir);
    return 0;
}
