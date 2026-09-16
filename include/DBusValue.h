#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Kohiko
{

// A small tagged-union tree that can represent any value that comes
// back from (or needs to go into) a D-Bus method call: NetworkManager
// and BlueZ both lean heavily on nested a{sv} property dictionaries,
// arrays of object paths, and arrays of structs, none of which map
// onto a single C++ type. Rather than have NetworkManagerClient and
// BluezClient each hand-walk DBusMessageIter (as
// ScreenSaverInhibitor.cpp does for its own much simpler messages),
// they build/read one of these instead and DBusClient/DBusValue own
// the iterator bookkeeping exactly once, in one place.
//
// Deliberately not a template/std::variant-based design - a plain
// discriminated struct with one vector/map member per composite kind
// keeps DBusValue.cpp's iterator-walking code straightforward
// recursion instead of visitor-pattern boilerplate, and every caller
// in this codebase only ever wants "give me the string/int/bool/array
// at this key" rather than generic functional-style access.
class DBusValue
{
public:

    enum class Type
    {
        Invalid,
        Bool,
        Byte,
        Int16,
        UInt16,
        Int32,
        UInt32,
        Int64,
        UInt64,
        Double,
        String,
        ObjectPath,
        Array,      // homogeneous list - see Items()
        Dict,       // a{sv}-style string-keyed map - see Entries()
        Struct,     // heterogeneous fixed-size tuple - see Items()
        Variant     // a single boxed value - see Variant Unwrap() below
    };

    DBusValue() = default;

    static DBusValue MakeBool(bool value);
    static DBusValue MakeByte(std::uint8_t value);
    static DBusValue MakeInt32(std::int32_t value);
    static DBusValue MakeUInt32(std::uint32_t value);
    static DBusValue MakeInt64(std::int64_t value);
    static DBusValue MakeUInt64(std::uint64_t value);
    static DBusValue MakeDouble(double value);
    static DBusValue MakeString(std::string value);
    static DBusValue MakeObjectPath(std::string value);
    static DBusValue MakeArray(std::vector<DBusValue> items, char elementSignature = 's');
    static DBusValue MakeStringArray(std::vector<std::string> items);
    static DBusValue MakeVariant(DBusValue boxed);

    // Builds a Type::Dict directly from already-parsed entries - used
    // by DBusClient when reading an a{sv}-shaped message argument back
    // off the wire (see DBusClient.cpp's ReadIterValue()), and
    // available to any caller that wants to build one to send (e.g.
    // NetworkManagerClient's AddAndActivateConnection() settings
    // argument).
    static DBusValue MakeDict(std::map<std::string, DBusValue> entries);
    static DBusValue MakeStruct(std::vector<DBusValue> items);

    Type GetType() const { return m_type; }
    bool IsValid() const { return m_type != Type::Invalid; }

    // Loose accessors - each returns `fallback` if this value isn't
    // (convertible to) the requested type, rather than throwing. Every
    // caller in this codebase is reading server-controlled data it
    // wants to treat as best-effort/optional (a property that may or
    // may not be present, a device that may not expose battery info,
    // ...), so "missing means fallback" is far more useful than an
    // exception every call site would have to guard against anyway.
    bool AsBool(bool fallback = false) const;
    std::int64_t AsInt(std::int64_t fallback = 0) const;
    std::uint64_t AsUInt(std::uint64_t fallback = 0) const;
    double AsDouble(double fallback = 0.0) const;
    std::string AsString(const std::string& fallback = "") const;

    // Array/Struct element access.
    const std::vector<DBusValue>& Items() const { return m_items; }

    // Dict (a{sv}) access - `key` not present returns an Invalid value
    // (IsValid() == false), same "caller checks/falls back" contract
    // as the scalar accessors above.
    const DBusValue& Get(const std::string& key) const;
    bool Has(const std::string& key) const;
    const std::map<std::string, DBusValue>& Entries() const { return m_entries; }

    // Unwraps one layer of Variant - a no-op (returns *this) if this
    // isn't actually a Variant, since a{sv} dict values are variants
    // by construction but plenty of call sites just want "the value"
    // without caring whether an extra variant box was in the way.
    const DBusValue& Unwrap() const;

    char ArrayElementSignature() const { return m_arrayElementSignature; }

private:

    Type m_type = Type::Invalid;

    bool m_bool = false;
    std::int64_t m_int = 0;
    std::uint64_t m_uint = 0;
    double m_double = 0.0;
    std::string m_string;

    std::vector<DBusValue> m_items;
    std::map<std::string, DBusValue> m_entries;
    char m_arrayElementSignature = 's';

    // Used only by Variant - the single boxed value. Kept as a
    // separate member (rather than reusing m_items[0]) so
    // Type::Variant and Type::Array/Struct never have to share one
    // ambiguous representation.
    std::shared_ptr<DBusValue> m_boxed;
};

}
