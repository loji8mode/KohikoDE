#pragma once

#include "Types.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace Kohiko
{

class ManagedWindow;
class MonitorManager;
class WorkspaceManager;

// One window's state as of the last clean shutdown. See
// WindowManager::Manage() for how session.restore_priority decides
// whether this or a matching windowrule= wins when both have an
// opinion about the same window.
struct SessionWindowState
{
    int workspace = 1;
    bool floating = false;
    bool fullscreen = false;
    Rect floatingGeometry{};   // width/height <= 0 means "none saved"
    std::string monitorName;   // XRandr output name; may be empty

    // Where this window sat in its workspace's BSP tree, expressed
    // relative to another window rather than as absolute coordinates
    // (which wouldn't survive a resolution/monitor-count change
    // anyway) - see BSPTree::CollectPlacementRules()/InsertNextTo()
    // for how this gets produced and replayed. hasNeighbor is false
    // for a floating window, and for whichever single tiled window in
    // each workspace happened to be the leftmost leaf (it needs no
    // neighbor - it's just however the ordinary Insert() places the
    // first window into an empty tree).
    bool hasNeighbor = false;
    WindowID neighborId = 0;
    SplitDirection neighborDirection = SplitDirection::Vertical;
};

// Persists every managed window's placement across a Kohiko restart,
// keyed by X11 Window ID - which, unlike everything else about a
// window, survives a restart of Kohiko itself unchanged (restarting
// the *window manager* process never touches any other client's
// windows; only a full X session restart hands out fresh IDs, and a
// session file that no longer matches anything simply goes unused,
// the same as it never having existed). Follows the same "flat file
// under Xdg::DataDir()" approach as HistoryStore.
class SessionStore
{
public:

    SessionStore();

    // Loaded once at startup, before the first Manage() call -
    // WindowManager::Initialize() calls this ahead of
    // AdoptExistingWindows().
    void Load();

    // nullptr if `id` has no saved state (a window that didn't exist,
    // or wasn't managed, last time Kohiko ran).
    const SessionWindowState* Find(
        WindowID id
    ) const;

    // The workspace `monitorName` (an XRandr output name) was showing
    // as of the last clean shutdown - the one piece of session state
    // that belongs to a monitor rather than any single window. 0 if
    // there's no record (first run, an unnamed/empty monitor, or an
    // output that wasn't connected last time). MonitorManager::Detect()
    // consults this - see its own comment - as a fallback for a
    // (re)connected output's *starting* workspace: below an explicit
    // `monitor=` rule, which is a deliberate pin and always wins, but
    // above "first workspace nothing else is showing", so an output
    // that was last showing workspace 4 comes back up on workspace 4
    // without needing a fixed rule to say so every time.
    int LastWorkspaceForMonitor(
        const std::string& monitorName
    ) const;

    // Snapshots every currently managed window and writes it to disk -
    // workspace/monitor/floating-geometry/fullscreen from `windows`
    // directly, and each tiled window's BSP neighbor via `workspaces`
    // (see BSPTree::CollectPlacementRules()). Called once, from
    // WindowManager::Shutdown(). Scratchpad windows are deliberately
    // skipped - Scratchpad's own single-slot state isn't part of what
    // session restore covers.
    void Save(
        const std::vector<ManagedWindow*>& windows,
        const MonitorManager& monitors,
        const WorkspaceManager& workspaces
    );

private:

    std::unordered_map<WindowID, SessionWindowState> m_records;

    // Keyed by XRandr output name - see LastWorkspaceForMonitor().
    // Populated by Save() from every *connected* monitor at shutdown,
    // independently of m_records (a monitor showing an otherwise-empty
    // workspace still gets a record here, even with zero windows to
    // save above).
    std::unordered_map<std::string, int> m_monitorWorkspaces;

};

}
