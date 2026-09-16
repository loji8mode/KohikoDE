#pragma once

#include "DBusValue.h"

#include <functional>
#include <string>
#include <vector>

// Kept opaque here for the same reason as ScreenSaverInhibitor.h - so
// this header stays cheap for callers that just hold/pass a
// DBusClient& around. Only DBusClient.cpp needs the real libdbus type.
struct DBusConnection;
struct DBusMessage;

namespace Kohiko
{

// A thin, synchronous-call-oriented wrapper around libdbus, shared by
// every new desktop-integration client (NetworkManagerClient,
// BluezClient, NotificationClient, PowerProfilesClient-if-ever-added,
// ...) so each of those stays focused on "what does this particular
// service's API look like" instead of re-deriving connection setup,
// argument marshalling, or signal-matching from scratch - exactly the
// duplication `docs/PROJECT_HISTORY.md`'s "Shared infrastructure"
// goal calls out.
//
// Deliberately synchronous for method calls (blocking, with a
// timeout) rather than fully async/pending-call-based: every call
// site in this codebase is a foreground UI action (list devices,
// toggle Wi-Fi, connect to a Bluetooth device, ...) where "block this
// one small app's event loop for up to a few seconds" is perfectly
// acceptable - exactly as blocking as the plain Xlib round-trips the
// rest of Kohiko already makes throughout WindowManager.cpp - and it
// keeps every backend client's control-flow linear instead of
// callback-fragmented. Signals (property/device changes) are the one
// thing that's genuinely asynchronous by nature, so those alone go
// through the registered-callback path instead (see Subscribe()
// below).
class DBusClient
{
public:

    enum class Bus
    {
        Session,
        System
    };

    using SignalHandler = std::function<void(
        const std::string& path,
        const std::string& interface,
        const std::string& member,
        const std::vector<DBusValue>& args
    )>;

    DBusClient();
    ~DBusClient();

    DBusClient(const DBusClient&) = delete;
    DBusClient& operator=(const DBusClient&) = delete;

    // Connects to the requested bus (System for NetworkManager/BlueZ,
    // Session for org.freedesktop.Notifications). Safe to call where
    // no such bus is running (a bare Xephyr test session, a container
    // with no system message bus) - Available() simply stays false
    // and every other method becomes a harmless no-op/false return
    // rather than throwing or crashing, same contract as
    // ScreenSaverInhibitor::Initialize().
    bool Connect(Bus bus);

    bool Available() const;

    // The fd to add to this process's own poll() loop (see
    // UiWindow::Run()) - -1 if !Available(). Becomes readable
    // whenever there's an incoming D-Bus message (almost always a
    // signal this client subscribed to - see Subscribe()) to process.
    int Fd() const;

    // Processes every pending incoming message, invoking whichever
    // Subscribe()d handler matches each signal. A no-op if
    // !Available(). Call whenever Fd() is readable.
    void Dispatch();

    // Blocking method call. `args` are appended to the message in
    // order; on success every top-level reply argument is appended to
    // `outReply` (may be null if the caller doesn't need the reply -
    // e.g. a `Set` property call). Returns false (and fills
    // `errorOut`, if given) on any D-Bus-level error (no such
    // object/method, permission denied, service not running, timeout,
    // ...) - callers treat that the same way as any other
    // "this feature isn't available right now" case rather than
    // crashing the whole app over one failed call.
    bool Call(
        const std::string& destination,
        const std::string& objectPath,
        const std::string& interface,
        const std::string& method,
        const std::vector<DBusValue>& args = {},
        std::vector<DBusValue>* outReply = nullptr,
        std::string* errorOut = nullptr,
        int timeoutMs = 4000
    );

    // org.freedesktop.DBus.Properties.Get - `out` is already unwrapped
    // (never a Type::Variant) for convenience, since every property
    // value D-Bus hands back is boxed in exactly one variant layer.
    bool GetProperty(
        const std::string& destination,
        const std::string& objectPath,
        const std::string& interface,
        const std::string& propertyName,
        DBusValue& out
    );

    // org.freedesktop.DBus.Properties.GetAll - `out` becomes a
    // Type::Dict whose values are still individually Get()-able
    // (DBusValue::Get()/AsString()/... already unwrap on read).
    bool GetAllProperties(
        const std::string& destination,
        const std::string& objectPath,
        const std::string& interface,
        DBusValue& out
    );

    // org.freedesktop.DBus.Properties.Set - `value` is boxed in a
    // variant automatically; callers just pass the plain value.
    bool SetProperty(
        const std::string& destination,
        const std::string& objectPath,
        const std::string& interface,
        const std::string& propertyName,
        const DBusValue& value
    );

    // org.freedesktop.DBus.ObjectManager.GetManagedObjects - the
    // primary way both NetworkManager and BlueZ hand back "every
    // object of interest, with every interface/property it currently
    // has" in a single round trip (see NetworkManagerClient::Refresh()/
    // BluezClient::Refresh()). `out` becomes a Type::Dict keyed by
    // object path, whose values are themselves Type::Dict keyed by
    // interface name, whose values are themselves Type::Dict keyed by
    // property name - i.e. exactly the a{oa{sa{sv}}} wire format,
    // just with everything already parsed.
    bool GetManagedObjects(
        const std::string& destination,
        const std::string& objectPath,
        DBusValue& out
    );

    // Registers `handler` for every incoming signal whose interface
    // and member match. `path`, if non-empty, additionally restricts
    // the match rule itself to signals from that exact object (used
    // e.g. for a single Bluetooth adapter's own PropertiesChanged,
    // as opposed to every object's). Safe to call multiple times with
    // different interface/member pairs; every match added this way
    // stays registered for this connection's lifetime.
    void Subscribe(
        const std::string& interface,
        const std::string& member,
        SignalHandler handler,
        const std::string& path = ""
    );

    // Public only because libdbus's C callback API needs a plain
    // function pointer to call into this - not a genuinely public
    // entry point, exactly like ScreenSaverInhibitor::HandleMessage().
    void HandleMessage(DBusMessage* message);

private:

    struct Subscription
    {
        std::string interface;
        std::string member;
        std::string path;
        SignalHandler handler;
    };

    bool m_available = false;
    DBusConnection* m_connection = nullptr;
    std::vector<Subscription> m_subscriptions;
};

}
