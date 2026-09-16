#include "BluezClient.h"

#include <algorithm>

namespace Kohiko
{

namespace
{

constexpr const char* kBluezService = "org.bluez";
constexpr const char* kRootPath = "/";
constexpr const char* kAdapterIface = "org.bluez.Adapter1";
constexpr const char* kDeviceIface = "org.bluez.Device1";
constexpr const char* kBatteryIface = "org.bluez.Battery1";

}

BluezClient::BluezClient() = default;
BluezClient::~BluezClient() = default;

bool BluezClient::Connect()
{
    if (!m_bus.Connect(DBusClient::Bus::System))
        return false;

    // BlueZ announces every adapter/device appear-or-disappear via
    // ObjectManager signals (discovery finding a new device is the
    // most frequent case) and every property change (paired,
    // connected, RSSI/battery, ...) via the standard
    // Properties.PropertiesChanged signal on the object itself - both
    // just trigger a full Refresh() rather than patching one field in
    // place, the same "re-derive from the latest snapshot" approach
    // NetworkManagerClient and PipeWireClient both take.
    m_bus.Subscribe("org.freedesktop.DBus.ObjectManager", "InterfacesAdded", [this](const std::string&, const std::string&, const std::string&, const std::vector<DBusValue>&)
    {
        Refresh();
        if (m_changeHandler) m_changeHandler();
    });

    m_bus.Subscribe("org.freedesktop.DBus.ObjectManager", "InterfacesRemoved", [this](const std::string&, const std::string&, const std::string&, const std::vector<DBusValue>&)
    {
        Refresh();
        if (m_changeHandler) m_changeHandler();
    });

    m_bus.Subscribe("org.freedesktop.DBus.Properties", "PropertiesChanged", [this](const std::string&, const std::string&, const std::string&, const std::vector<DBusValue>&)
    {
        Refresh();
        if (m_changeHandler) m_changeHandler();
    });

    Refresh();
    return true;
}

bool BluezClient::Available() const
{
    return m_bus.Available();
}

int BluezClient::Fd() const
{
    return m_bus.Fd();
}

void BluezClient::Dispatch()
{
    m_bus.Dispatch();
}

void BluezClient::Refresh()
{
    if (!m_bus.Available())
        return;

    m_adapters.clear();
    m_devices.clear();

    DBusValue managedObjects;
    if (!m_bus.GetManagedObjects(kBluezService, kRootPath, managedObjects))
        return; // bluetoothd not running / no adapter - Adapters() and Devices() just stay empty

    for (auto& [objectPath, interfaces] : managedObjects.Entries())
    {
        if (interfaces.Has(kAdapterIface))
        {
            const DBusValue& props = interfaces.Get(kAdapterIface);

            BluetoothAdapter adapter;
            adapter.objectPath = objectPath;
            adapter.address = props.Get("Address").AsString();
            adapter.name = props.Get("Alias").AsString();
            if (adapter.name.empty())
                adapter.name = props.Get("Name").AsString();
            adapter.powered = props.Get("Powered").AsBool();
            adapter.discoverable = props.Get("Discoverable").AsBool();
            adapter.discovering = props.Get("Discovering").AsBool();

            m_adapters.push_back(std::move(adapter));
        }

        if (interfaces.Has(kDeviceIface))
        {
            const DBusValue& props = interfaces.Get(kDeviceIface);

            BluetoothDevice device;
            device.objectPath = objectPath;
            device.address = props.Get("Address").AsString();
            device.name = props.Get("Alias").AsString();
            if (device.name.empty())
                device.name = props.Get("Name").AsString();
            if (device.name.empty())
                device.name = device.address;
            device.icon = props.Get("Icon").AsString();
            device.paired = props.Get("Paired").AsBool();
            device.connected = props.Get("Connected").AsBool();
            device.trusted = props.Get("Trusted").AsBool();

            if (interfaces.Has(kBatteryIface))
                device.batteryPercent = static_cast<int>(interfaces.Get(kBatteryIface).Get("Percentage").AsUInt());

            m_devices.push_back(std::move(device));
        }
    }

    std::sort(m_adapters.begin(), m_adapters.end(),
        [](const BluetoothAdapter& a, const BluetoothAdapter& b) { return a.name < b.name; });

    // Connected first, then paired, then everything discovery has
    // turned up but isn't paired to yet - matches how every other
    // desktop's Bluetooth panel orders this list, since "what am I
    // using right now" is what a user opens this app to check first.
    std::sort(m_devices.begin(), m_devices.end(), [](const BluetoothDevice& a, const BluetoothDevice& b)
    {
        if (a.connected != b.connected) return a.connected;
        if (a.paired != b.paired) return a.paired;
        return a.name < b.name;
    });
}

const std::vector<BluetoothAdapter>& BluezClient::Adapters() const
{
    return m_adapters;
}

const std::vector<BluetoothDevice>& BluezClient::Devices() const
{
    return m_devices;
}

void BluezClient::SetAdapterPowered(const std::string& adapterPath, bool powered)
{
    m_bus.SetProperty(kBluezService, adapterPath, kAdapterIface, "Powered", DBusValue::MakeBool(powered));
}

void BluezClient::SetAdapterDiscoverable(const std::string& adapterPath, bool discoverable)
{
    m_bus.SetProperty(kBluezService, adapterPath, kAdapterIface, "Discoverable", DBusValue::MakeBool(discoverable));
}

void BluezClient::StartDiscovery(const std::string& adapterPath)
{
    m_bus.Call(kBluezService, adapterPath, kAdapterIface, "StartDiscovery", {});
}

void BluezClient::StopDiscovery(const std::string& adapterPath)
{
    m_bus.Call(kBluezService, adapterPath, kAdapterIface, "StopDiscovery", {});
}

void BluezClient::PairDevice(const std::string& devicePath)
{
    // Pairing can legitimately take several seconds (PIN/passkey
    // confirmation round trips with the remote device) - a longer
    // timeout than DBusClient's 4-second default so a slow-but-
    // succeeding pairing isn't reported as a failure.
    m_bus.Call(kBluezService, devicePath, kDeviceIface, "Pair", {}, nullptr, nullptr, 30000);
}

void BluezClient::ConnectDevice(const std::string& devicePath)
{
    m_bus.Call(kBluezService, devicePath, kDeviceIface, "Connect", {}, nullptr, nullptr, 15000);
}

void BluezClient::DisconnectDevice(const std::string& devicePath)
{
    m_bus.Call(kBluezService, devicePath, kDeviceIface, "Disconnect", {});
}

void BluezClient::SetDeviceTrusted(const std::string& devicePath, bool trusted)
{
    m_bus.SetProperty(kBluezService, devicePath, kDeviceIface, "Trusted", DBusValue::MakeBool(trusted));
}

void BluezClient::RemoveDevice(const std::string& adapterPath, const std::string& devicePath)
{
    m_bus.Call(kBluezService, adapterPath, kAdapterIface, "RemoveDevice", { DBusValue::MakeObjectPath(devicePath) });
}

void BluezClient::SetChangeHandler(ChangeHandler handler)
{
    m_changeHandler = std::move(handler);
}

}
