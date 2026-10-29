#include "OperationWorker.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <map>
#include <set>
#include <unordered_set>
#include <vector>

#include <sys/eventfd.h>
#include <unistd.h>

namespace Kohiko
{

namespace
{

struct Item
{
    int lane = 0;
    std::string key;
    OperationWorker::Job job;
    OperationWorker::Completion done;
};

struct Finished
{
    std::string key;
    OperationWorker::Completion done;
    OperationResult result;
};

}

// Shared between the owner and the worker thread. Held by shared_ptr so
// that a worker which is still inside a long job when the owner is
// destroyed keeps everything it touches (queue, mutex, eventfd) alive
// until it has finished and noticed it was abandoned.
struct OperationWorker::State
{
    std::mutex mutex;
    std::condition_variable wake;

    // One FIFO and one thread per lane. A lane's thread is started the
    // first time something is submitted to it.
    std::map<int, std::deque<Item>> queues;   // submitted, not started
    std::set<int> laneThreads;                // lanes whose thread exists
    std::deque<Finished> finished;   // done, completion not yet delivered
    std::unordered_set<std::string> keys; // queued + running + finished-undelivered
    int runningJobs = 0;             // jobs executing right now (at most one per lane)
    bool abandoned = false;          // the owner is gone: discard results, exit when idle

    int eventFd = -1;
    std::size_t pending = 0;         // queued + running + finished-undelivered

    ~State()
    {
        if (eventFd >= 0)
            close(eventFd);
    }

    void Signal()
    {
        if (eventFd < 0)
            return;

        std::uint64_t one = 1;
        ssize_t written = write(eventFd, &one, sizeof(one));
        (void)written; // eventfd counter overflow is not a failure mode we can hit
    }

    void WorkerMain(int lane)
    {
        for (;;)
        {
            Item item;

            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this, lane] { return abandoned || !queues[lane].empty(); });

                if (queues[lane].empty())
                    return; // abandoned and nothing left to do

                item = std::move(queues[lane].front());
                queues[lane].pop_front();
                ++runningJobs;
            }

            OperationResult result;

            try
            {
                result = item.job ? item.job() : OperationResult::Failure("no operation");
            }
            catch (const std::exception& error)
            {
                result = OperationResult::Failure(std::string("operation failed: ") + error.what());
            }
            catch (...)
            {
                result = OperationResult::Failure("operation failed");
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                --runningJobs;

                if (abandoned)
                    continue; // nobody is listening any more; drop the result and look for more (none will be run)

                finished.push_back({ std::move(item.key), std::move(item.done), std::move(result) });
            }

            Signal();
        }
    }
};

OperationWorker::OperationWorker()
    : m_state(std::make_shared<State>())
{
    // No thread yet: each lane's thread is started by its first Submit().
    m_state->eventFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
}

OperationWorker::~OperationWorker()
{
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->abandoned = true;
        m_state->queues.clear();   // not started: simply never run
    }

    m_state->wake.notify_all();
    // No join: a job that is mid-flight is a blocking call we cannot
    // interrupt, and the owner is a window that is closing. The worker
    // thread holds its own reference to State and exits by itself.
}

bool OperationWorker::Submit(const std::string& key, Job job, Completion done, int lane)
{
    bool startThread = false;

    {
        std::lock_guard<std::mutex> lock(m_state->mutex);

        if (!key.empty() && m_state->keys.count(key))
            return false;

        if (!key.empty())
            m_state->keys.insert(key);

        ++m_state->pending;
        m_state->queues[lane].push_back({ lane, key, std::move(job), std::move(done) });

        startThread = m_state->laneThreads.insert(lane).second;
    }

    if (startThread)
    {
        std::shared_ptr<State> state = m_state;
        std::thread([state, lane] { state->WorkerMain(lane); }).detach();
    }

    m_state->wake.notify_all();   // every lane's thread waits on the one condition variable
    return true;
}

bool OperationWorker::IsBusy(const std::string& key) const
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return !key.empty() && m_state->keys.count(key) > 0;
}

bool OperationWorker::Busy() const
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->pending > 0;
}

std::size_t OperationWorker::Pending() const
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->pending;
}

int OperationWorker::Fd() const
{
    return m_state->eventFd;
}

void OperationWorker::Dispatch()
{
    // Drain the wakeup first, then take what is finished: a job that
    // finishes between the two re-arms the eventfd, so its completion
    // is picked up by the next poll() rather than lost.
    if (m_state->eventFd >= 0)
    {
        std::uint64_t counter = 0;
        ssize_t bytesRead = read(m_state->eventFd, &counter, sizeof(counter));
        (void)bytesRead;
    }

    std::deque<Finished> ready;

    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        ready.swap(m_state->finished);
    }

    for (Finished& entry : ready)
    {
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);

            if (!entry.key.empty())
                m_state->keys.erase(entry.key);

            --m_state->pending;
        }

        if (entry.done)
            entry.done(entry.result);
    }
}

std::size_t OperationWorker::CancelQueued()
{
    std::deque<Item> cancelled;

    {
        std::lock_guard<std::mutex> lock(m_state->mutex);

        for (auto& entry : m_state->queues)
        {
            for (Item& item : entry.second)
                cancelled.push_back(std::move(item));

            entry.second.clear();
        }

        for (const Item& item : cancelled)
        {
            if (!item.key.empty())
                m_state->keys.erase(item.key);

            --m_state->pending;
        }
    }

    for (Item& item : cancelled)
    {
        if (item.done)
            item.done(OperationResult::Failure("cancelled"));
    }

    return cancelled.size();
}

}
