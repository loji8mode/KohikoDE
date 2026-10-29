#pragma once

#include "DBusClient.h"

#include <cstdint>
#include <functional>
#include <map>
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
    // ---- Actions are ASYNCHRONOUS -----------------------------------
    //
    // Each of these returns immediately; the real work happens in
    // bluetoothd and its reply arrives later through Dispatch() (the
    // window already watches Fd()), so a slow Pair (up to 30 s while
    // the user confirms on the device) or Connect (up to 15 s) never
    // freezes the window the way the old blocking calls did. Outcomes
    // are reported through the optional DoneHandler AND recorded -
    // see IsBusy()/LastError() - and ChangeHandler fires after every
    // completion so the window can redraw. A failure is never silent.
    //
    // At most ONE operation runs per object path: asking for another
    // while one is in flight is refused (the DoneHandler gets
    // ok=false, "already in progress") rather than queued, so a double
    // click cannot send Connect twice.
    using DoneHandler = std::function<void(bool ok, const std::string& error)>;

    // True while an operation on `objectPath` is awaiting its reply, and
    // a short user-facing description of it ("Pairing\u2026").
    bool IsBusy(const std::string& objectPath) const;
    std::string BusyLabel(const std::string& objectPath) const;

    // The most recent failure (or "stopped waiting" notice) for an
    // object, or "" - cleared when its next operation starts.
    std::string LastError(const std::string& objectPath) const;

    // Cancels the operation in flight on `objectPath`, as far as that is
    // actually possible. For a Pair, bluetoothd is asked to
    // CancelPairing - a real cancellation. For anything else only the
    // WAITING stops: the request was already sent and may still
    // complete, and LastError() says exactly that rather than claiming
    // it was cancelled. Returns false if nothing was in flight.
    bool CancelPending(const std::string& objectPath);

    void SetAdapterPowered(const std::string& adapterPath, bool powered);
    void SetAdapterDiscoverable(const std::string& adapterPath, bool discoverable);
    void StartDiscovery(const std::string& adapterPath);
    void StopDiscovery(const std::string& adapterPath);

    void PairDevice(const std::string& devicePath, DoneHandler done = nullptr);
    void ConnectDevice(const std::string& devicePath, DoneHandler done = nullptr);
    void DisconnectDevice(const std::string& devicePath, DoneHandler done = nullptr);
    void SetDeviceTrusted(const std::string& devicePath, bool trusted);
    void RemoveDevice(const std::string& adapterPath, const std::string& devicePath, DoneHandler done = nullptr);

    // "Pair" button: Pair, then - once that has actually finished -
    // Connect (0.21.5 issued both back to back on the UI thread, up to
    // 45 s frozen). Connect is skipped if pairing failed, unless the
    // device was simply already paired.
    void PairAndConnectDevice(const std::string& devicePath);

    // "Disconnect and Forget": Disconnect, then RemoveDevice once it
    // has finished (whether or not the disconnect succeeded).
    void DisconnectAndRemoveDevice(const std::string& adapterPath, const std::string& devicePath);

    using ChangeHandler = std::function<void()>;
    void SetChangeHandler(ChangeHandler handler);

private:

    struct Operation
    {
        std::string label;
        std::string method;
        DBusClient::CallId callId = 0;
    };

    // Starts one device/adapter method call, enforcing one operation per
    // object path. See the public comment above.
    // `busyKey` is the object the operation is ABOUT (what IsBusy()
    // reports on); `targetPath` is the object that implements the method
    // (the same, except RemoveDevice, which is a method of the adapter).
    bool BeginOperation(
        const std::string& busyKey, const std::string& targetPath,
        const std::string& interface, const std::string& method,
        const std::string& label, const std::vector<DBusValue>& args,
        int timeoutMs, DoneHandler done);

    // Completion callback for the fire-and-forget calls (powered,
    // discoverable, trusted, discovery start/stop): they are async too,
    // and a failure is recorded under `objectPath` as
    // "<what> failed: <reason>" instead of being swallowed.
    DBusClient::AsyncCallback RecordFailure(const std::string& objectPath, const std::string& what);

    DBusClient m_bus;
    std::map<std::string, Operation> m_inFlight;
    std::map<std::string, std::string> m_errors;
    std::vector<BluetoothAdapter> m_adapters;
    std::vector<BluetoothDevice> m_devices;
    ChangeHandler m_changeHandler;
};

}
