#pragma once

#include "DBusClient.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Kohiko
{

enum class NetworkDeviceKind
{
    Ethernet,
    WiFi,
    Vpn,
    Other
};

// A Wi-Fi access point currently in range of a Wi-Fi device, as seen
// by the last RequestScan()/Refresh() cycle.
struct WiFiAccessPoint
{
    std::string objectPath;
    std::string ssid;
    std::uint8_t strength = 0;   // 0-100
    bool secured = false;        // has WPA/RSN flags set
    bool isActiveConnection = false;
};

// One IPv4 or IPv6 address the spec asks kohiko-network to surface -
// kept as a single small struct shared between both families rather
// than two near-identical ones, since the UI (see NetworkWindow's
// "IP information" panel) presents them side by side anyway.
struct IpAddressInfo
{
    std::string address;
    std::uint32_t prefixLength = 0;
    std::string gateway;
    std::vector<std::string> dnsServers;
};

// One network device NetworkManager knows about (a Wi-Fi radio, an
// Ethernet port, ...). Deliberately a flat snapshot struct - see
// AudioNode's comment in PipeWireClient.h for why: kohiko-network's
// UI re-renders from NetworkManagerClient::Devices() every time it
// changes rather than tracking individual property deltas itself.
struct NetworkDevice
{
    std::string objectPath;
    std::string interfaceName;
    NetworkDeviceKind kind = NetworkDeviceKind::Other;
    bool connected = false;
    std::string activeConnectionName;
    std::string macAddress;
    IpAddressInfo ipv4;
    IpAddressInfo ipv6;

    // Only meaningful for kind == WiFi.
    std::vector<WiFiAccessPoint> accessPoints;
    std::uint8_t activeSignalStrength = 0;
};

// A previously-configured connection profile (from
// NetworkManager.Settings) - what the spec's "saved connections" and
// "VPN" lists are built from. Not every device currently has one of
// these active; this is the full saved list independent of what's
// plugged in/connected right now.
struct SavedConnection
{
    std::string objectPath;
    std::string id;              // display name
    std::string type;            // "802-11-wireless", "802-3-ethernet", "vpn", "wireguard", ...
    bool isVpn = false;
    bool isActive = false;

    // The live org.freedesktop.NetworkManager.Connection.Active
    // object path while isActive is true - what DeactivateConnection()
    // needs, since NetworkManager's own DeactivateConnection() method
    // takes the *active* connection, not the saved settings object.
    // Kept here (rather than as a separate "active connections" list)
    // because every saved connection - VPN or not - maps to at most
    // one of these at a time, which is exactly the shape the UI wants
    // to render ("this saved profile is currently active" or isn't).
    std::string activeConnectionPath;
};

// Decodes a D-Bus "array of bytes" value - how NetworkManager
// represents strings that aren't guaranteed valid UTF-8 (an SSID,
// notably, is defined as an arbitrary byte string, not text) - back
// into a plain std::string. Exposed here (rather than kept file-local
// inside NetworkManagerClient.cpp, where it originally lived) so it
// can be unit-tested directly - see tests/test_networkmanagerclient.cpp,
// and that test's own comment for the real bug this exists to guard
// against: every caller of this function ultimately reads a value out
// of a GetSettings()-shaped nested dict, where the value arrives
// wrapped in a D-Bus Variant (DBusClient's own wire-reading code wraps
// every a{sv} dict-entry value this way) - reading .Items() off that
// without unwrapping first silently returns an empty string instead
// of a decode error, which is exactly what made a real bug here go
// unnoticed for a while (see CHANGELOG.md).
std::string BytesToString(const DBusValue& arrayOfBytes);

class NetworkManagerClient
{
public:

    NetworkManagerClient();
    ~NetworkManagerClient();

    bool Connect();
    bool Available() const;

    int Fd() const;
    void Dispatch();

    // Re-reads every device, access point, IP config and saved
    // connection from NetworkManager in one pass. Called on startup
    // and after any signal indicates something may have changed
    // (StateChanged, a device's PropertiesChanged, ...) - see
    // SetChangeHandler().
    void Refresh();

    const std::vector<NetworkDevice>& Devices() const;
    const std::vector<SavedConnection>& SavedConnections() const;

    bool NetworkingEnabled() const;
    bool WirelessEnabled() const;
    bool WwanEnabled() const;

    // Toggles NetworkManager's own WirelessEnabled + WwanEnabled
    // flags together - the closest native equivalent to a hardware
    // "airplane mode" switch that NetworkManager itself exposes (a
    // literal RF-kill of every radio is outside NetworkManager's own
    // remit; see kohiko-network's README section on this for the
    // caveat spelled out for users).
    void SetAirplaneMode(bool enabled);

    void SetWirelessEnabled(bool enabled);

    // Starts an asynchronous Wi-Fi scan on every Wi-Fi device found;
    // results land in Devices()[i].accessPoints after the next
    // Refresh() (NetworkManager emits PropertiesChanged once the scan
    // completes, which SetChangeHandler()'s caller uses to know when
    // to call Refresh() again).
    void RequestWiFiScan();

    // Connects to an already-known access point using its strongest
    // matching saved connection if one exists, otherwise creates and
    // activates a brand new open/unsecured connection - `password`
    // is ignored for open networks and used for the initial PSK
    // otherwise. Every failure mode (wrong password, out of range,
    // ...) surfaces the same way: the device simply never reaches
    // the Activated state and Devices() keeps reporting it
    // disconnected, exactly what a user watching NetworkManager's own
    // nm-applet would also see.
    void ConnectToAccessPoint(const std::string& devicePath, const std::string& apPath, const std::string& ssid, const std::string& password);

    // Whether a saved 802-11-wireless connection profile already
    // exists for `ssid` *and* NetworkManager will actually hand back
    // a usable secret for it right now - the check
    // `NetworkWindow::PromptAndConnect()`'s caller uses to decide
    // whether to show the password prompt at all, added in 0.20.4
    // alongside the fix it exists for (see CHANGELOG.md): the UI
    // previously prompted for *every* secured access point
    // unconditionally, with no way to tell "there's already a saved,
    // working password for this one" apart from "there genuinely
    // isn't". `GetSettings()` deliberately never includes secrets -
    // NetworkManager's own security boundary, so anyone able to read
    // a connection's settings can't thereby read every stored Wi-Fi
    // password - only a separate, explicit `GetSecrets()` call
    // returns the actual `psk`, and only to a caller NetworkManager
    // is willing to grant it to (ordinarily true for a connection
    // this same desktop session created; false, and therefore
    // correctly treated as "no usable secret, must prompt", for
    // anything else - wrong owning user, connection requires
    // re-authentication, or no such saved connection at all). Only
    // ever checks `psk` (WPA-PSK/WPA3-SAE both store their passphrase
    // there in NetworkManager's own data model) - a saved WEP
    // connection's `wep-key0` isn't covered, consistent with this
    // task's own WPA/WPA2 scope.
    bool HasUsableSavedSecret(const std::string& ssid);

    // `devicePath` may be "/" (NetworkManager's own placeholder for
    // "pick automatically") - what's used for VPN connections, which
    // route over whichever device already has connectivity rather
    // than owning a device of their own.
    void ActivateSavedConnection(const std::string& connectionPath, const std::string& devicePath);

    // Looks up `connectionPath`'s current SavedConnection::
    // activeConnectionPath and deactivates that - works uniformly for
    // both a normal device-bound connection and a VPN connection.
    void DeactivateConnection(const std::string& connectionPath);
    void ForgetConnection(const std::string& connectionPath);

    void DisconnectDevice(const std::string& devicePath);

    using ChangeHandler = std::function<void()>;
    void SetChangeHandler(ChangeHandler handler);

private:

    DBusClient m_bus;
    std::vector<NetworkDevice> m_devices;
    std::vector<SavedConnection> m_savedConnections;
    bool m_networkingEnabled = true;
    bool m_wirelessEnabled = true;
    bool m_wwanEnabled = true;
    ChangeHandler m_changeHandler;

    void RefreshDevices();
    void RefreshSavedConnections();
    void RefreshGlobalState();
};

}
