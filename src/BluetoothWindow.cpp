#include "BluetoothWindow.h"

namespace Kohiko
{

namespace
{

constexpr int kWindowWidth = 640;
constexpr int kWindowHeight = 540;
constexpr int kSidebarWidth = 160;
constexpr int kMargin = 16;

class IconWidget : public Widget
{
public:
    std::string name;
    void Draw(UiWindow& window) override { window.DrawIcon(bounds, name); DrawChildren(window); }
};

std::string DeviceSubtitle(const BluetoothDevice& device)
{
    std::string status = device.connected ? "Connected" : device.paired ? "Paired" : "Available";

    if (device.batteryPercent >= 0)
        status += "  \u2022  Battery " + std::to_string(device.batteryPercent) + "%";

    return status;
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
    RebuildPage();

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
    sidebar->Layout({ kMargin, kMargin, kSidebarWidth, kWindowHeight - kMargin * 2 });
    m_sidebar = static_cast<Sidebar*>(root->AddChild(std::move(sidebar)));

    auto header = std::make_unique<Label>();
    header->text = "Bluetooth Devices";
    header->bounds = { kSidebarWidth + kMargin * 2, kMargin, kWindowWidth - kSidebarWidth - kMargin * 3, 28 };
    root->AddChild(std::move(header));

    auto scrollView = std::make_unique<ScrollView>();
    scrollView->bounds = {
        kSidebarWidth + kMargin * 2, kMargin + 40,
        kWindowWidth - kSidebarWidth - kMargin * 3, kWindowHeight - kMargin * 2 - 40
    };
    m_scrollView = static_cast<ScrollView*>(root->AddChild(std::move(scrollView)));

    m_window.SetRoot(std::move(root));
}

void BluetoothWindow::RebuildPage()
{
    int contentWidth = m_scrollView->bounds.width;
    auto content = std::make_unique<Widget>();
    int y = 0;

    if (!m_bluez.Available())
    {
        auto label = std::make_unique<Label>();
        label->text = "bluetoothd is not available.";
        label->color = UiTheme::Default().muted;
        label->bounds = { 0, y, contentWidth, 24 };
        y += 24;
        content->AddChild(std::move(label));

        content->bounds = { 0, 0, contentWidth, y };
        m_scrollView->SetContent(std::move(content));
        return;
    }

    if (m_bluez.Adapters().empty())
    {
        auto label = std::make_unique<Label>();
        label->text = "No Bluetooth adapter found.";
        label->color = UiTheme::Default().muted;
        label->bounds = { 0, y, contentWidth, 24 };
        y += 24;
        content->AddChild(std::move(label));

        content->bounds = { 0, 0, contentWidth, y };
        m_scrollView->SetContent(std::move(content));
        return;
    }

    const BluetoothAdapter& adapter = m_bluez.Adapters().front();
    std::string adapterPath = adapter.objectPath;

    // --- adapter power / discoverable row ---------------------------------

    auto adapterRow = std::make_unique<Widget>();
    adapterRow->bounds = { 0, y, contentWidth, 56 };

    auto poweredLabel = std::make_unique<Label>();
    poweredLabel->text = adapter.name.empty() ? "Bluetooth" : adapter.name;
    poweredLabel->bounds = { 16, y + 8, 220, 20 };
    adapterRow->AddChild(std::move(poweredLabel));

    auto poweredSub = std::make_unique<Label>();
    poweredSub->text = adapter.discovering ? "Scanning\u2026" : (adapter.powered ? "On" : "Off");
    poweredSub->color = UiTheme::Default().muted;
    poweredSub->bounds = { 16, y + 30, 220, 18 };
    adapterRow->AddChild(std::move(poweredSub));

    auto poweredToggle = std::make_unique<ToggleSwitch>();
    poweredToggle->value = adapter.powered;
    poweredToggle->bounds = { contentWidth - 66, y + 15, 46, 26 };
    poweredToggle->onChange = [this, adapterPath](bool on) { m_bluez.SetAdapterPowered(adapterPath, on); };
    adapterRow->AddChild(std::move(poweredToggle));

    y += 66;
    content->AddChild(std::move(adapterRow));

    if (!adapter.powered)
    {
        auto label = std::make_unique<Label>();
        label->text = "Bluetooth is turned off.";
        label->color = UiTheme::Default().muted;
        label->bounds = { 0, y, contentWidth, 24 };
        y += 24;
        content->AddChild(std::move(label));

        content->bounds = { 0, 0, contentWidth, y };
        m_scrollView->SetContent(std::move(content));
        return;
    }

    // --- discoverable + scan row -------------------------------------------

    auto optionsRow = std::make_unique<Widget>();
    optionsRow->bounds = { 0, y, contentWidth, 44 };

    auto discoverableLabel = std::make_unique<Label>();
    discoverableLabel->text = "Discoverable";
    discoverableLabel->bounds = { 16, y + 12, 160, 20 };
    optionsRow->AddChild(std::move(discoverableLabel));

    auto discoverableToggle = std::make_unique<ToggleSwitch>();
    discoverableToggle->value = adapter.discoverable;
    discoverableToggle->bounds = { 180, y + 9, 46, 26 };
    discoverableToggle->onChange = [this, adapterPath](bool on) { m_bluez.SetAdapterDiscoverable(adapterPath, on); };
    optionsRow->AddChild(std::move(discoverableToggle));

    auto scanButton = std::make_unique<Button>();
    scanButton->label = adapter.discovering ? "Stop scanning" : "Scan for devices";
    scanButton->bounds = { contentWidth - 170, y + 6, 170, 32 };
    bool discovering = adapter.discovering;
    scanButton->onClick = [this, adapterPath, discovering]
    {
        if (discovering)
            m_bluez.StopDiscovery(adapterPath);
        else
            m_bluez.StartDiscovery(adapterPath);
    };
    optionsRow->AddChild(std::move(scanButton));

    y += 56;
    content->AddChild(std::move(optionsRow));

    auto separator = std::make_unique<Separator>();
    separator->bounds = { 0, y, contentWidth, 1 };
    y += 13;
    content->AddChild(std::move(separator));

    if (m_bluez.Devices().empty())
    {
        auto label = std::make_unique<Label>();
        label->text = "No devices found. Try scanning.";
        label->color = UiTheme::Default().muted;
        label->bounds = { 0, y, contentWidth, 24 };
        y += 24;
        content->AddChild(std::move(label));
    }

    for (const BluetoothDevice& device : m_bluez.Devices())
    {
        const int rowHeight = 66;

        std::string devicePath = device.objectPath;
        bool paired = device.paired;
        bool connected = device.connected;
        bool trusted = device.trusted;

        auto row = std::make_unique<ClickableContainer>();
        row->bounds = { 0, y, contentWidth, rowHeight };
        row->selected = connected;
        row->onClick = [this, devicePath, paired, connected]
        {
            if (connected)
                m_bluez.DisconnectDevice(devicePath);
            else if (paired)
                m_bluez.ConnectDevice(devicePath);
            else
                m_bluez.PairDevice(devicePath); // BlueZ auto-connects most profiles right after a successful pairing
        };

        auto icon = std::make_unique<IconWidget>();
        icon->name = device.icon.empty() ? "bluetooth" : device.icon;
        icon->bounds = { 14, y + (rowHeight - 28) / 2, 28, 28 };
        row->AddChild(std::move(icon));

        auto title = std::make_unique<Label>();
        title->text = device.name;
        title->bounds = { 54, y + 10, contentWidth - 260, 20 };
        row->AddChild(std::move(title));

        auto subtitle = std::make_unique<Label>();
        subtitle->text = DeviceSubtitle(device);
        subtitle->color = UiTheme::Default().muted;
        subtitle->bounds = { 54, y + 32, contentWidth - 260, 18 };
        row->AddChild(std::move(subtitle));

        if (paired)
        {
            auto trustToggle = std::make_unique<ToggleSwitch>();
            trustToggle->value = trusted;
            trustToggle->bounds = { contentWidth - 190, y + (rowHeight - 26) / 2, 40, 26 };
            trustToggle->onChange = [this, devicePath](bool t) { m_bluez.SetDeviceTrusted(devicePath, t); };
            row->AddChild(std::move(trustToggle));

            auto trustLabel = std::make_unique<Label>();
            trustLabel->text = "Trust";
            trustLabel->color = UiTheme::Default().muted;
            trustLabel->bounds = { contentWidth - 150, y + (rowHeight - 18) / 2, 50, 18 };
            row->AddChild(std::move(trustLabel));

            auto removeButton = std::make_unique<Button>();
            removeButton->label = "Remove";
            removeButton->bounds = { contentWidth - 90, y + (rowHeight - 30) / 2, 76, 30 };
            removeButton->onClick = [this, adapterPath, devicePath] { m_bluez.RemoveDevice(adapterPath, devicePath); };
            row->AddChild(std::move(removeButton));
        }

        y += rowHeight + 8;
        content->AddChild(std::move(row));
    }

    content->bounds = { 0, 0, contentWidth, y };
    m_scrollView->SetContent(std::move(content));
}

}
