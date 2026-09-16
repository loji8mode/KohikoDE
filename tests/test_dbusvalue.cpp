// Standalone correctness test for DBusValue - the variant type
// DBusClient uses to marshal/unmarshal every D-Bus method call
// (see include/DBusValue.h). Pure data-structure logic with no
// libdbus/system-bus connection involved, same "no X11/D-Bus needed"
// category as test_launcherscoring/test_placementhabits.
//
// Build & run: see the "test-dbusvalue" target in the Makefile, or
// `ctest` via CMake.

#include "DBusValue.h"

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
    std::printf("DBusValue tests:\n");

    // --- scalar round trips -----------------------------------------------

    Check(DBusValue::MakeBool(true).AsBool() == true, "bool true round-trips");
    Check(DBusValue::MakeBool(false).AsBool(true) == false, "bool false round-trips (not the fallback)");
    Check(DBusValue::MakeUInt32(42).AsInt() == 42, "uint32 readable as int");
    Check(DBusValue::MakeInt32(-7).AsInt() == -7, "negative int32 preserved");
    Check(DBusValue::MakeString("hello").AsString() == "hello", "string round-trips");
    Check(DBusValue::MakeObjectPath("/org/freedesktop/NetworkManager").AsString() == "/org/freedesktop/NetworkManager",
        "object path readable as string");
    Check(DBusValue::MakeDouble(3.5).AsDouble() == 3.5, "double round-trips");

    // --- fallback behavior on type mismatch --------------------------------

    Check(DBusValue::MakeString("x").AsBool(true) == true, "AsBool() on a String returns the fallback, not a coerced value");
    Check(DBusValue().AsString("fallback") == "fallback", "an Invalid DBusValue reports the fallback");
    Check(DBusValue().IsValid() == false, "a default-constructed DBusValue is Invalid");

    // --- numeric conversions between related integer kinds -----------------

    Check(DBusValue::MakeByte(200).AsUInt() == 200, "byte readable as uint (e.g. AccessPoint.Strength)");
    Check(DBusValue::MakeUInt64(1000000).AsInt() == 1000000, "uint64 readable as int for values that fit");

    // --- arrays -------------------------------------------------------------

    {
        std::vector<DBusValue> items = { DBusValue::MakeString("a"), DBusValue::MakeString("b"), DBusValue::MakeString("c") };
        DBusValue array = DBusValue::MakeArray(items, 's');
        Check(array.GetType() == DBusValue::Type::Array, "MakeArray produces Type::Array");
        Check(array.Items().size() == 3, "array preserves element count");
        Check(array.Items()[1].AsString() == "b", "array preserves element order");
    }

    {
        // What NetworkManagerClient::BytesToString() builds from an
        // AccessPoint's raw "ay" Ssid property.
        std::vector<std::string> ssidChars = { "K", "o", "h", "i", "k", "o" };
        DBusValue array = DBusValue::MakeStringArray(ssidChars);
        std::string joined;
        for (auto& item : array.Items())
            joined += item.AsString();
        Check(joined == "Kohiko", "MakeStringArray round-trips element-by-element");
    }

    // --- dicts (a{sv}) --------------------------------------------------------

    {
        std::map<std::string, DBusValue> entries;
        entries.emplace("Address", DBusValue::MakeVariant(DBusValue::MakeString("192.168.1.50")));
        entries.emplace("Strength", DBusValue::MakeVariant(DBusValue::MakeByte(87)));
        DBusValue dict = DBusValue::MakeDict(std::move(entries));

        Check(dict.GetType() == DBusValue::Type::Dict, "MakeDict produces Type::Dict");
        Check(dict.Has("Address"), "Has() finds a present key");
        Check(!dict.Has("Missing"), "Has() correctly reports an absent key");
        Check(dict.Get("Address").AsString() == "192.168.1.50",
            "Get() on a variant-boxed dict entry auto-unwraps (property values are always variants on the wire)");
        Check(dict.Get("Strength").AsUInt() == 87, "Get() works for non-string entry types too");
        Check(dict.Get("Missing").IsValid() == false, "Get() on an absent key returns an Invalid value, not a crash");
    }

    // --- variant unwrapping ---------------------------------------------------

    {
        DBusValue boxed = DBusValue::MakeVariant(DBusValue::MakeString("inner"));
        Check(boxed.GetType() == DBusValue::Type::Variant, "MakeVariant produces Type::Variant");
        Check(boxed.AsString() == "inner", "AsString() unwraps a Variant transparently");
        Check(boxed.Unwrap().GetType() == DBusValue::Type::String, "Unwrap() returns the boxed value's real type");

        // Double-boxed - shouldn't come up in practice, but Unwrap()
        // is documented to fully unwrap rather than stop at one layer.
        DBusValue doubleBoxed = DBusValue::MakeVariant(DBusValue::MakeVariant(DBusValue::MakeInt32(9)));
        Check(doubleBoxed.AsInt() == 9, "Unwrap() fully unwraps nested variants, not just one layer");
    }

    // --- structs ----------------------------------------------------------------

    {
        // What AddAndActivateConnection's reply (o,o) parses into.
        DBusValue tuple = DBusValue::MakeStruct({
            DBusValue::MakeObjectPath("/org/freedesktop/NetworkManager/Settings/1"),
            DBusValue::MakeObjectPath("/org/freedesktop/NetworkManager/ActiveConnection/1"),
        });
        Check(tuple.GetType() == DBusValue::Type::Struct, "MakeStruct produces Type::Struct (not Array)");
        Check(tuple.Items().size() == 2, "struct preserves item count");
        Check(tuple.Items()[0].AsString() == "/org/freedesktop/NetworkManager/Settings/1", "struct preserves item order");
    }

    std::printf("All %d DBusValue checks passed.\n", g_pass);
    return 0;
}
