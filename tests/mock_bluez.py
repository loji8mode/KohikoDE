#!/usr/bin/env python3
# Minimal mock of the slice of BlueZ's D-Bus API that BluezClient
# actually exercises - org.freedesktop.DBus.ObjectManager's
# GetManagedObjects() on "/" for the initial adapter/device/battery
# dump, plus org.bluez.Adapter1/Device1's own methods and
# org.freedesktop.DBus.Properties.Set - to give kohiko-bluetooth
# something real-looking to render for live GUI testing, without real
# Bluetooth hardware. Not a general-purpose BlueZ stand-in - anything
# this project doesn't touch isn't implemented.
#
# Usage: mock_bluez.py <scenario.json> <call-log-path>
#
# Runs on whatever bus DBUS_SYSTEM_BUS_ADDRESS points to - see
# test_bluetooth_live.sh, which points that at a private, throwaway
# dbus-daemon instance, never the real system bus.
#
# <scenario.json> shape:
#   {
#     "adapters": [{"path", "address", "alias", "powered", "discoverable", "discovering"}],
#     "devices": [{"path", "adapter", "address", "alias", "icon", "paired", "connected",
#                  "trusted", "battery" (optional, -1/absent = no Battery1 interface)}]
#   }

import sys
import json

import dbus
import dbus.exceptions
import dbus.service
import dbus.mainloop.glib
from gi.repository import GLib

BLUEZ_SERVICE = "org.bluez"
ADAPTER_IFACE = "org.bluez.Adapter1"
DEVICE_IFACE = "org.bluez.Device1"
BATTERY_IFACE = "org.bluez.Battery1"
OBJECT_MANAGER_IFACE = "org.freedesktop.DBus.ObjectManager"
PROPERTIES_IFACE = "org.freedesktop.DBus.Properties"

_log_path = None


def log_call(line):
    with open(_log_path, "a") as f:
        f.write(line + "\n")


