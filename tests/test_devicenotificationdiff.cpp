// Regression tests for DeviceNotificationDiff.h, added after a real,
// field-reported bug: a single physical device connecting produced
// TWO "connected" toasts, because a PipeWire sink's own automatically-
// created ".monitor" source companion was being diffed as if it were a
// second, separate device. This logic used to live inline, untested,
// in kohiko-audio-tray.cpp's main() - exactly why the bug went
// unnoticed by the original (Xvfb-only) test pass. Pure logic, no
// X11/PipeWire dependency at all.

#include "DeviceNotificationDiff.h"

#include <cstdio>
#include <cstdlib>

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

bool Contains(const std::vector<DeviceChange>& changes, DeviceChangeKind kind, const std::string& label)
{
    for (auto& change : changes)
        if (change.kind == kind && change.label == label)
            return true;

    return false;
}

}

int main()
{
    std::printf("DeviceNotificationDiff tests (pure logic, no X11/PipeWire):\n");

    std::printf("-- IsMonitorSource(): the actual field bug --\n");
    {
        Check(IsMonitorSource(/*isSource=*/true, "testheadphones.monitor"),
              "a source node named \"<sink>.monitor\" is recognized as a monitor companion, not a real device - "
              "this is the exact real-world node name a plugged-in sink's own monitor gets (confirmed against a "
              "real pipewire+wireplumber session), and the root cause of the field-reported duplicate notification");

        Check(!IsMonitorSource(/*isSource=*/true, "USB Headset Microphone"),
              "an ordinary, real microphone source is NOT mistaken for a monitor just because it's a source");

        Check(!IsMonitorSource(/*isSource=*/false, "testheadphones"),
              "a sink itself (isSource=false) is never treated as a monitor, regardless of its name");

        Check(!IsMonitorSource(/*isSource=*/true, "monitor"),
              "the bare word \"monitor\" with no leading dot is not treated as the \".monitor\" suffix - this "
              "checks the specific naming convention, not merely \"contains the word monitor\"");

        Check(!IsMonitorSource(/*isSource=*/true, ""),
              "an empty name doesn't crash and is not treated as a monitor");
    }

    std::printf("\n-- ComputeDeviceChanges(): one real device event produces exactly one notification --\n");
    {
        std::map<std::uint32_t, std::string> previous = {{1, "Built-in Speakers"}};
        std::map<std::uint32_t, std::string> current = {{1, "Built-in Speakers"}, {2, "USB Headphones"}};

        auto changes = ComputeDeviceChanges(previous, current);

        Check(changes.size() == 1,
              "one new id in `current` produces exactly one DeviceChange - not two, not zero");
        Check(Contains(changes, DeviceChangeKind::Connected, "USB Headphones"),
              "...and it's correctly reported as Connected, with the right label");
    }

    std::printf("\n-- ComputeDeviceChanges(): disconnect produces exactly one notification --\n");
    {
        std::map<std::uint32_t, std::string> previous = {{1, "Built-in Speakers"}, {2, "USB Headphones"}};
        std::map<std::uint32_t, std::string> current = {{1, "Built-in Speakers"}};

        auto changes = ComputeDeviceChanges(previous, current);

        Check(changes.size() == 1, "one id removed from `current` produces exactly one DeviceChange");
        Check(Contains(changes, DeviceChangeKind::Disconnected, "USB Headphones"),
              "...and it's correctly reported as Disconnected, with the right label");
    }

    std::printf("\n-- ComputeDeviceChanges(): no change at all produces no notifications --\n");
    {
        std::map<std::uint32_t, std::string> snapshot = {{1, "Built-in Speakers"}, {2, "USB Headphones"}};

        auto changes = ComputeDeviceChanges(snapshot, snapshot);

        Check(changes.empty(),
              "re-diffing an unchanged snapshot against itself produces zero changes - a SetChangeHandler firing "
              "for an unrelated reason (volume, default-device switch - see that method's own comment) must not "
              "re-post a notification for devices that didn't actually change");
    }

    std::printf("\n-- ComputeDeviceChanges(): simultaneous connect and disconnect (e.g. a port/profile swap) --\n");
    {
        std::map<std::uint32_t, std::string> previous = {{1, "Old Device"}};
        std::map<std::uint32_t, std::string> current = {{2, "New Device"}};

        auto changes = ComputeDeviceChanges(previous, current);

        Check(changes.size() == 2, "one id leaving and a different id arriving in the same diff produces exactly "
                                    "two changes, not conflated into one and not missing either");
        Check(Contains(changes, DeviceChangeKind::Connected, "New Device"), "...the arrival is reported");
        Check(Contains(changes, DeviceChangeKind::Disconnected, "Old Device"), "...and the departure is reported");
    }

    std::printf("\n-- ComputeDeviceChanges(): repeated connect/disconnect cycles never duplicate or leak --\n");
    {
        std::map<std::uint32_t, std::string> known = {};

        // Cycle 1: connect id=5, then disconnect it.
        std::map<std::uint32_t, std::string> afterConnect1 = {{5, "Headphones"}};
        auto changes1 = ComputeDeviceChanges(known, afterConnect1);
        Check(changes1.size() == 1 && Contains(changes1, DeviceChangeKind::Connected, "Headphones"),
              "cycle 1 connect: exactly one Connected change");
        known = afterConnect1;

        std::map<std::uint32_t, std::string> afterDisconnect1 = {};
        auto changes2 = ComputeDeviceChanges(known, afterDisconnect1);
        Check(changes2.size() == 1 && Contains(changes2, DeviceChangeKind::Disconnected, "Headphones"),
              "cycle 1 disconnect: exactly one Disconnected change");
        known = afterDisconnect1;

        // Cycle 2: the *same* real device reconnects, getting a *new*
        // id this time (PipeWire node ids are never reused - a
        // reconnect is a brand new node) - must be treated as a fresh
        // connect, not silently ignored as "already known" and not
        // duplicated.
        std::map<std::uint32_t, std::string> afterConnect2 = {{9, "Headphones"}};
        auto changes3 = ComputeDeviceChanges(known, afterConnect2);
        Check(changes3.size() == 1 && Contains(changes3, DeviceChangeKind::Connected, "Headphones"),
              "cycle 2 connect (same device, new id after reconnecting): exactly one Connected change - not "
              "zero (silently dropped) and not duplicated from cycle 1's stale state");
        known = afterConnect2;

        std::map<std::uint32_t, std::string> afterDisconnect2 = {};
        auto changes4 = ComputeDeviceChanges(known, afterDisconnect2);
        Check(changes4.size() == 1 && Contains(changes4, DeviceChangeKind::Disconnected, "Headphones"),
              "cycle 2 disconnect: exactly one Disconnected change, and nothing left over from cycle 1");
    }

    std::printf("\n-- ComputeDeviceChanges(): a sink and its own monitor arriving together, once filtered --\n");
    {
        // Mirrors NodeLabels() in kohiko-audio-tray.cpp: the monitor
        // companion is expected to already be filtered out by
        // IsMonitorSource() *before* either snapshot is built, so from
        // this function's own point of view there is only ever one new
        // id, not two - this is the actual fix for the field bug,
        // expressed as a diff-level assertion rather than only a
        // filter-level one.
        std::map<std::uint32_t, std::string> previous = {};
        std::map<std::uint32_t, std::string> current = {{1, "Test Headphones"}}; // .monitor already filtered out

        auto changes = ComputeDeviceChanges(previous, current);

        Check(changes.size() == 1,
              "with the monitor companion already filtered out of the snapshot, one physical device connecting "
              "produces exactly one notification, not two");
    }

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    return 0;
}
