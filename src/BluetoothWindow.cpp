#include "BluetoothWindow.h"
#include "UiListRow.h"

#include <algorithm>

namespace Kohiko
{

namespace
{

constexpr int kWindowWidth = 820;
constexpr int kWindowHeight = 580;
constexpr int kMargin = 24;
constexpr int kCardGap = 16;

// Same "cap and center past a comfortable width" treatment as
// AudioWindow/NetworkWindow - see kMaxContentWidth's comment there.
constexpr int kMaxContentWidth = 960;

// Below this, the device list(s) and the details panel stack into one
// column instead of sitting side by side.
constexpr int kSplitBreakpoint = 720;

// Below this, the toolbar wraps its Discoverable toggle and Scan
// button onto their own lines rather than clipping.
constexpr int kToolbarNarrowBreakpoint = 640;

std::string BatteryIconFor(int percent)
{
    if (percent >= 80) return "battery-full";
    if (percent >= 50) return "battery-good";
    if (percent >= 20) return "battery-low";
    return "battery-caution";
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

    m_bluez.Connect(); // fine if this fails - RebuildPage() just shows an unavailable message, see its own comment
    m_bluez.SetChangeHandler([this] { ScheduleRebuild(); });

    RebuildChrome();

    m_window.SetResizeHandler([this](int, int) { RebuildChrome(); });

    m_window.WatchFd(m_instanceLock.Fd(), [this] { m_instanceLock.Dispatch(); });

    if (m_bluez.Available())
        m_window.WatchFd(m_bluez.Fd(), [this] { m_bluez.Dispatch(); });

    // Results of the slow operations arrive on this fd (an eventfd the
    // worker thread signals when it finishes) - the loop sleeps in
    // poll() until then; there is no timer waiting for them.
    m_window.WatchFd(m_bluez.OperationFd(), [this] { m_bluez.DispatchOperations(); });

    return true;
}

void BluetoothWindow::ScheduleRebuild()
{
    m_bluezDirty = true;

    if (m_rebuildTimer)
        return;

    // Throttled, and guarded by !IsInteracting() so a rebuild can never
    // pull the widget tree out from under a press in progress (see
    // UiWindow::IsInteracting()) - but only registered while a rebuild
    // is actually waiting, and cancelled the moment it has run. A
    // window with nothing happening has no timer at all.
    m_rebuildTimer = m_window.SetInterval(300, [this]
    {
        if (m_bluezDirty && !m_window.IsInteracting())
        {
            m_bluezDirty = false;
            RebuildPage();
            m_window.RequestRedraw();
        }

        if (!m_bluezDirty && m_rebuildTimer)
        {
            m_window.CancelInterval(*m_rebuildTimer);
            m_rebuildTimer.reset();
        }
    });
}

void BluetoothWindow::OnOperationDone(const std::string& target, const OperationResult& result)
{
    if (result.ok)
    {
        m_notes.erase(target);
    }
    else
    {
        m_notes[target] = { result.message, true };

        // Reported where the user is looking AND where they can read the
        // whole text; the on-device line is necessarily short.
        if (m_notifications.Available())
            m_notifications.Notify("Kohiko Bluetooth", "bluetooth", "Bluetooth", result.message, 8000);
    }

    ScheduleRebuild();
}

void BluetoothWindow::Run()
{
    m_window.Run();
}

void BluetoothWindow::RebuildChrome()
{
    auto root = std::make_unique<Widget>();

    int available = std::max(1, m_window.Width() - kMargin * 2);
    int contentWidth = std::min(available, kMaxContentWidth);
    int marginX = kMargin + (available - contentWidth) / 2;

    auto scrollView = std::make_unique<ScrollView>();
    scrollView->bounds = { marginX, kMargin, contentWidth, std::max(1, m_window.Height() - kMargin * 2) };
    m_scrollView = static_cast<ScrollView*>(root->AddChild(std::move(scrollView)));

    m_window.SetRoot(std::move(root));

    RebuildPage();
}

int BluetoothWindow::AppendToolbar(Widget& content, int y, int contentWidth)
{
    bool available = !m_bluez.Adapters().empty();
    const BluetoothAdapter* adapter = available ? &m_bluez.Adapters().front() : nullptr;
    std::string adapterPath = adapter ? adapter->objectPath : std::string();

    bool narrow = contentWidth < kToolbarNarrowBreakpoint;
    const int rowHeight = 40;

    Rect row1{ 0, y, contentWidth, rowHeight };

    auto btLabel = std::make_unique<Label>();
    btLabel->text = "Bluetooth";
    btLabel->bounds = { row1.x, row1.y, 76, rowHeight };
    content.AddChild(std::move(btLabel));

    auto btToggle = std::make_unique<ToggleSwitch>();
    btToggle->value = adapter && adapter->powered;
    btToggle->enabled = available;
    btToggle->bounds = { row1.x + 80, row1.y + (rowHeight - 26) / 2, 46, 26 };
    btToggle->onChange = [this, adapterPath](bool enabled)
    {
        m_bluez.SetAdapterPoweredAsync(adapterPath, enabled, [this, adapterPath](const OperationResult& r) { OnOperationDone(adapterPath, r); });
    };
    content.AddChild(std::move(btToggle));

    int cursorX = row1.x + 80 + 46 + 24;

    auto discLabel = std::make_unique<Label>();
    discLabel->text = "Discoverable";
    int discLabelW = 108;
    auto discToggle = std::make_unique<ToggleSwitch>();
    discToggle->value = adapter && adapter->discoverable;
    discToggle->enabled = available;
    discToggle->onChange = [this, adapterPath](bool enabled)
    {
        m_bluez.SetAdapterDiscoverableAsync(adapterPath, enabled, [this, adapterPath](const OperationResult& r) { OnOperationDone(adapterPath, r); });
    };

    const int refreshW = rowHeight;
    const int scanW = 150;

    if (!narrow)
    {
        discLabel->bounds = { cursorX, row1.y, discLabelW, rowHeight };
        content.AddChild(std::move(discLabel));
        cursorX += discLabelW;

        discToggle->bounds = { cursorX, row1.y + (rowHeight - 26) / 2, 46, 26 };
        content.AddChild(std::move(discToggle));

        auto scanBtn = std::make_unique<Button>();
        scanBtn->label = "Scan for devices";
        scanBtn->enabled = available;
        scanBtn->bounds = { row1.Right() - refreshW - 10 - scanW, row1.y, scanW, rowHeight };
        scanBtn->onClick = [this, adapterPath]
        {
            m_bluez.StartDiscoveryAsync(adapterPath, [this, adapterPath](const OperationResult& r) { OnOperationDone(adapterPath, r); });
            ScheduleRebuild();
        };
        content.AddChild(std::move(scanBtn));

        auto refresh = std::make_unique<IconButton>();
        refresh->iconName = "view-refresh";
        refresh->enabled = available;
        refresh->bounds = { row1.Right() - refreshW, row1.y, refreshW, rowHeight };
        refresh->onClick = [this] { m_bluez.Refresh(); RebuildPage(); m_window.RequestRedraw(); };
        content.AddChild(std::move(refresh));

        y += rowHeight + kCardGap;
    }
    else
    {
        auto refresh = std::make_unique<IconButton>();
        refresh->iconName = "view-refresh";
        refresh->enabled = available;
        refresh->bounds = { row1.Right() - refreshW, row1.y, refreshW, rowHeight };
        refresh->onClick = [this] { m_bluez.Refresh(); RebuildPage(); m_window.RequestRedraw(); };
        content.AddChild(std::move(refresh));
        y += rowHeight + 10;

        Rect row2{ 0, y, contentWidth, rowHeight };
        discLabel->bounds = { row2.x, row2.y, discLabelW, rowHeight };
        content.AddChild(std::move(discLabel));
        discToggle->bounds = { row2.x + discLabelW, row2.y + (rowHeight - 26) / 2, 46, 26 };
        content.AddChild(std::move(discToggle));
        y += rowHeight + 10;

        auto scanBtn = std::make_unique<Button>();
        scanBtn->label = "Scan for devices";
        scanBtn->enabled = available;
        scanBtn->bounds = { 0, y, contentWidth, rowHeight };
        scanBtn->onClick = [this, adapterPath]
        {
            m_bluez.StartDiscoveryAsync(adapterPath, [this, adapterPath](const OperationResult& r) { OnOperationDone(adapterPath, r); });
            ScheduleRebuild();
        };
        content.AddChild(std::move(scanBtn));
        y += rowHeight + kCardGap;
    }

    return y;
}

std::unique_ptr<Widget> BluetoothWindow::BuildDeviceRow(const BluetoothDevice& device, const Rect& rowBounds, bool showDivider)
{
    auto row = std::make_unique<ListRow>();
    row->flat = true;
    row->showDivider = showDivider;
    row->selected = (device.objectPath == m_selectedDevicePath);
    row->iconName = device.icon.empty() ? "bluetooth" : device.icon;
    row->title = device.name.empty() ? device.address : device.name;
    row->subtitle = device.connected ? "Connected" : (device.paired ? "Paired" : "Available");

    std::string devicePath = device.objectPath;
    row->onClick = [this, devicePath] { m_selectedDevicePath = devicePath; RebuildPage(); m_window.RequestRedraw(); };

    if (device.batteryPercent >= 0)
    {
        const int gap = 8;
        const int battTextW = 36;
        const int battIconW = 18;

        row->Layout(rowBounds, nullptr, battTextW + gap + battIconW);

        Rect iconRect{ rowBounds.Right() - 14 - battIconW, rowBounds.y + (rowBounds.height - 18) / 2, battIconW, 18 };
        auto battIcon = std::make_unique<IconView>();
        battIcon->name = BatteryIconFor(device.batteryPercent);
        battIcon->bounds = iconRect;
        row->AddChild(std::move(battIcon));

        Rect textRect{ iconRect.x - gap - battTextW, rowBounds.y, battTextW, rowBounds.height };
        auto battText = std::make_unique<Label>();
        battText->text = std::to_string(device.batteryPercent) + "%";
        battText->align = Label::Align::Right;
        battText->color = UiTheme::Default().muted;
        battText->bounds = textRect;
        row->AddChild(std::move(battText));
    }
    else
    {
        row->Layout(rowBounds, nullptr, 0);
    }

    return row;
}

std::unique_ptr<Widget> BluetoothWindow::BuildDetailsPanel(const BluetoothDevice& device, const std::string& adapterPath, const Rect& panelBounds)
{
    auto card = std::make_unique<Card>();
    card->bounds = panelBounds;

    int x = panelBounds.x + 18;
    int w = panelBounds.width - 36;
    int y = panelBounds.y + 18;

    auto icon = std::make_unique<IconView>();
    icon->name = device.icon.empty() ? "bluetooth" : device.icon;
    icon->bounds = { x, y, 40, 40 };
    card->AddChild(std::move(icon));

    auto name = std::make_unique<Label>();
    name->text = device.name.empty() ? device.address : device.name;
    name->bounds = { x + 52, y + 2, w - 52, 22 };
    card->AddChild(std::move(name));

    const UiTheme& theme = UiTheme::Default();

    // What the second line says: a pending operation ("Working..."),
    // else the last failure on this device, else its state. The failure
    // text is the full BlueZ message; Label clips what does not fit, and
    // the same text was also sent as a desktop notification.
    const bool busy = m_bluez.IsDeviceBusy(device.objectPath);
    auto note = m_notes.find(device.objectPath);

    auto status = std::make_unique<Label>();

    if (busy)
    {
        status->text = "Working...";
        status->color = theme.muted;
    }
    else if (note != m_notes.end())
    {
        status->text = note->second.text;
        status->color = theme.danger;
    }
    else
    {
        status->text = device.connected ? "Connected" : (device.paired ? "Not Connected" : "Not Paired");
        status->color = device.connected ? theme.accent : theme.muted;
    }
    status->bounds = { x + 52, y + 22, w - 52, 20 };
    card->AddChild(std::move(status));

    y += 40 + 18;

    card->AddChild(MakeDetailRow({ x, y, w, 22 }, "Connection", device.connected ? "Connected" : "Not Connected", device.connected ? theme.accent : 0));
    y += 30;

    card->AddChild(MakeDetailRow({ x, y, w, 22 }, "Battery", device.batteryPercent >= 0 ? std::to_string(device.batteryPercent) + "%" : "\u2014"));
    y += 30;

    card->AddChild(MakeDetailRow({ x, y, w, 22 }, "Trusted", device.trusted ? "Yes" : "No"));
    y += 30 + 10;

    std::string devicePath = device.objectPath;
    std::string adapterPathCopy = adapterPath;
    const int buttonHeight = 44;
    const int buttonGap = 10;

    if (!device.paired)
    {
        auto pairBtn = std::make_unique<Button>();
        pairBtn->label = "Pair";
        pairBtn->primary = true;
        pairBtn->bounds = { x, y, w, buttonHeight };
        pairBtn->enabled = !busy;
        if (busy) pairBtn->label = "Pairing...";
        pairBtn->onClick = [this, devicePath]
        {
            m_notes.erase(devicePath);
            m_bluez.PairAndConnectAsync(devicePath, [this, devicePath](const OperationResult& r) { OnOperationDone(devicePath, r); });
            ScheduleRebuild();
        };
        card->AddChild(std::move(pairBtn));
        y += buttonHeight;
    }
    else if (device.connected)
    {
        auto disconnectBtn = std::make_unique<Button>();
        disconnectBtn->label = "Disconnect";
        disconnectBtn->tone = Button::Tone::Danger;
        disconnectBtn->bounds = { x, y, w, buttonHeight };
        disconnectBtn->enabled = !busy;
        disconnectBtn->onClick = [this, devicePath]
        {
            m_notes.erase(devicePath);
            m_bluez.DisconnectDeviceAsync(devicePath, [this, devicePath](const OperationResult& r) { OnOperationDone(devicePath, r); });
            ScheduleRebuild();
        };
        card->AddChild(std::move(disconnectBtn));
        y += buttonHeight + buttonGap;

        auto forgetBtn = std::make_unique<Button>();
        forgetBtn->label = "Forget Device";
        forgetBtn->bounds = { x, y, w, buttonHeight };
        forgetBtn->enabled = !busy;
        forgetBtn->onClick = [this, devicePath, adapterPathCopy]
        {
            m_notes.erase(devicePath);
            m_bluez.RemoveDeviceAsync(adapterPathCopy, devicePath, [this, devicePath](const OperationResult& r) { OnOperationDone(devicePath, r); });
            ScheduleRebuild();
        };
        card->AddChild(std::move(forgetBtn));
        y += buttonHeight + buttonGap;

        auto bothBtn = std::make_unique<Button>();
        bothBtn->label = "Disconnect and Forget";
        bothBtn->bounds = { x, y, w, buttonHeight };
        bothBtn->enabled = !busy;
        bothBtn->onClick = [this, devicePath, adapterPathCopy]
        {
            m_notes.erase(devicePath);
            m_bluez.DisconnectAndRemoveAsync(adapterPathCopy, devicePath, [this, devicePath](const OperationResult& r) { OnOperationDone(devicePath, r); });
            ScheduleRebuild();
        };
        card->AddChild(std::move(bothBtn));
        y += buttonHeight;
    }
    else
    {
        auto connectBtn = std::make_unique<Button>();
        connectBtn->label = "Connect";
        connectBtn->tone = Button::Tone::Accent;
        connectBtn->bounds = { x, y, w, buttonHeight };
        connectBtn->enabled = !busy;
        if (busy) connectBtn->label = "Connecting...";
        connectBtn->onClick = [this, devicePath]
        {
            m_notes.erase(devicePath);
            m_bluez.ConnectDeviceAsync(devicePath, [this, devicePath](const OperationResult& r) { OnOperationDone(devicePath, r); });
            ScheduleRebuild();
        };
        card->AddChild(std::move(connectBtn));
        y += buttonHeight + buttonGap;

        auto forgetBtn = std::make_unique<Button>();
        forgetBtn->label = "Forget Device";
        forgetBtn->bounds = { x, y, w, buttonHeight };
        forgetBtn->enabled = !busy;
        forgetBtn->onClick = [this, devicePath, adapterPathCopy]
        {
            m_notes.erase(devicePath);
            m_bluez.RemoveDeviceAsync(adapterPathCopy, devicePath, [this, devicePath](const OperationResult& r) { OnOperationDone(devicePath, r); });
            ScheduleRebuild();
        };
        card->AddChild(std::move(forgetBtn));
        y += buttonHeight;
    }

    return card;
}

void BluetoothWindow::EnsureSelection(const std::vector<BluetoothDevice>& devices)
{
    for (auto& d : devices)
        if (d.objectPath == m_selectedDevicePath)
            return;

    for (auto& d : devices)
    {
        if (d.connected)
        {
            m_selectedDevicePath = d.objectPath;
            return;
        }
    }

    m_selectedDevicePath = devices.empty() ? std::string() : devices.front().objectPath;
}

int BluetoothWindow::ComputeDetailsPanelHeight(const BluetoothDevice& device)
{
    int buttonsHeight = !device.paired ? 44
        : device.connected ? (44 + 10 + 44 + 10 + 44)
        : (44 + 10 + 44);

    return 18 + 40 + 18 + 3 * 30 + 10 + buttonsHeight + 18;
}

int BluetoothWindow::AppendDeviceSection(Widget& content, int y, int contentWidth)
{
    if (m_bluez.Adapters().empty())
    {
        auto header = std::make_unique<SectionHeader>();
        header->text = "CONNECTED DEVICES";
        header->bounds = { 0, y, contentWidth, 18 };
        content.AddChild(std::move(header));
        y += 18 + 10;

        const int emptyHeight = 64;
        auto card = std::make_unique<Card>();
        card->bounds = { 0, y, contentWidth, emptyHeight };

        auto label = std::make_unique<Label>();
        label->text = "No Bluetooth adapter found.";
        label->color = UiTheme::Default().muted;
        label->bounds = { 20, y, contentWidth - 40, emptyHeight };
        card->AddChild(std::move(label));

        content.AddChild(std::move(card));
        return y + emptyHeight;
    }

    const std::vector<BluetoothDevice>& devices = m_bluez.Devices();
    EnsureSelection(devices);

    std::vector<const BluetoothDevice*> connected;
    std::vector<const BluetoothDevice*> nearby;
    for (auto& d : devices)
        (d.connected ? connected : nearby).push_back(&d);

    bool split = contentWidth >= kSplitBreakpoint;
    int leftWidth = split ? (contentWidth - kCardGap) * 56 / 100 : contentWidth;
    const int rowHeight = 64;

    int leftY = y;

    auto appendList = [&](const char* headerText, const std::vector<const BluetoothDevice*>& list, const char* emptyText)
    {
        auto header = std::make_unique<SectionHeader>();
        header->text = headerText;
        header->bounds = { 0, leftY, leftWidth, 18 };
        content.AddChild(std::move(header));
        leftY += 18 + 10;

        if (list.empty())
        {
            const int h = 56;
            auto card = std::make_unique<Card>();
            card->bounds = { 0, leftY, leftWidth, h };

            auto label = std::make_unique<Label>();
            label->text = emptyText;
            label->color = UiTheme::Default().muted;
            label->bounds = { 20, leftY, leftWidth - 40, h };
            card->AddChild(std::move(label));

            content.AddChild(std::move(card));
            leftY += h + kCardGap;
        }
        else
        {
            int h = rowHeight * static_cast<int>(list.size());
            auto card = std::make_unique<Card>();
            card->bounds = { 0, leftY, leftWidth, h };

            for (std::size_t i = 0; i < list.size(); ++i)
            {
                Rect rowBounds{ 0, leftY + static_cast<int>(i) * rowHeight, leftWidth, rowHeight };
                card->AddChild(BuildDeviceRow(*list[i], rowBounds, i + 1 != list.size()));
            }

            content.AddChild(std::move(card));
            leftY += h + kCardGap;
        }
    };

    appendList("CONNECTED DEVICES", connected, "No devices connected.");
    appendList("NEARBY DEVICES", nearby, "No nearby devices found. Try scanning.");

    const BluetoothDevice* selected = nullptr;
    for (auto& d : devices)
        if (d.objectPath == m_selectedDevicePath) { selected = &d; break; }

    if (!selected)
        return leftY;

    int detailsX = split ? leftWidth + kCardGap : 0;
    int detailsY = split ? y : leftY;
    int detailsWidth = split ? contentWidth - leftWidth - kCardGap : contentWidth;

    int detailsHeight = ComputeDetailsPanelHeight(*selected);

    std::string adapterPath = m_bluez.Adapters().front().objectPath;
    Rect panelBounds{ detailsX, detailsY, detailsWidth, detailsHeight };
    content.AddChild(BuildDetailsPanel(*selected, adapterPath, panelBounds));

    return std::max(leftY, detailsY + detailsHeight);
}

int BluetoothWindow::RebuildMainPage(Widget& content, int contentWidth)
{
    int y = AppendToolbar(content, 0, contentWidth);
    y = AppendDeviceSection(content, y, contentWidth);
    y += kCardGap;

    const int advancedHeight = 56;
    auto advancedCard = std::make_unique<Card>();
    advancedCard->bounds = { 0, y, contentWidth, advancedHeight };

    auto advancedRow = std::make_unique<ListRow>();
    advancedRow->flat = true;
    advancedRow->showDivider = false;
    advancedRow->title = "Advanced Settings";
    advancedRow->onClick = [this] { m_page = Page::Advanced; RebuildPage(); m_window.RequestRedraw(); };

    auto chevron = std::make_unique<Label>();
    chevron->text = "\u203a";
    chevron->align = Label::Align::Center;
    chevron->color = UiTheme::Default().muted;
    chevron->bounds = { 0, 0, 20, 20 };
    advancedRow->Layout({ 0, y, contentWidth, advancedHeight }, std::move(chevron), 20);

    advancedCard->AddChild(std::move(advancedRow));
    content.AddChild(std::move(advancedCard));
    y += advancedHeight;

    return y;
}

int BluetoothWindow::RebuildAdvancedPage(Widget& content, int contentWidth)
{
    int y = 0;

    auto backRow = std::make_unique<ListRow>();
    backRow->flat = true;
    backRow->showDivider = true;
    backRow->title = "\u2039  Back to Kohiko Bluetooth";
    backRow->onClick = [this] { m_page = Page::Main; RebuildPage(); m_window.RequestRedraw(); };
    backRow->Layout({ 0, y, contentWidth, 44 });
    content.AddChild(std::move(backRow));
    y += 44 + kCardGap;

    auto header = std::make_unique<SectionHeader>();
    header->text = "TRUSTED DEVICES";
    header->bounds = { 0, y, contentWidth, 18 };
    content.AddChild(std::move(header));
    y += 18 + 10;

    if (m_bluez.Adapters().empty())
    {
        auto label = std::make_unique<Label>();
        label->text = "No Bluetooth adapter found.";
        label->color = UiTheme::Default().muted;
        label->bounds = { 0, y, contentWidth, 24 };
        content.AddChild(std::move(label));
        y += 24;
        return y;
    }

    std::vector<const BluetoothDevice*> paired;
    for (auto& d : m_bluez.Devices())
        if (d.paired)
            paired.push_back(&d);

    if (paired.empty())
    {
        auto label = std::make_unique<Label>();
        label->text = "No paired devices yet.";
        label->color = UiTheme::Default().muted;
        label->bounds = { 0, y, contentWidth, 24 };
        content.AddChild(std::move(label));
        y += 24;
        return y;
    }

    const int rowHeight = 64;
    auto card = std::make_unique<Card>();
    card->bounds = { 0, y, contentWidth, rowHeight * static_cast<int>(paired.size()) };

    for (std::size_t i = 0; i < paired.size(); ++i)
    {
        const BluetoothDevice& device = *paired[i];
        Rect rowBounds{ 0, y + static_cast<int>(i) * rowHeight, contentWidth, rowHeight };

        auto row = std::make_unique<ListRow>();
        row->flat = true;
        row->showDivider = (i + 1 != paired.size());
        row->iconName = device.icon.empty() ? "bluetooth" : device.icon;
        row->title = device.name.empty() ? device.address : device.name;
        row->subtitle = device.trusted ? "Trusted" : "Not trusted";

        std::string devicePath = device.objectPath;

        auto toggle = std::make_unique<ToggleSwitch>();
        toggle->value = device.trusted;
        toggle->bounds = { 0, 0, 0, 26 };
        toggle->onChange = [this, devicePath](bool trusted)
        {
            m_bluez.SetDeviceTrustedAsync(devicePath, trusted, [this, devicePath](const OperationResult& r) { OnOperationDone(devicePath, r); });
        };

        row->Layout(rowBounds, std::move(toggle), 46);
        card->AddChild(std::move(row));
    }

    y += rowHeight * static_cast<int>(paired.size());
    content.AddChild(std::move(card));

    return y;
}

void BluetoothWindow::RebuildPage()
{
    m_window.ClearFocus();

    int contentWidth = m_scrollView->bounds.width;
    auto content = std::make_unique<Widget>();

    int y = (m_page == Page::Advanced)
        ? RebuildAdvancedPage(*content, contentWidth)
        : RebuildMainPage(*content, contentWidth);

    content->bounds = { 0, 0, contentWidth, y };
    m_scrollView->SetContent(std::move(content));
}

}
