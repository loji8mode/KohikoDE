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

constexpr int kWindowWidth = 820;
constexpr int kWindowHeight = 580;
constexpr int kSidebarWidth = 200;
constexpr int kMargin = 24;
constexpr int kCardGap = 16;

std::string SignalQuality(std::uint8_t strength)
{
    if (strength >= 80) return "Excellent signal";
    if (strength >= 55) return "Good signal";
    if (strength >= 30) return "Fair signal";
    return "Weak signal";
}

std::string WiFiIconFor(std::uint8_t strength, bool secured)
{
    const char* level = strength >= 80 ? "excellent" : strength >= 55 ? "good" : strength >= 30 ? "ok" : "weak";
    std::string name = std::string("network-wireless-signal-") + level;
    (void)secured; // the lock is drawn as its own small badge/icon rather than folded into the icon name
    return name;
}

// A small caption-over-value field, e.g. "GATEWAY" / "192.168.1.1" -
// what BuildEthernetCard() arranges into a 1-, 2-, or 4-column grid
// depending on how much width is available, so a wide window shows
// Address/Gateway/DNS/MAC as a proper labeled grid instead of the
// single cramped summary line a narrow one falls back to.
std::unique_ptr<Widget> BuildInfoField(const Rect& rect, const std::string& caption, const std::string& value)
{
    auto field = std::make_unique<Widget>();
    field->bounds = rect;

    auto captionLabel = std::make_unique<Label>();
    captionLabel->text = caption;
    captionLabel->color = UiTheme::Default().muted;
    captionLabel->bounds = { rect.x, rect.y, rect.width, 16 };
    field->AddChild(std::move(captionLabel));

    auto valueLabel = std::make_unique<Label>();
    valueLabel->text = value.empty() ? "\u2014" : value;
    valueLabel->bounds = { rect.x, rect.y + 18, rect.width, 20 };
    field->AddChild(std::move(valueLabel));

    return field;
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

    m_window.SetResizeHandler([this](int, int) { RebuildChrome(); });

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
    sidebar->Layout({ kMargin, kMargin, kSidebarWidth, m_window.Height() - kMargin * 2 });
    sidebar->onSelect = [this](int index)
    {
        m_currentPage = static_cast<Page>(index);
        m_settings.SetString("last_page", m_currentPage == Page::WiFi ? "wifi" : m_currentPage == Page::Ethernet ? "ethernet" : "vpn");
        m_settings.Save();

        if (m_currentPage == Page::WiFi)
            m_networkManager.RequestWiFiScan();

        RebuildPage();
        m_window.RequestRedraw();
    };
    m_sidebar = static_cast<Sidebar*>(root->AddChild(std::move(sidebar)));

    auto scrollView = std::make_unique<ScrollView>();
    scrollView->bounds = {
        kSidebarWidth + kMargin * 2, kMargin,
        m_window.Width() - kSidebarWidth - kMargin * 3, m_window.Height() - kMargin * 2
    };
    m_scrollView = static_cast<ScrollView*>(root->AddChild(std::move(scrollView)));

    m_window.SetRoot(std::move(root));

    if (m_currentPage == Page::WiFi)
        m_networkManager.RequestWiFiScan();

    RebuildPage();
}

