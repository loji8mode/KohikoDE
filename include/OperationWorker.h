#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace Kohiko
{

// What one background operation reports back. `ok == false` always
// comes with a `message` a UI can show as is ("Connection refused",
// "org.bluez.Error.Failed: ...", "timed out").
struct OperationResult
{
    bool ok = false;
    std::string message;

    static OperationResult Success(std::string message = std::string())
    {
        return { true, std::move(message) };
    }

    static OperationResult Failure(std::string message)
    {
        return { false, std::move(message) };
    }
};

// Runs slow, blocking calls (a Bluetooth pair/connect that takes 15 s, a
// NetworkManager secrets lookup that waits on a keyring prompt) OFF the
// thread that owns the window, and hands the result back to that thread
// without a timer and without polling.
//
// How it fits the apps' existing event loops (UiWindow::Run(),
// TrayIconClient::Run()): Fd() is an eventfd. Register it with
// WatchFd() exactly like the D-Bus or PipeWire fd; when it becomes
// readable call Dispatch(). Nothing wakes the loop while no operation
// is finishing, so an idle app still blocks in poll() indefinitely.
//
// Threading contract - the whole point of the class:
//   * Submit(), IsBusy(), Busy(), Pending(), CancelQueued(), Dispatch()
//     and the destructor are called only on the OWNER thread.
//   * A Job runs on the worker thread and must touch only state it
//     owns or was handed by value (its own D-Bus connection, copies of
//     the data it needs). It must never touch X11, the widget tree or
//     any member of the owning window - those belong to the owner
//     thread. Its only way back is its return value.
//   * A Completion runs on the owner thread, inside Dispatch(), after
//     the job has finished: free to touch the UI.
//
// Semantics:
//   * Jobs run one at a time per LANE, in submission order (a "pair"
//     followed by a "connect" on the same device cannot overtake each
//     other). Independent kinds of work are put on different lanes -
//     Submit()'s `lane` argument - so a 15-second Bluetooth connect
//     does not hold up toggling the adapter: each lane has its own
//     worker thread, started the first time the lane is used. Lane 0
//     is the default; a class that never names a lane has exactly one
//     thread, as before.
//   * Submit() with a non-empty `key` that is already queued or running
//     (or finished but not yet delivered) is refused - a double click
//     cannot start the same operation twice. The key is released just
//     before its completion runs, so the completion may submit again.
//   * CancelQueued() cancels only jobs that have NOT started: they
//     never run and their completions report "cancelled". A job that is
//     already running cannot be interrupted - it is a blocking call in
//     another thread - so it is left to finish and is never reported
//     as cancelled.
//   * Destroying the worker never waits for a running job. The thread
//     is detached, finishes its current job on its own and exits; the
//     result is dropped. (Closing a window must not hang for the length
//     of a Bluetooth connection attempt.)
class OperationWorker
{
public:

    using Job = std::function<OperationResult()>;
    using Completion = std::function<void(const OperationResult&)>;

    OperationWorker();
    ~OperationWorker();

    OperationWorker(const OperationWorker&) = delete;
    OperationWorker& operator=(const OperationWorker&) = delete;

    // Returns false (nothing is queued, `done` is never called) if `key`
    // is non-empty and already pending (in any lane).
    bool Submit(const std::string& key, Job job, Completion done, int lane = 0);

    bool IsBusy(const std::string& key) const;
    bool Busy() const;
    std::size_t Pending() const;

    // Readable when at least one completion is waiting for Dispatch(). -1 if unavailable.
    int Fd() const;

    // Runs every ready completion, in submission order, on the calling thread.
    void Dispatch();

    // See the class comment. Returns how many queued jobs were cancelled.
    std::size_t CancelQueued();

private:

    struct State;
    std::shared_ptr<State> m_state;
};

}
