#include "MouseManager.h"

#include "Config.h"
#include "KeyCombo.h"
#include "Utils.h"
#include "WindowManager.h"
#include "XConnection.h"

namespace Kohiko
{

MouseManager::MouseManager(
    XConnection& connection,
    WindowManager& windowManager)
    :
    m_connection(connection),
    m_windowManager(windowManager)
{
}

void MouseManager::Configure(
    const Config& config)
{
    m_connection.UngrabAllButtonsOnRoot();

    auto parse = [this](const std::string& text)
    {
        Binding binding;
        ParsedCombo combo = ParseCombo(text);

        if (combo.valid)
        {
            binding.modifiers = combo.modifiers;
            binding.button = ParseButton(combo.token);
        }

        return binding;
    };

    m_swapBinding   = parse(config.GetString("mouse.swap",   "SUPER+BTN1"));
    m_resizeBinding = parse(config.GetString("mouse.resize", "SUPER+BTN3"));

    long eventMask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask;

    if (m_swapBinding.button != 0)
    {
        for (unsigned int variant : m_connection.LockVariants(m_swapBinding.modifiers))
            m_connection.GrabButtonOnRoot(m_swapBinding.button, variant, eventMask);
    }

    if (m_resizeBinding.button != 0)
    {
        for (unsigned int variant : m_connection.LockVariants(m_resizeBinding.modifiers))
            m_connection.GrabButtonOnRoot(m_resizeBinding.button, variant, eventMask);
    }
}

void MouseManager::HandlePress(
    const XButtonEvent& event)
{
    Point point{event.x_root, event.y_root};

    if (Matches(m_swapBinding, event.state, event.button))
    {
        m_lastX = event.x_root;
        m_lastY = event.y_root;

        // A floating window sits visually on top of whatever tile
        // happens to be underneath it - check for one first so
        // grabbing it picks *it* up (to move it) rather than the tile
        // beneath (to swap it), which is all WindowAt()'s tree hit-test
        // alone would ever find.
        ManagedWindow* floating = m_windowManager.FloatingWindowAt(point);

        if (floating)
        {
            m_dragWindow = floating;
            m_mode = DragMode::Move;
            m_windowManager.BeginFloatingDrag(m_dragWindow, point);
            return;
        }

        m_dragWindow = m_windowManager.WindowAt(point);

        if (!m_dragWindow)
            return; // nothing under the cursor to pick up

        m_mode = DragMode::Swap;
        m_windowManager.BeginSwapDrag(m_dragWindow, point);
        return;
    }

    if (Matches(m_resizeBinding, event.state, event.button))
    {
        m_lastX = event.x_root;
        m_lastY = event.y_root;

        // Same "check for a floating window on top first" reasoning as
        // the swap/move binding above - grabbing near a floating
        // window's edge should resize *it*, not the tile sitting
        // underneath it.
        ManagedWindow* floating = m_windowManager.FloatingWindowAt(point);

        if (floating)
        {
            m_dragWindow = floating;
            m_mode = DragMode::FloatingResize;
            m_windowManager.SetResizingCursor(true);
            m_windowManager.BeginFloatingResize(m_dragWindow, point);
            return;
        }

        m_dragWindow = m_windowManager.WindowAt(point);

        if (!m_dragWindow)
            return;

        m_mode = DragMode::Resize;
        m_windowManager.SetResizingCursor(true);
        return;
    }
}

void MouseManager::HandleMotion(
    const XMotionEvent& event)
{
    if (m_mode == DragMode::Idle || !m_dragWindow)
    {
        // No Super+drag in progress - this is ambient tracking (root
        // has PointerMotionMask selected all the time now, see
        // XConnection::BecomeWindowManager()), purely so the focused
        // monitor can follow the cursor across bare desktop. Never
        // touches m_lastX/Y (those belong to an active drag's own
        // delta tracking) or falls through to the drag logic below.
        m_windowManager.HandlePointerMotion(event);
        return;
    }

    Point point{event.x_root, event.y_root};

    if (m_mode == DragMode::Swap)
    {
        // The picked-up window just follows the cursor 1:1 here -
        // nothing else moves, and no swap happens, until the button
        // is released. "Where it lands" is decided once, at drop
        // time, in WindowManager::EndSwapDrag().
        m_windowManager.UpdateSwapDrag(m_dragWindow, point);
    }
    else if (m_mode == DragMode::Move)
    {
        m_windowManager.UpdateFloatingDrag(m_dragWindow, point);
    }
    else if (m_mode == DragMode::Resize)
    {
        int dx = event.x_root - m_lastX;
        int dy = event.y_root - m_lastY;

        m_windowManager.ResizeWindow(m_dragWindow, dx, dy);
    }
    else if (m_mode == DragMode::FloatingResize)
    {
        m_windowManager.UpdateFloatingResize(m_dragWindow, point);
    }

    m_lastX = event.x_root;
    m_lastY = event.y_root;
}

void MouseManager::HandleRelease(
    const XButtonEvent& event)
{
    if (m_mode == DragMode::Swap && m_dragWindow)
    {
        Point point{event.x_root, event.y_root};
        m_windowManager.EndSwapDrag(m_dragWindow, point);
    }
    else if (m_mode == DragMode::Move && m_dragWindow)
    {
        Point point{event.x_root, event.y_root};
        m_windowManager.EndFloatingDrag(m_dragWindow, point);
    }
    else if (m_mode == DragMode::Resize)
    {
        m_windowManager.SetResizingCursor(false);
    }
    else if (m_mode == DragMode::FloatingResize && m_dragWindow)
    {
        Point point{event.x_root, event.y_root};
        m_windowManager.EndFloatingResize(m_dragWindow, point);
        m_windowManager.SetResizingCursor(false);
    }

    m_mode = DragMode::Idle;
    m_dragWindow = nullptr;
}

void WindowManager::HandleLauncherButtonPress(
    const XButtonEvent& event)
{
    if (event.window == m_launcher.WindowId())
        m_launcher.HandleButtonPress(event);
}

bool WindowManager::HandleBarOrPowerMenuButtonPress(
    const XButtonEvent& event)
{
    if (event.window == m_powerMenu.WindowId())
    {
        m_powerMenu.HandleButtonPress(event);
        ClosePowerMenu();
        return true;
    }

    for (const auto& [monitor, bar] : m_bars)
    {
        if (bar->WindowId() != event.window)
            continue;

        if (bar->PowerButtonRect().Contains(Point{event.x, event.y}))
        {
            if (m_powerMenu.IsOpen())
            {
                ClosePowerMenu();
            }
            else
            {
                Rect button = bar->PowerButtonRect();

                Point anchor{
                    bar->Geometry().x + button.x,
                    bar->Geometry().y + button.Bottom()
                };

                ManagedWindow* focused = m_repository.Focused();
                m_focusBeforeModal = focused ? focused->Id() : 0;

                m_powerMenu.Open(anchor, monitor->Geometry());
                m_connection.SetInputFocus(m_powerMenu.WindowId());
            }
        }
        else if (m_powerMenu.IsOpen())
        {
            // A click elsewhere on the same bar (or a different one)
            // reads the same as clicking any other background window
            // while a modal is open: dismiss it without acting.
            ClosePowerMenu();
        }
        else if (event.button == Button1)
        {
            // Left-click on a workspace number: go to that workspace, on
            // THIS bar's monitor, through exactly the function every
            // keyboard shortcut and `kohikoctl dispatch workspace N`
            // ends in - so a click and SUPER+N can never behave
            // differently (an open Launcher follows, a workspace
            // already shown on another monitor is refused with the same
            // notice, the bar's highlight is refreshed by the same
            // path). Only the left button; a click that lands between or
            // beside the numbers does nothing.
            if (int workspace = bar->WorkspaceAt(event.x, event.y))
                SwitchWorkspaceOnMonitor(*monitor, workspace);
        }

        return true;
    }

    return false;
}

void WindowManager::ClosePowerMenu()
{
    if (!m_powerMenu.IsOpen())
        return;

    m_powerMenu.Close();
    RestoreFocusAfterModal();
}

bool MouseManager::Matches(
    const Binding& binding,
    unsigned int state,
    unsigned int button) const
{
    if (binding.button == 0 || binding.button != button)
        return false;

    static constexpr unsigned int kRelevantMask =
        ShiftMask | ControlMask | Mod1Mask | Mod4Mask;

    return (state & kRelevantMask) == binding.modifiers;
}

unsigned int MouseManager::ParseButton(
    const std::string& token) const
{
    std::string lower = Utils::Lower(token);

    if (lower.rfind("btn", 0) != 0 || lower.size() <= 3)
        return 0;

    try
    {
        switch (std::stoi(lower.substr(3)))
        {
            case 1: return Button1;
            case 2: return Button2;
            case 3: return Button3;
            case 4: return Button4;
            case 5: return Button5;
            default: return 0;
        }
    }
    catch (...)
    {
        return 0;
    }
}

}