void NetworkWindow::RebuildWiFiPage(Widget* content, int& y, int contentWidth)
{
    bool available = m_networkManager.Available();
    bool wirelessEnabled = available && m_networkManager.WirelessEnabled();

    const NetworkDevice* wifiDevice = nullptr;
    if (available)
        for (auto& device : m_networkManager.Devices())
            if (device.kind == NetworkDeviceKind::WiFi) { wifiDevice = &device; break; }

    const WiFiAccessPoint* activeAp = nullptr;
    if (wifiDevice)
        for (auto& ap : wifiDevice->accessPoints)
            if (ap.isActiveConnection) { activeAp = &ap; break; }

    std::string subtitle;
    if (!available) subtitle = "NetworkManager is not available.";
    else if (!wirelessEnabled) subtitle = "Turned off";
    else if (activeAp) subtitle = "Connected to \"" + activeAp->ssid + "\"  \u2022  " + SignalQuality(activeAp->strength);
    else subtitle = "Not connected";

    std::unique_ptr<Widget> toggle;
    if (available)
    {
        auto t = std::make_unique<ToggleSwitch>();
        t->value = wirelessEnabled;
        t->bounds.height = 26;
        t->onChange = [this](bool enabled) { m_networkManager.SetWirelessEnabled(enabled); };
        toggle = std::move(t);
    }

    auto header = MakePageHeader({ 0, y, contentWidth, 52 }, "Wi-Fi", subtitle, std::move(toggle), 46);
    y += 52 + kCardGap;
    content->AddChild(std::move(header));

    if (!available)
        return;

    if (!wirelessEnabled)
    {
        auto label = std::make_unique<Label>();
        label->text = "Turn Wi-Fi on to see nearby networks.";
        label->color = UiTheme::Default().muted;
        label->bounds = { 0, y, contentWidth, 24 };
        y += 24;
        content->AddChild(std::move(label));
        return;
    }

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

    // --- sub-header: count + scan action, spanning the full width ----------

    auto subHeader = std::make_unique<Widget>();
    subHeader->bounds = { 0, y, contentWidth, 36 };

    auto countLabel = std::make_unique<Label>();
    countLabel->text = wifiDevice->accessPoints.empty()
        ? "No networks found yet"
        : std::to_string(wifiDevice->accessPoints.size()) + " network"
            + (wifiDevice->accessPoints.size() == 1 ? "" : "s") + " found";
    countLabel->color = UiTheme::Default().muted;
    countLabel->bounds = { 0, 8, contentWidth - 110, 20 };
    subHeader->AddChild(std::move(countLabel));

    auto scanButton = std::make_unique<Button>();
    scanButton->label = "Scan";
    scanButton->bounds = { contentWidth - 100, 2, 100, 32 };
    scanButton->onClick = [this] { m_networkManager.RequestWiFiScan(); };
    subHeader->AddChild(std::move(scanButton));

    y += 36 + kCardGap;
    content->AddChild(std::move(subHeader));

    if (wifiDevice->accessPoints.empty())
        return;

    // --- network rows: full-width, revealing a signal percentage once
    //     there's room for it (see the width check below) rather than
    //     just stretching the same three pieces of information across
    //     however wide the window happens to be. -------------------------

    bool showPercent = contentWidth >= 640;
    std::string devicePath = wifiDevice->objectPath;

    for (auto& ap : wifiDevice->accessPoints)
    {
        const int rowHeight = 68;

        std::string apPath = ap.objectPath;
        std::string ssid = ap.ssid;
        bool secured = ap.secured;
        bool isActive = ap.isActiveConnection;
        std::uint8_t strength = ap.strength;

        auto row = std::make_unique<ClickableContainer>();
        row->bounds = { 0, y, contentWidth, rowHeight };
        row->selected = isActive;
        row->onClick = [this, devicePath, apPath, ssid, secured, isActive]
        {
            if (isActive)
                return;
            if (!secured)
                m_networkManager.ConnectToAccessPoint(devicePath, apPath, ssid, "");
            else
                PromptAndConnect(devicePath, apPath, ssid, secured);
        };

        auto icon = std::make_unique<IconView>();
        icon->name = WiFiIconFor(strength, secured);
        icon->bounds = { 16, y + (rowHeight - 28) / 2, 28, 28 };
        row->AddChild(std::move(icon));

        // Right-aligned cluster (built right-to-left so its total
        // width is known before the text block's width is): action
        // control, optional percentage, security lock.
        int clusterX = contentWidth - 16;

        std::unique_ptr<Widget> actionWidget;
        int actionWidth = 0;
        if (isActive)
        {
            actionWidth = Badge::MeasureWidth(m_window, "Connected");
            auto badge = std::make_unique<Badge>();
            badge->text = "Connected";
            badge->tone = Badge::Tone::Positive;
            badge->bounds = { 0, 0, actionWidth, 26 };
            actionWidget = std::move(badge);
        }
        else
        {
            actionWidth = 100;
            auto button = std::make_unique<Button>();
            button->label = "Connect";
            button->primary = true;
            button->bounds = { 0, 0, actionWidth, 32 };
            std::string ssidCopy = ssid, apPathCopy = apPath, devicePathCopy = devicePath;
            bool securedCopy = secured;
            button->onClick = [this, devicePathCopy, apPathCopy, ssidCopy, securedCopy]
            {
                if (!securedCopy)
                    m_networkManager.ConnectToAccessPoint(devicePathCopy, apPathCopy, ssidCopy, "");
                else
                    PromptAndConnect(devicePathCopy, apPathCopy, ssidCopy, securedCopy);
            };
            actionWidget = std::move(button);
        }

        clusterX -= actionWidth;
        actionWidget->bounds = { clusterX, y + (rowHeight - actionWidget->bounds.height) / 2, actionWidth, actionWidget->bounds.height };
        row->AddChild(std::move(actionWidget));

        if (showPercent)
        {
            int percentWidth = 46;
            clusterX -= percentWidth + 12;

            auto percentLabel = std::make_unique<Label>();
            percentLabel->text = std::to_string(strength) + "%";
            percentLabel->color = UiTheme::Default().muted;
            percentLabel->align = Label::Align::Right;
            percentLabel->bounds = { clusterX, y + (rowHeight - 18) / 2, percentWidth, 18 };
            row->AddChild(std::move(percentLabel));
        }

        if (secured)
        {
            int lockWidth = 20;
            clusterX -= lockWidth + 10;

            auto lockIcon = std::make_unique<IconView>();
            lockIcon->name = "network-wireless-encrypted";
            lockIcon->bounds = { clusterX, y + (rowHeight - lockWidth) / 2, lockWidth, lockWidth };
            row->AddChild(std::move(lockIcon));
        }

        auto title = std::make_unique<Label>();
        title->text = ssid;
        title->bounds = { 58, y + 12, clusterX - 58 - 12, 22 };
        row->AddChild(std::move(title));

        auto subtitleLabel = std::make_unique<Label>();
        subtitleLabel->text = SignalQuality(strength);
        subtitleLabel->color = UiTheme::Default().muted;
        subtitleLabel->bounds = { 58, y + 36, clusterX - 58 - 12, 18 };
        row->AddChild(std::move(subtitleLabel));

        y += rowHeight + 10;
        content->AddChild(std::move(row));
    }
}

