#include "ScreenSaverInhibitor.h"

#include "Logger.h"

#ifdef KOHIKO_HAVE_DBUS
#include <dbus/dbus.h>
#endif

namespace Kohiko
{

#ifdef KOHIKO_HAVE_DBUS

namespace
{

// Both interfaces' Introspect() reply - just enough for a well-
// behaved client that introspects before calling Inhibit()/
// UnInhibit() (most don't bother; they just call it directly, since
// both interfaces are a de-facto standard already).
const char* const kIntrospectXml =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.1//EN\"\n"
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
    "    <method name=\"Introspect\">\n"
    "      <arg name=\"xml_data\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.ScreenSaver\">\n"
    "    <method name=\"Inhibit\">\n"
    "      <arg name=\"application_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"reason_for_inhibit\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"cookie\" type=\"u\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"UnInhibit\">\n"
    "      <arg name=\"cookie\" type=\"u\" direction=\"in\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "</node>\n";

DBusHandlerResult FilterCallback(DBusConnection* connection, DBusMessage* message, void* userData)
{
    static_cast<ScreenSaverInhibitor*>(userData)->HandleMessage(connection, message);

    // Every call this service actually understands gets a definitive
    // reply (a return value or an explicit error) inside
    // HandleMessage() itself, so nothing else on the bus needs a
    // chance to also see it - but see HandleMessage()'s own comment
    // for the one case (a totally unrelated signal) where this still
    // matters less than it sounds.
    return DBUS_HANDLER_RESULT_HANDLED;
}

}

#endif // KOHIKO_HAVE_DBUS

ScreenSaverInhibitor::ScreenSaverInhibitor()
{
}

ScreenSaverInhibitor::~ScreenSaverInhibitor()
{
#ifdef KOHIKO_HAVE_DBUS
    if (m_connection)
    {
        dbus_connection_close(m_connection);
        dbus_connection_unref(m_connection);
    }
#endif
}

void ScreenSaverInhibitor::Initialize()
{
#ifdef KOHIKO_HAVE_DBUS

    DBusError error;
    dbus_error_init(&error);

    // A *private* connection (as opposed to the shared one
    // dbus_bus_get() hands out) - Kohiko is this connection's only
    // owner and only user, so it's the one responsible for closing it
    // again in the destructor, which dbus_bus_get()'s shared
    // connections deliberately don't allow.
    m_connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);

    if (!m_connection)
    {
        // No session bus at all - entirely normal for a bare `startx`
        // session with no systemd --user/dbus-launch running. Not an
        // error worth logging; display-sleep inhibition just falls
        // back to the X11 fullscreen heuristic alone (see
        // WindowManager::CheckSleepInhibition()).
        dbus_error_free(&error);
        return;
    }

    // Kohiko manages this connection's lifetime itself (via Fd()/
    // Dispatch() on the main event loop) - it must never have libdbus
    // unilaterally exit() the whole process just because the session
    // bus went away.
    dbus_connection_set_exit_on_disconnect(m_connection, FALSE);

    bool gotScreenSaverName = RequestName("org.freedesktop.ScreenSaver");
    bool gotPowerManagementName = RequestName("org.freedesktop.PowerManagement");

    if (!gotScreenSaverName && !gotPowerManagementName)
    {
        // Neither name was available - almost certainly because a
        // full desktop environment's own screensaver/power-management
        // service already owns them. Back off entirely rather than
        // fight over it; that existing service is already doing this
        // job.
        dbus_connection_close(m_connection);
        dbus_connection_unref(m_connection);
        m_connection = nullptr;
        return;
    }

    dbus_bus_add_match(
        m_connection,
        "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged'",
        &error);
    dbus_error_free(&error); // a failed add_match just means a crashed inhibitor's cookie leaks until UnInhibit/restart - not fatal

    dbus_connection_add_filter(m_connection, FilterCallback, this, nullptr);

    m_available = true;

#endif
}

bool ScreenSaverInhibitor::Available() const
{
    return m_available;
}

bool ScreenSaverInhibitor::AnyActiveInhibit() const
{
    return !m_inhibitors.empty();
}

int ScreenSaverInhibitor::Fd() const
{
#ifdef KOHIKO_HAVE_DBUS

    if (!m_available)
        return -1;

    int fd = -1;

    if (!dbus_connection_get_unix_fd(m_connection, &fd))
        return -1;

    return fd;

#else

    return -1;

#endif
}

void ScreenSaverInhibitor::Dispatch()
{
#ifdef KOHIKO_HAVE_DBUS

    if (!m_available)
        return;

    // Non-blocking (timeout 0): read whatever's already arrived on
    // the socket, dispatch every complete message that produces to
    // this service's own filter (see FilterCallback()), and stop the
    // moment there's nothing left rather than waiting for more.
    while (dbus_connection_read_write_dispatch(m_connection, 0))
    {
        if (dbus_connection_get_dispatch_status(m_connection) != DBUS_DISPATCH_DATA_REMAINS)
            break;
    }

#endif
}

