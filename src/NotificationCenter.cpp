#include "NotificationCenter.h"

#include "EventLoop.h" // TickSchedule::IsDue() - see Tick()'s own comment for why this reuses that instead of its own expiry check
#include "NotificationLayout.h"

namespace Kohiko
{

NotificationCenter::NotificationCenter()
{
}

NotificationCenter::~NotificationCenter()
{
}

bool NotificationCenter::Initialize(
    Display* display,
    int screen,
    ::Window root,
    const std::string& fontPattern,
    const UiTheme& theme)
{
    m_display = display;
    m_screen = screen;
    m_root = root;
    m_theme = theme;

    return m_font.Load(display, screen, fontPattern);
}

void NotificationCenter::Post(
    const std::string& text,
    const Rect& monitorGeometry,
    std::chrono::milliseconds lifetime)
{
    if (!m_display || !m_font.IsLoaded())
        return;

    // "Handled cleanly if several events happen close together" - per
    // the spec - includes the degenerate case of something posting
    // faster than notifications expire; force the single oldest one
    // out rather than let the stack grow without bound. m_active is
    // only ever appended to below, never reordered, so the front is
    // always the least-recently-posted entry.
    if (static_cast<int>(m_active.size()) >= NotificationLayout::kMaxStacked)
    {
        Rect affectedMonitor = m_active.front().monitorGeometry;
        m_active.erase(m_active.begin());
        RestackMonitor(affectedMonitor);
    }

    auto popup = std::make_unique<NotificationPopup>();

    if (!popup->Create(m_display, m_screen, m_root, m_font, m_theme))
        return;

    // Newest goes at the very bottom-right corner; whatever's already
    // active *on this same monitor* stacks above it - see
    // NotificationLayout::ComputeRect()'s own comment for why the
    // offset (not the monitor geometry itself) is what changes per
    // slot.
    int stackOffset = 0;

    for (auto& entry : m_active)
        if (entry.monitorGeometry == monitorGeometry)
            stackOffset += entry.popup->Height() + NotificationLayout::kGap;

    int textWidth = m_font.TextWidth(text);
    int width = NotificationLayout::ComputeWidth(textWidth);
    int height = NotificationLayout::ComputeHeight(m_font.Height());

    Rect rect = NotificationLayout::ComputeRect(monitorGeometry, width, height, stackOffset);

    auto expiresAt = std::chrono::steady_clock::now() + lifetime;

    popup->Show(text, Point{rect.x, rect.y}, expiresAt);

    m_active.push_back(Entry{std::move(popup), monitorGeometry});
}

void NotificationCenter::Tick()
{
    // Cheap fast path for the overwhelmingly common steady state (no
    // notification currently showing) - see the class comment and
    // WindowManager::HasActiveNotification() for why this matters:
    // whatever's calling Tick() may be doing so at an elevated rate
    // purely because this returned true a moment ago, so keeping the
    // empty case a single comparison is what keeps that "no
    // unnecessary... redraw loops" (per the spec).
    if (m_active.empty())
        return;

    auto now = std::chrono::steady_clock::now();

    std::vector<Rect> affectedMonitors;

    for (std::size_t i = 0; i < m_active.size();)
    {
        if (TickSchedule::IsDue(now, m_active[i].popup->ExpiresAt()))
        {
            affectedMonitors.push_back(m_active[i].monitorGeometry);
            m_active.erase(m_active.begin() + static_cast<std::ptrdiff_t>(i));
        }
        else
        {
            ++i;
        }
    }

    // Restacking is deferred until every expired entry has actually
    // been erased (rather than done inline in the loop above) so a
    // monitor with two simultaneously-expiring notifications only
    // gets its survivors repositioned once, against their final
    // count - not once per erase, each time against a still-stale
    // count.
    for (auto& monitor : affectedMonitors)
        RestackMonitor(monitor);
}

void NotificationCenter::HandleExpose(::Window window)
{
    for (auto& entry : m_active)
    {
        if (entry.popup->WindowId() == window)
        {
            entry.popup->HandleExpose();
            return;
        }
    }
}

bool NotificationCenter::HasActive() const
{
    return !m_active.empty();
}

std::size_t NotificationCenter::ActiveCount() const
{
    return m_active.size();
}

bool NotificationCenter::OwnsWindow(::Window window) const
{
    for (auto& entry : m_active)
        if (entry.popup->WindowId() == window)
            return true;

    return false;
}

void NotificationCenter::RestackMonitor(const Rect& monitorGeometry)
{
    int offset = 0;

    for (auto& entry : m_active)
    {
        if (!(entry.monitorGeometry == monitorGeometry))
            continue;

        Rect rect = NotificationLayout::ComputeRect(
            monitorGeometry, entry.popup->Width(), entry.popup->Height(), offset);

        entry.popup->MoveTo(Point{rect.x, rect.y});

        offset += entry.popup->Height() + NotificationLayout::kGap;
    }
}

}
