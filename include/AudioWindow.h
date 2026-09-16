#pragma once

#include "AppConfigStore.h"
#include "AppInstanceLock.h"
#include "NotificationCenter.h"
#include "PipeWireClient.h"
#include "UiScrollView.h"
#include "UiWindow.h"

namespace Kohiko
{

// kohiko-audio's own top-level app class - a single scrollable page
// (Output Devices card, Input Devices card, Advanced Settings link)
// matching the Kohiko Audio mockup, backed entirely by
// PipeWireClient::Nodes(). The level meters and the "notify on
// default change" option aren't part of that mockup, so they - along
// with everything else that isn't - live on a second Advanced
// Settings page reached via the link at the bottom, rather than being
// dropped (see RebuildAdvancedPage()).
//
// Every device row reflows itself between a single-line layout (icon,
// name, badge, percent, slider, mute button, all in one row) and a
// two-line one (name/badge on top, slider/percent/mute below) once
// the window gets too narrow for the single-line shape to stay
// legible - see BuildDeviceRow(). The whole page reflows from scratch
// on every resize (see UiWindow::SetResizeHandler()) rather than
// assuming whatever width the window opened at, and content width is
// capped and centered past a comfortable reading width so a
// fullscreen/ultra-wide window gains breathing room instead of
// stretched, ever-wider rows.
class AudioWindow
{
public:

    AudioWindow();

    bool Initialize();
    void Run();

private:

    enum class Page { Main, Advanced };

    void RebuildChrome();
    void RebuildPage();
    int RebuildMainPage(Widget& content, int contentWidth);
    int RebuildAdvancedPage(Widget& content, int contentWidth);

    // Appends one "SECTION HEADER" + card-of-rows group (Output
    // Devices or Input Devices) to `content`, starting at `y`
    // (already absolute-in-content), and returns the y position just
    // past it.
    int AppendDeviceSection(Widget& content, int y, int contentWidth, const std::string& headerText, bool wantSource);

    std::unique_ptr<Widget> BuildDeviceRow(const AudioNode& node, const Rect& rowBounds, bool narrow, bool showDivider);
    std::unique_ptr<Widget> BuildMeterCard(bool isSource, const Rect& cardBounds);

    void MaybeNotifyDefaultChanged();

    UiWindow m_window;
    PipeWireClient m_pipewire;
    AppInstanceLock m_instanceLock;
    AppConfigStore m_settings;

    // The "old mechanism" (0.20.10) - used to be a NotificationClient
    // calling org.freedesktop.Notifications over D-Bus, an external
    // daemon Kohiko doesn't ship or control the window classification
    // of (see NotificationClient.h's own comment). Now the exact same
    // native NotificationCenter class kohiko-audio-tray uses for its
    // own connect/disconnect toasts, so "Notify when the default
    // device changes" (see RebuildAdvancedPage()'s toggle - the
    // setting/behavior itself is unchanged) reads as one consistent
    // Kohiko-native notification, not two different mechanisms
    // depending on which app happens to notice the change first.
    NotificationCenter m_notifications;

    Page m_page = Page::Main;
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
