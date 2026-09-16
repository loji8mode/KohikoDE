#include "DBusClient.h"

#include <dbus/dbus.h>

#include <cstring>

namespace Kohiko
{

namespace
{

// --- reading -----------------------------------------------------------------

DBusValue ReadBasicValue(DBusMessageIter* iter, int argType)
{
    switch (argType)
    {
        case DBUS_TYPE_BOOLEAN:
        {
            dbus_bool_t v = FALSE;
            dbus_message_iter_get_basic(iter, &v);
            return DBusValue::MakeBool(v != FALSE);
        }
        case DBUS_TYPE_BYTE:
        {
            unsigned char v = 0;
            dbus_message_iter_get_basic(iter, &v);
            return DBusValue::MakeByte(v);
        }
        case DBUS_TYPE_INT16:
        {
            dbus_int16_t v = 0;
            dbus_message_iter_get_basic(iter, &v);
            return DBusValue::MakeInt32(v);
        }
        case DBUS_TYPE_UINT16:
        {
            dbus_uint16_t v = 0;
            dbus_message_iter_get_basic(iter, &v);
            return DBusValue::MakeUInt32(v);
        }
        case DBUS_TYPE_INT32:
        {
            dbus_int32_t v = 0;
            dbus_message_iter_get_basic(iter, &v);
            return DBusValue::MakeInt32(v);
        }
        case DBUS_TYPE_UINT32:
        {
            dbus_uint32_t v = 0;
            dbus_message_iter_get_basic(iter, &v);
            return DBusValue::MakeUInt32(v);
        }
        case DBUS_TYPE_INT64:
        {
            dbus_int64_t v = 0;
            dbus_message_iter_get_basic(iter, &v);
            return DBusValue::MakeInt64(v);
        }
        case DBUS_TYPE_UINT64:
        {
            dbus_uint64_t v = 0;
            dbus_message_iter_get_basic(iter, &v);
            return DBusValue::MakeUInt64(v);
        }
        case DBUS_TYPE_DOUBLE:
        {
            double v = 0.0;
            dbus_message_iter_get_basic(iter, &v);
            return DBusValue::MakeDouble(v);
        }
        case DBUS_TYPE_STRING:
        {
            const char* v = "";
            dbus_message_iter_get_basic(iter, &v);
            return DBusValue::MakeString(v ? v : "");
        }
        case DBUS_TYPE_OBJECT_PATH:
        {
            const char* v = "";
            dbus_message_iter_get_basic(iter, &v);
            return DBusValue::MakeObjectPath(v ? v : "");
        }
        case DBUS_TYPE_SIGNATURE:
        {
            const char* v = "";
            dbus_message_iter_get_basic(iter, &v);
            return DBusValue::MakeString(v ? v : "");
        }
        default:
            return DBusValue();
    }
}

DBusValue ReadIterValue(DBusMessageIter* iter)
{
    int argType = dbus_message_iter_get_arg_type(iter);

    switch (argType)
    {
        case DBUS_TYPE_VARIANT:
        {
            DBusMessageIter sub;
            dbus_message_iter_recurse(iter, &sub);
            return DBusValue::MakeVariant(ReadIterValue(&sub));
        }

        case DBUS_TYPE_ARRAY:
        {
            int elementType = dbus_message_iter_get_element_type(iter);

            DBusMessageIter sub;
            dbus_message_iter_recurse(iter, &sub);

            if (elementType == DBUS_TYPE_DICT_ENTRY)
            {
                std::map<std::string, DBusValue> entries;

                while (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_DICT_ENTRY)
                {
                    DBusMessageIter entryIter;
                    dbus_message_iter_recurse(&sub, &entryIter);

                    DBusValue key = ReadIterValue(&entryIter);
                    dbus_message_iter_next(&entryIter);
                    DBusValue value = ReadIterValue(&entryIter);

                    std::string keyString = key.AsString();
                    if (keyString.empty() && key.GetType() != DBusValue::Type::String &&
                        key.GetType() != DBusValue::Type::ObjectPath)
                    {
                        // Non-string dict keys (a{iv}, a{uv}, ...) don't
                        // come up anywhere Kohiko talks to
                        // (NetworkManager/BlueZ/Notifications are all
                        // string- or object-path-keyed dicts) - fall
                        // back to a numeric-looking string rather than
                        // silently dropping the entry.
                        keyString = std::to_string(key.AsUInt());
                    }

                    entries.emplace(std::move(keyString), std::move(value));
                    dbus_message_iter_next(&sub);
                }

                return DBusValue::MakeDict(std::move(entries));
            }

            std::vector<DBusValue> items;
            char elementSig = 's';

            while (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID)
            {
                elementSig = static_cast<char>(dbus_message_iter_get_arg_type(&sub));
                items.push_back(ReadIterValue(&sub));
                dbus_message_iter_next(&sub);
            }

            return DBusValue::MakeArray(std::move(items), elementSig);
        }

        case DBUS_TYPE_STRUCT:
        {
            DBusMessageIter sub;
            dbus_message_iter_recurse(iter, &sub);

            std::vector<DBusValue> items;
            while (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID)
            {
                items.push_back(ReadIterValue(&sub));
                dbus_message_iter_next(&sub);
            }

            return DBusValue::MakeStruct(std::move(items));
        }

        default:
            return ReadBasicValue(iter, argType);
    }
}

// --- writing -----------------------------------------------------------------

std::string BuildSignature(const DBusValue& value);

std::string DictValueSignature(const DBusValue& dict)
{
    if (dict.Entries().empty())
        return "v"; // empty a{sv} is by far the common case (GetAll() on an object with no props)

    return BuildSignature(dict.Entries().begin()->second);
}

std::string BuildSignature(const DBusValue& value)
{
    switch (value.GetType())
    {
        case DBusValue::Type::Bool:       return "b";
        case DBusValue::Type::Byte:       return "y";
        case DBusValue::Type::Int16:      return "n";
        case DBusValue::Type::UInt16:     return "q";
        case DBusValue::Type::Int32:      return "i";
        case DBusValue::Type::UInt32:     return "u";
        case DBusValue::Type::Int64:      return "x";
        case DBusValue::Type::UInt64:     return "t";
        case DBusValue::Type::Double:     return "d";
        case DBusValue::Type::String:     return "s";
        case DBusValue::Type::ObjectPath: return "o";
        case DBusValue::Type::Variant:    return "v";
        case DBusValue::Type::Dict:
            return "a{s" + DictValueSignature(value) + "}";
        case DBusValue::Type::Array:
        {
            if (!value.Items().empty())
                return "a" + BuildSignature(value.Items().front());

            return std::string("a") + value.ArrayElementSignature();
        }
        case DBusValue::Type::Struct:
        {
            std::string sig = "(";
            for (auto& item : value.Items())
                sig += BuildSignature(item);
            sig += ")";
            return sig;
        }
        default:
            return "v";
    }
}

void AppendValue(DBusMessageIter* iter, const DBusValue& value)
{
    switch (value.GetType())
    {
        case DBusValue::Type::Bool:
        {
            dbus_bool_t v = value.AsBool() ? TRUE : FALSE;
            dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &v);
            return;
        }
        case DBusValue::Type::Byte:
        {
            unsigned char v = static_cast<unsigned char>(value.AsUInt());
            dbus_message_iter_append_basic(iter, DBUS_TYPE_BYTE, &v);
            return;
        }
        case DBusValue::Type::Int32:
        {
            dbus_int32_t v = static_cast<dbus_int32_t>(value.AsInt());
            dbus_message_iter_append_basic(iter, DBUS_TYPE_INT32, &v);
            return;
        }
        case DBusValue::Type::UInt32:
        {
            dbus_uint32_t v = static_cast<dbus_uint32_t>(value.AsUInt());
            dbus_message_iter_append_basic(iter, DBUS_TYPE_UINT32, &v);
            return;
        }
        case DBusValue::Type::Int64:
        {
            dbus_int64_t v = static_cast<dbus_int64_t>(value.AsInt());
            dbus_message_iter_append_basic(iter, DBUS_TYPE_INT64, &v);
            return;
        }
        case DBusValue::Type::UInt64:
        {
            dbus_uint64_t v = static_cast<dbus_uint64_t>(value.AsUInt());
            dbus_message_iter_append_basic(iter, DBUS_TYPE_UINT64, &v);
            return;
        }
        case DBusValue::Type::Double:
        {
            double v = value.AsDouble();
            dbus_message_iter_append_basic(iter, DBUS_TYPE_DOUBLE, &v);
            return;
        }
        case DBusValue::Type::String:
        {
            std::string s = value.AsString();
            const char* v = s.c_str();
            dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &v);
            return;
        }
        case DBusValue::Type::ObjectPath:
        {
            std::string s = value.AsString();
            const char* v = s.c_str();
            dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH, &v);
            return;
        }
        case DBusValue::Type::Variant:
        {
            const DBusValue& boxed = value.Unwrap();
            std::string sig = BuildSignature(boxed);

            DBusMessageIter variantIter;
            dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, sig.c_str(), &variantIter);
            AppendValue(&variantIter, boxed);
            dbus_message_iter_close_container(iter, &variantIter);
            return;
        }
        case DBusValue::Type::Array:
        {
            std::string elementSig = value.Items().empty()
                ? std::string(1, value.ArrayElementSignature())
                : BuildSignature(value.Items().front());

            DBusMessageIter arrayIter;
            dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, elementSig.c_str(), &arrayIter);
            for (auto& item : value.Items())
                AppendValue(&arrayIter, item);
            dbus_message_iter_close_container(iter, &arrayIter);
            return;
        }
        case DBusValue::Type::Dict:
        {
            std::string valueSig = DictValueSignature(value);
            std::string containerSig = "{s" + valueSig + "}";

            DBusMessageIter arrayIter;
            dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, containerSig.c_str(), &arrayIter);

            for (auto& [key, entryValue] : value.Entries())
            {
                DBusMessageIter entryIter;
                dbus_message_iter_open_container(&arrayIter, DBUS_TYPE_DICT_ENTRY, nullptr, &entryIter);

                const char* keyPtr = key.c_str();
                dbus_message_iter_append_basic(&entryIter, DBUS_TYPE_STRING, &keyPtr);
                AppendValue(&entryIter, entryValue);

                dbus_message_iter_close_container(&arrayIter, &entryIter);
            }

            dbus_message_iter_close_container(iter, &arrayIter);
            return;
        }
        case DBusValue::Type::Struct:
        {
            DBusMessageIter structIter;
            dbus_message_iter_open_container(iter, DBUS_TYPE_STRUCT, nullptr, &structIter);
            for (auto& item : value.Items())
                AppendValue(&structIter, item);
            dbus_message_iter_close_container(iter, &structIter);
            return;
        }
        default:
            return;
    }
}

