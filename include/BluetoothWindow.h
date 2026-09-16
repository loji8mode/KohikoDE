#pragma once

#include "AppConfigStore.h"
#include "AppInstanceLock.h"
#include "BluezClient.h"
#include "NotificationClient.h"
#include "UiScrollView.h"
#include "UiWindow.h"

namespace Kohiko
{

// kohiko-bluetooth's top-level app class - a single page (toolbar,
// CONNECTED DEVICES and NEARBY DEVICES lists, and a details panel for
// whichever device is selected) matching the Kohiko Bluetooth
// mockup, backed entirely by BluezClient. Per-device Trust isn't part
// of that mockup (the details panel only ever *shows* "Trusted: Yes/
// No", it's not a control there), so - same as the other two apps -
// it lives on a second Advanced Settings page reached via the link at
// the bottom, rather than being dropped (see RebuildAdvancedPage()).
//
// "Connected" and "Nearby" are drawn straight from
// BluetoothDevice::connected - the first section is every device
// BlueZ currently reports as connected, the second is everything else
// it knows about (paired-but-out-of-range devices and not-yet-paired
// ones discovered by scanning alike). The device list and its details
// panel sit side by side once the window is wide enough and stack
// into a single column otherwise, the same responsive shape
// NetworkWindow's Wi-Fi list/details split uses.
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

    // The exact height BuildDetailsPanel() needs for `device` - top
    // padding, icon/name/status block, the three fixed detail rows,
    // and however many action buttons `device`'s paired/connected
    // state calls for, plus bottom padding. AppendDeviceSection() has
    // to know this *before* BuildDetailsPanel() runs (to size the
    // Card's own bounds up front), so both call this one function
    // rather than keeping two independently-maintained copies of the
    // same arithmetic in sync by hand - see CHANGELOG.md's 0.20.4
    // entry for why that duplication, while not currently wrong, was
    // worth removing rather than merely testing. `static` and public
    // for the same reason as UiWindow::HitTest()/HitTestForScroll() -
    // it doesn't touch `this`, and tests/test_bluetoothwindow.cpp
    // calls it directly.
    static int ComputeDetailsPanelHeight(const BluetoothDevice& device);

    // Also public, same reasoning: tests/test_bluetoothwindow.cpp
    // builds the real widget tree for a device and walks it checking
    // no child overflows `panelBounds` (clipping) and the tree's
    // actual extent lands close to `panelBounds.height` (excess
    // unused space) - both against ComputeDetailsPanelHeight()'s own
    // output, so the two independent code paths are cross-checked by
    // this test in the same run. NOT static, unlike the above - it
    // captures `this` in each button's onClick (for m_bluez calls the
    // test never triggers), so it needs a real, if un-Initialize()'d,
    // BluetoothWindow instance to call it on.
    std::unique_ptr<Widget> BuildDetailsPanel(const BluetoothDevice& device, const std::string& adapterPath, const Rect& panelBounds);

private:

    enum class Page { Main, Advanced };

    void RebuildChrome();
    void RebuildPage();
    int RebuildMainPage(Widget& content, int contentWidth);
    int RebuildAdvancedPage(Widget& content, int contentWidth);

    int AppendToolbar(Widget& content, int y, int contentWidth);

    // Appends the CONNECTED DEVICES and NEARBY DEVICES cards, plus
    // (once there's a selection) the details panel - side by side or
    // stacked depending on `contentWidth`, the same split
    // NetworkWindow's network list/details uses. Returns the y
    // position just past whichever ends up lower.
    int AppendDeviceSection(Widget& content, int y, int contentWidth);

    std::unique_ptr<Widget> BuildDeviceRow(const BluetoothDevice& device, const Rect& rowBounds, bool showDivider);

    // Picks a sensible selected device after every rebuild: keeps the
    // current selection if it still exists, otherwise falls back to
    // the first connected device, otherwise the first device overall.
    void EnsureSelection(const std::vector<BluetoothDevice>& devices);

    UiWindow m_window;
    BluezClient m_bluez;
    AppInstanceLock m_instanceLock;
    AppConfigStore m_settings;
    NotificationClient m_notifications;

    Page m_page = Page::Main;
    ScrollView* m_scrollView = nullptr;

    bool m_bluezDirty = false;

    std::string m_selectedDevicePath;
};

}
