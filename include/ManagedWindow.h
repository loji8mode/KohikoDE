#pragma once

#include "Types.h"

#include <string>

namespace Kohiko
{

// Everything Kohiko knows about one client window - the spec's list:
// geometry, state, workspace, monitor, title, PID, class, role,
// border, focus, urgent, floating, fullscreen, scratchpad.
class ManagedWindow
{
public:

    explicit ManagedWindow(
        WindowID id
    );

    WindowID Id() const;

    const Rect& Geometry() const;

    void SetGeometry(
        const Rect& rect
    );

    // Where a floating window sits/should be restored to - kept
    // separate from Geometry() so toggling fullscreen or re-floating
    // a tiled window can put it back where it was.
    const Rect& FloatingGeometry() const;

    void SetFloatingGeometry(
        const Rect& rect
    );

    void SetTitle(
        const std::string& title
    );

    const std::string& Title() const;

    void SetClassName(
        const std::string& className
    );

    const std::string& ClassName() const;

    void SetInstanceName(
        const std::string& instanceName
    );

    const std::string& InstanceName() const;

    void SetRole(
        const std::string& role
    );

    const std::string& Role() const;

    void SetPid(
        long pid
    );

    long Pid() const;

    void SetWorkspace(
        int workspace
    );

    int Workspace() const;

    void SetMonitor(
        int monitor
    );

    int Monitor() const;

    void SetFocused(
        bool focused
    );

    bool Focused() const;

    void SetUrgent(
        bool urgent
    );

    bool Urgent() const;

    void SetState(
        WindowState state
    );

    WindowState State() const;

    // What State() was before it got overridden by Fullscreen, so
    // ToggleFullscreen(off) knows what to go back to.
    void SetPreviousState(
        WindowState state
    );

    WindowState PreviousState() const;

    // Monotonic "last raised" stamp (see WindowManager::StampStacking()).
    // Among floating windows, a higher serial means more recently raised
    // and therefore stacked higher: ArrangeMonitor() raises floats in
    // ascending serial order, which makes its re-raise order-PRESERVING
    // instead of arbitrary. 0 = never stamped.
    unsigned long StackSerial() const { return m_stackSerial; }
    void SetStackSerial(unsigned long serial) { m_stackSerial = serial; }

    bool IsTiled() const;

    bool IsFloating() const;

    bool IsFullscreen() const;

    bool IsScratchpad() const;

    // Whether this window still has a leaf reserved for it in its
    // workspace's BSP tree right now - NOT the same question as
    // IsTiled(). A window that was tiled and then went fullscreen
    // (Super+F, a client's own EWMH request, `windowrule=fullscreen`)
    // moves to WindowState::Fullscreen but is deliberately left in the
    // tree exactly where it was, so toggling fullscreen back off
    // restores it to the same spot - which means IsTiled() alone is
    // the wrong question everywhere a caller actually needs to know
    // "does WindowManager::Unmanage()/MoveFocusedToWorkspace()/
    // ToggleScratchpadForFocused() need to remove a tree node for
    // this window", since checking only IsTiled() there answers "no"
    // for a currently-fullscreen window and leaves a stale leaf/split
    // behind reserving its share of the screen forever (until the WM
    // restarts) - this is what previously made a tiled window that
    // went fullscreen right before closing (Telegram's media viewer,
    // which asks for real fullscreen, being the case that actually
    // surfaced it) leave permanent dead space once closed.
    bool OccupiesTreeSlot() const;

    void SetBorderWidth(
        int width
    );

    int BorderWidth() const;

    // The client's own declared minimum usable size, straight from
    // WM_NORMAL_HINTS (PMinSize) - read once in Manage() and kept here
    // so every later capacity check (TryTile, FindWorkspaceWithRoom,
    // ToggleFloating back to tiled, ...) can defer to it without
    // re-querying X every time. 0 means the client didn't declare one;
    // callers fall back to the configured general.min_tile_width/
    // height in that case, same as before this existed.
    void SetMinSize(
        int width,
        int height
    );

    int MinWidth() const;
    int MinHeight() const;

    // How many times, in a row since this window was last (re-)tiled,
    // its own ConfigureRequest has asked for geometry different from
    // the fixed tile Kohiko actually gave it - WindowManager's proxy
    // signal for "this client is fighting tiled geometry" (repeated
    // resize attempts, repeated ConfigureRequest loops - see
    // general.tiling_misbehavior_threshold/fallback in the README).
    // Reset to 0 by ResetTilingMisbehavior() whenever this window is
    // freshly tiled or the fallback actually fires, so the count
    // always reflects how it's behaving *right now*, not some earlier
    // tile it may have already been moved out of.
    void RegisterTilingMisbehavior();

    void ResetTilingMisbehavior();

    int TilingMisbehaviorCount() const;

    // Called right before WindowManager unmaps this window itself
    // (workspace switch, scratchpad hide) so the resulting UnmapNotify
    // isn't mistaken for the client withdrawing/closing.
    void IgnoreNextUnmap();

    // Returns true (and consumes one) if an ignored unmap was pending.
    bool ConsumeIgnoredUnmap();

private:

    WindowID m_id;

    Rect m_geometry;
    Rect m_floatingGeometry;

    std::string m_title;
    std::string m_className;
    std::string m_instanceName;
    std::string m_role;
    long m_pid = 0;

    int m_workspace = 1;
    int m_monitor = 0;

    bool m_focused = false;
    unsigned long m_stackSerial = 0;
    bool m_urgent = false;

    WindowState m_state = WindowState::Tiled;
    WindowState m_previousState = WindowState::Tiled;

    int m_borderWidth = 0;

    int m_minWidth = 0;
    int m_minHeight = 0;

    int m_tilingMisbehaviorCount = 0;

    int m_ignoredUnmaps = 0;

};

}