class BluezObject(dbus.service.Object):
    """Every adadpter/device is one of these - holds its own
    {interface: {prop: value}} dict, exports the standard
    Properties.Get/GetAll/Set trio against it, and is *also* what
    GetManagedObjects() on the root object walks to build its own
    reply (see BluezRoot.GetManagedObjects)."""

    def __init__(self, bus, path):
        super().__init__(bus, path)
        self.path = path
        self.props = {}

    def set_prop(self, iface, name, value):
        self.props.setdefault(iface, {})[name] = value

    @dbus.service.method(PROPERTIES_IFACE, in_signature="ss", out_signature="v")
    def Get(self, interface, prop):
        return self.props.get(interface, {}).get(prop, dbus.String(""))

    @dbus.service.method(PROPERTIES_IFACE, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return dbus.Dictionary(self.props.get(interface, {}), signature="sv")

    @dbus.service.method(PROPERTIES_IFACE, in_signature="ssv", out_signature="")
    def Set(self, interface, prop, value):
        log_call(f"CALL Set path={self.path} iface={interface} prop={prop} value={value}")
        self.set_prop(interface, prop, value)


class Adapter(BluezObject):
    def __init__(self, bus, path, address, alias, powered, discoverable, discovering):
        super().__init__(bus, path)
        self.set_prop(ADAPTER_IFACE, "Address", dbus.String(address))
        self.set_prop(ADAPTER_IFACE, "Alias", dbus.String(alias))
        self.set_prop(ADAPTER_IFACE, "Powered", dbus.Boolean(powered))
        self.set_prop(ADAPTER_IFACE, "Discoverable", dbus.Boolean(discoverable))
        self.set_prop(ADAPTER_IFACE, "Discovering", dbus.Boolean(discovering))

    @dbus.service.method(ADAPTER_IFACE, in_signature="", out_signature="")
    def StartDiscovery(self):
        log_call(f"CALL StartDiscovery path={self.path}")
        self.set_prop(ADAPTER_IFACE, "Discovering", dbus.Boolean(True))

    @dbus.service.method(ADAPTER_IFACE, in_signature="", out_signature="")
    def StopDiscovery(self):
        log_call(f"CALL StopDiscovery path={self.path}")
        self.set_prop(ADAPTER_IFACE, "Discovering", dbus.Boolean(False))

    @dbus.service.method(ADAPTER_IFACE, in_signature="o", out_signature="")
    def RemoveDevice(self, device_path):
        log_call(f"CALL RemoveDevice adapter={self.path} device={device_path}")


class Device(BluezObject):
    def __init__(self, bus, path, address, alias, icon, paired, connected, trusted, battery):
        super().__init__(bus, path)
        self.set_prop(DEVICE_IFACE, "Address", dbus.String(address))
        self.set_prop(DEVICE_IFACE, "Alias", dbus.String(alias))
        self.set_prop(DEVICE_IFACE, "Icon", dbus.String(icon))
        self.set_prop(DEVICE_IFACE, "Paired", dbus.Boolean(paired))
        self.set_prop(DEVICE_IFACE, "Connected", dbus.Boolean(connected))
        self.set_prop(DEVICE_IFACE, "Trusted", dbus.Boolean(trusted))
        if battery is not None and battery >= 0:
            self.set_prop(BATTERY_IFACE, "Percentage", dbus.Byte(battery))

    # Optional per-device behaviour, from the scenario (all default to "instant success"):
    #   "pair_delay_ms" / "connect_delay_ms" - how long the call stays pending before it
    #       replies. Replies are scheduled with GLib.timeout_add through python-dbus's
    #       async callbacks, so the MOCK never blocks either - which is what lets tests
    #       observe a client that keeps working while a slow Pair/Connect is in flight.
    #   "pair_error" / "connect_error" - the call fails with this message instead.
    # CancelPairing aborts a pending Pair, as the real bluetoothd's does.
    delays = {}
    errors = {}
    pending_pair = None

    def _later(self, name, ok_action, reply_cb, error_cb):
        delay = self.delays.get(name, 0)
        error = self.errors.get(name)

        def finish():
            if name == "Pair":
                self.pending_pair = None

            if error:
                error_cb(dbus.exceptions.DBusException(error, name="org.bluez.Error.Failed"))
            else:
                ok_action()
                reply_cb()

            log_call(f"DONE {name} path={self.path} {'error' if error else 'ok'}")
            return False

        if delay <= 0:
            finish()
            return

        source = GLib.timeout_add(delay, finish)

        if name == "Pair":
            self.pending_pair = (source, error_cb)

    @dbus.service.method(DEVICE_IFACE, in_signature="", out_signature="", async_callbacks=("reply_cb", "error_cb"))
    def Pair(self, reply_cb, error_cb):
        log_call(f"CALL Pair path={self.path}")
        self._later("Pair", lambda: self.set_prop(DEVICE_IFACE, "Paired", dbus.Boolean(True)), reply_cb, error_cb)

    @dbus.service.method(DEVICE_IFACE, in_signature="", out_signature="")
    def CancelPairing(self):
        log_call(f"CALL CancelPairing path={self.path}")

        if self.pending_pair:
            source, error_cb = self.pending_pair
            self.pending_pair = None
            GLib.source_remove(source)
            error_cb(dbus.exceptions.DBusException("Authentication Canceled", name="org.bluez.Error.AuthenticationCanceled"))
            log_call(f"DONE Pair path={self.path} cancelled")

    @dbus.service.method(DEVICE_IFACE, in_signature="", out_signature="", async_callbacks=("reply_cb", "error_cb"))
    def Connect(self, reply_cb, error_cb):
        log_call(f"CALL Connect path={self.path}")
        self._later("Connect", lambda: self.set_prop(DEVICE_IFACE, "Connected", dbus.Boolean(True)), reply_cb, error_cb)

    @dbus.service.method(DEVICE_IFACE, in_signature="", out_signature="")
    def Disconnect(self):
        log_call(f"CALL Disconnect path={self.path}")
        self.set_prop(DEVICE_IFACE, "Connected", dbus.Boolean(False))


class BluezRoot(dbus.service.Object):
    """BlueZ hosts ObjectManager on '/' itself (unlike NetworkManager,
    which doesn't implement ObjectManager at all) - see kRootPath in
    BluezClient.cpp."""

    def __init__(self, bus):
        super().__init__(bus, "/")
        self.objects = []  # list of (path, BluezObject, [interfaces])

    def register(self, obj, interfaces):
        self.objects.append((obj.path, obj, interfaces))

    @dbus.service.method(OBJECT_MANAGER_IFACE, in_signature="", out_signature="a{oa{sa{sv}}}")
    def GetManagedObjects(self):
        log_call("CALL GetManagedObjects")
        result = {}
        for path, obj, interfaces in self.objects:
            result[dbus.ObjectPath(path)] = dbus.Dictionary(
                {iface: dbus.Dictionary(obj.props.get(iface, {}), signature="sv") for iface in interfaces},
                signature="sa{sv}",
            )
        return result


def main():
    global _log_path
    scenario_path, _log_path = sys.argv[1], sys.argv[2]

    with open(scenario_path) as f:
        scenario = json.load(f)

    open(_log_path, "w").close()

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    bus_name = dbus.service.BusName(BLUEZ_SERVICE, bus)  # kept alive - see mock_networkmanager.py's own note

    root = BluezRoot(bus)

    for a in scenario.get("adapters", []):
        adapter = Adapter(bus, a["path"], a.get("address", "AA:AA:AA:AA:AA:AA"), a.get("alias", "Adapter"),
                           a.get("powered", True), a.get("discoverable", False), a.get("discovering", False))
        root.register(adapter, [ADAPTER_IFACE])

    for d in scenario.get("devices", []):
        battery = d.get("battery", -1)
        device = Device(bus, d["path"], d.get("address", "BB:BB:BB:BB:BB:BB"), d.get("alias", "Device"),
                         d.get("icon", ""), d.get("paired", False), d.get("connected", False),
                         d.get("trusted", False), battery if battery is not None and battery >= 0 else None)
        device.delays = {"Pair": d.get("pair_delay_ms", 0), "Connect": d.get("connect_delay_ms", 0)}
        device.errors = {k: d[key] for k, key in (("Pair", "pair_error"), ("Connect", "connect_error")) if d.get(key)}
        interfaces = [DEVICE_IFACE]
        if battery is not None and battery >= 0:
            interfaces.append(BATTERY_IFACE)
        root.register(device, interfaces)

    print(f"mock_bluez: ready, {len(scenario.get('adapters', []))} adapter(s), "
          f"{len(scenario.get('devices', []))} device(s)", flush=True)

    GLib.MainLoop().run()


if __name__ == "__main__":
    main()