bool ScreenSaverInhibitor::RequestName(
    const char* busName)
{
#ifdef KOHIKO_HAVE_DBUS

    DBusError error;
    dbus_error_init(&error);

    // DO_NOT_QUEUE (never DBUS_NAME_FLAG_REPLACE_EXISTING): if
    // there's already an owner, this call simply fails rather than
    // ever taking the name away from an existing provider - see
    // Initialize()'s own comment on backing off entirely in that case.
    int result = dbus_bus_request_name(m_connection, busName, DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
    dbus_error_free(&error);

    return result == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER;

#else

    (void)busName;
    return false;

#endif
}

#ifdef KOHIKO_HAVE_DBUS

void ScreenSaverInhibitor::HandleMessage(
    DBusConnection* connection,
    void* messageVoid)
{
    auto* message = static_cast<DBusMessage*>(messageVoid);

    // Deliberately lenient about *which* of the two interfaces (or
    // which object path) a call arrives on - both have the exact same
    // signature and meaning (see this class's own header comment), so
    // a call is handled purely by its method name. Every real client
    // this exists for calls Inhibit()/UnInhibit() directly without
    // ever introspecting first anyway.
    const char* member = dbus_message_get_member(message);

    if (!member)
        return; // a non-method-call message this filter also sees, e.g. a reply to some other request

    if (dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_SIGNAL)
    {
        if (std::string(member) != "NameOwnerChanged")
            return;

        const char* name = nullptr;
        const char* oldOwner = nullptr;
        const char* newOwner = nullptr;

        DBusError error;
        dbus_error_init(&error);

        if (!dbus_message_get_args(
                message, &error,
                DBUS_TYPE_STRING, &name,
                DBUS_TYPE_STRING, &oldOwner,
                DBUS_TYPE_STRING, &newOwner,
                DBUS_TYPE_INVALID))
        {
            dbus_error_free(&error);
            return;
        }

        // An empty new owner means `name` just disappeared from the
        // bus entirely - if that was one of our inhibitors' unique
        // connection names, drop every cookie it held. This is the
        // whole reason a crashed browser can never wedge sleep
        // inhibition on forever.
        if (newOwner && newOwner[0] == '\0' && name)
        {
            std::string disappeared = name;

            for (auto it = m_inhibitors.begin(); it != m_inhibitors.end(); )
                it = (it->second == disappeared) ? m_inhibitors.erase(it) : std::next(it);
        }

        return;
    }

    if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return;

    std::string methodName = member;

    if (methodName == "Introspect")
    {
        DBusMessage* reply = dbus_message_new_method_return(message);
        const char* xml = kIntrospectXml;
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID);
        dbus_connection_send(connection, reply, nullptr);
        dbus_message_unref(reply);
        return;
    }

    if (methodName == "Inhibit")
    {
        const char* appName = "";
        const char* reason = "";

        DBusError error;
        dbus_error_init(&error);

        dbus_message_get_args(
            message, &error,
            DBUS_TYPE_STRING, &appName,
            DBUS_TYPE_STRING, &reason,
            DBUS_TYPE_INVALID);
        dbus_error_free(&error); // malformed args just means an empty appName/reason below - still honor the inhibit

        std::uint32_t cookie = m_nextCookie++;

        const char* sender = dbus_message_get_sender(message);
        m_inhibitors[cookie] = sender ? sender : "";

        Logger::Info(
            "Display sleep inhibited by \"" + std::string(appName) + "\"" +
            (reason && reason[0] ? (" (" + std::string(reason) + ")") : std::string()));

        DBusMessage* reply = dbus_message_new_method_return(message);
        dbus_message_append_args(reply, DBUS_TYPE_UINT32, &cookie, DBUS_TYPE_INVALID);
        dbus_connection_send(connection, reply, nullptr);
        dbus_message_unref(reply);
        return;
    }

    if (methodName == "UnInhibit")
    {
        std::uint32_t cookie = 0;

        DBusError error;
        dbus_error_init(&error);

        if (dbus_message_get_args(message, &error, DBUS_TYPE_UINT32, &cookie, DBUS_TYPE_INVALID))
            m_inhibitors.erase(cookie);

        dbus_error_free(&error);

        DBusMessage* reply = dbus_message_new_method_return(message);
        dbus_connection_send(connection, reply, nullptr);
        dbus_message_unref(reply);
        return;
    }

    // Anything else addressed to us (HasInhibit(), GetActive(), ...
    // some clients probe for extra methods GNOME's own implementation
    // happens to have) gets an explicit "unknown method" error rather
    // than silently never replying, so a well-behaved caller doesn't
    // just hang waiting for a timeout.
    if (!dbus_message_get_no_reply(message))
    {
        DBusMessage* reply = dbus_message_new_error(
            message, DBUS_ERROR_UNKNOWN_METHOD, "Method not supported by Kohiko's inhibit service");
        dbus_connection_send(connection, reply, nullptr);
        dbus_message_unref(reply);
    }
}

#endif // KOHIKO_HAVE_DBUS

}