DBusHandlerResult FilterCallback(DBusConnection*, DBusMessage* message, void* userData)
{
    static_cast<DBusClient*>(userData)->HandleMessage(message);
    return DBUS_HANDLER_RESULT_HANDLED;
}

}

DBusClient::DBusClient() = default;

DBusClient::~DBusClient()
{
    if (m_connection)
    {
        dbus_connection_close(m_connection);
        dbus_connection_unref(m_connection);
    }
}

bool DBusClient::Connect(Bus bus)
{
    DBusError error;
    dbus_error_init(&error);

    m_connection = dbus_bus_get_private(
        bus == Bus::System ? DBUS_BUS_SYSTEM : DBUS_BUS_SESSION,
        &error
    );

    if (!m_connection)
    {
        dbus_error_free(&error);
        return false;
    }

    dbus_connection_set_exit_on_disconnect(m_connection, FALSE);
    dbus_connection_add_filter(m_connection, FilterCallback, this, nullptr);

    m_available = true;
    return true;
}

bool DBusClient::Available() const
{
    return m_available;
}

int DBusClient::Fd() const
{
    if (!m_available)
        return -1;

    int fd = -1;
    if (!dbus_connection_get_unix_fd(m_connection, &fd))
        return -1;

    return fd;
}

void DBusClient::Dispatch()
{
    if (!m_available)
        return;

    while (dbus_connection_read_write_dispatch(m_connection, 0))
    {
        // dbus_connection_read_write_dispatch() returns FALSE once the
        // connection has been disconnected, TRUE otherwise (including
        // "nothing was ready to process this time") - loop only while
        // there's a full dispatch cycle actually making progress, one
        // iteration per call, matching how EventLoop.cpp drains the
        // X11 connection after its own fd goes readable.
        DBusDispatchStatus status = dbus_connection_get_dispatch_status(m_connection);
        if (status != DBUS_DISPATCH_DATA_REMAINS)
            break;
    }
}

