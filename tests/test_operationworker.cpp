// OperationWorker: the primitive that keeps the native control apps'
// event loops responsive while a slow call runs. Pure C++ - no X11, no
// D-Bus - so every guarantee the apps rely on is checked here directly,
// including under ThreadSanitizer (see the Makefile's test_operationworker_tsan
// when the toolchain has it).

#include "OperationWorker.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <unistd.h>

using namespace Kohiko;
using Clock = std::chrono::steady_clock;

static int g_pass = 0;
static int g_fail = 0;

static void Check(bool condition, const char* message)
{
    if (condition) { ++g_pass; std::printf("  PASS: %s\n", message); }
    else           { ++g_fail; std::printf("  FAIL: %s\n", message); }
}

static double MsSince(Clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// What an app's main loop does: poll(fd), Dispatch() when readable.
// Returns after `timeoutMs` or as soon as `until()` holds.
template <typename Pred>
static bool PumpUntil(OperationWorker& worker, int timeoutMs, Pred until)
{
    auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);

    while (Clock::now() < deadline)
    {
        if (until())
            return true;

        pollfd pfd{ worker.Fd(), POLLIN, 0 };
        int remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());

        if (poll(&pfd, 1, std::max(0, remaining)) > 0 && (pfd.revents & POLLIN))
            worker.Dispatch();
    }

    return until();
}

