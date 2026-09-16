#include "BluetoothWindow.h"

#include <algorithm>

namespace Kohiko
{

namespace
{

constexpr int kWindowWidth = 780;
constexpr int kWindowHeight = 560;
constexpr int kSidebarWidth = 200;
constexpr int kMargin = 24;
constexpr int kCardGap = 16;

std::string DeviceStatusText(const BluetoothDevice& device)
{
    std::string status = device.connected ? "Connected" : "Paired";
    if (device.batteryPercent >= 0)
        status += "  \u2022  Battery " + std::to_string(device.batteryPercent) + "%";
    return status;
}

// Same reasoning as AudioWindow's ComputeColumns(): capped so each
// tile keeps room for an icon, name, and Pair button, and never
// exceeds the number of tiles actually being placed.
int ComputeColumns(int width, int itemCount)
{
    int columns = 1;
    if (width >= 520) columns = 2;
    if (width >= 900) columns = 3;
    return std::max(1, std::min(columns, std::max(1, itemCount)));
}

}

BluetoothWindow::BluetoothWindow() : m_settings("kohiko-bluetooth.conf")
{
}

bool BluetoothWindow::Initialize()
{
    if (!m_instanceLock.AcquireOrListen("kohiko-bluetooth"))
        return false;

    m_instanceLock.SetRaiseHandler([this] { m_window.RaiseAndFocus(); });

    if (!m_window.Create("Kohiko Bluetooth", "kohiko-bluetooth", kWindowWidth, kWindowHeight))
        return false;

    m_notifications.Connect();

    m_bluez.Connect(); // fine if this fails - RebuildPage() shows an unavailable message, see its own comment
    m_bluez.SetChangeHandler([this] { m_bluezDirty = true; });

    RebuildChrome();

    m_window.SetResizeHandler([this](int, int) { RebuildChrome(); });

    m_window.WatchFd(m_instanceLock.Fd(), [this] { m_instanceLock.Dispatch(); });
    if (m_bluez.Available())
        m_window.WatchFd(m_bluez.Fd(), [this] { m_bluez.Dispatch(); });

    m_window.SetInterval(300, [this]
    {
        if (m_bluezDirty && !m_window.IsInteracting())
        {
            m_bluezDirty = false;
            RebuildPage();
            m_window.RequestRedraw();
        }
    });

    return true;
}

void BluetoothWindow::Run()
{
    m_window.Run();
}

void BluetoothWindow::RebuildChrome()
{
    auto root = std::make_unique<Widget>();

    auto sidebar = std::make_unique<Sidebar>();
    sidebar->SetItems({ { "Devices", "bluetooth" } });
    sidebar->SetSelected(0);
    sidebar->Layout({ kMargin, kMargin, kSidebarWidth, m_window.Height() - kMargin * 2 });
    m_sidebar = static_cast<Sidebar*>(root->AddChild(std::move(sidebar)));

    auto scrollView = std::make_unique<ScrollView>();
    scrollView->bounds = {
        kSidebarWidth + kMargin * 2, kMargin,
        m_window.Width() - kSidebarWidth - kMargin * 3, m_window.Height() - kMargin * 2
    };
    m_scrollView = static_cast<ScrollView*>(root->AddChild(std::move(scrollView)));

    m_window.SetRoot(std::move(root));

    RebuildPage();
}

std::unique_ptr<Widget> BluetoothWindow::BuildPairedDeviceCard(const BluetoothDevice& device, const std::string& adapterPath, int width)
{
    const int height = 92;
    std::string devicePath = device.objectPath;
    bool connected = device.connected;
    bool trusted = device.trusted;

    auto card = std::make_unique<ClickableContainer>();
    card->bounds = { 0, 0, width, height };
    card->selected = connected;
    card->onClick = [this, devicePath, connected]
    {
        if (connected)
            m_bluez.DisconnectDevice(devicePath);
        else
            m_bluez.ConnectDevice(devicePath);
    };

    auto icon = std::make_unique<IconView>();
    icon->name = device.icon.empty() ? "bluetooth" : device.icon;
    icon->bounds = { 16, (height - 32) / 2, 32, 32 };
    card->AddChild(std::move(icon));

    // Right-aligned cluster, built right-to-left: Remove button,
    // Trust toggle + label, status badge - same "reserve space from
    // the right, then give the text block whatever's left" approach
    // as NetworkWindow's Wi-Fi rows.
    int clusterX = width - 16;

    int removeWidth = 84;
    clusterX -= removeWidth;
    auto removeButton = std::make_unique<Button>();
    removeButton->label = "Remove";
    removeButton->bounds = { clusterX, (height - 32) / 2, removeWidth, 32 };
    removeButton->onClick = [this, adapterPath, devicePath] { m_bluez.RemoveDevice(adapterPath, devicePath); };
    card->AddChild(std::move(removeButton));

    clusterX -= 14;
    int trustWidth = 42;
    clusterX -= trustWidth;
    auto trustToggle = std::make_unique<ToggleSwitch>();
    trustToggle->value = trusted;
    trustToggle->bounds = { clusterX, (height - 26) / 2, trustWidth, 26 };
    trustToggle->onChange = [this, devicePath](bool t) { m_bluez.SetDeviceTrusted(devicePath, t); };
    card->AddChild(std::move(trustToggle));

    clusterX -= 6;
    int trustLabelWidth = 44;
    clusterX -= trustLabelWidth;
    auto trustLabel = std::make_unique<Label>();
    trustLabel->text = "Trust";
    trustLabel->color = UiTheme::Default().muted;
    trustLabel->bounds = { clusterX, (height - 18) / 2, trustLabelWidth, 18 };
    card->AddChild(std::move(trustLabel));

    clusterX -= 14;
    int badgeWidth = Badge::MeasureWidth(m_window, connected ? "Connected" : "Paired");
    clusterX -= badgeWidth;
    auto badge = std::make_unique<Badge>();
    badge->text = connected ? "Connected" : "Paired";
    badge->tone = connected ? Badge::Tone::Positive : Badge::Tone::Neutral;
    badge->bounds = { clusterX, (height - 24) / 2, badgeWidth, 24 };
    card->AddChild(std::move(badge));

    auto title = std::make_unique<Label>();
    title->text = device.name;
    title->bounds = { 60, 18, clusterX - 60 - 12, 22 };
    card->AddChild(std::move(title));

    auto subtitle = std::make_unique<Label>();
    subtitle->text = DeviceStatusText(device);
    subtitle->color = UiTheme::Default().muted;
    subtitle->bounds = { 60, 44, clusterX - 60 - 12, 18 };
    card->AddChild(std::move(subtitle));

    return card;
}

std::unique_ptr<Widget> BluetoothWindow::BuildAvailableDeviceCard(const BluetoothDevice& device, int width)
{
    const int height = 88;
    std::string devicePath = device.objectPath;

    auto card = std::make_unique<ClickableContainer>();
    card->bounds = { 0, 0, width, height };
    card->onClick = [this, devicePath] { m_bluez.PairDevice(devicePath); };

    auto icon = std::make_unique<IconView>();
    icon->name = device.icon.empty() ? "bluetooth" : device.icon;
    icon->bounds = { 16, 16, 28, 28 };
    card->AddChild(std::move(icon));

    auto title = std::make_unique<Label>();
    title->text = device.name;
    title->bounds = { 54, 16, width - 70, 22 };
    card->AddChild(std::move(title));

    auto pairButton = std::make_unique<Button>();
    pairButton->label = "Pair";
    pairButton->primary = true;
    pairButton->bounds = { 16, 50, width - 32, 30 };
    pairButton->onClick = [this, devicePath] { m_bluez.PairDevice(devicePath); };
    card->AddChild(std::move(pairButton));

    return card;
}

void BluetoothWindow::RebuildPage()
{
    int contentWidth = m_scrollView->bounds.width;
    auto content = std::make_unique<Widget>();
    int y = 0;

    if (!m_bluez.Available())
    {
        auto header = MakePageHeader({ 0, y, contentWidth, 52 }, "Bluetooth", "bluetoothd is not available.");
        y += 52;
        content->AddChild(std::move(header));
        content->bounds = { 0, 0, contentWidth, y };
        m_scrollView->SetContent(std::move(content));
        return;
    }

    if (m_bluez.Adapters().empty())
    {
        auto header = MakePageHeader({ 0, y, contentWidth, 52 }, "Bluetooth", "No Bluetooth adapter found.");
        y += 52;
        content->AddChild(std::move(header));
        content->bounds = { 0, 0, contentWidth, y };
        m_scrollView->SetContent(std::move(content));
        return;
    }

    const BluetoothAdapter& adapter = m_bluez.Adapters().front();
    std::string adapterPath = adapter.objectPath;

    std::string subtitle = adapter.discovering ? "Scanning\u2026" : (adapter.powered ? "On and visible" : "Off");

    auto poweredToggle = std::make_unique<ToggleSwitch>();
    poweredToggle->value = adapter.powered;
    poweredToggle->bounds.height = 26;
    poweredToggle->onChange = [this, adapterPath](bool on) { m_bluez.SetAdapterPowered(adapterPath, on); };

    auto header = MakePageHeader({ 0, y, contentWidth, 52 },
        adapter.name.empty() ? "Bluetooth" : adapter.name, subtitle, std::move(poweredToggle), 46);
    y += 52 + kCardGap;
    content->AddChild(std::move(header));

    if (!adapter.powered)
    {
        auto label = std::make_unique<Label>();
        label->text = "Turn Bluetooth on to see nearby devices.";
        label->color = UiTheme::Default().muted;
        label->bounds = { 0, y, contentWidth, 24 };
        y += 24;
        content->AddChild(std::move(label));
        content->bounds = { 0, 0, contentWidth, y };
        m_scrollView->SetContent(std::move(content));
        return;
    }

    // --- sub-header: discoverable toggle + scan action, full width ---------

    auto subHeader = std::make_unique<Widget>();
    subHeader->bounds = { 0, y, contentWidth, 36 };

    auto discoverableLabel = std::make_unique<Label>();
    discoverableLabel->text = "Discoverable to other devices";
    discoverableLabel->bounds = { 0, 8, contentWidth - 240, 20 };
    subHeader->AddChild(std::move(discoverableLabel));

    auto discoverableToggle = std::make_unique<ToggleSwitch>();
    discoverableToggle->value = adapter.discoverable;
    discoverableToggle->bounds = { contentWidth - 220, 5, 46, 26 };
    discoverableToggle->onChange = [this, adapterPath](bool on) { m_bluez.SetAdapterDiscoverable(adapterPath, on); };
    subHeader->AddChild(std::move(discoverableToggle));

    auto scanButton = std::make_unique<Button>();
    scanButton->label = adapter.discovering ? "Stop Scanning" : "Scan for Devices";
    scanButton->bounds = { contentWidth - 160, 2, 160, 32 };
    bool discovering = adapter.discovering;
    scanButton->onClick = [this, adapterPath, discovering]
    {
        if (discovering)
            m_bluez.StopDiscovery(adapterPath);
        else
            m_bluez.StartDiscovery(adapterPath);
    };
    subHeader->AddChild(std::move(scanButton));

    y += 36 + kCardGap;
    content->AddChild(std::move(subHeader));

    // --- paired/connected devices: one full-width card each -----------------

    std::vector<const BluetoothDevice*> paired;
    std::vector<const BluetoothDevice*> available;
    for (auto& device : m_bluez.Devices())
        (device.paired ? paired : available).push_back(&device);

    auto sectionHeader = std::make_unique<SectionHeader>();
    sectionHeader->text = paired.empty() ? "PAIRED DEVICES" : "PAIRED DEVICES (" + std::to_string(paired.size()) + ")";
    sectionHeader->bounds = { 0, y, contentWidth, 20 };
    y += 28;
    content->AddChild(std::move(sectionHeader));

    if (paired.empty())
    {
        auto label = std::make_unique<Label>();
        label->text = "No paired devices yet.";
        label->color = UiTheme::Default().muted;
        label->bounds = { 0, y, contentWidth, 24 };
        y += 24 + kCardGap;
        content->AddChild(std::move(label));
    }
    else
    {
        for (auto* device : paired)
        {
            auto card = BuildPairedDeviceCard(*device, adapterPath, contentWidth);
            card->bounds.y = y;
            y += card->bounds.height + kCardGap;
            content->AddChild(std::move(card));
        }
    }

    // --- available devices: a responsive tile grid --------------------------

    auto availableHeader = std::make_unique<SectionHeader>();
    availableHeader->text = "AVAILABLE DEVICES";
    availableHeader->bounds = { 0, y, contentWidth, 20 };
    y += 28;
    content->AddChild(std::move(availableHeader));

    if (available.empty())
    {
        auto label = std::make_unique<Label>();
        label->text = "No devices found. Try scanning.";
        label->color = UiTheme::Default().muted;
        label->bounds = { 0, y, contentWidth, 24 };
        y += 24;
        content->AddChild(std::move(label));
    }
    else
    {
        int columns = ComputeColumns(contentWidth, static_cast<int>(available.size()));
        int cardWidth = columns > 1 ? (contentWidth - (columns - 1) * kCardGap) / columns : contentWidth;
        int cardHeight = 88;
        int gridTop = y;

        for (std::size_t i = 0; i < available.size(); ++i)
        {
            int col = static_cast<int>(i) % columns;
            int row = static_cast<int>(i) / columns;

            auto card = BuildAvailableDeviceCard(*available[i], cardWidth);
            card->bounds.x = col * (cardWidth + kCardGap);
            card->bounds.y = gridTop + row * (cardHeight + kCardGap);
            content->AddChild(std::move(card));
        }

        int rows = (static_cast<int>(available.size()) + columns - 1) / columns;
        y = gridTop + rows * (cardHeight + kCardGap);
    }

    content->bounds = { 0, 0, contentWidth, y };
    m_scrollView->SetContent(std::move(content));
}

}