bool DBusClient::Call(
    const std::string& destination,
    const std::string& objectPath,
    const std::string& interface,
    const std::string& method,
    const std::vector<DBusValue>& args,
    std::vector<DBusValue>* outReply,
    std::string* errorOut,
    int timeoutMs)
{
    if (!m_available)
    {
        if (errorOut) *errorOut = "no D-Bus connection";
        return false;
    }

    DBusMessage* msg = dbus_message_new_method_call(
        destination.c_str(),
        objectPath.c_str(),
        interface.empty() ? nullptr : interface.c_str(),
        method.c_str()
    );

    if (!msg)
    {
        if (errorOut) *errorOut = "failed to allocate message";
        return false;
    }

    DBusMessageIter iter;
    dbus_message_iter_init_append(msg, &iter);
    for (auto& arg : args)
        AppendValue(&iter, arg);

    DBusError error;
    dbus_error_init(&error);

    DBusMessage* reply = dbus_connection_send_with_reply_and_block(m_connection, msg, timeoutMs, &error);
    dbus_message_unref(msg);

    if (!reply)
    {
        if (errorOut) *errorOut = error.message ? error.message : "unknown D-Bus error";
        dbus_error_free(&error);
        return false;
    }

    if (outReply)
    {
        DBusMessageIter replyIter;
        if (dbus_message_iter_init(reply, &replyIter))
        {
            do
            {
                outReply->push_back(ReadIterValue(&replyIter));
            }
            while (dbus_message_iter_next(&replyIter));
        }
    }

    dbus_message_unref(reply);
    return true;
}

