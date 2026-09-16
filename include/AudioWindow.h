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
// from a Sidebar, each a scrollable list of device rows with a
// volume Slider and mute ToggleSwitch, plus a live level meter.
//
// "Future-ready architecture": every device row is built from
// PipeWireClient::Nodes() alone, so a future page (per-app volume,
// equalizer, ...) is just another Sidebar entry and another
// RebuildXPage()-shaped method - nothing about the window shell,
// PipeWireClient, or the Output/Input pages needs to change to add
// one.
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
    std::unique_ptr<Widget> BuildMeterWidget(bool isSource);

    UiWindow m_window;
    PipeWireClient m_pipewire;
    AppInstanceLock m_instanceLock;
    AppConfigStore m_settings;
    NotificationClient m_notifications;

    Page m_currentPage = Page::Output;
    Sidebar* m_sidebar = nullptr;
    Label* m_headerLabel = nullptr;
    ScrollView* m_scrollView = nullptr;

    bool m_pipewireDirty = false;
};

}
