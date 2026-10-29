#include "ManagedWindow.h"

namespace Kohiko
{

ManagedWindow::ManagedWindow(
    WindowID id)
    :
    m_id(id)
{
}

WindowID ManagedWindow::Id() const
{
    return m_id;
}

const Rect& ManagedWindow::Geometry() const
{
    return m_geometry;
}

void ManagedWindow::SetGeometry(
    const Rect& rect)
{
    m_geometry = rect;
}

const Rect& ManagedWindow::FloatingGeometry() const
{
    return m_floatingGeometry;
}

void ManagedWindow::SetFloatingGeometry(
    const Rect& rect)
{
    m_floatingGeometry = rect;
}

void ManagedWindow::SetTitle(
    const std::string& title)
{
    m_title = title;
}

const std::string& ManagedWindow::Title() const
{
    return m_title;
}

void ManagedWindow::SetClassName(
    const std::string& className)
{
    m_className = className;
}

const std::string& ManagedWindow::ClassName() const
{
    return m_className;
}

void ManagedWindow::SetInstanceName(
    const std::string& instanceName)
{
    m_instanceName = instanceName;
}

const std::string& ManagedWindow::InstanceName() const
{
    return m_instanceName;
}

void ManagedWindow::SetRole(
    const std::string& role)
{
    m_role = role;
}

const std::string& ManagedWindow::Role() const
{
    return m_role;
}

void ManagedWindow::SetPid(
    long pid)
{
    m_pid = pid;
}

long ManagedWindow::Pid() const
{
    return m_pid;
}

void ManagedWindow::SetWorkspace(
    int workspace)
{
    m_workspace = workspace;
}

int ManagedWindow::Workspace() const
{
    return m_workspace;
}

void ManagedWindow::SetMonitor(
    int monitor)
{
    m_monitor = monitor;
}

int ManagedWindow::Monitor() const
{
    return m_monitor;
}

void ManagedWindow::SetFocused(
    bool focused)
{
    m_focused = focused;
}

bool ManagedWindow::Focused() const
{
    return m_focused;
}

void ManagedWindow::SetUrgent(
    bool urgent)
{
    m_urgent = urgent;
}

bool ManagedWindow::Urgent() const
{
    return m_urgent;
}

void ManagedWindow::SetState(
    WindowState state)
{
    m_state = state;
}

WindowState ManagedWindow::State() const
{
    return m_state;
}

void ManagedWindow::SetPreviousState(
    WindowState state)
{
    m_previousState = state;
}

WindowState ManagedWindow::PreviousState() const
{
    return m_previousState;
}

bool ManagedWindow::IsTiled() const
{
    return m_state == WindowState::Tiled;
}

bool ManagedWindow::IsFloating() const
{
    return m_state == WindowState::Floating;
}

bool ManagedWindow::IsFullscreen() const
{
    return m_state == WindowState::Fullscreen;
}

bool ManagedWindow::IsScratchpad() const
{
    return m_state == WindowState::Scratchpad;
}

bool ManagedWindow::OccupiesTreeSlot() const
{
    return IsTiled() || (IsFullscreen() && m_previousState == WindowState::Tiled);
}

void ManagedWindow::SetWindowType(
    const std::string& type)
{
    m_windowType = type;
}

const std::string& ManagedWindow::WindowType() const
{
    return m_windowType;
}

void ManagedWindow::SetRaiseSerial(
    unsigned long serial)
{
    m_raiseSerial = serial;
}

unsigned long ManagedWindow::RaiseSerial() const
{
    return m_raiseSerial;
}

void ManagedWindow::SetBorderWidth(
    int width)
{
    m_borderWidth = width;
}

int ManagedWindow::BorderWidth() const
{
    return m_borderWidth;
}

void ManagedWindow::SetMinSize(
    int width,
    int height)
{
    m_minWidth = width;
    m_minHeight = height;
}

int ManagedWindow::MinWidth() const
{
    return m_minWidth;
}

int ManagedWindow::MinHeight() const
{
    return m_minHeight;
}

void ManagedWindow::RegisterTilingMisbehavior()
{
    ++m_tilingMisbehaviorCount;
}

void ManagedWindow::ResetTilingMisbehavior()
{
    m_tilingMisbehaviorCount = 0;
}

int ManagedWindow::TilingMisbehaviorCount() const
{
    return m_tilingMisbehaviorCount;
}

void ManagedWindow::IgnoreNextUnmap()
{
    ++m_ignoredUnmaps;
}

bool ManagedWindow::ConsumeIgnoredUnmap()
{
    if (m_ignoredUnmaps > 0)
    {
        --m_ignoredUnmaps;
        return true;
    }

    return false;
}

}