std::unique_ptr<Widget> NetworkWindow::BuildEthernetCard(const NetworkDevice& device, int width)
{
    // At narrow widths, a single summary line; once there's genuine
    // room, a proper labeled grid - the concrete "show more
    // information as the window widens" behavior for this page,
    // matched by the card's own height growing to match.
    int columns = width >= 760 ? 4 : width >= 520 ? 2 : 1;
    int rows = columns == 1 ? 1 : (4 + columns - 1) / columns;
    int gridHeight = columns == 1 ? 20 : rows * 44;
    int height = 64 + gridHeight;

    auto card = std::make_unique<ClickableContainer>();
    card->bounds = { 0, 0, width, height };
    card->selected = device.connected;

    auto icon = std::make_unique<IconView>();
    icon->name = "network-wired";
    icon->bounds = { 16, 16, 30, 30 };
    card->AddChild(std::move(icon));

    int badgeWidth = Badge::MeasureWidth(m_window, device.connected ? "Connected" : "Not connected");

    auto title = std::make_unique<Label>();
    title->text = device.interfaceName.empty() ? "Ethernet" : device.interfaceName;
    title->bounds = { 58, 16, width - 58 - badgeWidth - 28, 22 };
    card->AddChild(std::move(title));

    auto badge = std::make_unique<Badge>();
    badge->text = device.connected ? "Connected" : "Not connected";
    badge->tone = device.connected ? Badge::Tone::Positive : Badge::Tone::Neutral;
    badge->bounds = { width - 16 - badgeWidth, 18, badgeWidth, 22 };
    card->AddChild(std::move(badge));

    if (columns == 1)
    {
        std::ostringstream summary;
        summary << (device.ipv4.address.empty() ? "No address" : device.ipv4.address + "/" + std::to_string(device.ipv4.prefixLength));
        if (!device.macAddress.empty())
            summary << "  \u2022  " << device.macAddress;

        auto summaryLabel = std::make_unique<Label>();
        summaryLabel->text = summary.str();
        summaryLabel->color = UiTheme::Default().muted;
        summaryLabel->bounds = { 58, 40, width - 74, 18 };
        card->AddChild(std::move(summaryLabel));
    }
    else
    {
        struct Field { std::string caption, value; };
        std::vector<Field> fields = {
            { "ADDRESS", device.ipv4.address.empty() ? "" : device.ipv4.address + "/" + std::to_string(device.ipv4.prefixLength) },
            { "GATEWAY", device.ipv4.gateway },
            { "DNS", device.ipv4.dnsServers.empty() ? "" : device.ipv4.dnsServers.front() },
            { "MAC ADDRESS", device.macAddress },
        };

        int fieldWidth = (width - 32 - (columns - 1) * kCardGap) / columns;
        int gridTop = 58;

        for (std::size_t i = 0; i < fields.size(); ++i)
        {
            int col = static_cast<int>(i) % columns;
            int row = static_cast<int>(i) / columns;
            Rect fieldRect{ 16 + col * (fieldWidth + kCardGap), gridTop + row * 44, fieldWidth, 40 };
            card->AddChild(BuildInfoField(fieldRect, fields[i].caption, fields[i].value));
        }
    }

    return card;
}

void NetworkWindow::RebuildEthernetPage(Widget* content, int& y, int contentWidth)
{
    auto header = MakePageHeader({ 0, y, contentWidth, 52 }, "Ethernet",
        m_networkManager.Available() ? "Wired connections" : "NetworkManager is not available.");
    y += 52 + kCardGap;
    content->AddChild(std::move(header));

    if (!m_networkManager.Available())
        return;

    bool any = false;
    for (auto& device : m_networkManager.Devices())
    {
        if (device.kind != NetworkDeviceKind::Ethernet)
            continue;

        any = true;
        auto card = BuildEthernetCard(device, contentWidth);
        card->bounds.y = y;
        y += card->bounds.height + kCardGap;
        content->AddChild(std::move(card));
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
    auto header = MakePageHeader({ 0, y, contentWidth, 52 }, "VPN",
        m_networkManager.Available() ? "Existing VPN connection profiles" : "NetworkManager is not available.");
    y += 52 + kCardGap;
    content->AddChild(std::move(header));

    if (!m_networkManager.Available())
        return;

    bool any = false;

    for (auto& conn : m_networkManager.SavedConnections())
    {
        if (!conn.isVpn)
            continue;

        any = true;
        const int height = 72;

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

        row->Layout({ 0, y, contentWidth, height }, std::move(toggle), 46);
        y += height + kCardGap;
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
