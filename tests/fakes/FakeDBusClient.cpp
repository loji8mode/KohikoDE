// TEST DOUBLE - not part of Kohiko, never linked into a release binary.
//
// A scripted, in-process stand-in for the D-Bus daemon, implementing the
// public API of Kohiko::DBusClient (include/DBusClient.h) so the REAL
// native control apps - BluezClient, BluetoothWindow, the worker queue,
// UiWindow, the event loop - can be run and driven under Xvfb without a
// bus, BlueZ or NetworkManager. What a call does is decided by a script
// file the test rewrites while the app runs:
//
//   FAKEBUS_SCRIPT   one rule per line:   <Method> [delay=<ms>] [error=<text>]
//                    <Method> is the D-Bus method name (Pair, Connect,
//                    Disconnect, RemoveDevice, StartDiscovery) or
//                    Set:<Property> (Set:Powered). A call blocks for
//                    `delay` ms - exactly like the real, synchronous
//                    DBusClient::Call blocks its caller - then fails
//                    with `error` or succeeds. Unlisted: instant success.
//   FAKEBUS_LOG      appended: "<ms> <thread> <event> <method>" lines, where
//                    thread is "ui" (the process's main thread) or "worker"
//                    and event is begin/end/fail. This is how a test learns
//                    WHICH THREAD a slow call ran on and how many times.
//   FAKEBUS_STATE    the "device" the fake BlueZ exposes (Paired/Connected)
//                    is process-wide state shared by every DBusClient
//                    instance, like a real daemon, and changes when
//                    Pair/Connect/Disconnect/RemoveDevice succeed.
//
// Signals: every client that called Subscribe() gets a pipe; a state
// change writes to it and Dispatch() then runs the handlers
// ("PropertiesChanged"), as libdbus + a poll loop would.

#include "DBusClient.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace Kohiko
{

namespace
{

std::mutex g_mutex;

struct Device
{
    bool present = true;
    bool paired = true;
    bool connected = false;
};

Device g_device;
bool g_adapterPowered = true;
std::set<int> g_signalWriteFds;     // write ends of every subscribed client's pipe
std::atomic<int> g_connectionsOpened{0};

long NowMs()
{
    using namespace std::chrono;
    return static_cast<long>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() % 100000000);
}

const char* ThreadName()
{
    return syscall(SYS_gettid) == getpid() ? "ui" : "worker";
}

void Log(const char* event, const std::string& what)
{
    const char* path = std::getenv("FAKEBUS_LOG");
    if (!path)
        return;

    std::lock_guard<std::mutex> lock(g_mutex);
    std::ofstream out(path, std::ios::app);
    out << NowMs() << ' ' << ThreadName() << ' ' << event << ' ' << what << '\n';
}

struct Rule
{
    int delayMs = 0;
    std::string error;
    bool found = false;
};

Rule RuleFor(const std::string& method)
{
    Rule rule;
    const char* path = std::getenv("FAKEBUS_SCRIPT");
    if (!path)
        return rule;

    std::ifstream in(path);
    std::string line;

    while (std::getline(in, line))
    {
        std::istringstream words(line);
        std::string name;
        words >> name;

        if (name != method)
            continue;

        rule.found = true;
        std::string token;
        while (words >> token)
        {
            if (token.rfind("delay=", 0) == 0)
                rule.delayMs = std::atoi(token.c_str() + 6);
            else if (token.rfind("error=", 0) == 0)
            {
                rule.error = token.substr(6);
                std::string rest;
                std::getline(words, rest);
                rule.error += rest;
            }
        }
    }

    return rule;
}

void NotifyAll()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int fd : g_signalWriteFds)
    {
        char byte = 1;
        ssize_t written = write(fd, &byte, 1);
        (void)written;
    }
}

}

// The fake keeps the real class's data members unused; its own per-
// instance state lives in a side table keyed by `this`.
namespace
{
struct Side
{
    int readFd = -1;
    int writeFd = -1;
    std::vector<DBusClient::SignalHandler> handlers;
    bool available = false;
};

// Heap-allocated and keyed by client, so a reference handed out by
// SideOf() stays valid while other threads add or remove other clients'
// entries (a std::vector would reallocate under them).
std::mutex g_sideMutex;
std::map<const DBusClient*, std::unique_ptr<Side>> g_sides;

Side& SideOf(const DBusClient* client)
{
    std::lock_guard<std::mutex> lock(g_sideMutex);
    auto& slot = g_sides[client];

    if (!slot)
        slot = std::make_unique<Side>();

    return *slot;
}

void DropSide(const DBusClient* client)
{
    std::unique_ptr<Side> side;

    {
        std::lock_guard<std::mutex> lock(g_sideMutex);
        auto it = g_sides.find(client);

        if (it == g_sides.end())
            return;

        side = std::move(it->second);
        g_sides.erase(it);
    }

    if (side->readFd >= 0) close(side->readFd);

    if (side->writeFd >= 0)
    {
        { std::lock_guard<std::mutex> l(g_mutex); g_signalWriteFds.erase(side->writeFd); }
        close(side->writeFd);
    }
}
}

