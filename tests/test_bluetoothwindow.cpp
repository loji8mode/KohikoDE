// Regression test guarding kohiko-bluetooth's details panel against
// the two failure modes Task 8 ("kohiko-bluetooth GUI responsiveness")
// explicitly names: clipping (a child widget spilling past the card's
// own bottom edge) and excessive unused space (the card sized taller
// than its actual content needs). Live Xvfb testing against a mock
// BlueZ service (see CHANGELOG.md's 0.20.4 entry) found neither
// currently happens - this test isn't fixing a bug, it's protecting
// against introducing one.
//
// Both failure modes share a root cause worth guarding against
// directly rather than only observing wasn't present today:
// AppendDeviceSection() has to know the details panel's height
// *before* BuildDetailsPanel() constructs it (to size the Card's own
// bounds up front, since a ScrollView's content Widget needs a
// bounds.height before anything inside it can be positioned), which
// used to mean two independently-maintained copies of the exact same
// "18 + 40 + 18 + 3*30 + 10 + buttonsHeight + 18" arithmetic, one in
// each function - a shape where the two are still equal today but
// nothing stops a future edit to one from silently not being made to
// the other. That's now a single function,
// BluetoothWindow::ComputeDetailsPanelHeight(), both call - so this
// test isn't just re-deriving the same number from the same formula
// twice; it builds the *actual* widget tree BuildDetailsPanel()
// produces and checks its real geometry against
// ComputeDetailsPanelHeight()'s output, the same cross-check
// AppendDeviceSection()/BuildDetailsPanel() rely on staying true at
// runtime.

#include "BluetoothWindow.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace Kohiko;

namespace
{

int g_pass = 0;

void Check(bool condition, const char* what)
{
    if (condition)
    {
        ++g_pass;
        std::printf("  PASS: %s\n", what);
    }
    else
    {
        std::printf("  FAIL: %s\n", what);
        std::exit(1);
    }
}

// The deepest bottom edge (relative to the card's own origin) among
// every descendant of `widget` - i.e. how far down this widget tree
// actually extends, regardless of how tall its own declared bounds
// claim to be.
int DeepestDescendantBottom(const Widget& widget, int originY)
{
    int deepest = widget.bounds.y - originY + widget.bounds.height;

    for (auto& child : widget.Children())
        deepest = std::max(deepest, DeepestDescendantBottom(*child, originY));

    return deepest;
}

BluetoothDevice MakeDevice(const std::string& name, bool paired, bool connected)
{
    BluetoothDevice device;
    device.objectPath = "/org/bluez/hci0/dev_test";
    device.address = "AA:BB:CC:DD:EE:FF";
    device.name = name;
    device.paired = paired;
    device.connected = connected;
    device.trusted = paired;
    device.batteryPercent = paired ? 73 : -1;
    return device;
}

void CheckOneDeviceState(BluetoothWindow& window, const BluetoothDevice& device, const char* label)
{
    int height = BluetoothWindow::ComputeDetailsPanelHeight(device);
    Check(height > 0, std::string(std::string(label) + ": ComputeDetailsPanelHeight() returns a positive height").c_str());

    Rect panelBounds{ 0, 0, 300, height };
    std::unique_ptr<Widget> card = window.BuildDetailsPanel(device, "/org/bluez/hci0", panelBounds);

    Check(card != nullptr, std::string(std::string(label) + ": BuildDetailsPanel() returns a real widget").c_str());

    int deepestBottom = DeepestDescendantBottom(*card, panelBounds.y);

    // Clipping: nothing inside the card should extend past the
    // height ComputeDetailsPanelHeight() said the card would be.
    Check(deepestBottom <= height,
          std::string(std::string(label) + ": no child widget extends past the computed card height "
              "(" + std::to_string(deepestBottom) + " <= " + std::to_string(height) + ")").c_str());

    // Excessive unused space: the deepest content shouldn't fall far
    // short of the computed height either - a generous few pixels of
    // intentional bottom padding is fine (BuildDetailsPanel() adds
    // 18px on purpose, matching its top padding), a card noticeably
    // taller than its content is the "excessive unused space" this
    // whole task is about.
    Check(deepestBottom >= height - 4,
          std::string(std::string(label) + ": the card isn't oversized for its actual content "
              "(deepest content at " + std::to_string(deepestBottom) + ", card height " + std::to_string(height) + ")").c_str());
}

}

int main()
{
    BluetoothWindow window; // never Initialize()'d - no X11/D-Bus needed for pure layout construction

    std::printf("Details panel height vs. actual content, for every paired/connected state:\n");

    CheckOneDeviceState(window, MakeDevice("New Device", /*paired=*/false, /*connected=*/false),
                        "not paired (just the Pair button)");
    CheckOneDeviceState(window, MakeDevice("Old Bluetooth Mouse", /*paired=*/true, /*connected=*/false),
                        "paired, not connected (Connect + Forget)");
    CheckOneDeviceState(window, MakeDevice("Kohiko Wireless Headphones", /*paired=*/true, /*connected=*/true),
                        "paired and connected (Disconnect + Forget + Disconnect-and-Forget)");

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    return 0;
}
