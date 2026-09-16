#include "IdleWatcher.h"

#ifdef KOHIKO_HAVE_XSS
#include <X11/extensions/scrnsaver.h>
#endif

namespace Kohiko
{

IdleWatcher::IdleWatcher()
{
}

IdleWatcher::~IdleWatcher()
{
#ifdef KOHIKO_HAVE_XSS
    if (m_info)
        XFree(m_info);
#endif
}

void IdleWatcher::Initialize(
    Display* display)
{
#ifdef KOHIKO_HAVE_XSS

    if (!display)
        return;

    int eventBase = 0;
    int errorBase = 0;

    if (!XScreenSaverQueryExtension(display, &eventBase, &errorBase))
        return;

    m_info = XScreenSaverAllocInfo();
    m_available = (m_info != nullptr);

#else

    (void)display;

#endif
}

bool IdleWatcher::Available() const
{
    return m_available;
}

std::chrono::milliseconds IdleWatcher::IdleTime(
    Display* display) const
{
#ifdef KOHIKO_HAVE_XSS

    if (!m_available || !display)
        return std::chrono::milliseconds::max();

    auto* info = static_cast<XScreenSaverInfo*>(m_info);

    if (XScreenSaverQueryInfo(display, DefaultRootWindow(display), info) == 0)
        return std::chrono::milliseconds::max();

    return std::chrono::milliseconds(info->idle);

#else

    (void)display;
    return std::chrono::milliseconds::max();

#endif
}

}