bool DBusClient::GetProperty(
    const std::string& destination,
    const std::string& objectPath,
    const std::string& interface,
    const std::string& propertyName,
    DBusValue& out)
{
    std::vector<DBusValue> reply;
    bool ok = Call(
        destination, objectPath, "org.freedesktop.DBus.Properties", "Get",
        { DBusValue::MakeString(interface), DBusValue::MakeString(propertyName) },
        &reply
    );

    if (!ok || reply.empty())
        return false;

    out = reply.front().Unwrap();
    return true;
}

bool DBusClient::GetAllProperties(
    const std::string& destination,
    const std::string& objectPath,
    const std::string& interface,
    DBusValue& out)
{
    std::vector<DBusValue> reply;
    bool ok = Call(
        destination, objectPath, "org.freedesktop.DBus.Properties", "GetAll",
        { DBusValue::MakeString(interface) },
        &reply
    );

    if (!ok || reply.empty())
        return false;

    out = reply.front();
    return true;
}

bool DBusClient::SetProperty(
    const std::string& destination,
    const std::string& objectPath,
    const std::string& interface,
    const std::string& propertyName,
    const DBusValue& value)
{
    return Call(
        destination, objectPath, "org.freedesktop.DBus.Properties", "Set",
        { DBusValue::MakeString(interface), DBusValue::MakeString(propertyName), DBusValue::MakeVariant(value) },
        nullptr
    );
}

bool DBusClient::GetManagedObjects(
    const std::string& destination,
    const std::string& objectPath,
    DBusValue& out)
{
    std::vector<DBusValue> reply;
    bool ok = Call(
        destination, objectPath, "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
        {}, &reply
    );

    if (!ok || reply.empty())
        return false;

    out = reply.front();
    return true;
}

void DBusClient::Subscribe(
    const std::string& interface,
    const std::string& member,
    SignalHandler handler,
    const std::string& path)
{
    if (!m_available)
        return;

    std::string rule = "type='signal',interface='" + interface + "',member='" + member + "'";
    if (!path.empty())
        rule += ",path='" + path + "'";

    DBusError error;
    dbus_error_init(&error);
    dbus_bus_add_match(m_connection, rule.c_str(), &error);
    dbus_error_free(&error); // a failed add_match just means this one signal is silently missed - not fatal to the rest of the app

    m_subscriptions.push_back({ interface, member, path, std::move(handler) });
}

void DBusClient::HandleMessage(DBusMessage* message)
{
    if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL)
        return;

    const char* path = dbus_message_get_path(message);
    const char* interface = dbus_message_get_interface(message);
    const char* member = dbus_message_get_member(message);

    if (!path || !interface || !member)
        return;

    std::vector<DBusValue> args;
    DBusMessageIter iter;
    if (dbus_message_iter_init(message, &iter))
    {
        do
        {
            args.push_back(ReadIterValue(&iter));
        }
        while (dbus_message_iter_next(&iter));
    }

    for (auto& sub : m_subscriptions)
    {
        if (sub.interface != interface || sub.member != member)
            continue;

        if (!sub.path.empty() && sub.path != path)
            continue;

        sub.handler(path, interface, member, args);
    }
}

}
