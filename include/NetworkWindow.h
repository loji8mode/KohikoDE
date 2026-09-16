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
// Wi-Fi, Ethernet, VPN - each rebuilt straight from
// NetworkManagerClient's latest snapshot, the same "rebuild the page
// from the current snapshot" shape AudioWindow uses for its Output/
// Input pages. Airplane mode (radios on/off) lives as a toggle at the
// top of the Wi-Fi page rather than its own sidebar entry, since it's
// a single switch rather than something with its own list of things
// to show.
//
// Every page reflows on resize (see UiWindow::SetResizeHandler()) and
// uses the extra width a wider window provides to show *more*
// information rather than just stretching the same content - Wi-Fi
// rows reveal a numeric signal percentage once there's room for it,
// and Ethernet cards grow from a single-line summary into a proper
// labeled Address/Gateway/DNS/MAC grid (see BuildEthernetCard() in
// NetworkWindow.cpp).
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

    // Builds one Ethernet device's card - a single-line summary at
    // narrow widths, growing into a labeled Address/Gateway/DNS/MAC
    // grid once there's room (see NetworkWindow.cpp).
    std::unique_ptr<Widget> BuildEthernetCard(const NetworkDevice& device, int width);

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
    ScrollView* m_scrollView = nullptr;

    bool m_networkDirty = false;
};

}
