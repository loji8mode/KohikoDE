#!/usr/bin/env python3
# Minimal mock of the slice of NetworkManager's D-Bus API that
# NetworkManagerClient actually exercises - just enough of
# NetworkManager/Settings/Settings.Connection/Device/Device.Wireless/
# AccessPoint to drive real round-trip regression tests for the
# 0.20.4 credential-reuse fix (see CHANGELOG.md), and to give
# kohiko-network something real-looking to render for live GUI
# testing, without a real NetworkManager or Wi-Fi hardware. Not a
# general-purpose NetworkManager stand-in - anything this project
# doesn't touch isn't implemented.
#
# Usage: mock_networkmanager.py <scenario.json> <call-log-path>
#
# Runs on whatever bus DBUS_SYSTEM_BUS_ADDRESS points to - see
# test_networkmanager_live.sh, which points that at a private,
# throwaway dbus-daemon instance for the test's duration, never the
# real system bus.
#
# <scenario.json> shape:
#   {
#     "connections": [{"id", "ssid", "has_secret", "psk"}, ...],
#     "access_points": [{"ssid", "strength", "secured"}, ...]
#   }
#
# Every call this mock receives is appended to <call-log-path> as one
# line, in the shape test_networkmanager_live.sh greps for, e.g.:
#   CALL ActivateConnection connection=/.../Settings/0
#   CALL AddAndActivateConnection ssid=BrandNewNetwork psk=somepassword
#   CALL Update connection=/.../Settings/0 psk=newpass123!
#   CALL GetSecrets connection=/.../Settings/1

import sys
import json

import dbus
import dbus.service
import dbus.mainloop.glib
from gi.repository import GLib

NM_PATH = "/org/freedesktop/NetworkManager"
NM_IFACE = "org.freedesktop.NetworkManager"
SETTINGS_PATH = "/org/freedesktop/NetworkManager/Settings"
SETTINGS_IFACE = "org.freedesktop.NetworkManager.Settings"
CONNECTION_IFACE = "org.freedesktop.NetworkManager.Settings.Connection"
DEVICE_IFACE = "org.freedesktop.NetworkManager.Device"
WIRELESS_IFACE = "org.freedesktop.NetworkManager.Device.Wireless"
AP_IFACE = "org.freedesktop.NetworkManager.AccessPoint"
PROPERTIES_IFACE = "org.freedesktop.DBus.Properties"

NM_DEVICE_TYPE_WIFI = 2
NM_DEVICE_STATE_ACTIVATED = 100

_log_path = None


def log_call(line):
    with open(_log_path, "a") as f:
        f.write(line + "\n")


def ssid_bytes(ssid):
    return dbus.Array([dbus.Byte(b) for b in ssid.encode("utf-8")], signature="y")


