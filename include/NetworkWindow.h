#pragma once

#include "AppConfigStore.h"
#include "AppInstanceLock.h"
#include "NetworkManagerClient.h"
#include "NotificationClient.h"
#include "UiScrollView.h"
#include "UiWindow.h"

namespace Kohiko
{

// Filters clipboard/PRIMARY-selection text pasted into the Wi-Fi
// password prompt down to what it will actually accept - every byte
// that isn't an ASCII control character (0x00-0x1f, plus DEL/0x7f),
// so a multi-byte UTF-8 character (any byte with the high bit set)
// passes through untouched while a stray trailing newline - what X11
// clipboard contents almost always end with when copied from a
// terminal or a password manager that appends one - does not. A free
// function, not inlined into the modal's own SelectionNotify handler
// in NetworkWindow.cpp, specifically so it's testable without a real
// X11 selection round trip - see tests/test_networkwindow.cpp.
std::string FilterPastedPasswordText(const std::string& raw);

// kohiko-network's top-level app class - a single Wi-Fi-focused page
// (toolbar with the Wi-Fi/VPN state and a refresh button, a live
// search field, an AVAILABLE NETWORKS list, and a details panel for
// whichever network is selected) matching the Kohiko Network mockup.
// Ethernet management and VPN profile management aren't part of that
// mockup, so - same as AudioWindow's level meters - they live on a
// second Advanced Settings page reached via the link at the bottom,
// rather than being dropped (see RebuildAdvancedPage()).
//
// The network list and its details panel sit side by side once the
// window is wide enough (see kSplitBreakpoint) and stack into a
// single column otherwise, same as every other page in these three
// apps - narrow windows never lose access to a selected network's
// details, they just see them below the list instead of beside it.
class NetworkWindow
{
public:

    NetworkWindow();

    bool Initialize();
    void Run();

private:

    enum class Page { Main, Advanced };
    enum class AdvancedTab { Ethernet, Vpn };

    void RebuildChrome();
    void RebuildPage();
    int RebuildMainPage(Widget& content, int contentWidth);
    int RebuildAdvancedPage(Widget& content, int contentWidth);

    // Appends the toolbar (Wi-Fi toggle, VPN status, refresh button)
    // to `content`, wrapping onto a second line once the window's too
    // narrow to fit all of it on one - returns the y position just
    // past it.
    int AppendToolbar(Widget& content, int y, int contentWidth);

    // Appends the AVAILABLE NETWORKS card and (once there's a
    // selection) the details panel, side by side or stacked depending
    // on `contentWidth` - returns the y position just past whichever
    // of the two ends up lower.
    int AppendNetworkSection(Widget& content, int y, int contentWidth);

    std::unique_ptr<Widget> BuildNetworkRow(const WiFiAccessPoint& ap, const std::string& devicePath, const Rect& rowBounds, bool showDivider, bool showPercent);
    std::unique_ptr<Widget> BuildDetailsPanel(const NetworkDevice& wifiDevice, const WiFiAccessPoint& ap, const Rect& panelBounds);

    // Builds one Ethernet device's card for the Advanced Settings
    // page - a single-line summary at narrow widths, growing into a
    // labeled Address/Gateway/DNS/MAC grid once there's room (see
    // NetworkWindow.cpp).
    std::unique_ptr<Widget> BuildEthernetCard(const NetworkDevice& device, const Rect& cardBounds);

    // Shows a small modal password prompt (see NetworkWindow.cpp) and
    // attempts to connect `ssid` on `devicePath`/`apPath` with
    // whatever was entered - a self-contained one-off, not part of
    // the shared UI toolkit, since no other app needs free-text input
    // in a modal (kohiko-network's live search field is a different,
    // non-modal use of TextField).
    void PromptAndConnect(const std::string& devicePath, const std::string& apPath, const std::string& ssid, bool secured);

    // Picks a sensible selected network after every rebuild: keeps
    // the current selection if it's still in range, otherwise falls
    // back to the active connection, otherwise the first network in
    // the (possibly search-filtered) list.
    void EnsureSelection(const std::vector<const WiFiAccessPoint*>& visibleAccessPoints);

    UiWindow m_window;
    NetworkManagerClient m_networkManager;
    AppInstanceLock m_instanceLock;
    AppConfigStore m_settings;
    NotificationClient m_notifications;

    Page m_page = Page::Main;
    AdvancedTab m_advancedTab = AdvancedTab::Ethernet;
    ScrollView* m_scrollView = nullptr;

    // Non-owning; only valid while m_page == Page::Main (nulled out
    // otherwise) - see RebuildPage()'s focus-preservation dance for
    // why RebuildMainPage() needs the caller to be able to find it
    // again after rebuilding.
    TextField* m_searchField = nullptr;

    bool m_networkDirty = false;

    std::string m_selectedApPath;
    std::string m_searchQuery;
};

}
