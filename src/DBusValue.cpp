#include "DBusValue.h"

namespace Kohiko
{

namespace
{
const DBusValue kInvalid{};
}

DBusValue DBusValue::MakeBool(bool value)
{
    DBusValue v;
    v.m_type = Type::Bool;
    v.m_bool = value;
    return v;
}

DBusValue DBusValue::MakeByte(std::uint8_t value)
{
    DBusValue v;
    v.m_type = Type::Byte;
    v.m_uint = value;
    return v;
}

DBusValue DBusValue::MakeInt32(std::int32_t value)
{
    DBusValue v;
    v.m_type = Type::Int32;
    v.m_int = value;
    return v;
}

DBusValue DBusValue::MakeUInt32(std::uint32_t value)
{
    DBusValue v;
    v.m_type = Type::UInt32;
    v.m_uint = value;
    return v;
}

DBusValue DBusValue::MakeInt64(std::int64_t value)
{
    DBusValue v;
    v.m_type = Type::Int64;
    v.m_int = value;
    return v;
}

DBusValue DBusValue::MakeUInt64(std::uint64_t value)
{
    DBusValue v;
    v.m_type = Type::UInt64;
    v.m_uint = value;
    return v;
}

DBusValue DBusValue::MakeDouble(double value)
{
    DBusValue v;
    v.m_type = Type::Double;
    v.m_double = value;
    return v;
}

DBusValue DBusValue::MakeString(std::string value)
{
    DBusValue v;
    v.m_type = Type::String;
    v.m_string = std::move(value);
    return v;
}

DBusValue DBusValue::MakeObjectPath(std::string value)
{
    DBusValue v;
    v.m_type = Type::ObjectPath;
    v.m_string = std::move(value);
    return v;
}

DBusValue DBusValue::MakeArray(std::vector<DBusValue> items, char elementSignature)
{
    DBusValue v;
    v.m_type = Type::Array;
    v.m_items = std::move(items);
    v.m_arrayElementSignature = elementSignature;
    return v;
}

DBusValue DBusValue::MakeStringArray(std::vector<std::string> items)
{
    std::vector<DBusValue> boxed;
    boxed.reserve(items.size());

    for (auto& s : items)
        boxed.push_back(MakeString(std::move(s)));

    return MakeArray(std::move(boxed), 's');
}

DBusValue DBusValue::MakeStruct(std::vector<DBusValue> items)
{
    DBusValue v;
    v.m_type = Type::Struct;
    v.m_items = std::move(items);
    return v;
}

DBusValue DBusValue::MakeDict(std::map<std::string, DBusValue> entries)
{
    DBusValue v;
    v.m_type = Type::Dict;
    v.m_entries = std::move(entries);
    return v;
}

DBusValue DBusValue::MakeVariant(DBusValue boxed)
{
    DBusValue v;
    v.m_type = Type::Variant;
    v.m_boxed = std::make_shared<DBusValue>(std::move(boxed));
    return v;
}

bool DBusValue::AsBool(bool fallback) const
{
    const DBusValue& v = Unwrap();

    switch (v.m_type)
    {
        case Type::Bool:   return v.m_bool;
        case Type::Byte:
        case Type::UInt16:
        case Type::UInt32:
        case Type::UInt64: return v.m_uint != 0;
        case Type::Int16:
        case Type::Int32:
        case Type::Int64:  return v.m_int != 0;
        default:           return fallback;
    }
}

std::int64_t DBusValue::AsInt(std::int64_t fallback) const
{
    const DBusValue& v = Unwrap();

    switch (v.m_type)
    {
        case Type::Byte:
        case Type::UInt16:
        case Type::UInt32:
        case Type::UInt64: return static_cast<std::int64_t>(v.m_uint);
        case Type::Int16:
        case Type::Int32:
        case Type::Int64:  return v.m_int;
        case Type::Double: return static_cast<std::int64_t>(v.m_double);
        case Type::Bool:   return v.m_bool ? 1 : 0;
        default:           return fallback;
    }
}

std::uint64_t DBusValue::AsUInt(std::uint64_t fallback) const
{
    const DBusValue& v = Unwrap();

    switch (v.m_type)
    {
        case Type::Byte:
        case Type::UInt16:
        case Type::UInt32:
        case Type::UInt64: return v.m_uint;
        case Type::Int16:
        case Type::Int32:
        case Type::Int64:  return v.m_int < 0 ? 0 : static_cast<std::uint64_t>(v.m_int);
        case Type::Bool:   return v.m_bool ? 1 : 0;
        default:           return fallback;
    }
}

double DBusValue::AsDouble(double fallback) const
{
    const DBusValue& v = Unwrap();

    switch (v.m_type)
    {
        case Type::Double: return v.m_double;
        case Type::Byte:
        case Type::UInt16:
        case Type::UInt32:
        case Type::UInt64: return static_cast<double>(v.m_uint);
        case Type::Int16:
        case Type::Int32:
        case Type::Int64:  return static_cast<double>(v.m_int);
        default:           return fallback;
    }
}

std::string DBusValue::AsString(const std::string& fallback) const
{
    const DBusValue& v = Unwrap();

    if (v.m_type == Type::String || v.m_type == Type::ObjectPath)
        return v.m_string;

    return fallback;
}

const DBusValue& DBusValue::Get(const std::string& key) const
{
    const DBusValue& v = Unwrap();

    auto it = v.m_entries.find(key);
    if (it == v.m_entries.end())
        return kInvalid;

    return it->second;
}

bool DBusValue::Has(const std::string& key) const
{
    const DBusValue& v = Unwrap();
    return v.m_entries.find(key) != v.m_entries.end();
}

const DBusValue& DBusValue::Unwrap() const
{
    const DBusValue* v = this;

    // A variant can (rarely) box another variant - unwrap fully
    // rather than just once.
    while (v->m_type == Type::Variant && v->m_boxed)
        v = v->m_boxed.get();

    return *v;
}

}
