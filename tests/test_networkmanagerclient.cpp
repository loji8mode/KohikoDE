// Regression test for a real bug found while investigating a live
// Wi-Fi "psk mismatch reported by supplicant" report: kohiko-network's
// BytesToString() - which decodes NetworkManager's "array of bytes"
// representation of an SSID back into a plain std::string - read
// .Items() directly off the value it was given, without unwrapping
// it first.
//
// That value is always a D-Bus Variant at the one call site that
// mattered most: ConnectToAccessPoint()'s "reuse an existing saved
// connection for this SSID" logic reads the saved connection's own
// "ssid" out of a GetSettings()-shaped nested dict
// (settingsReply.front().Get("802-11-wireless").Get("ssid")), and at
// that specific nesting depth, DBusClient's own wire-reading code
// (ReadIterValue(), in DBusClient.cpp) wraps every a{sv} dict-entry
// value in a Variant, unconditionally - that's simply what an a{sv}
// dict's values are, on the wire. A Variant stores its payload in a
// separate internal slot from the one Items() reads, so reading
// .Items() straight off an un-unwrapped Variant doesn't fail loudly -
// it just quietly returns zero items, and BytesToString silently
// produces "" instead of the real SSID.
//
// The real-world consequence: BytesToString("") never equals any real
// SSID, so ConnectToAccessPoint()'s "is this SSID already saved"
// comparison never matched anything, for any network, ever - every
// connection attempt fell through to creating a brand-new connection
// profile instead of reusing (and, after a second, related fix,
// correctly updating the secret on) the existing one. See
// CHANGELOG.md's entry on this for the full investigation, including
// why this reproduces identically via plain `nmcli` and is not
// specific to Kohiko's own UI in any way that changes the root cause,
// even though the root cause itself is squarely in Kohiko's own code.
//
// Pure logic, no D-Bus connection needed - constructs DBusValue trees
// by hand, in exactly the shape DBusClient's own wire-reading code
// produces for a real GetSettings() reply's nested "ssid" field.

#include "NetworkManagerClient.h"

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

// Builds a DBusValue exactly the shape DBusClient's ReadIterValue()
// produces for one a{sv} dict-entry's value where that value is an
// array of bytes (ay) - i.e. a Variant boxing an Array of Byte -
// which is precisely what wifiSection.Get("ssid") returns in the real
// code this test is guarding.
DBusValue MakeWireFormatSsid(const std::string& ssid)
{
    std::vector<DBusValue> bytes;

    for (char c : ssid)
        bytes.push_back(DBusValue::MakeByte(static_cast<std::uint8_t>(c)));

    return DBusValue::MakeVariant(DBusValue::MakeArray(std::move(bytes), 'y'));
}

}

int main()
{
    std::printf("BytesToString() tests:\n");

    std::printf("-- The real bug: a Variant-wrapped byte array, exactly as GetSettings() returns --\n");
    {
        DBusValue wireFormat = MakeWireFormatSsid("Linux for best");

        Check(wireFormat.GetType() == DBusValue::Type::Variant,
              "sanity check: the constructed value really is Variant-wrapped, matching the real wire format");

        std::string decoded = BytesToString(wireFormat);

        Check(decoded == "Linux for best",
              "decodes correctly through the Variant wrapper - this is the exact case that used to silently "
              "produce \"\" instead, which is why ConnectToAccessPoint()'s saved-connection SSID match never fired");
    }

    std::printf("\n-- A second, differently-named SSID, to rule out some fixed/hardcoded string --\n");
    {
        std::string decoded = BytesToString(MakeWireFormatSsid("lolk"));
        Check(decoded == "lolk", "a different SSID decodes to itself, not to the first test's value or to \"\"");
    }

    std::printf("\n-- Defensive: an already-unwrapped array (not wrapped in a Variant) still works --\n");
    {
        std::vector<DBusValue> bytes;
        for (char c : std::string("open-network"))
            bytes.push_back(DBusValue::MakeByte(static_cast<std::uint8_t>(c)));
        DBusValue alreadyUnwrapped = DBusValue::MakeArray(std::move(bytes), 'y');

        Check(alreadyUnwrapped.GetType() == DBusValue::Type::Array,
              "sanity check: this one is genuinely not Variant-wrapped");

        std::string decoded = BytesToString(alreadyUnwrapped);
        Check(decoded == "open-network",
              "Unwrap() is a documented no-op on a non-Variant, so this path was never broken - "
              "confirming the fix doesn't depend on every caller wrapping its input the same way");
    }

    std::printf("\n-- Empty SSID bytes decode to an empty string (not a crash, not a placeholder) --\n");
    {
        std::string decoded = BytesToString(MakeWireFormatSsid(""));
        Check(decoded.empty(), "a genuinely empty SSID still decodes to \"\" - this only matters because "
                                "the bug this test guards against produced the exact same \"\" for every "
                                "*non*-empty SSID too, which is what made it silent");
    }

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    return 0;
}
