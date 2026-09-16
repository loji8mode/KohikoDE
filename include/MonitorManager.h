#pragma once

#include "Monitor.h"
#include "MonitorRule.h"
#include "Types.h"

#include <X11/Xlib.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Kohiko
{

class XConnection;
class WorkspaceManager;
class Config;

// Detects physical outputs via XRandR when it was available at build
// time (KOHIKO_HAVE_XRANDR) and falls back to treating the whole X
// display as a single monitor otherwise - either way, everything
// above this class (WindowManager) only ever deals in Monitor
// objects and never has to know which path produced them.
//
// Owns monitor *detection and identity* only: enumerating outputs,
// matching them across a hotplug so a Monitor's identity (and
// therefore its ActiveWorkspace()) survives a mere geometry change,
// assigning a freshly-connected monitor its starting workspace, and
// tracking which one is focused. Actually moving windows/workspaces
// around in response to a hotplug is WindowManager's job (via the
// callback installed with SetBeforeMonitorRemovedCallback()) - this
// class never touches WindowRepository/ManagedWindow at all.
class MonitorManager
{
public:

    MonitorManager(
        XConnection& connection,
        WorkspaceManager& workspaces
    );

    void Initialize(
        const Config& config
    );

    void SetRules(
        std::vector<MonitorRule> rules
    );

    bool Detect();

    bool IsRandrEvent(
        const XEvent& event
    ) const;

    bool HandleXEvent(
        const XEvent& event
    );

    using MonitorRemovedCallback = std::function<void(Monitor&)>;

    void SetBeforeMonitorRemovedCallback(
        MonitorRemovedCallback callback
    );

    // Consulted by Detect() to pick a newly-(re)connected output's
    // *starting* workspace, below an explicit `monitor=` rule but
    // above "first workspace nothing else is showing" - see the
    // fallback chain there. WindowManager::Initialize() wires this to
    // SessionStore::LastWorkspaceForMonitor() before the first
    // Initialize()/Detect() call, the same "set a callback, then let
    // the owning subsystem call it when relevant" shape already used
    // for SetBeforeMonitorRemovedCallback() just above. Takes the
    // XRandr output name, returns a workspace id, or <1 for "nothing
    // saved" (which Detect() treats exactly like an unmatched rule).
    using SessionWorkspaceLookup = std::function<int(const std::string&)>;

    void SetSessionWorkspaceLookup(
        SessionWorkspaceLookup lookup
    );

    Monitor& Primary() const;

    Monitor* Focused() const;

    void SetFocused(
        Monitor* monitor
    );

    Monitor* Find(
        int id
    ) const;

    Monitor* FindByName(
        const std::string& name
    ) const;

    Monitor* Containing(
        const Point& p
    ) const;

    Monitor* Neighbor(
        Monitor& from,
        Direction direction
    ) const;

    Monitor* ByIndex(
        int oneBasedIndex
    ) const;

    const std::vector<
        std::unique_ptr<Monitor>>&
    All() const;

private:

    struct DetectedOutput
    {
        std::string name;
        Rect geometry;
        bool primary = false;
    };

    std::vector<DetectedOutput> QueryOutputs() const;

    bool WorkspaceTaken(
        int workspaceId,
        const std::vector<std::unique_ptr<Monitor>>& already
    ) const;

private:

    XConnection& m_connection;
    WorkspaceManager& m_workspaces;

    std::vector<
        std::unique_ptr<Monitor>>
    m_monitors;

    Monitor* m_focused = nullptr;

    std::vector<MonitorRule> m_rules;

    MonitorRemovedCallback m_beforeMonitorRemoved;

    SessionWorkspaceLookup m_sessionWorkspaceLookup;

    int m_randrEventBase = -1;
    int m_randrErrorBase = -1;

};

}
