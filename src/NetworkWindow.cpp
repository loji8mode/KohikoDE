#include "NetworkWindow.h"
#include "UiListRow.h"

#include <X11/Xatom.h>
#include <X11/keysym.h>

#include <optional>
#include <sstream>

namespace Kohiko
{

namespace
{

constexpr int kWindowWidth = 640;
constexpr int kWindowHeight = 540;
constexpr int kSidebarWidth = 160;
constexpr int kMargin = 16;

std::string SignalQuality(std::uint8_t strength)
{
    if (strength >= 80) return "Excellent signal";
    if (strength >= 55) return "Good signal";
    if (strength >= 30) return "Fair signal";
    return "Weak signal";
}

std::string IpSummary(const IpAddressInfo& ip)
{
    if (ip.address.empty())
        return "No address";

    std::ostringstream out;
    out << ip.address << "/" << ip.prefixLength;
    if (!ip.gateway.empty())
        out << "  \u2022  gw " << ip.gateway;
    if (!ip.dnsServers.empty())
        out << "  \u2022  dns " << ip.dnsServers.front();
    return out.str();
}

// A small self-contained modal password prompt - not part of the
// shared UI toolkit (see NetworkWindow.h's comment on
// PromptAndConnect()), since connecting to a secured Wi-Fi network is
// the one place across all three new apps that needs free-text
// keyboard input at all. Structured the same way PopupMenu is: an
// override-redirect window, an exclusive grab, a small self-contained
// XNextEvent loop that returns once the user is done.
std::optional<std::string> PromptForPassword(
    Display* display, int screen, Visual* visual, Colormap colormap,
    Font& font, const UiTheme& theme, const std::string& ssid)
{
    const int width = 340;
    const int height = 132;

    int screenWidth = DisplayWidth(display, screen);
    int screenHeight = DisplayHeight(display, screen);

    XSetWindowAttributes attrs{};
    attrs.override_redirect = True;
    attrs.background_pixel = theme.surface;
    attrs.event_mask = ExposureMask | KeyPressMask | ButtonPressMask;

    Window win = XCreateWindow(
        display, RootWindow(display, screen),
        (screenWidth - width) / 2, (screenHeight - height) / 2, width, height, 0,
        DefaultDepth(display, screen), InputOutput, visual,
        CWOverrideRedirect | CWBackPixel | CWEventMask, &attrs
    );

    XMapRaised(display, win);
    XFlush(display);

    XGrabKeyboard(display, win, False, GrabModeAsync, GrabModeAsync, CurrentTime);
    XGrabPointer(display, win, False, ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None, CurrentTime);

    GC gc = XCreateGC(display, win, 0, nullptr);
    XftDraw* xftDraw = XftDrawCreate(display, win, visual, colormap);

    std::string password;
    bool confirmed = false;
    bool done = false;

    Rect fieldRect{ 20, 62, width - 40, 32 };

    auto redraw = [&]()
    {
        XSetForeground(display, gc, theme.surface);
        XFillRectangle(display, win, gc, 0, 0, width, height);
        XSetForeground(display, gc, theme.border);
        XDrawRectangle(display, win, gc, 0, 0, width - 1, height - 1);

        TextColor fg(display, visual, colormap, theme.foreground);
        TextColor muted(display, visual, colormap, theme.muted);

        std::string title = "Password for \"" + ssid + "\"";
        font.DrawString(xftDraw, 20, 30, title, fg.Get());
        font.DrawString(xftDraw, 20, 50, "Press Enter to connect, Escape to cancel", muted.Get());

        XSetForeground(display, gc, theme.surfaceActive);
        XFillRectangle(display, win, gc, fieldRect.x, fieldRect.y, fieldRect.width, fieldRect.height);

        std::string masked(password.size(), '*');
        font.DrawString(xftDraw, fieldRect.x + 8, fieldRect.y + 22, masked, fg.Get());

        XFlush(display);
    };

    redraw();

    while (!done)
    {
        XEvent event;
        XNextEvent(display, &event);

        switch (event.type)
        {
            case Expose:
                redraw();
                break;

            case ButtonPress:
                if (event.xbutton.x < 0 || event.xbutton.x >= width || event.xbutton.y < 0 || event.xbutton.y >= height)
                    done = true; // clicked outside - same "outside == dismiss" convention as PopupMenu
                break;

            case KeyPress:
            {
                char buffer[32];
                KeySym keysym;
                int len = XLookupString(&event.xkey, buffer, sizeof(buffer) - 1, &keysym, nullptr);

                if (keysym == XK_Return || keysym == XK_KP_Enter)
                {
                    confirmed = true;
                    done = true;
                }
                else if (keysym == XK_Escape)
                {
                    done = true;
                }
                else if (keysym == XK_BackSpace)
                {
                    if (!password.empty())
                        password.pop_back();
                    redraw();
                }
                else if (len > 0 && static_cast<unsigned char>(buffer[0]) >= 0x20)
                {
                    password.append(buffer, len);
                    redraw();
                }
                break;
            }

            default:
                break;
        }
    }

    XUngrabKeyboard(display, CurrentTime);
    XUngrabPointer(display, CurrentTime);
    XftDrawDestroy(xftDraw);
    XFreeGC(display, gc);
    XDestroyWindow(display, win);
    XFlush(display);

    if (!confirmed)
        return std::nullopt;

    return password;
}

}

NetworkWindow::NetworkWindow() : m_settings("kohiko-network.conf")
{
}

bool NetworkWindow::Initialize()
{
    if (!m_instanceLock.AcquireOrListen("kohiko-network"))
        return false;

    m_instanceLock.SetRaiseHandler([this] { m_window.RaiseAndFocus(); });

    if (!m_window.Create("Kohiko Network", "kohiko-network", kWindowWidth, kWindowHeight))
        return false;

    m_notifications.Connect();

    m_networkManager.Connect(); // fine if this fails - pages just show an unavailable message, see RebuildPage()
    m_networkManager.SetChangeHandler([this] { m_networkDirty = true; });

    std::string lastPage = m_settings.GetString("last_page", "wifi");
    m_currentPage = lastPage == "ethernet" ? Page::Ethernet : lastPage == "vpn" ? Page::Vpn : Page::WiFi;

    RebuildChrome();
    RebuildPage();

    m_window.WatchFd(m_instanceLock.Fd(), [this] { m_instanceLock.Dispatch(); });
    if (m_networkManager.Available())
        m_window.WatchFd(m_networkManager.Fd(), [this] { m_networkManager.Dispatch(); });

    m_window.SetInterval(300, [this]
    {
        if (m_networkDirty && !m_window.IsInteracting())
        {
            m_networkDirty = false;
            RebuildPage();
            m_window.RequestRedraw();
        }
    });

    return true;
}

void NetworkWindow::Run()
{
    m_window.Run();
}

void NetworkWindow::RebuildChrome()
{
    auto root = std::make_unique<Widget>();

    auto sidebar = std::make_unique<Sidebar>();
    sidebar->SetItems({
        { "Wi-Fi", "network-wireless" },
        { "Ethernet", "network-wired" },
        { "VPN", "network-vpn" },
    });
    sidebar->SetSelected(static_cast<int>(m_currentPage));
    sidebar->Layout({ kMargin, kMargin, kSidebarWidth, kWindowHeight - kMargin * 2 });
    sidebar->onSelect = [this](int index)
    {
        m_currentPage = static_cast<Page>(index);
        m_settings.SetString("last_page", m_currentPage == Page::WiFi ? "wifi" : m_currentPage == Page::Ethernet ? "ethernet" : "vpn");
        m_settings.Save();

        static const char* titles[] = { "Wi-Fi", "Ethernet", "VPN" };
        m_headerLabel->text = titles[index];

        if (m_currentPage == Page::WiFi)
            m_networkManager.RequestWiFiScan();

        RebuildPage();
        m_window.RequestRedraw();
    };
    m_sidebar = static_cast<Sidebar*>(root->AddChild(std::move(sidebar)));

    auto header = std::make_unique<Label>();
    static const char* titles[] = { "Wi-Fi", "Ethernet", "VPN" };
    header->text = titles[static_cast<int>(m_currentPage)];
    header->bounds = { kSidebarWidth + kMargin * 2, kMargin, kWindowWidth - kSidebarWidth - kMargin * 3, 28 };
    m_headerLabel = static_cast<Label*>(root->AddChild(std::move(header)));

    auto scrollView = std::make_unique<ScrollView>();
    scrollView->bounds = {
        kSidebarWidth + kMargin * 2, kMargin + 40,
        kWindowWidth - kSidebarWidth - kMargin * 3, kWindowHeight - kMargin * 2 - 40
    };
    m_scrollView = static_cast<ScrollView*>(root->AddChild(std::move(scrollView)));

    m_window.SetRoot(std::move(root));

    if (m_currentPage == Page::WiFi)
        m_networkManager.RequestWiFiScan();
}

void NetworkWindow::RebuildWiFiPage(Widget* content, int& y, int contentWidth)
{
    if (!m_networkManager.Available())
    {
        auto label = std::make_unique<Label>();
        label->text = "NetworkManager is not available.";
        label->color = UiTheme::Default().muted;
        label->bounds = { 0, y, contentWidth, 24 };
        y += 24;
        content->AddChild(std::move(label));
        return;
    }

    auto toggleRow = std::make_unique<ClickableContainer>();
    toggleRow->bounds = { 0, y, contentWidth, 56 };

    auto toggleLabel = std::make_unique<Label>();
    toggleLabel->text = "Wi-Fi";
    toggleLabel->bounds = { 16, y + 18, 200, 20 };
    toggleRow->AddChild(std::move(toggleLabel));

    auto toggle = std::make_unique<ToggleSwitch>();
    toggle->value = m_networkManager.WirelessEnabled();
    toggle->bounds = { contentWidth - 66, y + 15, 46, 26 };
    toggle->onChange = [this](bool enabled) { m_networkManager.SetWirelessEnabled(enabled); };
    toggleRow->AddChild(std::move(toggle));

    y += 66;
    content->AddChild(std::move(toggleRow));

    if (!m_networkManager.WirelessEnabled())
    {
        auto label = std::make_unique<Label>();
        label->text = "Wi-Fi is turned off.";
        label->color = UiTheme::Default().muted;
        label->bounds = { 0, y, contentWidth, 24 };
        y += 24;
        content->AddChild(std::move(label));
        return;
    }

    const NetworkDevice* wifiDevice = nullptr;
    for (auto& device : m_networkManager.Devices())
        if (device.kind == NetworkDeviceKind::WiFi) { wifiDevice = &device; break; }

    if (!wifiDevice)
    {
        auto label = std::make_unique<Label>();
        label->text = "No Wi-Fi adapter found.";
        label->color = UiTheme::Default().muted;
        label->bounds = { 0, y, contentWidth, 24 };
        y += 24;
        content->AddChild(std::move(label));
        return;
    }

    auto scanButton = std::make_unique<Button>();
    scanButton->label = "Scan for networks";
    scanButton->bounds = { 0, y, 180, 32 };
    scanButton->onClick = [this] { m_networkManager.RequestWiFiScan(); };
    y += 44;
    content->AddChild(std::move(scanButton));

    if (wifiDevice->accessPoints.empty())
    {
        auto label = std::make_unique<Label>();
        label->text = "No Wi-Fi networks found yet.";
        label->color = UiTheme::Default().muted;
        label->bounds = { 0, y, contentWidth, 24 };
        y += 24;
        content->AddChild(std::move(label));
        return;
    }

    std::string devicePath = wifiDevice->objectPath;

    for (auto& ap : wifiDevice->accessPoints)
    {
        auto row = std::make_unique<ListRow>();
        row->iconName = ap.secured ? "network-wireless-encrypted" : "network-wireless";
        row->title = ap.ssid;
        row->subtitle = ap.isActiveConnection ? ("Connected \u2022 " + SignalQuality(ap.strength)) : SignalQuality(ap.strength);
        row->selected = ap.isActiveConnection;

        std::string apPath = ap.objectPath;
        std::string ssid = ap.ssid;
        bool secured = ap.secured;
        bool isActive = ap.isActiveConnection;

        row->onClick = [this, devicePath, apPath, ssid, secured, isActive]
        {
            if (isActive)
                return;

            if (!secured)
                m_networkManager.ConnectToAccessPoint(devicePath, apPath, ssid, "");
            else
                PromptAndConnect(devicePath, apPath, ssid, secured);
        };

        row->Layout({ 0, y, contentWidth, 60 });
        y += 66;
        content->AddChild(std::move(row));
    }
}

void NetworkWindow::RebuildEthernetPage(Widget* content, int& y, int contentWidth)
{
    if (!m_networkManager.Available())
    {
        auto label = std::make_unique<Label>();
        label->text = "NetworkManager is not available.";
        label->color = UiTheme::Default().muted;
        label->bounds = { 0, y, contentWidth, 24 };
        y += 24;
        content->AddChild(std::move(label));
        return;
    }

    bool any = false;

    for (auto& device : m_networkManager.Devices())
    {
        if (device.kind != NetworkDeviceKind::Ethernet)
            continue;

        any = true;

        auto row = std::make_unique<ListRow>();
        row->iconName = "network-wired";
        row->title = device.interfaceName + (device.connected ? " \u2022 Connected" : " \u2022 Not connected");

        std::ostringstream subtitle;
        subtitle << (device.macAddress.empty() ? "" : device.macAddress + "  \u2022  ") << IpSummary(device.ipv4);
        row->subtitle = subtitle.str();
        row->selected = device.connected;

        std::string devicePath = device.objectPath;
        bool connected = device.connected;

        auto toggle = std::make_unique<ToggleSwitch>();
        toggle->value = connected;
        toggle->onChange = [this, devicePath](bool enable)
        {
            if (!enable)
                m_networkManager.DisconnectDevice(devicePath);
        };

        row->Layout({ 0, y, contentWidth, 60 }, std::move(toggle), 46);
        y += 66;
        content->AddChild(std::move(row));
    }

    if (!any)
    {
        auto label = std::make_unique<Label>();
        label->text = "No Ethernet devices found.";
        label->color = UiTheme::Default().muted;
        label->bounds = { 0, y, contentWidth, 24 };
        y += 24;
        content->AddChild(std::move(label));
    }
}

void NetworkWindow::RebuildVpnPage(Widget* content, int& y, int contentWidth)
{
    if (!m_networkManager.Available())
    {
        auto label = std::make_unique<Label>();
        label->text = "NetworkManager is not available.";
        label->color = UiTheme::Default().muted;
        label->bounds = { 0, y, contentWidth, 24 };
        y += 24;
        content->AddChild(std::move(label));
        return;
    }

    bool any = false;

    for (auto& conn : m_networkManager.SavedConnections())
    {
        if (!conn.isVpn)
            continue;

        any = true;

        auto row = std::make_unique<ListRow>();
        row->iconName = "network-vpn";
        row->title = conn.id;
        row->subtitle = conn.isActive ? "Connected" : "Not connected";
        row->selected = conn.isActive;

        std::string connPath = conn.objectPath;
        bool isActive = conn.isActive;

        auto toggle = std::make_unique<ToggleSwitch>();
        toggle->value = isActive;
        toggle->onChange = [this, connPath](bool enable)
        {
            if (enable)
                m_networkManager.ActivateSavedConnection(connPath, "/");
            else
                m_networkManager.DeactivateConnection(connPath);
        };

        row->Layout({ 0, y, contentWidth, 60 }, std::move(toggle), 46);
        y += 66;
        content->AddChild(std::move(row));
    }

    if (!any)
    {
        auto label = std::make_unique<Label>();
        label->text = "No VPN connections configured. Add one with nm-connection-editor or your VPN provider's setup tool.";
        label->color = UiTheme::Default().muted;
        label->bounds = { 0, y, contentWidth, 48 };
        y += 48;
        content->AddChild(std::move(label));
    }
}

void NetworkWindow::RebuildPage()
{
    int contentWidth = m_scrollView->bounds.width;

    auto content = std::make_unique<Widget>();
    int y = 0;

    switch (m_currentPage)
    {
        case Page::WiFi:     RebuildWiFiPage(content.get(), y, contentWidth); break;
        case Page::Ethernet: RebuildEthernetPage(content.get(), y, contentWidth); break;
        case Page::Vpn:      RebuildVpnPage(content.get(), y, contentWidth); break;
    }

    content->bounds = { 0, 0, contentWidth, y };
    m_scrollView->SetContent(std::move(content));
}

void NetworkWindow::PromptAndConnect(const std::string& devicePath, const std::string& apPath, const std::string& ssid, bool /*secured*/)
{
    Display* display = m_window.GetDisplay();
    int screen = DefaultScreen(display);

    auto password = PromptForPassword(
        display, screen,
        DefaultVisual(display, screen), DefaultColormap(display, screen),
        m_window.GetFont(), m_window.Theme(), ssid
    );

    if (password.has_value())
        m_networkManager.ConnectToAccessPoint(devicePath, apPath, ssid, *password);
}

}
