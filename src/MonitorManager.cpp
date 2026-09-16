#include "MonitorManager.h"

#include "Utils.h"
#include "WorkspaceManager.h"
#include "Workspace.h"
#include "XConnection.h"

#include <X11/Xlib.h>

#ifdef KOHIKO_HAVE_XRANDR
#include <X11/extensions/Xrandr.h>
#endif

#include <algorithm>
#include <cstdlib>

namespace Kohiko
{

MonitorManager::MonitorManager(
    XConnection& connection,
    WorkspaceManager& workspaces)
    :
    m_connection(connection),
    m_workspaces(workspaces)
{
}

void MonitorManager::Initialize(
    const Config& config)
{
    SetRules(LoadMonitorRules(config));

#ifdef KOHIKO_HAVE_XRANDR

    int errorBase = 0;

    if (XRRQueryExtension(m_connection.GetDisplay(), &m_randrEventBase, &errorBase))
    {
        m_randrErrorBase = errorBase;

        // RRScreenChangeNotifyMask alone (the "legacy" RandR 1.0
        // event) is what actually needs XRRUpdateConfiguration() run
        // against it, but on its own it only reliably fires for a
        // resolution/rotation change to the screen as a whole - an
        // output being unplugged/replugged without changing the
        // overall screen size shows up as RRCrtcChangeNotifyMask /
        // RROutputChangeNotifyMask instead (both delivered as a
        // generic RRNotify event with a subtype - see IsRandrEvent()).
        // Selecting all three is what actually catches every hotplug
        // case, not just "the resolution changed".
        XRRSelectInput(
            m_connection.GetDisplay(),
            m_connection.Root(),
            RRScreenChangeNotifyMask |
            RRCrtcChangeNotifyMask   |
            RROutputChangeNotifyMask |
            RROutputPropertyNotifyMask);
    }

#else

    (void)config;

#endif

    Detect();
}

void MonitorManager::SetRules(
    std::vector<MonitorRule> rules)
{
    m_rules = std::move(rules);
}

std::vector<MonitorManager::DetectedOutput>
MonitorManager::QueryOutputs() const
{
    std::vector<DetectedOutput> result;

#ifdef KOHIKO_HAVE_XRANDR

    RROutput primaryOutput = None;

    // Failure here just means "nothing claims to be primary" - not
    // fatal, Primary() already falls back to the leftmost monitor.
    primaryOutput = XRRGetOutputPrimary(m_connection.GetDisplay(), m_connection.Root());

    XRRScreenResources* resources =
        XRRGetScreenResourcesCurrent(
            m_connection.GetDisplay(),
            m_connection.Root());

    if (resources)
    {
        for (int i = 0; i < resources->noutput; ++i)
        {
            XRROutputInfo* output =
                XRRGetOutputInfo(m_connection.GetDisplay(), resources, resources->outputs[i]);

            if (!output)
                continue;

            if (output->connection == RR_Connected && output->crtc)
            {
                XRRCrtcInfo* crtc =
                    XRRGetCrtcInfo(m_connection.GetDisplay(), resources, output->crtc);

                if (crtc && crtc->width > 0 && crtc->height > 0)
                {
                    DetectedOutput detected;

                    detected.name = std::string(output->name, static_cast<std::size_t>(output->nameLen));

                    detected.geometry.x = crtc->x;
                    detected.geometry.y = crtc->y;
                    detected.geometry.width  = static_cast<int>(crtc->width);
                    detected.geometry.height = static_cast<int>(crtc->height);

                    detected.primary = (resources->outputs[i] == primaryOutput);

                    result.push_back(std::move(detected));
                }

                if (crtc)
                    XRRFreeCrtcInfo(crtc);
            }

            XRRFreeOutputInfo(output);
        }

        XRRFreeScreenResources(resources);
    }

#endif

    if (result.empty())
    {
        // No XRandR at build time, or it reported nothing usable:
        // fall back to a single monitor spanning the whole display.
        // Named "default" (rather than left empty) so it's still a
        // stable, matchable identity across a Detect() call that
        // finds nothing has changed, and so `monitor=default,...`
        // is a valid (if unusual) rule for a single-monitor setup.
        DetectedOutput fallback;

        fallback.name = "default";
        fallback.geometry.x = 0;
        fallback.geometry.y = 0;
        fallback.geometry.width  = DisplayWidth(m_connection.GetDisplay(), m_connection.Screen());
        fallback.geometry.height = DisplayHeight(m_connection.GetDisplay(), m_connection.Screen());
        fallback.primary = true;

        result.push_back(fallback);
    }

    return result;
}

bool MonitorManager::WorkspaceTaken(
    int workspaceId,
    const std::vector<std::unique_ptr<Monitor>>& already) const
{
    for (const auto& monitor : already)
        if (monitor->ActiveWorkspace() && monitor->ActiveWorkspace()->Id() == workspaceId)
            return true;

    // Also check whatever's still sitting in m_monitors, un-matched so
    // far - Detect() erases each entry from m_monitors the moment it
    // decides that output survives, so at any point mid-reconciliation
    // this is exactly "every monitor not yet accounted for", whether
    // it'll end up carried over later in the same pass or dropped as
    // disconnected. Either way its workspace still counts as spoken
    // for until that's actually decided.
    for (const auto& monitor : m_monitors)
        if (monitor->ActiveWorkspace() && monitor->ActiveWorkspace()->Id() == workspaceId)
            return true;

    return false;
}

bool MonitorManager::Detect()
{
    std::vector<DetectedOutput> detected = QueryOutputs();

    std::string focusedName = m_focused ? m_focused->Name() : std::string();

    std::vector<std::unique_ptr<Monitor>> next;
    next.reserve(detected.size());

    bool changed = false;

    for (const DetectedOutput& out : detected)
    {
        auto it = std::find_if(
            m_monitors.begin(), m_monitors.end(),
            [&](const std::unique_ptr<Monitor>& m) { return m->Name() == out.name; });

        if (it != m_monitors.end())
        {
            // Same output as before (matched by name) - keep its
            // identity (and therefore its ActiveWorkspace()) exactly
            // as-is, only refreshing whatever XRandR says changed.
            (*it)->SetGeometry(out.geometry);
            (*it)->SetPrimary(out.primary);
            (*it)->SetConnected(true);

            next.push_back(std::move(*it));
            m_monitors.erase(it);

            continue;
        }

        // A genuinely new output - hotplugged in, or the very first
        // Detect() call at startup (everything is "new" then).
        changed = true;

        auto monitor = std::make_unique<Monitor>(0); // renumbered below, once the final set is known

        monitor->SetName(out.name);
        monitor->SetGeometry(out.geometry);
        monitor->SetPrimary(out.primary);
        monitor->SetConnected(true);

        int desired = 0;

        for (const MonitorRule& rule : m_rules)
        {
            if (rule.workspace >= 1 && Utils::Lower(rule.outputName) == Utils::Lower(out.name))
                desired = rule.workspace; // last matching rule for this output wins
        }

        if (desired < 1 || desired > m_workspaces.Count() || WorkspaceTaken(desired, next))
        {
            // No usable rule (none matched, or the one that did is
            // already showing on another monitor right now). Before
            // falling all the way back to "first available", try
            // whichever workspace this output was actually showing as
            // of the last clean shutdown (see
            // SetSessionWorkspaceLookup()'s comment) - a monitor= rule
            // is for pinning an output somewhere on purpose, but most
            // outputs never get one, and a monitor that was on
            // workspace 4 last time shouldn't reset to whatever's
            // "first available" every single restart just because
            // nobody bothered to pin it.
            desired = m_sessionWorkspaceLookup ? m_sessionWorkspaceLookup(out.name) : 0;
        }

        if (desired < 1 || desired > m_workspaces.Count() || WorkspaceTaken(desired, next))
        {
            // Neither an explicit rule nor a saved session workspace
            // panned out - fall back to the lowest-numbered workspace
            // nothing else is currently showing, same "first
            // available" convention
            // WindowManager::FindWorkspaceWithFewestWindows() uses.
            desired = 0;

            for (int id = 1; id <= m_workspaces.Count(); ++id)
            {
                if (!WorkspaceTaken(id, next))
                {
                    desired = id;
                    break;
                }
            }

            // Degenerate case: more monitors than workspaces exist.
            // Doubling a workspace up onto two monitors is still
            // safer than leaving one with none at all (Arrange()
            // requires every monitor to have an ActiveWorkspace()) -
            // SwitchWorkspaceOnMonitor()'s swap logic is exactly what
            // untangles this the moment the user switches either
            // monitor to something else.
            if (desired == 0)
                desired = 1;
        }

        monitor->SetWorkspace(&m_workspaces.Get(desired));

        next.push_back(std::move(monitor));
    }

    // Whatever's left in m_monitors now genuinely disappeared -
    // give WindowManager one last look at each before it's freed.
    for (auto& removed : m_monitors)
    {
        changed = true;

        removed->SetConnected(false);

        if (m_beforeMonitorRemoved)
            m_beforeMonitorRemoved(*removed);
    }

    m_monitors = std::move(next);

    // Renumber every surviving monitor by on-screen position - see
    // Monitor::Id()'s comment for why this (not an opaque persistent
    // id) is what "monitor 1/2/3" and left/right addressing key off.
    std::stable_sort(
        m_monitors.begin(), m_monitors.end(),
        [](const std::unique_ptr<Monitor>& a, const std::unique_ptr<Monitor>& b)
        {
            if (a->Geometry().x != b->Geometry().x)
                return a->Geometry().x < b->Geometry().x;

            return a->Geometry().y < b->Geometry().y;
        });

    for (std::size_t i = 0; i < m_monitors.size(); ++i)
        m_monitors[i]->SetId(static_cast<int>(i) + 1);

    // Re-resolve focus if it pointed at a monitor that's now gone -
    // falls back to Primary(), never left null.
    m_focused = FindByName(focusedName);

    if (!m_focused)
        m_focused = &Primary();

    return changed;
}

bool MonitorManager::IsRandrEvent(
    const XEvent& event) const
{
#ifdef KOHIKO_HAVE_XRANDR

    if (m_randrEventBase < 0)
        return false;

    return event.type == m_randrEventBase + RRScreenChangeNotify ||
           event.type == m_randrEventBase + RRNotify;

#else

    (void)event;
    return false;

#endif
}

bool MonitorManager::HandleXEvent(
    const XEvent& event)
{
    if (!IsRandrEvent(event))
        return false;

#ifdef KOHIKO_HAVE_XRANDR

    // Required specifically for the legacy RRScreenChangeNotify form -
    // Xlib caches the screen's own size/rotation and only refreshes
    // that cache when handed the actual event, so skipping this can
    // leave QueryOutputs()'s XRRGetScreenResourcesCurrent() call
    // seeing stale data on some drivers immediately afterwards.
    XRRUpdateConfiguration(const_cast<XEvent*>(&event));

#endif

    Detect();

    return true;
}

void MonitorManager::SetBeforeMonitorRemovedCallback(
    MonitorRemovedCallback callback)
{
    m_beforeMonitorRemoved = std::move(callback);
}

void MonitorManager::SetSessionWorkspaceLookup(
    SessionWorkspaceLookup lookup)
{
    m_sessionWorkspaceLookup = std::move(lookup);
}

Monitor& MonitorManager::Primary() const
{
    for (const auto& monitor : m_monitors)
        if (monitor->IsPrimary())
            return *monitor;

    return *m_monitors.front();
}

Monitor* MonitorManager::Focused() const
{
    return m_focused;
}

void MonitorManager::SetFocused(
    Monitor* monitor)
{
    if (monitor)
        m_focused = monitor;
}

Monitor* MonitorManager::Find(
    int id) const
{
    for (const auto& monitor : m_monitors)
        if (monitor->Id() == id)
            return monitor.get();

    return nullptr;
}

Monitor* MonitorManager::FindByName(
    const std::string& name) const
{
    if (name.empty())
        return nullptr;

    for (const auto& monitor : m_monitors)
        if (monitor->Name() == name)
            return monitor.get();

    return nullptr;
}

Monitor* MonitorManager::Containing(
    const Point& p) const
{
    for (const auto& monitor : m_monitors)
        if (monitor->Geometry().Contains(p))
            return monitor.get();

    return nullptr;
}

Monitor* MonitorManager::Neighbor(
    Monitor& from,
    Direction direction) const
{
    if (m_monitors.size() <= 1)
        return &from;

    const Rect& fromGeo = from.Geometry();
    int fromCx = fromGeo.CenterX();
    int fromCy = fromGeo.CenterY();

    bool horizontal = (direction == Direction::Left || direction == Direction::Right);

    auto isInDirection = [&](Direction dir, int cx, int cy)
    {
        switch (dir)
        {
            case Direction::Left:  return cx < fromCx;
            case Direction::Right: return cx > fromCx;
            case Direction::Up:    return cy < fromCy;
            case Direction::Down:  return cy > fromCy;
        }

        return false;
    };

    Monitor* best = nullptr;
    int bestPrimary = 0;
    int bestSecondary = 0;

    for (const auto& candidate : m_monitors)
    {
        if (candidate.get() == &from)
            continue;

        int cx = candidate->Geometry().CenterX();
        int cy = candidate->Geometry().CenterY();

        if (!isInDirection(direction, cx, cy))
            continue;

        int primaryDist   = std::abs(horizontal ? (cx - fromCx) : (cy - fromCy));
        int secondaryDist = std::abs(horizontal ? (cy - fromCy) : (cx - fromCx));

        if (!best || primaryDist < bestPrimary ||
            (primaryDist == bestPrimary && secondaryDist < bestSecondary))
        {
            best = candidate.get();
            bestPrimary = primaryDist;
            bestSecondary = secondaryDist;
        }
    }

    if (best)
        return best;

    // Nothing further in that direction - wrap around to whichever
    // monitor sits furthest toward the *opposite* side instead, the
    // same edge a one-step-at-a-time wrap would eventually land on.
    Direction opposite =
        direction == Direction::Left  ? Direction::Right :
        direction == Direction::Right ? Direction::Left  :
        direction == Direction::Up    ? Direction::Down  :
                                         Direction::Up;

    for (const auto& candidate : m_monitors)
    {
        if (candidate.get() == &from)
            continue;

        int cx = candidate->Geometry().CenterX();
        int cy = candidate->Geometry().CenterY();

        if (!isInDirection(opposite, cx, cy))
            continue;

        int primaryDist = std::abs(horizontal ? (cx - fromCx) : (cy - fromCy));

        if (!best || primaryDist > bestPrimary)
        {
            best = candidate.get();
            bestPrimary = primaryDist;
        }
    }

    return best ? best : &from;
}

Monitor* MonitorManager::ByIndex(
    int oneBasedIndex) const
{
    if (oneBasedIndex < 1 || oneBasedIndex > static_cast<int>(m_monitors.size()))
        return nullptr;

    return m_monitors[static_cast<std::size_t>(oneBasedIndex - 1)].get();
}

const std::vector<std::unique_ptr<Monitor>>&
MonitorManager::All() const
{
    return m_monitors;
}

}
