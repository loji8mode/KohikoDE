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

#include <poll.h>

#include <functional>

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

    // The client's actions are asynchronous: their replies arrive through
    // Dispatch(), driven by the client's own fd - exactly what a native
    // window's event loop does. These helpers do the same, and wait for a
    // condition without ever blocking inside a D-Bus call.
    auto pumpUntil = [&](const std::function<bool()>& done, int timeoutMs)
    {
        for (int waited = 0; waited < timeoutMs && !done(); waited += 20)
        {
            pollfd pfd{ nm.Fd(), POLLIN, 0 };

            if (poll(&pfd, 1, 20) > 0)
                nm.Dispatch();
        }

        return done();
    };

    auto usable = [&](const char* ssid)
    {
        bool answered = false;
        bool result = false;

        nm.QueryUsableSavedSecret(ssid, [&](bool u) { answered = true; result = u; });
        Check(pumpUntil([&] { return answered; }, 3000), "the secret query answered through the event loop");

        return result;
    };

    auto connect = [&](const char* device, const char* ap, const char* ssid, const char* password)
    {
        bool finished = false;
        bool succeeded = false;

        nm.ConnectToAccessPoint(device, ap, ssid, password, [&](bool ok, const std::string&) { finished = true; succeeded = ok; });
        Check(pumpUntil([&] { return finished; }, 3000), "the connect finished through the event loop");

        return succeeded;
    };

    std::printf("QueryUsableSavedSecret() against the mock:\n");

    Check(usable("HomeNet"),
          "HomeNet (saved, real stored psk) reports a usable secret - the Connect button "
          "should skip the password prompt for this one");
    Check(!usable("OldNet"),
          "OldNet (saved connection profile exists, but GetSecrets returns nothing usable - "
          "a stale/forgotten secret) reports no usable secret - the Connect button should "
          "still prompt for this one, not silently try to connect with nothing");
    Check(!usable("BrandNewNetwork"),
          "a network with no saved connection profile at all reports no usable secret");

    std::printf("\nDriving the three ConnectToAccessPoint() calls the shell script checks "
                "against the mock's own call log:\n");

    // 1. Reuse path: empty password, saved secret exists - should
    //    activate the existing connection, never call Update() (an
    //    empty password isn't "the user wants no password", it's "the
    //    UI decided a prompt wasn't needed"), never create a new one.
    Check(connect("/fake/device0", "/fake/ap/HomeNet", "HomeNet", ""), "ConnectToAccessPoint(HomeNet, \"\") completed successfully");

    // 2. Update path: a new password for an SSID that already has a
    //    saved (if stale) connection - should update that same
    //    connection's secret, then activate it - not create a second,
    //    duplicate profile for the same SSID.
    Check(connect("/fake/device0", "/fake/ap/HomeNet", "HomeNet", "newpass123!"), "ConnectToAccessPoint(HomeNet, \"newpass123!\") completed successfully");

    // 3. Genuinely new network - no saved connection to reuse or
    //    update, so this is the one case that legitimately should
    //    call AddAndActivateConnection().
    Check(connect("/fake/device1", "/fake/ap/BrandNewNetwork", "BrandNewNetwork", "somepassword"), "ConnectToAccessPoint(BrandNewNetwork, \"somepassword\") completed successfully");

    Check(nm.PendingActions() == 0, "no action is left pending afterwards");
    Check(nm.LastError().empty(), "and none reported a failure");

    std::printf("\n%d in-process checks passed; see the driving shell script for the "
                "call-log assertions.\n", g_pass);
    return 0;
}
