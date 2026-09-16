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

void NetworkManagerClient::SetAirplaneMode(bool enabled)
{
    // "Airplane mode" here means "every NetworkManager-managed radio
    // off" - enabled=true turns radios off, matching the usual
    // physical-switch meaning of the phrase, which is the inverse of
    // the WirelessEnabled/WwanEnabled property sense.
    bool radiosOn = !enabled;
    m_bus.SetProperty(kNmService, kNmManagerPath, kNmManagerIface, "WirelessEnabled", DBusValue::MakeBool(radiosOn));
    m_bus.SetProperty(kNmService, kNmManagerPath, kNmManagerIface, "WwanEnabled", DBusValue::MakeBool(radiosOn));
}

void NetworkManagerClient::SetWirelessEnabled(bool enabled)
{
    m_bus.SetProperty(kNmService, kNmManagerPath, kNmManagerIface, "WirelessEnabled", DBusValue::MakeBool(enabled));
}

void NetworkManagerClient::RequestWiFiScan()
{
    for (auto& device : m_devices)
    {
        if (device.kind != NetworkDeviceKind::WiFi)
            continue;

        m_bus.Call(kNmService, device.objectPath, kNmWirelessIface, "RequestScan",
            { DBusValue::MakeDict({}) });
    }
}

void NetworkManagerClient::ConnectToAccessPoint(
    const std::string& devicePath, const std::string& apPath,
    const std::string& ssid, const std::string& password)
{
    // First preference: reuse an existing saved profile for this
    // exact SSID, exactly like clicking a known network in nm-applet
    // does - it carries over any previously-entered credentials
    // rather than asking again.
    for (auto& saved : m_savedConnections)
    {
        if (saved.type != "802-11-wireless")
            continue;

        std::vector<DBusValue> settingsReply;
        if (!m_bus.Call(kNmService, saved.objectPath, kNmConnectionIface, "GetSettings", {}, &settingsReply) || settingsReply.empty())
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

                m_bus.Call(kNmService, saved.objectPath, kNmConnectionIface, "Update",
                    { DBusValue::MakeDict(std::move(topLevel)) });
            }

            m_bus.Call(kNmService, kNmManagerPath, kNmManagerIface, "ActivateConnection",
                { DBusValue::MakeObjectPath(saved.objectPath), DBusValue::MakeObjectPath(devicePath), DBusValue::MakeObjectPath(apPath) });
            return;
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

    m_bus.Call(kNmService, kNmManagerPath, kNmManagerIface, "AddAndActivateConnection",
        { DBusValue::MakeDict(std::move(topLevel)),
          DBusValue::MakeObjectPath(devicePath), DBusValue::MakeObjectPath(apPath) });
}

void NetworkManagerClient::ActivateSavedConnection(const std::string& connectionPath, const std::string& devicePath)
{
    m_bus.Call(kNmService, kNmManagerPath, kNmManagerIface, "ActivateConnection",
        { DBusValue::MakeObjectPath(connectionPath), DBusValue::MakeObjectPath(devicePath), DBusValue::MakeObjectPath("/") });
}

void NetworkManagerClient::DeactivateConnection(const std::string& connectionPath)
{
    for (auto& saved : m_savedConnections)
    {
        if (saved.objectPath == connectionPath && !saved.activeConnectionPath.empty())
        {
            m_bus.Call(kNmService, kNmManagerPath, kNmManagerIface, "DeactivateConnection",
                { DBusValue::MakeObjectPath(saved.activeConnectionPath) });
            return;
        }
    }
}

void NetworkManagerClient::ForgetConnection(const std::string& connectionPath)
{
    m_bus.Call(kNmService, connectionPath, kNmConnectionIface, "Delete", {});
}

void NetworkManagerClient::DisconnectDevice(const std::string& devicePath)
{
    m_bus.Call(kNmService, devicePath, kNmDeviceIface, "Disconnect", {});
}

void NetworkManagerClient::SetChangeHandler(ChangeHandler handler)
{
    m_changeHandler = std::move(handler);
}

}
