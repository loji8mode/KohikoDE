#pragma once

#include "AppConfigStore.h"
#include "AppInstanceLock.h"
#include "BluezClient.h"
#include "NotificationClient.h"
#include "UiScrollView.h"
#include "UiSidebar.h"
#include "UiWindow.h"

namespace Kohiko
{

// kohiko-bluetooth's top-level app class. A single "Devices" page for
// now (see RebuildPage()) - the Sidebar exists anyway (rather than a
// bare header+list) so the spec's own "future additions" (BLE tools,
// file transfer, audio codec information, advanced adapter
// configuration) each become one more Sidebar entry later without
// reshaping the window itself, the same reasoning AudioWindow's and
// NetworkWindow's Sidebars already follow.
//
// Pairing/connecting to a Bluetooth device is a genuinely slow,
// blocking D-Bus call (BluezClient::PairDevice()/ConnectDevice() use
// generous timeouts precisely because real hardware pairing can take
// several seconds - see BluezClient.cpp) - this window accepts that a
// click on such a row will make the whole app briefly unresponsive
// rather than build a full async-with-progress-spinner system for
// it, the same "synchronous D-Bus calls are fine for a foreground UI
// action" trade-off DBusClient's own header comment already makes
// explicit.
class BluetoothWindow
{
public:

    BluetoothWindow();

    bool Initialize();
    void Run();

private:

    void RebuildChrome();
    void RebuildPage();

    UiWindow m_window;
    BluezClient m_bluez;
    AppInstanceLock m_instanceLock;
    AppConfigStore m_settings;
    NotificationClient m_notifications;

    Sidebar* m_sidebar = nullptr;
    ScrollView* m_scrollView = nullptr;

    bool m_bluezDirty = false;
};

}
