#include "DBusOperationQueue.h"

namespace Kohiko
{

DBusOperationQueue::DBusOperationQueue(DBusClient::Bus bus)
    : m_kind(bus)
{
}

OperationWorker::Job DBusOperationQueue::Wrap(Job job, int lane)
{
    // Called on the owner thread. Each lane gets its own connection, used
    // only by that lane's thread. The lambda holds its own reference to
    // it: if the window is destroyed while a job is mid-flight, the
    // worker thread finishes with a connection that is still valid and
    // releases it.
    std::shared_ptr<WorkerBus>& slot = m_buses[lane];

    if (!slot)
    {
        slot = std::make_shared<WorkerBus>();
        slot->kind = m_kind;
    }

    std::shared_ptr<WorkerBus> bus = slot;

    return [bus, job = std::move(job)]() -> OperationResult
    {
        // One thread per lane, jobs in a lane strictly serial: `bus` is
        // only ever used from that lane's thread, never concurrently.
        if (!bus->tried)
        {
            bus->tried = true;
            bus->connected = bus->client.Connect(bus->kind);
        }

        if (!bus->connected)
            return OperationResult::Failure("cannot connect to the D-Bus daemon");

        return job(bus->client);
    };
}

OperationResult DBusOperationQueue::FromCall(bool ok, const std::string& error, const std::string& fallback)
{
    if (ok)
        return OperationResult::Success();

    return OperationResult::Failure(error.empty() ? fallback : error);
}

}
