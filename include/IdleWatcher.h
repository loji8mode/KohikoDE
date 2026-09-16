#pragma once

#include <X11/Xlib.h>

#include <chrono>

// Wraps the X11 XScreenSaver extension purely to answer one question:
// how long has it been since the last keyboard/mouse input anywhere
// on this X server - not just input directed at one of Kohiko's own
// windows, which is the whole point of using this extension instead
// of Kohiko just watching its own KeyPress/ButtonPress/MotionNotify
// traffic. Backs Idle-timeout locking (see WindowManager::Tick() and
// `lockscreen.idle_timeout_minutes`) and display-sleep inhibition
// (see WindowManager's DPMS inhibition and `general.
// inhibit_sleep_during_playback`).
//
// Same "gracefully degrade, never hard-require" approach as
// MonitorManager's own XRandr usage: built conditionally
// (KOHIKO_HAVE_XSS) against whatever's available at compile time, and
// Available() reports false at runtime too on a server that doesn't
// actually implement the extension - either way, every caller treats
// "unavailable" as "just never fires", not as an error.
namespace Kohiko
{

class IdleWatcher
{
public:

    IdleWatcher();
    ~IdleWatcher();

    // No copying - owns an XScreenSaverInfo* allocated for the
    // lifetime of this object (see the .cpp).
    IdleWatcher(const IdleWatcher&) = delete;
    IdleWatcher& operator=(const IdleWatcher&) = delete;

    void Initialize(Display* display);

    bool Available() const;

    // How long since the last input event anywhere on the server -
    // std::chrono::milliseconds::max() if !Available(), so a naive
    // `IdleTime() >= threshold` check from a caller that forgot to
    // check Available() first still fails safe (never "instantly
    // idle" for a threshold of zero-ish, and never spuriously fires
    // an idle-triggered action from a display that can't actually
    // measure idle time at all).
    std::chrono::milliseconds IdleTime(Display* display) const;

private:

    bool m_available = false;

#ifdef KOHIKO_HAVE_XSS
    void* m_info = nullptr; // XScreenSaverInfo*, opaque here to keep the extension header out of this file
#endif

};

}
