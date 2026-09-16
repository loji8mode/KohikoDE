#include "NotificationClient.h"

namespace Kohiko
{

NotificationClient::NotificationClient() = default;
NotificationClient::~NotificationClient() = default;

bool NotificationClient::Connect()
{
    return m_bus.Connect(DBusClient::Bus::Session);
}

bool NotificationClient::Available() const
{
    return m_bus.Available();
}

std::uint32_t NotificationClient::Notify(
    const std::string& appName,
    const std::string& iconName,
    const std::string& summary,
    const std::string& body,
    int timeoutMs)
{
    if (!m_bus.Available())
        return 0;

    std::vector<DBusValue> reply;
    bool ok = m_bus.Call(
        "org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify",
        {
            DBusValue::MakeString(appName),
            DBusValue::MakeUInt32(0),                    // replaces_id - always a fresh notification
            DBusValue::MakeString(iconName),
            DBusValue::MakeString(summary),
            DBusValue::MakeString(body),
            DBusValue::MakeArray({}, 's'),                // actions - none
            DBusValue::MakeDict({}),                      // hints - none
            DBusValue::MakeInt32(timeoutMs)
        },
        &reply
    );

    if (!ok || reply.empty())
        return 0;

    return static_cast<std::uint32_t>(reply.front().AsUInt());
}

}
