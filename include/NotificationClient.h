#pragma once

#include "DBusClient.h"

#include <string>

namespace Kohiko
{

// Thin wrapper over org.freedesktop.Notifications.Notify - the one
// piece of "notification helpers" shared infrastructure the spec
// calls out. Every tray widget uses this the same way: a short-lived
// DBusClient on the session bus, fire off one notification, done -
// there's no need to track notification IDs for update/replace here
// since none of the three tray widgets currently need to update a
// notification in place (a battery-low warning, a "device connected"
// toast, ... are all fire-and-forget), but Notify()'s return value is
// still handed back in case a future caller wants it.
class NotificationClient
{
public:

    NotificationClient();
    ~NotificationClient();

    bool Connect();
    bool Available() const;

    // `iconName` is a freedesktop icon-theme name (e.g.
    // "audio-volume-high", "network-wireless", "bluetooth-active") -
    // every desktop notification daemon (including Kohiko's own, once
    // one exists) resolves these the same way regular application
    // icons are resolved elsewhere in Kohiko (see IconResolver.h).
    // `timeoutMs` of 0 requests a server-decided default; -1 requests
    // "never expire".
    std::uint32_t Notify(
        const std::string& appName,
        const std::string& iconName,
        const std::string& summary,
        const std::string& body,
        int timeoutMs = -1
    );

private:

    DBusClient m_bus;
};

}
