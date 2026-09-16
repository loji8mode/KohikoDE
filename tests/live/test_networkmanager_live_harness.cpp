// Driven by tests/test_networkmanager_live.sh - a real
// NetworkManagerClient talking, over a real (private, throwaway)
// D-Bus system bus, to tests/mock_networkmanager.py standing in for
// NetworkManager. Everything this binary can check directly (return
// values) is asserted here; everything about *what got called on the
// mock and with what arguments* (Update, ActivateConnection vs
// AddAndActivateConnection) is asserted by the driving shell script
// afterward, by inspecting the mock's own call-log file - this binary
// only needs to run the three ConnectToAccessPoint() calls the
// scenario needs, in order, then exit successfully so the log has
// something in it to check.
//
// Scenario this expects (see tests/network_live_scenario.json):
//   - "HomeNet"  - saved, has_secret=true,  psk="hunter2network"
//   - "OldNet"   - saved, has_secret=false (a stale/forgotten secret)
//   - "BrandNewNetwork" - not saved at all

#include "NetworkManagerClient.h"

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

}

int main()
{
    NetworkManagerClient nm;

    if (!nm.Connect())
    {
        std::printf("  FAIL: could not connect to the (mock) system bus at all - is "
                     "DBUS_SYSTEM_BUS_ADDRESS pointed at the fake bus, and is "
                     "mock_networkmanager.py actually running?\n");
        return 1;
    }

    std::printf("HasUsableSavedSecret() against the mock:\n");

    Check(nm.HasUsableSavedSecret("HomeNet"),
          "HomeNet (saved, real stored psk) reports a usable secret - the Connect button "
          "should skip the password prompt for this one");
    Check(!nm.HasUsableSavedSecret("OldNet"),
          "OldNet (saved connection profile exists, but GetSecrets returns nothing usable - "
          "a stale/forgotten secret) reports no usable secret - the Connect button should "
          "still prompt for this one, not silently try to connect with nothing");
    Check(!nm.HasUsableSavedSecret("BrandNewNetwork"),
          "a network with no saved connection profile at all reports no usable secret");

    std::printf("\nDriving the three ConnectToAccessPoint() calls the shell script checks "
                "against the mock's own call log:\n");

    // 1. Reuse path: empty password, saved secret exists - should
    //    activate the existing connection, never call Update() (an
    //    empty password isn't "the user wants no password", it's "the
    //    UI decided a prompt wasn't needed"), never create a new one.
    nm.ConnectToAccessPoint("/fake/device0", "/fake/ap/HomeNet", "HomeNet", "");
    std::printf("  ran: ConnectToAccessPoint(HomeNet, \"\")\n");

    // 2. Update path: a new password for an SSID that already has a
    //    saved (if stale) connection - should update that same
    //    connection's secret, then activate it - not create a second,
    //    duplicate profile for the same SSID.
    nm.ConnectToAccessPoint("/fake/device0", "/fake/ap/HomeNet", "HomeNet", "newpass123!");
    std::printf("  ran: ConnectToAccessPoint(HomeNet, \"newpass123!\")\n");

    // 3. Genuinely new network - no saved connection to reuse or
    //    update, so this is the one case that legitimately should
    //    call AddAndActivateConnection().
    nm.ConnectToAccessPoint("/fake/device1", "/fake/ap/BrandNewNetwork", "BrandNewNetwork", "somepassword");
    std::printf("  ran: ConnectToAccessPoint(BrandNewNetwork, \"somepassword\")\n");

    std::printf("\n%d in-process checks passed; see the driving shell script for the "
                "call-log assertions.\n", g_pass);
    return 0;
}
