#pragma once

#include "DBusClient.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Kohiko
{

// One Bluetooth adapter (almost always exactly one, "hci0") - kept as
// its own struct rather than a global singleton state on
// BluezClient itself since a future multi-adapter machine (a laptop
// dock with a second USB dongle, say) is exactly the kind of thing
// the spec's "advanced adapter configuration" future page would need
// more than one of.
struct BluetoothAdapter
{
    std::string objectPath;
    std::string name;         // Alias - the user-facing adapter name
    std::string address;
    bool powered = false;
    bool discoverable = false;
    bool discovering = false;
};

struct BluetoothDevice
{
    std::string objectPath;
    std::string address;
    std::string name;         // Alias, falling back to Name/Address - see BluezClient.cpp
    std::string icon;         // BlueZ's own icon hint ("audio-card", "input-keyboard", ...)
    bool paired = false;
    bool connected = false;
    bool trusted = false;

    // -1 when the device doesn't expose org.bluez.Battery1 at all
    // (most non-audio/non-HID peripherals don't) - kept signed for
    // exactly that "unknown" sentinel rather than a separate bool,
    // since every call site already needs to branch on "do we even
    // have a number to show" before formatting one.
    int batteryPercent = -1;
};

class BluezClient
{
public:

    BluezClient();
    ~BluezClient();

    bool Connect();
    bool Available() const;

    int Fd() const;
    void Dispatch();

    // Re-reads every adapter and device from BlueZ's ObjectManager in
    // one pass - called on startup and after any InterfacesAdded/
    // Removed/PropertiesChanged signal (see SetChangeHandler()).
    void Refresh();

    const std::vector<BluetoothAdapter>& Adapters() const;
    const std::vector<BluetoothDevice>& Devices() const;

    // Every method below is a no-op if no adapter is present (see
    // Adapters().empty()) - BluezClient never crashes or asserts over
    // "there's no Bluetooth hardware right now", the same way
    // PipeWireClient tolerates "no PipeWire daemon reachable" and
    // NetworkManagerClient tolerates "no network devices".
    void SetAdapterPowered(const std::string& adapterPath, bool powered);
    void SetAdapterDiscoverable(const std::string& adapterPath, bool discoverable);
    void StartDiscovery(const std::string& adapterPath);
    void StopDiscovery(const std::string& adapterPath);

    void PairDevice(const std::string& devicePath);
    void ConnectDevice(const std::string& devicePath);
    void DisconnectDevice(const std::string& devicePath);
    void SetDeviceTrusted(const std::string& devicePath, bool trusted);
    void RemoveDevice(const std::string& adapterPath, const std::string& devicePath);

    using ChangeHandler = std::function<void()>;
    void SetChangeHandler(ChangeHandler handler);

private:

    DBusClient m_bus;
    std::vector<BluetoothAdapter> m_adapters;
    std::vector<BluetoothDevice> m_devices;
    ChangeHandler m_changeHandler;
};

}
