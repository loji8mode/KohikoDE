#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// This is the one shared-infrastructure backend that is deliberately
// NOT a DBusClient consumer - PipeWire has no D-Bus interface of its
// own (WirePlumber, the session/policy manager the spec calls out
// alongside it, doesn't either - it's a client of PipeWire like any
// other). It speaks its own native protocol over a private
// UNIX-socket connection, via libpipewire - see PipeWireClient.cpp
// for the real pw_core/pw_registry/pw_stream/pw_metadata usage this
// header intentionally keeps out of every other translation unit
// that just wants to hold/observe a PipeWireClient.
namespace Kohiko
{

// One audio node PipeWire/WirePlumber knows about - a hardware or
// virtual sink (Audio/Sink) or source (Audio/Source). Kept as a
// plain snapshot struct (not a live-updating object) so
// kohiko-audio's UI can just re-render its device list every time
// PipeWireClient::Nodes() changes, the same "re-derive the whole
// view from the latest snapshot" approach the rest of Kohiko's UI
// code already leans on.
struct AudioNode
{
    std::uint32_t id = 0;
    std::string name;         // node.name - stable, used for default-device metadata matching
    std::string description;  // node.description - what the UI shows
    bool isSource = false;    // false = an output (Audio/Sink), true = an input (Audio/Source)
    bool isDefault = false;
    bool muted = false;

    // Linear 0.0-1.0, averaged across channels (see
    // PipeWireClient::HandleNodeParam()) - "per-channel volume" is
    // one of the spec's own listed future pages, so this
    // intentionally isn't the only thing recorded: perChannelVolumes
    // below keeps the detail available without the *current* UI
    // having to deal with more than one number per device yet.
    float volume = 1.0f;

    std::vector<float> perChannelVolumes;
};

class PipeWireClient
{
public:

    PipeWireClient();
    ~PipeWireClient();

    PipeWireClient(const PipeWireClient&) = delete;
    PipeWireClient& operator=(const PipeWireClient&) = delete;

    // Connects to the user's PipeWire instance ($PIPEWIRE_REMOTE or
    // the default "pipewire-0" socket) and starts tracking every
    // Audio/Sink and Audio/Source node plus the "default" metadata
    // object. Safe to call where no PipeWire daemon is reachable at
    // all (a bare X session with no audio stack running yet) -
    // Available() simply stays false and every other method becomes
    // a harmless no-op, same contract as DBusClient::Connect().
    bool Connect();

    void Disconnect();

    bool Available() const;

    // The fd to add to this process's own poll() loop - -1 if
    // !Available(). PipeWire's own main loop is what actually pumps
    // callbacks (registry updates, node param changes, stream
    // process callbacks), so this is a thin adapter: readable means
    // "there is PipeWire work to do", handled by calling Iterate().
    int Fd() const;

    // Runs one non-blocking iteration of PipeWire's own event loop.
    // Call whenever Fd() is readable, and also periodically (see
    // UiWindow's timer) since level-meter updates arrive on their own
    // schedule independent of registry/socket readability.
    void Iterate();

    // Every currently-known node, sinks and sources together - UI
    // code filters by IsSource() itself (see kohiko-audio's Output/
    // Input pages) rather than this exposing two separate lists, so
    // adding a third node role later (e.g. a filter/virtual device)
    // never needs a third accessor here.
    const std::vector<AudioNode>& Nodes() const;

    // Changes the "default.audio.sink"/"default.audio.source"
    // metadata key, exactly what wpctl/pavucontrol/GNOME's own
    // volume mixer do to switch the system's default device - every
    // other PipeWire-aware application (browsers, media players,
    // ...) picks that change up immediately, the same way switching
    // it from any of those other tools would.
    void SetDefaultNode(std::uint32_t nodeId);

    // `linear` is clamped to [0, 1.5] (150%, matching the overshoot
    // headroom pavucontrol/wpctl both allow) and applied identically
    // to every channel - see AudioNode::perChannelVolumes for why a
    // future per-channel page doesn't need a different entry point,
    // just a different setter added alongside this one.
    void SetVolume(std::uint32_t nodeId, float linear);
    void SetMute(std::uint32_t nodeId, bool muted);

    // Nudges a node's volume by `deltaLinear` (positive or negative),
    // clamped the same way SetVolume() is - what the tray widget's
    // scroll wheel calls, so the small "which direction is bigger"
    // arithmetic lives here once instead of duplicated in every
    // scroll-wheel handler.
    void AdjustVolume(std::uint32_t nodeId, float deltaLinear);

    // Instantaneous peak amplitude (0.0-1.0) of whichever node is
    // currently the default sink/source, updated continuously via a
    // small monitoring capture stream (see StartMetering()) rather
    // than polled on demand - reading it is just reading the latest
    // value written by the stream's own process callback.
    float OutputPeakLevel() const;
    float InputPeakLevel() const;

    // Starts/stops the monitoring capture streams behind
    // OutputPeakLevel()/InputPeakLevel(). kohiko-audio starts these
    // only while its own window/level-meter UI is actually visible
    // (see AudioWindow) - an always-running capture stream for an
    // app that's just sitting in the tray would be wasted CPU/wakeups
    // for a meter nobody's looking at.
    void StartMetering();
    void StopMetering();

    using ChangeHandler = std::function<void()>;

    // Called after any Nodes()/default-device change this client
    // becomes aware of (a new device plugged in, a volume changed
    // from another app, the default sink switching, ...) so the UI
    // knows to re-render without polling every node on every frame.
    void SetChangeHandler(ChangeHandler handler);

    struct Impl;

private:

    std::unique_ptr<Impl> m_impl;
};

}
