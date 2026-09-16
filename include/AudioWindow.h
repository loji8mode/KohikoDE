#pragma once

#include "AppConfigStore.h"
#include "AppInstanceLock.h"
#include "NotificationClient.h"
#include "PipeWireClient.h"
#include "UiScrollView.h"
#include "UiSidebar.h"
#include "UiWindow.h"

namespace Kohiko
{

// kohiko-audio's own top-level app class - the permanent audio
// control center the spec asks for. Two pages (Output/Input) picked
// from a Sidebar; each page is a page header, a live level meter
// card, a responsive grid of device cards, and an options card -
// laid out to actually use a wide window's width (a 1- or 2-column
// device grid depending on available space and device count, see
// RebuildDeviceGrid()) rather than a single narrow column stretched
// across it.
//
// "Future-ready architecture": every device card is built from
// PipeWireClient::Nodes() alone, so a future page (per-app volume,
// equalizer, ...) is just another Sidebar entry and another
// RebuildXPage()-shaped method - nothing about the window shell,
// PipeWireClient, or the Output/Input pages needs to change to add
// one. Likewise, the whole page reflows from scratch on every resize
// (see UiWindow::SetResizeHandler()) rather than assuming whatever
// width the window opened at, so a tiled half-screen window, a
// maximized window, and an ultra-wide monitor all get a layout suited
// to their own size instead of the same fixed one stretched or
// clipped to fit.
class AudioWindow
{
public:

    AudioWindow();

    bool Initialize();
    void Run();

private:

    enum class Page { Output, Input };

    void RebuildChrome();
    void RebuildPage();

    std::unique_ptr<Widget> BuildMeterCard(bool isSource, int width);
    std::unique_ptr<Widget> BuildDeviceCard(const AudioNode& node, int width);
    std::unique_ptr<Widget> BuildOptionsCard(int width);

    void MaybeNotifyDefaultChanged();

    UiWindow m_window;
    PipeWireClient m_pipewire;
    AppInstanceLock m_instanceLock;
    AppConfigStore m_settings;
    NotificationClient m_notifications;

    Page m_currentPage = Page::Output;
    Sidebar* m_sidebar = nullptr;
    ScrollView* m_scrollView = nullptr;

    bool m_pipewireDirty = false;

    // Tracks the default output's description across rebuilds purely
    // to detect a genuine *change* (as opposed to every routine
    // rebuild) for MaybeNotifyDefaultChanged() - not shown anywhere
    // in the UI itself, which always just reads PipeWireClient::
    // Nodes() fresh.
    std::string m_lastDefaultOutputName;
    bool m_haveSeenDefaultOutput = false;
};

}