static void Sleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int main()
{
    const std::thread::id ownerThread = std::this_thread::get_id();

    std::printf("Jobs run off the owner thread; completions come back on it:\n");
    {
        OperationWorker worker;
        std::thread::id jobThread, completionThread;
        bool completed = false;
        OperationResult got;

        worker.Submit("a",
            [&] { jobThread = std::this_thread::get_id(); return OperationResult::Success("done"); },
            [&](const OperationResult& r) { completionThread = std::this_thread::get_id(); got = r; completed = true; });

        Check(PumpUntil(worker, 2000, [&] { return completed; }), "the completion is delivered");
        Check(jobThread != ownerThread, "the job ran on the worker thread");
        Check(completionThread == ownerThread, "the completion ran on the owner thread (inside Dispatch)");
        Check(got.ok && got.message == "done", "the result - success and message - arrives intact");
    }

    std::printf("\nThe owner is never blocked by a slow job:\n");
    {
        OperationWorker worker;
        bool completed = false;

        auto t0 = Clock::now();
        worker.Submit("slow", [] { Sleep(600); return OperationResult::Success(); },
                      [&](const OperationResult&) { completed = true; });
        double submitMs = MsSince(t0);
        Check(submitMs < 50, "Submit() of a 600 ms job returns immediately");

        // The owner keeps doing "UI work" - a loop standing in for poll()
        // serving X events - and every iteration is prompt.
        double worstGap = 0;
        auto last = Clock::now();
        while (!completed)
        {
            pollfd pfd{ worker.Fd(), POLLIN, 0 };
            if (poll(&pfd, 1, 10) > 0)
                worker.Dispatch();

            auto now = Clock::now();
            worstGap = std::max(worstGap, std::chrono::duration<double, std::milli>(now - last).count());
            last = now;
        }

        Check(worstGap < 100, "the owner's loop stayed responsive for the whole run (worst iteration gap < 100 ms)");
        Check(MsSince(t0) >= 590, "...and the job really did take its full time (nothing was skipped)");
    }

    std::printf("\nNothing wakes an idle owner:\n");
    {
        OperationWorker worker;
        pollfd pfd{ worker.Fd(), POLLIN, 0 };
        Check(poll(&pfd, 1, 150) == 0, "with no operation finishing the fd stays quiet (no timer, no polling)");

        bool done = false;
        worker.Submit("x", [] { return OperationResult::Success(); }, [&](const OperationResult&) { done = true; });
        PumpUntil(worker, 1000, [&] { return done; });

        pfd = { worker.Fd(), POLLIN, 0 };
        Check(poll(&pfd, 1, 100) == 0, "after Dispatch() the fd is quiet again");
    }

    std::printf("\nOrder, and one operation at a time:\n");
    {
        OperationWorker worker;
        std::mutex m;
        std::vector<int> started, delivered;
        std::atomic<int> concurrent{0};
        std::atomic<int> maxConcurrent{0};
        int remaining = 20;

        for (int i = 0; i < 20; ++i)
        {
            worker.Submit("", [&, i]
            {
                int now = ++concurrent;
                maxConcurrent = std::max(maxConcurrent.load(), now);
                { std::lock_guard<std::mutex> l(m); started.push_back(i); }
                Sleep(2);
                --concurrent;
                return OperationResult::Success();
            },
            [&, i](const OperationResult&) { delivered.push_back(i); --remaining; });
        }

        PumpUntil(worker, 4000, [&] { return remaining == 0; });
        bool ordered = started.size() == 20 && delivered.size() == 20;
        for (int i = 0; ordered && i < 20; ++i)
            ordered = started[i] == i && delivered[i] == i;

        Check(ordered, "20 jobs start and complete in exactly submission order, each exactly once");
        Check(maxConcurrent == 1, "never two jobs at the same time");
    }

    std::printf("\nDuplicate operations are refused until the first one has been delivered:\n");
    {
        OperationWorker worker;
        std::atomic<int> runs{0};
        std::atomic<bool> release{false};
        bool done = false;

        bool first = worker.Submit("connect:AA", [&] { ++runs; while (!release) Sleep(2); return OperationResult::Success(); },
                                   [&](const OperationResult&) { done = true; });
        Sleep(30); // the first job is now running
        bool dupRunning = worker.Submit("connect:AA", [&] { ++runs; return OperationResult::Success(); }, nullptr);
        bool otherKey   = worker.Submit("connect:BB", [&] { ++runs; return OperationResult::Success(); }, nullptr);

        Check(first && !dupRunning, "a second click while the first is RUNNING is refused");
        Check(otherKey, "a different operation (another device) is accepted");
        Check(worker.IsBusy("connect:AA") && worker.IsBusy("connect:BB") && !worker.IsBusy("connect:CC"), "IsBusy() reports exactly the pending keys");

        bool dupQueued = worker.Submit("connect:BB", [&] { ++runs; return OperationResult::Success(); }, nullptr);
        Check(!dupQueued, "a duplicate of a QUEUED operation is refused too");

        release = true;
        PumpUntil(worker, 3000, [&] { return !worker.Busy(); });
        Check(done && runs == 2, "the refused duplicates never ran (2 jobs total)");
        Check(!worker.IsBusy("connect:AA"), "the key is free once delivered");
        Check(worker.Submit("connect:AA", [] { return OperationResult::Success(); }, nullptr), "...and can be submitted again");
        PumpUntil(worker, 2000, [&] { return !worker.Busy(); });
    }

    std::printf("\nA completion may submit the next step (pair, then connect):\n");
    {
        OperationWorker worker;
        std::vector<std::string> log;
        bool finished = false;

        worker.Submit("dev", [] { return OperationResult::Success("paired"); },
            [&](const OperationResult& r)
            {
                log.push_back(r.message);
                // same key, from inside its own completion: allowed (released just before)
                bool accepted = worker.Submit("dev", [] { return OperationResult::Success("connected"); },
                    [&](const OperationResult& r2) { log.push_back(r2.message); finished = true; });
                log.push_back(accepted ? "resubmitted" : "REFUSED");
            });

        PumpUntil(worker, 2000, [&] { return finished; });
        Check(log.size() == 3 && log[0] == "paired" && log[1] == "resubmitted" && log[2] == "connected", "pair -> (completion submits connect) -> connected, no deadlock");
    }

    std::printf("\nFailures are reported, not swallowed:\n");
    {
        OperationWorker worker;
        OperationResult failure, thrown;
        int delivered = 0;

        worker.Submit("f", [] { return OperationResult::Failure("org.bluez.Error.Failed: Page Timeout"); },
                      [&](const OperationResult& r) { failure = r; ++delivered; });
        worker.Submit("t", []() -> OperationResult { throw std::runtime_error("boom"); },
                      [&](const OperationResult& r) { thrown = r; ++delivered; });

        PumpUntil(worker, 2000, [&] { return delivered == 2; });
        Check(!failure.ok && failure.message == "org.bluez.Error.Failed: Page Timeout", "a failing job's message reaches the completion");
        Check(!thrown.ok && thrown.message.find("boom") != std::string::npos, "a job that throws becomes a failure result (the worker survives)");

        bool after = false;
        worker.Submit("ok", [] { return OperationResult::Success(); }, [&](const OperationResult&) { after = true; });
        Check(PumpUntil(worker, 1000, [&] { return after; }), "and the worker still serves later jobs");
    }

    std::printf("\nCancellation only claims what is true:\n");
    {
        OperationWorker worker;
        std::atomic<bool> release{false};
        std::atomic<int> ran{0};
        std::vector<std::string> results;

        worker.Submit("running", [&] { ++ran; while (!release) Sleep(2); return OperationResult::Success("finished"); },
                      [&](const OperationResult& r) { results.push_back("running:" + r.message); });
        worker.Submit("q1", [&] { ++ran; return OperationResult::Success(); }, [&](const OperationResult& r) { results.push_back("q1:" + r.message); });
        worker.Submit("q2", [&] { ++ran; return OperationResult::Success(); }, [&](const OperationResult& r) { results.push_back("q2:" + r.message); });
        Sleep(40);

        std::size_t cancelled = worker.CancelQueued();
        Check(cancelled == 2, "two queued (not started) jobs are cancelled");
        Check(results.size() == 2 && results[0] == "q1:cancelled" && results[1] == "q2:cancelled", "their completions say 'cancelled'");
        Check(worker.IsBusy("running") && !worker.IsBusy("q1"), "the running job is NOT cancelled - it is still busy");

        release = true;
        PumpUntil(worker, 2000, [&] { return !worker.Busy(); });
        Check(ran == 1 && results.back() == "running:finished", "the running job finished normally and the cancelled ones never ran");
    }

    std::printf("\nDestroying the owner never waits for a running job:\n");
    {
        std::atomic<bool> jobFinished{false};
        auto* worker = new OperationWorker();
        worker->Submit("long", [&] { Sleep(500); jobFinished = true; return OperationResult::Success(); },
                       [](const OperationResult&) { std::printf("  (completion ran after destruction - BUG)\n"); std::fflush(stdout); });
        Sleep(30);

        auto t0 = Clock::now();
        delete worker;
        double destroyMs = MsSince(t0);

        Check(destroyMs < 50, "the destructor returns at once although a 500 ms job is running (window can close)");
        Check(!jobFinished, "...the job was still running when the owner was destroyed");
        Sleep(700);
        Check(jobFinished, "the abandoned job finishes on its own and the process stays healthy (no crash, no completion callback)");
    }


    std::printf("\nIndependent lanes do not wait for each other; one lane stays strictly ordered:\n");
    {
        OperationWorker worker;
        std::atomic<bool> release{false};
        std::atomic<bool> laneOneRan{false};
        std::vector<std::string> order;
        int delivered = 0;

        worker.Submit("slow-device-op", [&] { while (!release) Sleep(2); return OperationResult::Success(); },
                      [&](const OperationResult&) { order.push_back("lane0-slow"); ++delivered; }, 0);
        Sleep(30);

        auto t0 = Clock::now();
        worker.Submit("adapter-toggle", [&] { laneOneRan = true; return OperationResult::Success(); },
                      [&](const OperationResult&) { order.push_back("lane1-fast"); ++delivered; }, 1);

        PumpUntil(worker, 1500, [&] { return delivered >= 1; });
        Check(laneOneRan && MsSince(t0) < 500, "a quick operation on lane 1 finishes while lane 0 is still stuck in a long one");
        Check(order.size() == 1 && order[0] == "lane1-fast", "...and its completion is delivered first");
        Check(worker.IsBusy("slow-device-op"), "the long lane-0 operation is still running");

        // Same lane: strictly after.
        std::atomic<int> sameLaneStarted{0};
        worker.Submit("behind", [&] { sameLaneStarted = 1; return OperationResult::Success(); },
                      [&](const OperationResult&) { order.push_back("lane0-behind"); ++delivered; }, 0);
        Sleep(60);
        Check(sameLaneStarted == 0, "a second lane-0 operation waits behind the running one (ordering preserved)");

        release = true;
        PumpUntil(worker, 3000, [&] { return delivered == 3; });
        Check(order.size() == 3 && order[1] == "lane0-slow" && order[2] == "lane0-behind", "lane 0 completes in submission order");
    }

    std::printf("\nStress: many operations, every one delivered exactly once:\n");
    {
        OperationWorker worker;
        constexpr int kCount = 2000;
        std::vector<int> seen(kCount, 0);
        int delivered = 0;

        for (int i = 0; i < kCount; ++i)
            worker.Submit("", [] { return OperationResult::Success(); }, [&, i](const OperationResult&) { ++seen[i]; ++delivered; });

        PumpUntil(worker, 10000, [&] { return delivered == kCount; });
        bool once = true;
        for (int count : seen) once = once && count == 1;
        Check(delivered == kCount && once, "2000 operations: each completion delivered exactly once");
        Check(!worker.Busy(), "nothing left pending");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
