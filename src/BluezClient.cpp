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

bool BluezClient::IsBusy(const std::string& objectPath) const
{
    return m_inFlight.count(objectPath) != 0;
}

std::string BluezClient::BusyLabel(const std::string& objectPath) const
{
    auto it = m_inFlight.find(objectPath);
    return it == m_inFlight.end() ? std::string() : it->second.label;
}

std::string BluezClient::LastError(const std::string& objectPath) const
{
    auto it = m_errors.find(objectPath);
    return it == m_errors.end() ? std::string() : it->second;
}

bool BluezClient::BeginOperation(
    const std::string& objectPath, const std::string& targetPath,
    const std::string& interface, const std::string& method,
    const std::string& label, const std::vector<DBusValue>& args,
    int timeoutMs, DoneHandler done)
{
    if (m_inFlight.count(objectPath))
    {
        if (done)
            done(false, "already in progress");

        return false;
    }

    m_errors.erase(objectPath);

    Operation operation;
    operation.label = label;
    operation.method = method;
    m_inFlight[objectPath] = operation;

    DBusClient::CallId id = m_bus.CallAsync(
        kBluezService, targetPath, interface, method, args,
        [this, objectPath, method, done](const DBusClient::AsyncResult& result)
        {
            m_inFlight.erase(objectPath);

            if (result.cancelled)
            {
                m_errors[objectPath] = method == "Pair"
                    ? "Pairing cancelled"
                    : method + ": stopped waiting (the request was already sent and may still complete)";
            }
            else if (!result.ok)
            {
                m_errors[objectPath] = method + " failed: " + result.error;
            }

            if (done)
                done(result.ok, result.error);

            if (m_changeHandler)
                m_changeHandler();
        },
        timeoutMs);

    // CallAsync() runs the callback itself, before returning, only if the
    // call could not even be sent - in which case the entry is already gone.
    if (id != 0)
    {
        auto it = m_inFlight.find(objectPath);

        if (it != m_inFlight.end())
            it->second.callId = id;
    }

    if (m_changeHandler)
        m_changeHandler(); // the window can show "Pairing\u2026" right away

    return true;
}

bool BluezClient::CancelPending(const std::string& objectPath)
{
    auto it = m_inFlight.find(objectPath);

    if (it == m_inFlight.end())
        return false;

    const std::string method = it->second.method;
    const DBusClient::CallId id = it->second.callId;

    // A Pair has a real cancel in the service; ask for it. (Any other
    // in-flight call has none, so for those only the waiting stops.)
    if (method == "Pair")
        m_bus.CallAsync(kBluezService, objectPath, kDeviceIface, "CancelPairing", {}, nullptr);

    m_bus.CancelAsync(id); // runs the completion callback above, which records the message

    return true;
}

DBusClient::AsyncCallback BluezClient::RecordFailure(const std::string& objectPath, const std::string& what)
{
    return [this, objectPath, what](const DBusClient::AsyncResult& result)
    {
        if (!result.ok && !result.cancelled)
            m_errors[objectPath] = what + " failed: " + result.error;

        if (m_changeHandler)
            m_changeHandler();
    };
}

void BluezClient::SetAdapterPowered(const std::string& adapterPath, bool powered)
{
    m_bus.SetPropertyAsync(kBluezService, adapterPath, kAdapterIface, "Powered",
        DBusValue::MakeBool(powered), RecordFailure(adapterPath, powered ? "Turning Bluetooth on" : "Turning Bluetooth off"));
}

void BluezClient::SetAdapterDiscoverable(const std::string& adapterPath, bool discoverable)
{
    m_bus.SetPropertyAsync(kBluezService, adapterPath, kAdapterIface, "Discoverable",
        DBusValue::MakeBool(discoverable), RecordFailure(adapterPath, "Changing discoverability"));
}

void BluezClient::StartDiscovery(const std::string& adapterPath)
{
    m_bus.CallAsync(kBluezService, adapterPath, kAdapterIface, "StartDiscovery", {},
        RecordFailure(adapterPath, "Starting the scan"));
}

void BluezClient::StopDiscovery(const std::string& adapterPath)
{
    m_bus.CallAsync(kBluezService, adapterPath, kAdapterIface, "StopDiscovery", {},
        RecordFailure(adapterPath, "Stopping the scan"));
}

void BluezClient::PairDevice(const std::string& devicePath, DoneHandler done)
{
    BeginOperation(devicePath, devicePath, kDeviceIface, "Pair", "Pairing\u2026", {}, 30000, std::move(done));
}

void BluezClient::ConnectDevice(const std::string& devicePath, DoneHandler done)
{
    BeginOperation(devicePath, devicePath, kDeviceIface, "Connect", "Connecting\u2026", {}, 15000, std::move(done));
}

void BluezClient::DisconnectDevice(const std::string& devicePath, DoneHandler done)
{
    BeginOperation(devicePath, devicePath, kDeviceIface, "Disconnect", "Disconnecting\u2026", {}, 4000, std::move(done));
}

void BluezClient::SetDeviceTrusted(const std::string& devicePath, bool trusted)
{
    m_bus.SetPropertyAsync(kBluezService, devicePath, kDeviceIface, "Trusted",
        DBusValue::MakeBool(trusted), RecordFailure(devicePath, "Changing trust"));
}

void BluezClient::RemoveDevice(const std::string& adapterPath, const std::string& devicePath, DoneHandler done)
{
    // RemoveDevice is a method of the ADAPTER, but what is busy - and what
    // the window shows "Removing\u2026" on - is the device.
    BeginOperation(devicePath, adapterPath, kAdapterIface, "RemoveDevice", "Removing\u2026",
        { DBusValue::MakeObjectPath(devicePath) }, 4000, std::move(done));
}

void BluezClient::PairAndConnectDevice(const std::string& devicePath)
{
    PairDevice(devicePath, [this, devicePath](bool ok, const std::string& error)
    {
        // Already paired is fine: the user wants it connected.
        if (ok || error.find("AlreadyExists") != std::string::npos || error.find("Already Exists") != std::string::npos)
            ConnectDevice(devicePath);
    });
}

void BluezClient::DisconnectAndRemoveDevice(const std::string& adapterPath, const std::string& devicePath)
{
    DisconnectDevice(devicePath, [this, adapterPath, devicePath](bool, const std::string&)
    {
        RemoveDevice(adapterPath, devicePath);
    });
}

void BluezClient::SetChangeHandler(ChangeHandler handler)
{
    m_changeHandler = std::move(handler);
}

}
