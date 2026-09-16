#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Kohiko
{

// True if `name` (a PipeWire node's own node.name, not its
// description) is a sink's own automatically-created monitor
// companion, not a real, separate, user-visible device - confirmed
// against a real pipewire+wireplumber session (`pactl list short
// sources` shows e.g. "testheadphones" paired with
// "testheadphones.monitor") that *every* Audio/Sink node, virtual or
// real hardware alike, gets one of these paired Audio/Source nodes:
// this is a universal PipeWire/PulseAudio-compatibility convention,
// not an artifact of any one device or test setup. Without this
// filter, one physical output device connecting produces two "new
// node" events - the sink itself and its own monitor - which is what
// "the notification is duplicated" in the field actually was; see
// kohiko-audio-tray.cpp's own NodeLabels(), the only caller, for where
// this gets applied before anything is ever diffed.
inline bool IsMonitorSource(bool isSource, const std::string& name)
{
    static const std::string kSuffix = ".monitor";

    if (!isSource || name.size() < kSuffix.size())
        return false;

    return name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;
}

enum class DeviceChangeKind
{
    Connected,
    Disconnected
};

struct DeviceChange
{
    DeviceChangeKind kind;
    std::string label;
};

// The actual "was a device really connected or disconnected" logic -
// diffs `current` against `previous` (both id -> display label,
// already filtered down to whatever actually deserves a notification;
// see IsMonitorSource() above and kohiko-audio-tray.cpp's own
// NodeLabels()) and returns exactly one DeviceChange per id that
// appeared or disappeared between the two snapshots. Deliberately
// pure and stateless - it doesn't retain or mutate either map itself,
// so the *caller* owns the single, authoritative "what did we know
// last time" snapshot (exactly one copy of it - see
// kohiko-audio-tray.cpp's own knownNodes) and is responsible for
// replacing it with `current` after acting on the result. That's what
// actually guarantees "no duplicate notification ownership": there is
// only ever one snapshot in play, so the same id can't be reported as
// newly connected twice in a row unless it genuinely disappeared and
// reappeared in between.
inline std::vector<DeviceChange> ComputeDeviceChanges(
    const std::map<std::uint32_t, std::string>& previous,
    const std::map<std::uint32_t, std::string>& current)
{
    std::vector<DeviceChange> changes;

    for (auto& [id, label] : current)
        if (previous.find(id) == previous.end())
            changes.push_back({DeviceChangeKind::Connected, label});

    for (auto& [id, label] : previous)
        if (current.find(id) == current.end())
            changes.push_back({DeviceChangeKind::Disconnected, label});

    return changes;
}

}
