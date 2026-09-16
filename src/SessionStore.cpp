#include "SessionStore.h"

#include "ManagedWindow.h"
#include "Monitor.h"
#include "MonitorManager.h"
#include "Workspace.h"
#include "WorkspaceManager.h"
#include "Xdg.h"

#include <fstream>
#include <unordered_map>

namespace Kohiko
{

namespace
{

std::filesystem::path SessionFilePath()
{
    return Xdg::DataDir() / "session";
}

// A blank XRandr output name isn't representable as a whitespace-
// delimited token on its own, so it's written/read as this sentinel
// instead - the same reasoning HistoryStore's own flat format relies
// on for "these tokens never contain whitespace in practice", just
// with an explicit stand-in for the one field that can legitimately
// be empty.
const char* const kNoMonitorName = "-";

// Same idea for "no recorded BSP neighbor" (a floating window, or
// whichever tiled window happened to be the leftmost leaf on its
// workspace - see BSPTree::CollectPlacementRules()).
constexpr WindowID kNoNeighbor = 0;
const char* const kNoDirection = "-";

// First line of the file, purely a format marker. Bumped from the
// pre-BSP-position format (see CHANGELOG) so a session file written
// by an older Kohiko - which has no such line, and starts straight
// in with a numeric window ID - is recognized as unusable rather than
// having its first record's fields silently misread as this format's
// extra neighbor/direction columns. See Load()'s comment.
//
// V3 appends a trailing "MONITORS <count>" section after the window
// records for LastWorkspaceForMonitor() - see Save()/Load(). A V2
// file simply has no such section, which Load() already treats as
// "nothing more to read", so this bump doesn't need its own rejection
// case; only the pre-BSP-position jump above needed one, because that
// one changed the *meaning* of already-present columns rather than
// just adding an optional section after them. A V2 file is still
// accepted here: this class's whole reason for keying on a version
// string in the first place, rather than silently misreading old
// fields, is exactly to make additions like this one safe.
const char* const kFormatVersion = "KOHIKO_SESSION_V2";

// Marker introducing the trailing monitor-workspace section (see
// kFormatVersion's comment). Not a valid WindowID token, so it
// reliably ends the `while (file >> id >> ...)` window-record loop in
// Load() without that loop needing to know the record count up front.
const char* const kMonitorSectionMarker = "MONITORS";

}

SessionStore::SessionStore()
{
    Load();
}

void SessionStore::Load()
{
    m_records.clear();
    m_monitorWorkspaces.clear();

    std::ifstream file(SessionFilePath());

    if (!file)
        return;

    std::string header;
    std::getline(file, header);

    if (header != kFormatVersion)
    {
        // Either there was no session file yet, or it's in the format
        // a pre-BSP-position Kohiko wrote (see kFormatVersion's
        // comment) - either way there's nothing safe to parse here.
        // Session restore just sits this one restart out, exactly as
        // if Kohiko had never run before; nothing is lost beyond this
        // one round trip's worth of placement memory.
        return;
    }

    WindowID id;
    int workspace;
    int floating;
    int fullscreen;
    Rect geometry;
    std::string monitorName;
    WindowID neighborId;
    std::string directionToken;

    while (file >> id >> workspace >> floating >> fullscreen >>
                   geometry.x >> geometry.y >> geometry.width >> geometry.height >>
                   monitorName >> neighborId >> directionToken)
    {
        SessionWindowState state;
        state.workspace = workspace;
        state.floating = floating != 0;
        state.fullscreen = fullscreen != 0;
        state.floatingGeometry = geometry;
        state.monitorName = (monitorName == kNoMonitorName) ? std::string() : monitorName;
        state.hasNeighbor = (neighborId != kNoNeighbor) && (directionToken != kNoDirection);
        state.neighborId = neighborId;
        state.neighborDirection =
            (directionToken == "h") ? SplitDirection::Horizontal : SplitDirection::Vertical;

        m_records[id] = state;
    }

    // The window-record loop above stops the moment it can't parse a
    // WindowID - either plain EOF (a V2 file, or a V3 file that just
    // happens to have no monitors to record), or the MONITORS marker
    // (see kFormatVersion's comment). Either way, clear the failure
    // before trying to read further; there's nothing to recover, this
    // is simply how the marker was designed to be found.
    file.clear();

    std::string marker;
    int monitorCount;

    if (file >> marker >> monitorCount && marker == kMonitorSectionMarker)
    {
        std::string monitorName;
        int workspace;

        for (int i = 0; i < monitorCount && (file >> monitorName >> workspace); ++i)
            m_monitorWorkspaces[monitorName] = workspace;
    }
}

const SessionWindowState* SessionStore::Find(
    WindowID id) const
{
    auto it = m_records.find(id);
    return (it != m_records.end()) ? &it->second : nullptr;
}

int SessionStore::LastWorkspaceForMonitor(
    const std::string& monitorName) const
{
    if (monitorName.empty())
        return 0;

    auto it = m_monitorWorkspaces.find(monitorName);
    return (it != m_monitorWorkspaces.end()) ? it->second : 0;
}

void SessionStore::Save(
    const std::vector<ManagedWindow*>& windows,
    const MonitorManager& monitors,
    const WorkspaceManager& workspaces)
{
    // One BSPTree::PlacementRule per tiled window, across every
    // workspace, keyed by the window it's *about* (not its neighbor) -
    // exactly the lookup the per-window loop below needs. A window
    // with no entry here is either floating or was the leftmost leaf
    // on its workspace (see CollectPlacementRules()'s comment) -
    // either way it's saved with kNoNeighbor/kNoDirection below.
    std::unordered_map<WindowID, BSPTree::PlacementRule> neighborOf;

    for (int id = 1; id <= workspaces.Count(); ++id)
        for (const BSPTree::PlacementRule& rule : workspaces.Get(id).Tree().CollectPlacementRules())
            neighborOf[rule.window] = rule;

    std::ofstream file(SessionFilePath(), std::ios::trunc);

    file << kFormatVersion << '\n';

    for (ManagedWindow* window : windows)
    {
        // The scratchpad's own single-slot state (which window, shown
        // or hidden) isn't part of what session restore covers - see
        // this class's header comment. A scratchpad-assigned window
        // just gets adopted back as an ordinary window on restart.
        if (window->IsScratchpad())
            continue;

        Monitor* monitor = monitors.Find(window->Monitor());
        const std::string& monitorName = monitor ? monitor->Name() : std::string();

        auto neighborIt = neighborOf.find(window->Id());
        bool hasNeighbor = window->IsTiled() && neighborIt != neighborOf.end();

        file << static_cast<unsigned long>(window->Id()) << ' '
             << window->Workspace() << ' '
             << (window->IsFloating() ? 1 : 0) << ' '
             << (window->IsFullscreen() ? 1 : 0) << ' '
             << window->FloatingGeometry().x << ' '
             << window->FloatingGeometry().y << ' '
             << window->FloatingGeometry().width << ' '
             << window->FloatingGeometry().height << ' '
             << (monitorName.empty() ? kNoMonitorName : monitorName.c_str()) << ' '
             << (hasNeighbor ? static_cast<unsigned long>(neighborIt->second.neighbor) : kNoNeighbor) << ' '
             << (hasNeighbor
                     ? (neighborIt->second.direction == SplitDirection::Horizontal ? "h" : "v")
                     : kNoDirection)
             << '\n';
    }

    // Every *connected, named* monitor gets a workspace record here,
    // independently of the per-window loop above - a monitor showing
    // an otherwise-empty workspace still needs its own state saved,
    // since no window's record would carry it. An unnamed monitor
    // (kNoMonitorName's blank case) is skipped: LastWorkspaceForMonitor()
    // has nothing meaningful to key it by, and MonitorManager::Detect()
    // never looks a saved workspace up by anything else.
    std::vector<Monitor*> named;

    for (const auto& monitor : monitors.All())
        if (!monitor->Name().empty())
            named.push_back(monitor.get());

    file << kMonitorSectionMarker << ' ' << named.size() << '\n';

    for (Monitor* monitor : named)
        file << monitor->Name() << ' ' << monitor->ActiveWorkspace()->Id() << '\n';
}

}
