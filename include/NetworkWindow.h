#pragma once

#include "AppConfigStore.h"
#include "AppInstanceLock.h"
#include "NetworkManagerClient.h"
#include "NotificationClient.h"
#include "UiScrollView.h"
#include "UiSidebar.h"
#include "UiWindow.h"

namespace Kohiko
{

// kohiko-network's top-level app class. Three Sidebar pages -
// Wi-Fi, Ethernet, VPN - each a scrollable list built straight from
// NetworkManagerClient's latest snapshot, the same "rebuild the page
// from the current snapshot" shape AudioWindow uses for its Output/
// Input pages. Airplane mode (radios on/off) lives as a toggle at the
// top of the Wi-Fi page rather than its own sidebar entry, since it's
// a single switch rather than something with its own list of things
// to show.
class NetworkWindow
{
public:

    NetworkWindow();

    bool Initialize();
    void Run();

private:

    enum class Page { WiFi, Ethernet, Vpn };

    void RebuildChrome();
    void RebuildPage();
    void RebuildWiFiPage(Widget* content, int& y, int contentWidth);
    void RebuildEthernetPage(Widget* content, int& y, int contentWidth);
    void RebuildVpnPage(Widget* content, int& y, int contentWidth);

    // Shows a small modal password prompt (see NetworkWindow.cpp) and
    // attempts to connect `ssid` on `devicePath`/`apPath` with
    // whatever was entered - a self-contained one-off, not part of
    // the shared UI toolkit, since no other app needs free-text input.
    void PromptAndConnect(const std::string& devicePath, const std::string& apPath, const std::string& ssid, bool secured);

    UiWindow m_window;
    NetworkManagerClient m_networkManager;
    AppInstanceLock m_instanceLock;
    AppConfigStore m_settings;
    NotificationClient m_notifications;

    Page m_currentPage = Page::WiFi;
    Sidebar* m_sidebar = nullptr;
    Label* m_headerLabel = nullptr;
    ScrollView* m_scrollView = nullptr;

    bool m_networkDirty = false;
};

}