class PropertyObject(dbus.service.Object):
    """A dbus.service.Object with a generic org.freedesktop.DBus.Properties
    implementation backed by a plain {interface: {name: value}} dict -
    every mock object below (NetworkManager itself, each Device, each
    AccessPoint) just populates self.props instead of writing its own
    Get/GetAll."""

    def __init__(self, bus, path):
        super().__init__(bus, path)
        self.props = {}

    def set_prop(self, iface, name, value):
        self.props.setdefault(iface, {})[name] = value

    @dbus.service.method(PROPERTIES_IFACE, in_signature="ss", out_signature="v")
    def Get(self, interface, prop):
        return self.props.get(interface, {}).get(prop, dbus.String(""))

    @dbus.service.method(PROPERTIES_IFACE, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return dbus.Dictionary(self.props.get(interface, {}), signature="sv")


class AccessPoint(PropertyObject):
    def __init__(self, bus, path, ssid, strength, secured):
        super().__init__(bus, path)
        self.path = path
        self.set_prop(AP_IFACE, "Ssid", ssid_bytes(ssid))
        self.set_prop(AP_IFACE, "Strength", dbus.Byte(strength))
        # AccessPointIsSecured() just checks "either flags word is
        # non-zero" - the exact bit doesn't matter for this mock, only
        # zero-vs-nonzero does.
        self.set_prop(AP_IFACE, "WpaFlags", dbus.UInt32(0x100 if secured else 0))
        self.set_prop(AP_IFACE, "RsnFlags", dbus.UInt32(0))


class Device(PropertyObject):
    def __init__(self, bus, path, interface_name, access_points):
        super().__init__(bus, path)
        self.path = path
        self.set_prop(DEVICE_IFACE, "DeviceType", dbus.UInt32(NM_DEVICE_TYPE_WIFI))
        self.set_prop(DEVICE_IFACE, "Interface", dbus.String(interface_name))
        self.set_prop(DEVICE_IFACE, "State", dbus.UInt32(NM_DEVICE_STATE_ACTIVATED))
        self.set_prop(DEVICE_IFACE, "ActiveConnection", dbus.ObjectPath("/"))
        self.set_prop(DEVICE_IFACE, "Ip4Config", dbus.ObjectPath("/"))
        self.set_prop(DEVICE_IFACE, "Ip6Config", dbus.ObjectPath("/"))
        self.set_prop(WIRELESS_IFACE, "HwAddress", dbus.String("AA:BB:CC:DD:EE:FF"))
        self.set_prop(WIRELESS_IFACE, "ActiveAccessPoint", dbus.ObjectPath("/"))
        self.set_prop(WIRELESS_IFACE, "AccessPoints",
                       dbus.Array([ap.path for ap in access_points], signature="o"))


class Connection(PropertyObject):
    def __init__(self, bus, path, conn_id, ssid, has_secret, psk):
        super().__init__(bus, path)
        self.path = path
        self.conn_id = conn_id
        self.ssid = ssid
        self.has_secret = has_secret
        self.psk = psk

    @dbus.service.method(CONNECTION_IFACE, in_signature="", out_signature="a{sa{sv}}")
    def GetSettings(self):
        log_call(f"CALL GetSettings connection={self.path}")
        return {
            "connection": {
                "id": dbus.String(self.conn_id),
                "type": dbus.String("802-11-wireless"),
            },
            "802-11-wireless": {
                "ssid": ssid_bytes(self.ssid),
            },
        }

    @dbus.service.method(CONNECTION_IFACE, in_signature="s", out_signature="a{sa{sv}}")
    def GetSecrets(self, setting_name):
        log_call(f"CALL GetSecrets connection={self.path}")
        if setting_name != "802-11-wireless-security" or not self.has_secret:
            return {}
        return {"802-11-wireless-security": {"psk": dbus.String(self.psk)}}

    @dbus.service.method(CONNECTION_IFACE, in_signature="a{sa{sv}}", out_signature="")
    def Update(self, settings):
        new_psk = None
        sec = settings.get("802-11-wireless-security")
        if sec is not None and "psk" in sec:
            new_psk = str(sec["psk"])

        log_call(f"CALL Update connection={self.path} psk={new_psk}")

        if new_psk is not None:
            self.psk = new_psk
            self.has_secret = True


class Settings(PropertyObject):
    def __init__(self, bus):
        super().__init__(bus, SETTINGS_PATH)
        self.bus = bus
        self.connections = []

    @dbus.service.method(SETTINGS_IFACE, in_signature="", out_signature="ao")
    def ListConnections(self):
        log_call("CALL ListConnections")
        return dbus.Array([c.path for c in self.connections], signature="o")

    def add_connection(self, conn_id, ssid, has_secret, psk):
        index = len(self.connections)
        path = f"{SETTINGS_PATH}/{index}"
        conn = Connection(self.bus, path, conn_id, ssid, has_secret, psk)
        self.connections.append(conn)
        return conn


class NetworkManager(PropertyObject):
    def __init__(self, bus, settings):
        super().__init__(bus, NM_PATH)
        self.bus = bus
        self.settings = settings
        self._next_active_id = 0
        self.set_prop(NM_IFACE, "ActiveConnections", dbus.Array([], signature="o"))
        self.set_prop(NM_IFACE, "Devices", dbus.Array([], signature="o"))

    def _fake_active_path(self):
        self._next_active_id += 1
        return f"/org/freedesktop/NetworkManager/ActiveConnection/{self._next_active_id}"

    def set_devices(self, device_paths):
        self.set_prop(NM_IFACE, "Devices", dbus.Array(device_paths, signature="o"))

    @dbus.service.method(NM_IFACE, in_signature="ooo", out_signature="o")
    def ActivateConnection(self, connection, device, specific_object):
        log_call(f"CALL ActivateConnection connection={connection}")
        return self._fake_active_path()

    @dbus.service.method(NM_IFACE, in_signature="a{sa{sv}}oo", out_signature="oo")
    def AddAndActivateConnection(self, connection_settings, device, specific_object):
        wifi = connection_settings.get("802-11-wireless", {})
        raw_ssid = wifi.get("ssid")
        ssid = bytes(bytearray(raw_ssid)).decode("utf-8", "replace") if raw_ssid is not None else "?"

        sec = connection_settings.get("802-11-wireless-security", {})
        psk = str(sec["psk"]) if "psk" in sec else None

        conn_meta = connection_settings.get("connection", {})
        conn_id = str(conn_meta.get("id", ssid))

        log_call(f"CALL AddAndActivateConnection ssid={ssid} psk={psk}")

        new_conn = self.settings.add_connection(conn_id, ssid, psk is not None, psk or "")
        return new_conn.path, self._fake_active_path()


def main():
    global _log_path
    scenario_path, _log_path = sys.argv[1], sys.argv[2]

    with open(scenario_path) as f:
        scenario = json.load(f)

    open(_log_path, "w").close()  # truncate/create

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    # Must be kept alive (bound to a name, not just constructed) for
    # as long as this process should own the service name - an
    # unreferenced BusName can be garbage-collected immediately,
    # silently releasing ownership before a single call ever arrives.
    bus_name = dbus.service.BusName("org.freedesktop.NetworkManager", bus)

    settings = Settings(bus)
    nm = NetworkManager(bus, settings)

    for c in scenario.get("connections", []):
        settings.add_connection(c["id"], c["ssid"], c.get("has_secret", False), c.get("psk", ""))

    aps = []
    for i, apcfg in enumerate(scenario.get("access_points", [])):
        ap_path = f"/org/freedesktop/NetworkManager/AccessPoint/{i}"
        aps.append(AccessPoint(bus, ap_path, apcfg["ssid"], apcfg.get("strength", 50), apcfg.get("secured", True)))

    if aps:
        device = Device(bus, "/org/freedesktop/NetworkManager/Devices/0", "wlan0", aps)
        nm.set_devices([device.path])

    print(f"mock_networkmanager: ready, {len(settings.connections)} saved connection(s), "
          f"{len(aps)} access point(s)", flush=True)

    GLib.MainLoop().run()


if __name__ == "__main__":
    main()
