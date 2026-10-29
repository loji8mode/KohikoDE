#include "NetworkManagerClient.h"

#include <algorithm>

namespace Kohiko
{

// Declared in NetworkManagerClient.h (see that declaration's own
// comment for the "why" and the full CHANGELOG.md reference) - defined
// here, in Kohiko:: scope rather than this file's own anonymous
// namespace, specifically so it's reachable from
// tests/test_networkmanagerclient.cpp.
std::string BytesToString(const DBusValue& arrayOfBytes)
{
    const DBusValue& v = arrayOfBytes.Unwrap();

    std::string result;
    result.reserve(v.Items().size());

    for (auto& b : v.Items())
        result.push_back(static_cast<char>(b.AsUInt()));

    return result;
}

namespace
{

constexpr const char* kNmService = "org.freedesktop.NetworkManager";
constexpr const char* kNmManagerPath = "/org/freedesktop/NetworkManager";
constexpr const char* kNmManagerIface = "org.freedesktop.NetworkManager";
constexpr const char* kNmSettingsPath = "/org/freedesktop/NetworkManager/Settings";
constexpr const char* kNmSettingsIface = "org.freedesktop.NetworkManager.Settings";
constexpr const char* kNmConnectionIface = "org.freedesktop.NetworkManager.Settings.Connection";
constexpr const char* kNmDeviceIface = "org.freedesktop.NetworkManager.Device";
constexpr const char* kNmWirelessIface = "org.freedesktop.NetworkManager.Device.Wireless";
constexpr const char* kNmWiredIface = "org.freedesktop.NetworkManager.Device.Wired";
constexpr const char* kNmApIface = "org.freedesktop.NetworkManager.AccessPoint";
constexpr const char* kNmIp4Iface = "org.freedesktop.NetworkManager.IP4Config";
constexpr const char* kNmIp6Iface = "org.freedesktop.NetworkManager.IP6Config";
constexpr const char* kNmActiveIface = "org.freedesktop.NetworkManager.Connection.Active";

// NMDeviceType, as documented on
// developer.gnome.org/NetworkManager - only the values kohiko-network
// actually branches on are named here.
constexpr std::uint32_t kNmDeviceTypeEthernet = 1;
constexpr std::uint32_t kNmDeviceTypeWiFi = 2;
constexpr std::uint32_t kNmDeviceTypeWireGuard = 29;

// NM80211ApFlags/NM80211ApSecurityFlags - a nonzero WPA/RSN flag set
// (anything beyond NM_802_11_AP_SEC_NONE = 0) means the network asks
// for a passphrase.
bool AccessPointIsSecured(std::uint32_t wpaFlags, std::uint32_t rsnFlags)
{
    return wpaFlags != 0 || rsnFlags != 0;
}

NetworkDeviceKind DeviceKindFromType(std::uint32_t nmDeviceType)
{
    switch (nmDeviceType)
    {
        case kNmDeviceTypeEthernet:  return NetworkDeviceKind::Ethernet;
        case kNmDeviceTypeWiFi:      return NetworkDeviceKind::WiFi;
        case kNmDeviceTypeWireGuard: return NetworkDeviceKind::Vpn;
        default:                    return NetworkDeviceKind::Other;
    }
}

IpAddressInfo ReadIpConfig(DBusClient& bus, const std::string& configPath, const char* iface)
{
    IpAddressInfo info;
    if (configPath.empty() || configPath == "/")
        return info;

    DBusValue addressData;
    if (bus.GetProperty(kNmService, configPath, iface, "AddressData", addressData))
    {
        if (!addressData.Items().empty())
        {
            const DBusValue& first = addressData.Items().front();
            info.address = first.Get("address").AsString();
            info.prefixLength = static_cast<std::uint32_t>(first.Get("prefix").AsUInt());
        }
    }

    DBusValue gateway;
    if (bus.GetProperty(kNmService, configPath, iface, "Gateway", gateway))
        info.gateway = gateway.AsString();

    DBusValue nameserverData;
    if (bus.GetProperty(kNmService, configPath, iface, "NameserverData", nameserverData))
    {
        for (auto& entry : nameserverData.Items())
        {
            std::string addr = entry.Get("address").AsString();
            if (!addr.empty())
                info.dnsServers.push_back(addr);
        }
    }

    return info;
}

}

NetworkManagerClient::NetworkManagerClient() = default;
NetworkManagerClient::~NetworkManagerClient() = default;

bool NetworkManagerClient::Connect()
{
    if (!m_bus.Connect(DBusClient::Bus::System))
        return false;

    m_bus.Subscribe(kNmManagerIface, "StateChanged", [this](const std::string&, const std::string&, const std::string&, const std::vector<DBusValue>&)
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

bool NetworkManagerClient::Available() const
{
    return m_bus.Available();
}

int NetworkManagerClient::Fd() const
{
    return m_bus.Fd();
}

void NetworkManagerClient::Dispatch()
{
    m_bus.Dispatch();
}

void NetworkManagerClient::Refresh()
{
    if (!m_bus.Available())
        return;

    RefreshGlobalState();
    RefreshDevices();
    RefreshSavedConnections();
}

void NetworkManagerClient::RefreshGlobalState()
{
    DBusValue v;

    if (m_bus.GetProperty(kNmService, kNmManagerPath, kNmManagerIface, "NetworkingEnabled", v))
        m_networkingEnabled = v.AsBool(true);

    if (m_bus.GetProperty(kNmService, kNmManagerPath, kNmManagerIface, "WirelessEnabled", v))
        m_wirelessEnabled = v.AsBool(true);

    if (m_bus.GetProperty(kNmService, kNmManagerPath, kNmManagerIface, "WwanEnabled", v))
        m_wwanEnabled = v.AsBool(true);
}

void NetworkManagerClient::RefreshDevices()
{
    m_devices.clear();

    DBusValue devicesValue;
    if (!m_bus.GetProperty(kNmService, kNmManagerPath, kNmManagerIface, "Devices", devicesValue))
        return;

    for (auto& devicePathValue : devicesValue.Items())
    {
        std::string devicePath = devicePathValue.AsString();
        if (devicePath.empty())
            continue;

        DBusValue typeValue;
        m_bus.GetProperty(kNmService, devicePath, kNmDeviceIface, "DeviceType", typeValue);
        NetworkDeviceKind kind = DeviceKindFromType(static_cast<std::uint32_t>(typeValue.AsUInt()));

        // The spec only asks for Wi-Fi/Ethernet/VPN/airplane-mode
        // devices - loopback, bridges, tun/tap helper devices etc.
        // NetworkManager also enumerates aren't anything a user would
        // expect to see or manage from kohiko-network.
        if (kind == NetworkDeviceKind::Other)
            continue;

        NetworkDevice device;
        device.objectPath = devicePath;
        device.kind = kind;

        DBusValue ifaceValue;
        if (m_bus.GetProperty(kNmService, devicePath, kNmDeviceIface, "Interface", ifaceValue))
            device.interfaceName = ifaceValue.AsString();

        DBusValue stateValue;
        if (m_bus.GetProperty(kNmService, devicePath, kNmDeviceIface, "State", stateValue))
            device.connected = stateValue.AsUInt() == 100; // NM_DEVICE_STATE_ACTIVATED

        DBusValue activeConnValue;
        std::string activeConnPath;
        if (m_bus.GetProperty(kNmService, devicePath, kNmDeviceIface, "ActiveConnection", activeConnValue))
            activeConnPath = activeConnValue.AsString();

        if (!activeConnPath.empty() && activeConnPath != "/")
        {
            DBusValue idValue;
            if (m_bus.GetProperty(kNmService, activeConnPath, kNmActiveIface, "Id", idValue))
                device.activeConnectionName = idValue.AsString();
        }

        const char* hwIface = kind == NetworkDeviceKind::Ethernet ? kNmWiredIface
                             : kind == NetworkDeviceKind::WiFi     ? kNmWirelessIface
                             : nullptr;
        if (hwIface)
        {
            DBusValue hwValue;
            if (m_bus.GetProperty(kNmService, devicePath, hwIface, "HwAddress", hwValue))
                device.macAddress = hwValue.AsString();
        }

        DBusValue ip4PathValue;
        if (m_bus.GetProperty(kNmService, devicePath, kNmDeviceIface, "Ip4Config", ip4PathValue))
            device.ipv4 = ReadIpConfig(m_bus, ip4PathValue.AsString(), kNmIp4Iface);

        DBusValue ip6PathValue;
        if (m_bus.GetProperty(kNmService, devicePath, kNmDeviceIface, "Ip6Config", ip6PathValue))
            device.ipv6 = ReadIpConfig(m_bus, ip6PathValue.AsString(), kNmIp6Iface);

        if (kind == NetworkDeviceKind::WiFi)
        {
            std::string activeApPath;
            DBusValue activeApValue;
            if (m_bus.GetProperty(kNmService, devicePath, kNmWirelessIface, "ActiveAccessPoint", activeApValue))
                activeApPath = activeApValue.AsString();

            DBusValue apsValue;
            if (m_bus.GetProperty(kNmService, devicePath, kNmWirelessIface, "AccessPoints", apsValue))
            {
                for (auto& apPathValue : apsValue.Items())
                {
                    std::string apPath = apPathValue.AsString();
                    if (apPath.empty())
                        continue;

                    WiFiAccessPoint ap;
                    ap.objectPath = apPath;
                    ap.isActiveConnection = (apPath == activeApPath);

                    DBusValue ssidValue;
                    if (m_bus.GetProperty(kNmService, apPath, kNmApIface, "Ssid", ssidValue))
                        ap.ssid = BytesToString(ssidValue);

                    DBusValue strengthValue;
                    if (m_bus.GetProperty(kNmService, apPath, kNmApIface, "Strength", strengthValue))
                        ap.strength = static_cast<std::uint8_t>(strengthValue.AsUInt());

                    DBusValue wpaValue, rsnValue;
                    m_bus.GetProperty(kNmService, apPath, kNmApIface, "WpaFlags", wpaValue);
                    m_bus.GetProperty(kNmService, apPath, kNmApIface, "RsnFlags", rsnValue);
                    ap.secured = AccessPointIsSecured(
                        static_cast<std::uint32_t>(wpaValue.AsUInt()),
                        static_cast<std::uint32_t>(rsnValue.AsUInt())
                    );

                    if (ap.isActiveConnection)
                        device.activeSignalStrength = ap.strength;

                    if (!ap.ssid.empty())
                        device.accessPoints.push_back(std::move(ap));
                }
            }

            std::sort(device.accessPoints.begin(), device.accessPoints.end(),
                [](const WiFiAccessPoint& a, const WiFiAccessPoint& b) { return a.strength > b.strength; });
        }

        m_devices.push_back(std::move(device));
    }
}

void NetworkManagerClient::RefreshSavedConnections()
{
    m_savedConnections.clear();

    std::vector<DBusValue> reply;
    if (!m_bus.Call(kNmService, kNmSettingsPath, kNmSettingsIface, "ListConnections", {}, &reply) || reply.empty())
        return;

    // Build a path -> active-connection map once, from the manager's
    // ActiveConnections property, rather than one Properties.Get
    // round trip per saved connection.
    std::vector<std::pair<std::string, std::string>> activeBySettingsPath; // settingsPath -> activeConnectionPath

    DBusValue activeConnsValue;
    if (m_bus.GetProperty(kNmService, kNmManagerPath, kNmManagerIface, "ActiveConnections", activeConnsValue))
    {
        for (auto& activePathValue : activeConnsValue.Items())
        {
            std::string activePath = activePathValue.AsString();
            DBusValue connValue;
            if (m_bus.GetProperty(kNmService, activePath, kNmActiveIface, "Connection", connValue))
                activeBySettingsPath.emplace_back(connValue.AsString(), activePath);
        }
    }

    for (auto& connPathValue : reply.front().Items())
    {
        std::string connPath = connPathValue.AsString();
        if (connPath.empty())
            continue;

        std::vector<DBusValue> settingsReply;
        if (!m_bus.Call(kNmService, connPath, kNmConnectionIface, "GetSettings", {}, &settingsReply) || settingsReply.empty())
            continue;

        const DBusValue& settings = settingsReply.front();
        const DBusValue& connectionSection = settings.Get("connection");

        SavedConnection saved;
        saved.objectPath = connPath;
        saved.id = connectionSection.Get("id").AsString();
        saved.type = connectionSection.Get("type").AsString();
        saved.isVpn = (saved.type == "vpn" || saved.type == "wireguard");

        for (auto& [settingsPath, activePath] : activeBySettingsPath)
        {
            if (settingsPath == connPath)
            {
                saved.isActive = true;
                saved.activeConnectionPath = activePath;
                break;
            }
        }

        if (!saved.id.empty())
            m_savedConnections.push_back(std::move(saved));
    }

    std::sort(m_savedConnections.begin(), m_savedConnections.end(),
        [](const SavedConnection& a, const SavedConnection& b) { return a.id < b.id; });
}

const std::vector<NetworkDevice>& NetworkManagerClient::Devices() const
{
    return m_devices;
}

const std::vector<SavedConnection>& NetworkManagerClient::SavedConnections() const
{
    return m_savedConnections;
}

bool NetworkManagerClient::NetworkingEnabled() const { return m_networkingEnabled; }
bool NetworkManagerClient::WirelessEnabled() const { return m_wirelessEnabled; }
bool NetworkManagerClient::WwanEnabled() const { return m_wwanEnabled; }

// ---------------------------------------------------------------------
// Operations, written once against an explicit D-Bus connection and an
// explicit copy of the data they need.
//
// The window-facing methods further down run them two ways: the
// synchronous ones on this client's own connection (kept for the
// scripted harness and any caller that really can block), the *Async
// ones on a worker thread's PRIVATE connection with a snapshot taken at
// submission time. These functions touch no member of the client, which
// is what makes the second use thread-safe: everything they read or
// write is a parameter.
// ---------------------------------------------------------------------

namespace
{

std::string OrUnknown(const std::string& error)
{
    return error.empty() ? std::string("unknown error") : error;
}

OperationResult NmSetRadios(DBusClient& bus, bool wifiOn, bool setWwan, bool wwanOn)
{
    // Both radios are always attempted (as the synchronous version
    // always did); the first failure is what gets reported.
    std::string wifiError;
    bool wifiOk = bus.SetProperty(kNmService, kNmManagerPath, kNmManagerIface, "WirelessEnabled", DBusValue::MakeBool(wifiOn), &wifiError);

    std::string wwanError;
    bool wwanOk = !setWwan || bus.SetProperty(kNmService, kNmManagerPath, kNmManagerIface, "WwanEnabled", DBusValue::MakeBool(wwanOn), &wwanError);

    if (!wifiOk)
        return OperationResult::Failure(std::string("Turning Wi-Fi ") + (wifiOn ? "on" : "off") + " failed: " + OrUnknown(wifiError));

    if (!wwanOk)
        return OperationResult::Failure("Changing mobile data failed: " + OrUnknown(wwanError));

    return OperationResult::Success();
}

OperationResult NmRequestWiFiScan(DBusClient& bus, const std::vector<std::string>& wifiDevicePaths)
{
    std::string firstError;
    bool any = false;

    for (const std::string& path : wifiDevicePaths)
    {
        std::string error;

        if (bus.Call(kNmService, path, kNmWirelessIface, "RequestScan", { DBusValue::MakeDict({}) }, nullptr, &error))
            any = true;
        else if (firstError.empty())
            firstError = error;
    }

    if (!wifiDevicePaths.empty() && !any)
        return OperationResult::Failure("Scanning for networks failed: " + OrUnknown(firstError));

    return OperationResult::Success();
}

bool NmHasUsableSavedSecret(DBusClient& bus, const std::vector<SavedConnection>& savedConnections, const std::string& ssid)
{
    for (auto& saved : savedConnections)
    {
        if (saved.type != "802-11-wireless")
            continue;

        std::vector<DBusValue> settingsReply;
        if (!bus.Call(kNmService, saved.objectPath, kNmConnectionIface, "GetSettings", {}, &settingsReply) || settingsReply.empty())
            continue;

        const DBusValue& wifiSection = settingsReply.front().Get("802-11-wireless");
        if (BytesToString(wifiSection.Get("ssid")) != ssid)
            continue;

        // Found the saved profile for this SSID - whether it counts
        // as "usable" now hinges entirely on GetSecrets(), not on
        // anything GetSettings() already told us (see this method's
        // own header comment for why those are deliberately separate
        // calls).
        std::vector<DBusValue> secretsReply;
        if (!bus.Call(kNmService, saved.objectPath, kNmConnectionIface, "GetSecrets",
                { DBusValue::MakeString("802-11-wireless-security") }, &secretsReply) || secretsReply.empty())
            return false;

        const DBusValue& securitySection = secretsReply.front().Get("802-11-wireless-security");
        return !securitySection.Get("psk").AsString().empty();
    }

    return false; // no saved connection at all for this SSID
}

OperationResult NmConnectToAccessPoint(
    DBusClient& bus, const std::vector<SavedConnection>& savedConnections,
    const std::string& devicePath, const std::string& apPath,
    const std::string& ssid, const std::string& password)
{
    // First preference: reuse an existing saved profile for this
    // exact SSID, exactly like clicking a known network in nm-applet
    // does - it carries over any previously-entered credentials
    // rather than asking again.
    for (auto& saved : savedConnections)
    {
        if (saved.type != "802-11-wireless")
            continue;

        std::vector<DBusValue> settingsReply;
        if (!bus.Call(kNmService, saved.objectPath, kNmConnectionIface, "GetSettings", {}, &settingsReply) || settingsReply.empty())
            continue;

        const DBusValue& wifiSection = settingsReply.front().Get("802-11-wireless");
        std::string savedSsid = BytesToString(wifiSection.Get("ssid"));

        if (savedSsid == ssid)
        {
            if (!password.empty())
            {
                // The user was just asked for, and just typed, this
                // password - it has to actually reach NetworkManager,
                // or a previously-saved (possibly wrong, possibly
                // just outdated after the AP's passphrase changed)
                // secret keeps getting silently reactivated instead,
                // no matter what gets typed into the prompt each
                // time. This was unreachable before the BytesToString()
                // fix just above it (the SSID comparison this sits
                // inside never used to succeed at all - see that
                // function's own comment), so this exact gap could
                // never have been hit in practice until that was
                // fixed either - both halves of this bug had to be
                // fixed together, not just the first one found.
                //
                // Settings.Connection.Update() replaces the *entire*
                // connection - it is not a merge/patch operation, per
                // NetworkManager's own D-Bus reference ("Update the
                // connection with new settings and properties,
                // replacing all previous settings and properties").
                // So this starts from every section GetSettings() (via
                // settingsReply.front()) just returned - preserving
                // ipv4/ipv6/connection/etc. untouched - and only adds
                // or overwrites the one thing that actually needs to
                // change: this section's own "psk". Each outer-level
                // section is re-wrapped in exactly one Variant on the
                // way back out (Unwrap() first in case it already
                // carried one, then MakeVariant() once), matching how
                // the brand-new-connection path a few lines down
                // already writes this same shape - normalizing rather
                // than assuming which format GetSettings() itself
                // used avoids having to guess about a nesting-depth
                // question this file already got wrong once (see
                // BytesToString() above).
                std::map<std::string, DBusValue> topLevel;

                for (const auto& [sectionName, sectionValue] : settingsReply.front().Entries())
                    topLevel.emplace(sectionName, DBusValue::MakeVariant(sectionValue.Unwrap()));

                std::map<std::string, DBusValue> securitySection;

                if (auto it = topLevel.find("802-11-wireless-security"); it != topLevel.end())
                    securitySection = it->second.Unwrap().Entries();

                if (!securitySection.count("key-mgmt"))
                    securitySection.emplace("key-mgmt", DBusValue::MakeVariant(DBusValue::MakeString("wpa-psk")));

                securitySection["psk"] = DBusValue::MakeVariant(DBusValue::MakeString(password));

                topLevel["802-11-wireless-security"] = DBusValue::MakeVariant(DBusValue::MakeDict(std::move(securitySection)));

                std::string updateError;
                if (!bus.Call(kNmService, saved.objectPath, kNmConnectionIface, "Update",
                        { DBusValue::MakeDict(std::move(topLevel)) }, nullptr, &updateError))
                    return OperationResult::Failure("Saving the new password failed: " + (updateError.empty() ? std::string("unknown error") : updateError));
            }

            std::string activateError;
            bool activated = bus.Call(kNmService, kNmManagerPath, kNmManagerIface, "ActivateConnection",
                { DBusValue::MakeObjectPath(saved.objectPath), DBusValue::MakeObjectPath(devicePath), DBusValue::MakeObjectPath(apPath) },
                nullptr, &activateError);
            return activated ? OperationResult::Success("Connecting to " + ssid)
                             : OperationResult::Failure("Connecting to " + ssid + " failed: " + (activateError.empty() ? std::string("unknown error") : activateError));
        }
    }

    // No saved profile - build a brand new minimal connection,
    // exactly the two settings sections NetworkManager requires to
    // add-and-activate one from scratch: an SSID, and (if a password
    // was given) a WPA-PSK secret.
    std::map<std::string, DBusValue> connectionSection;
    connectionSection.emplace("id", DBusValue::MakeVariant(DBusValue::MakeString(ssid)));
    connectionSection.emplace("type", DBusValue::MakeVariant(DBusValue::MakeString("802-11-wireless")));

    std::vector<DBusValue> ssidBytes;
    for (char c : ssid)
        ssidBytes.push_back(DBusValue::MakeByte(static_cast<std::uint8_t>(c)));

    std::map<std::string, DBusValue> wifiSection;
    wifiSection.emplace("ssid", DBusValue::MakeVariant(DBusValue::MakeArray(std::move(ssidBytes), 'y')));

    std::map<std::string, DBusValue> topLevel;
    topLevel.emplace("connection", DBusValue::MakeVariant(DBusValue::MakeDict(std::move(connectionSection))));
    topLevel.emplace("802-11-wireless", DBusValue::MakeVariant(DBusValue::MakeDict(std::move(wifiSection))));

    if (!password.empty())
    {
        std::map<std::string, DBusValue> securitySection;
        securitySection.emplace("key-mgmt", DBusValue::MakeVariant(DBusValue::MakeString("wpa-psk")));
        securitySection.emplace("psk", DBusValue::MakeVariant(DBusValue::MakeString(password)));
        topLevel.emplace("802-11-wireless-security", DBusValue::MakeVariant(DBusValue::MakeDict(std::move(securitySection))));
    }

    std::string addError;
    bool added = bus.Call(kNmService, kNmManagerPath, kNmManagerIface, "AddAndActivateConnection",
        { DBusValue::MakeDict(std::move(topLevel)),
          DBusValue::MakeObjectPath(devicePath), DBusValue::MakeObjectPath(apPath) },
        nullptr, &addError);
    return added ? OperationResult::Success("Connecting to " + ssid)
                 : OperationResult::Failure("Connecting to " + ssid + " failed: " + (addError.empty() ? std::string("unknown error") : addError));
}

OperationResult NmActivateSavedConnection(DBusClient& bus, const std::string& connectionPath, const std::string& devicePath)
{
    std::string error;
    bool ok = bus.Call(kNmService, kNmManagerPath, kNmManagerIface, "ActivateConnection",
        { DBusValue::MakeObjectPath(connectionPath), DBusValue::MakeObjectPath(devicePath), DBusValue::MakeObjectPath("/") },
        nullptr, &error);

    return ok ? OperationResult::Success("Connecting") : OperationResult::Failure("Connecting failed: " + OrUnknown(error));
}

OperationResult NmDeactivateConnection(DBusClient& bus, const std::vector<SavedConnection>& savedConnections, const std::string& connectionPath)
{
    for (auto& saved : savedConnections)
    {
        if (saved.objectPath == connectionPath && !saved.activeConnectionPath.empty())
        {
            std::string error;
            bool ok = bus.Call(kNmService, kNmManagerPath, kNmManagerIface, "DeactivateConnection",
                { DBusValue::MakeObjectPath(saved.activeConnectionPath) }, nullptr, &error);

            return ok ? OperationResult::Success("Disconnected") : OperationResult::Failure("Disconnecting failed: " + OrUnknown(error));
        }
    }

    return OperationResult::Success(); // nothing active to deactivate
}

OperationResult NmForgetConnection(DBusClient& bus, const std::string& connectionPath)
{
    std::string error;
    bool ok = bus.Call(kNmService, connectionPath, kNmConnectionIface, "Delete", {}, nullptr, &error);

    return ok ? OperationResult::Success("Network forgotten") : OperationResult::Failure("Forgetting the network failed: " + OrUnknown(error));
}

OperationResult NmDisconnectDevice(DBusClient& bus, const std::string& devicePath)
{
    std::string error;
    bool ok = bus.Call(kNmService, devicePath, kNmDeviceIface, "Disconnect", {}, nullptr, &error);

    return ok ? OperationResult::Success("Disconnected") : OperationResult::Failure("Disconnecting failed: " + OrUnknown(error));
}

std::vector<std::string> WiFiDevicePaths(const std::vector<NetworkDevice>& devices)
{
    std::vector<std::string> paths;

    for (auto& device : devices)
    {
        if (device.kind == NetworkDeviceKind::WiFi)
            paths.push_back(device.objectPath);
    }

    return paths;
}

}

// --- synchronous versions: same behaviour as always, on this client's own connection ---

void NetworkManagerClient::SetAirplaneMode(bool enabled)
{
    // "Airplane mode" here means "every NetworkManager-managed radio
    // off" - enabled=true turns radios off, matching the usual
    // physical-switch meaning of the phrase, which is the inverse of
    // the WirelessEnabled/WwanEnabled property sense.
    NmSetRadios(m_bus, !enabled, true, !enabled);
}

void NetworkManagerClient::SetWirelessEnabled(bool enabled)
{
    NmSetRadios(m_bus, enabled, false, false);
}

void NetworkManagerClient::RequestWiFiScan()
{
    NmRequestWiFiScan(m_bus, WiFiDevicePaths(m_devices));
}

bool NetworkManagerClient::HasUsableSavedSecret(const std::string& ssid)
{
    return NmHasUsableSavedSecret(m_bus, m_savedConnections, ssid);
}

void NetworkManagerClient::ConnectToAccessPoint(
    const std::string& devicePath, const std::string& apPath,
    const std::string& ssid, const std::string& password)
{
    NmConnectToAccessPoint(m_bus, m_savedConnections, devicePath, apPath, ssid, password);
}

void NetworkManagerClient::ActivateSavedConnection(const std::string& connectionPath, const std::string& devicePath)
{
    NmActivateSavedConnection(m_bus, connectionPath, devicePath);
}

void NetworkManagerClient::DeactivateConnection(const std::string& connectionPath)
{
    NmDeactivateConnection(m_bus, m_savedConnections, connectionPath);
}

void NetworkManagerClient::ForgetConnection(const std::string& connectionPath)
{
    NmForgetConnection(m_bus, connectionPath);
}

void NetworkManagerClient::DisconnectDevice(const std::string& devicePath)
{
    NmDisconnectDevice(m_bus, devicePath);
}

// --- asynchronous versions: worker thread, private connection, snapshot of the data ---

// Lanes (see OperationWorker): connection work, radio/scan toggles, and
// the secret lookup - which can wait on a keyring prompt, i.e. on the
// user - never queue behind each other.
constexpr int kRadioLane = 1;
constexpr int kSecretLane = 2;

std::string NetworkManagerClient::DeviceKey(const std::string& devicePath) { return "device:" + devicePath; }
std::string NetworkManagerClient::ConnectionKey(const std::string& connectionPath) { return "connection:" + connectionPath; }
std::string NetworkManagerClient::RadioKey() { return "radio"; }
std::string NetworkManagerClient::ScanKey() { return "scan"; }

bool NetworkManagerClient::IsBusy(const std::string& key) const { return m_ops.IsBusy(key); }
bool NetworkManagerClient::AnyOperationPending() const { return m_ops.Busy(); }
int NetworkManagerClient::OperationFd() const { return m_ops.Fd(); }
void NetworkManagerClient::DispatchOperations() { m_ops.Dispatch(); }

bool NetworkManagerClient::SetAirplaneModeAsync(bool enabled, Completion done)
{
    return m_ops.Submit(RadioKey(), [enabled](DBusClient& bus) { return NmSetRadios(bus, !enabled, true, !enabled); }, std::move(done), kRadioLane);
}

bool NetworkManagerClient::SetWirelessEnabledAsync(bool enabled, Completion done)
{
    return m_ops.Submit(RadioKey(), [enabled](DBusClient& bus) { return NmSetRadios(bus, enabled, false, false); }, std::move(done), kRadioLane);
}

bool NetworkManagerClient::RequestWiFiScanAsync(Completion done)
{
    std::vector<std::string> paths = WiFiDevicePaths(m_devices);
    return m_ops.Submit(ScanKey(), [paths](DBusClient& bus) { return NmRequestWiFiScan(bus, paths); }, std::move(done), kRadioLane);
}

bool NetworkManagerClient::HasUsableSavedSecretAsync(const std::string& ssid, std::function<void(bool usable)> done)
{
    // The secret lookup can wait on a keyring/secret-agent prompt, i.e.
    // on the user - the worst thing to do on the UI thread. `usable`
    // is delivered through the result's ok flag.
    std::vector<SavedConnection> saved = m_savedConnections;

    return m_ops.Submit("secret:" + ssid,
        [saved, ssid](DBusClient& bus)
        {
            return NmHasUsableSavedSecret(bus, saved, ssid) ? OperationResult::Success() : OperationResult::Failure(std::string());
        },
        [done = std::move(done)](const OperationResult& result) { if (done) done(result.ok); },
        kSecretLane);
}

bool NetworkManagerClient::ConnectToAccessPointAsync(
    const std::string& devicePath, const std::string& apPath,
    const std::string& ssid, const std::string& password, Completion done)
{
    std::vector<SavedConnection> saved = m_savedConnections;

    return m_ops.Submit(DeviceKey(devicePath),
        [saved, devicePath, apPath, ssid, password](DBusClient& bus)
        {
            return NmConnectToAccessPoint(bus, saved, devicePath, apPath, ssid, password);
        },
        std::move(done));
}

bool NetworkManagerClient::ActivateSavedConnectionAsync(const std::string& connectionPath, const std::string& devicePath, Completion done)
{
    return m_ops.Submit(ConnectionKey(connectionPath),
        [connectionPath, devicePath](DBusClient& bus) { return NmActivateSavedConnection(bus, connectionPath, devicePath); },
        std::move(done));
}

bool NetworkManagerClient::DeactivateConnectionAsync(const std::string& connectionPath, Completion done)
{
    std::vector<SavedConnection> saved = m_savedConnections;

    return m_ops.Submit(ConnectionKey(connectionPath),
        [saved, connectionPath](DBusClient& bus) { return NmDeactivateConnection(bus, saved, connectionPath); },
        std::move(done));
}

bool NetworkManagerClient::ForgetConnectionAsync(const std::string& connectionPath, Completion done)
{
    return m_ops.Submit(ConnectionKey(connectionPath),
        [connectionPath](DBusClient& bus) { return NmForgetConnection(bus, connectionPath); },
        std::move(done));
}

bool NetworkManagerClient::DisconnectDeviceAsync(const std::string& devicePath, Completion done)
{
    return m_ops.Submit(DeviceKey(devicePath),
        [devicePath](DBusClient& bus) { return NmDisconnectDevice(bus, devicePath); },
        std::move(done));
}

void NetworkManagerClient::SetChangeHandler(ChangeHandler handler)
{
    m_changeHandler = std::move(handler);
}

}
