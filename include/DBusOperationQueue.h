#pragma once

#include "DBusClient.h"
#include "OperationWorker.h"

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace Kohiko
{

// OperationWorker (see that header for the threading contract) for D-Bus
// operations: the jobs run on the worker thread against a PRIVATE
// DBusClient connection that only that thread ever touches. The
// window's own DBusClient - the one whose fd the event loop watches for
// signals - is never used from the worker, so nothing about libdbus is
// shared between threads beyond what libdbus itself documents as safe
// (DBusClient::Connect() calls dbus_threads_init_default()).
//
// The private connection is opened lazily, on the worker thread, the
// first time a job runs - so a window that never performs a slow
// operation never opens a second connection.
class DBusOperationQueue
{
public:

    using Job = std::function<OperationResult(DBusClient& bus)>;
    using Completion = OperationWorker::Completion;

    explicit DBusOperationQueue(DBusClient::Bus bus);

    // `lane`: jobs in one lane run one at a time in submission order;
    // different lanes run concurrently, each on its own thread with its
    // own private D-Bus connection (see OperationWorker). Use separate
    // lanes for work that must not wait on each other.
    bool Submit(const std::string& key, Job job, Completion done, int lane = 0)
    {
        return m_worker.Submit(key, Wrap(std::move(job), lane), std::move(done), lane);
    }

    bool IsBusy(const std::string& key) const { return m_worker.IsBusy(key); }
    bool Busy() const { return m_worker.Busy(); }
    int Fd() const { return m_worker.Fd(); }
    void Dispatch() { m_worker.Dispatch(); }
    std::size_t CancelQueued() { return m_worker.CancelQueued(); }

    // Turns one blocking Call()'s outcome into an OperationResult with
    // the bus error text (or `fallback` if the bus gave none).
    static OperationResult FromCall(bool ok, const std::string& error, const std::string& fallback);

private:

    struct WorkerBus
    {
        DBusClient client;
        DBusClient::Bus kind;
        bool tried = false;
        bool connected = false;
    };

    OperationWorker::Job Wrap(Job job, int lane);

    DBusClient::Bus m_kind;
    std::map<int, std::shared_ptr<WorkerBus>> m_buses; // one per lane; each used by that lane's thread only
    OperationWorker m_worker;
};

}