DBusClient::DBusClient() = default;

DBusClient::~DBusClient()
{
    DropSide(this);
}

bool DBusClient::Connect(Bus bus)
{
    Side& side = SideOf(this);
    side.available = true;
    ++g_connectionsOpened;
    Log("connect", bus == Bus::System ? "system" : "session");

    int fds[2];
    if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0)
    {
        side.readFd = fds[0];
        side.writeFd = fds[1];
    }

    m_available = true;
    return true;
}

bool DBusClient::Available() const
{
    return m_available;
}

int DBusClient::Fd() const
{
    return SideOf(this).readFd;
}

void DBusClient::Dispatch()
{
    Side& side = SideOf(this);

    char buffer[64];
    while (side.readFd >= 0 && read(side.readFd, buffer, sizeof(buffer)) > 0) {}

    std::vector<SignalHandler> handlers = side.handlers;
    for (auto& handler : handlers)
        handler("/org/bluez/hci0/dev_AA", "org.freedesktop.DBus.Properties", "PropertiesChanged", {});
}

void DBusClient::Subscribe(const std::string&, const std::string&, SignalHandler handler, const std::string&)
{
    Side& side = SideOf(this);
    side.handlers.push_back(std::move(handler));

    std::lock_guard<std::mutex> lock(g_mutex);
    if (side.writeFd >= 0)
        g_signalWriteFds.insert(side.writeFd);
}

void DBusClient::HandleMessage(DBusMessage*) {}

bool DBusClient::Call(
    const std::string&, const std::string& objectPath, const std::string&, const std::string& method,
    const std::vector<DBusValue>&, std::vector<DBusValue>*, std::string* errorOut, int)
{
    Log("begin", method + " " + objectPath);

    Rule rule = RuleFor(method);

    if (rule.delayMs > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(rule.delayMs));

    if (!rule.error.empty())
    {
        if (errorOut)
            *errorOut = rule.error;
        Log("fail", method);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (method == "Pair") g_device.paired = true;
        else if (method == "Connect") g_device.connected = true;
        else if (method == "Disconnect") g_device.connected = false;
        else if (method == "RemoveDevice") { g_device.present = false; g_device.connected = false; }
    }

    Log("end", method);

    if (method == "Pair" || method == "Connect" || method == "Disconnect" || method == "RemoveDevice")
        NotifyAll();

    return true;
}

bool DBusClient::GetProperty(const std::string&, const std::string&, const std::string&, const std::string&, DBusValue&)
{
    return false;
}

bool DBusClient::GetAllProperties(const std::string&, const std::string&, const std::string&, DBusValue&)
{
    return false;
}

bool DBusClient::SetProperty(
    const std::string&, const std::string&, const std::string&, const std::string& propertyName,
    const DBusValue& value, std::string* errorOut)
{
    std::string method = "Set:" + propertyName;
    Log("begin", method);

    Rule rule = RuleFor(method);
    if (rule.delayMs > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(rule.delayMs));

    if (!rule.error.empty())
    {
        if (errorOut)
            *errorOut = rule.error;
        Log("fail", method);
        return false;
    }

    if (propertyName == "Powered")
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_adapterPowered = value.AsBool();
    }

    Log("end", method);
    NotifyAll();
    return true;
}

bool DBusClient::GetManagedObjects(const std::string&, const std::string&, DBusValue& out)
{
    Device device;
    bool powered;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        device = g_device;
        powered = g_adapterPowered;
    }

    std::map<std::string, DBusValue> objects;

    objects["/org/bluez/hci0"] = DBusValue::MakeDict({
        { "org.bluez.Adapter1", DBusValue::MakeDict({
            { "Address", DBusValue::MakeString("00:11:22:33:44:55") },
            { "Alias", DBusValue::MakeString("TestAdapter") },
            { "Powered", DBusValue::MakeBool(powered) },
            { "Discoverable", DBusValue::MakeBool(false) },
            { "Discovering", DBusValue::MakeBool(false) },
        }) }
    });

    if (device.present)
    {
        objects["/org/bluez/hci0/dev_AA"] = DBusValue::MakeDict({
            { "org.bluez.Device1", DBusValue::MakeDict({
                { "Address", DBusValue::MakeString("AA:BB:CC:DD:EE:FF") },
                { "Alias", DBusValue::MakeString("Test Headset") },
                { "Icon", DBusValue::MakeString("audio-headset") },
                { "Paired", DBusValue::MakeBool(device.paired) },
                { "Connected", DBusValue::MakeBool(device.connected) },
                { "Trusted", DBusValue::MakeBool(false) },
            }) }
        });
    }

    out = DBusValue::MakeDict(std::move(objects));
    return true;
}

}
