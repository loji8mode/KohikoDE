#include "SessionLockBridge.h"

#ifdef KOHIKO_HAVE_DBUS
#include <dbus/dbus.h>
#endif

#include <unistd.h>

#include <cstdint>

namespace Kohiko
{

#ifdef KOHIKO_HAVE_DBUS

namespace
{

constexpr const char* kLogindDestination = "org.freedesktop.login1";
constexpr const char* kManagerPath = "/org/freedesktop/login1";
constexpr const char* kManagerInterface = "org.freedesktop.login1.Manager";
constexpr const char* kSessionInterface = "org.freedesktop.login1.Session";

DBusHandlerResult FilterCallback(DBusConnection* connection, DBusMessage* message, void* userData)
{
    static_cast<SessionLockBridge*>(userData)->HandleMessage(connection, message);

    // Same reasoning as ScreenSaverInhibitor::FilterCallback(): the
    // one signal this cares about is always fully handled right here,
    // so nothing else on the bus needs a chance to also see it.
    return DBUS_HANDLER_RESULT_HANDLED;
}

}

#endif // KOHIKO_HAVE_DBUS

SessionLockBridge::SessionLockBridge()
{
}

SessionLockBridge::~SessionLockBridge()
{
#ifdef KOHIKO_HAVE_DBUS
    if (m_connection)
    {
        dbus_connection_close(m_connection);
        dbus_connection_unref(m_connection);
    }
#endif
}

void SessionLockBridge::Initialize()
{
#ifdef KOHIKO_HAVE_DBUS

    DBusError error;
    dbus_error_init(&error);

    // A private connection, same reasoning as ScreenSaverInhibitor's
    // own - Kohiko owns this connection's whole lifetime.
    m_connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);

    if (!m_connection)
    {
        // No session bus at all - entirely normal outside a full
        // login-manager-started session (a bare `startx`, say). Not
        // an error worth logging, same as ScreenSaverInhibitor.
        dbus_error_free(&error);
        return;
    }

    dbus_connection_set_exit_on_disconnect(m_connection, FALSE);

    // Resolve *this* process's own logind session - GetSessionByPID()
    // rather than reading $XDG_SESSION_ID, so this still works even
    // when that variable isn't set (see this class's header comment).
    // A short, bounded, one-time blocking call is acceptable here for
    // the same reason Authenticator's own PAM check is synchronous -
    // this only ever happens once, during startup.
    std::uint32_t pid = static_cast<std::uint32_t>(getpid());

    DBusMessage* request = dbus_message_new_method_call(
        kLogindDestination, kManagerPath, kManagerInterface, "GetSessionByPID");

    if (!request)
    {
        dbus_connection_close(m_connection);
        dbus_connection_unref(m_connection);
        m_connection = nullptr;
        return;
    }

    dbus_message_append_args(request, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INVALID);

    DBusMessage* reply = dbus_connection_send_with_reply_and_block(m_connection, request, 2000, &error);
    dbus_message_unref(request);

    if (!reply)
    {
        // No logind on this system at all, or this process isn't a
        // session logind is tracking - neither is an error, just an
        // environment where this integration has nothing to do. See
        // this class's own header comment.
        dbus_error_free(&error);
        dbus_connection_close(m_connection);
        dbus_connection_unref(m_connection);
        m_connection = nullptr;
        return;
    }

    const char* sessionPath = nullptr;

    bool gotPath = dbus_message_get_args(
        reply, &error, DBUS_TYPE_OBJECT_PATH, &sessionPath, DBUS_TYPE_INVALID);

    dbus_error_free(&error);

    if (gotPath && sessionPath)
        m_sessionPath = sessionPath;

    dbus_message_unref(reply);

    if (m_sessionPath.empty())
    {
        dbus_connection_close(m_connection);
        dbus_connection_unref(m_connection);
        m_connection = nullptr;
        return;
    }

    // Only this exact session's own Lock signal - never every
    // session's, which would let one user's Kohiko react to another
    // logged-in user's lock request on a shared multi-session machine.
    std::string matchRule =
        "type='signal',interface='" + std::string(kSessionInterface) +
        "',member='Lock',path='" + m_sessionPath + "'";

    dbus_bus_add_match(m_connection, matchRule.c_str(), &error);
    dbus_error_free(&error); // a failed add_match just means external Lock requests silently aren't relayed - not fatal, see NotifyLocked() which still works either way

    dbus_connection_add_filter(m_connection, FilterCallback, this, nullptr);

    m_available = true;

#endif
}

bool SessionLockBridge::Available() const
{
    return m_available;
}

int SessionLockBridge::Fd() const
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

void SessionLockBridge::Dispatch()
{
#ifdef KOHIKO_HAVE_DBUS

    if (!m_available)
        return;

    while (dbus_connection_read_write_dispatch(m_connection, 0))
    {
        if (dbus_connection_get_dispatch_status(m_connection) != DBUS_DISPATCH_DATA_REMAINS)
            break;
    }

#endif
}

void SessionLockBridge::SetLockRequestedCallback(
    LockRequestedCallback callback)
{
    m_lockRequested = std::move(callback);
}

void SessionLockBridge::NotifyLocked(
    bool locked)
{
#ifdef KOHIKO_HAVE_DBUS

    if (!m_available)
        return;

    DBusMessage* message = dbus_message_new_method_call(
        kLogindDestination, m_sessionPath.c_str(), kSessionInterface, "SetLockedHint");

    if (!message)
        return;

    dbus_bool_t value = locked ? TRUE : FALSE;
    dbus_message_append_args(message, DBUS_TYPE_BOOLEAN, &value, DBUS_TYPE_INVALID);

    // Fire-and-forget: nothing here depends on logind having actually
    // received this by any particular time, or at all - LockScreen's
    // own state is already the real source of truth (see this class's
    // header comment), this is purely informational for external
    // consumers (loginctl, a display manager's user switcher).
    dbus_message_set_no_reply(message, TRUE);
    dbus_connection_send(m_connection, message, nullptr);
    dbus_message_unref(message);
    dbus_connection_flush(m_connection);

#else

    (void)locked;

#endif
}

#ifdef KOHIKO_HAVE_DBUS

void SessionLockBridge::HandleMessage(
    DBusConnection* connection,
    void* messageVoid)
{
    (void)connection;

    auto* message = static_cast<DBusMessage*>(messageVoid);

    if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL)
        return;

    const char* member = dbus_message_get_member(message);

    if (!member || std::string(member) != "Lock")
        return;

    if (m_lockRequested)
        m_lockRequested();
}

#endif // KOHIKO_HAVE_DBUS

}
