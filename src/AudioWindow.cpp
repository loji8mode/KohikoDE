#include "AudioWindow.h"

#include <algorithm>
#include <cmath>

namespace Kohiko
{

namespace
{

constexpr int kWindowWidth = 640;
constexpr int kWindowHeight = 520;
constexpr int kSidebarWidth = 160;
constexpr int kMargin = 16;

// A thin animated bar reflecting a live 0.0-1.0 level - what backs
// both the "output level meter" and "microphone level meter" the spec
// asks for. Reads its value through a callback (rather than storing
// one directly) so it always shows PipeWireClient's *current* peak
// even though this widget itself is only rebuilt when the device list
// changes, not on every one of PipeWireClient's much more frequent
// level updates.
class LevelMeterWidget : public Widget
{
public:

    std::function<float()> getLevel;

    void Draw(UiWindow& window) override
    {
        const UiTheme& theme = window.Theme();
        float level = getLevel ? std::clamp(getLevel(), 0.0f, 1.0f) : 0.0f;

        window.FillRoundedRect(bounds, theme.surfaceActive, bounds.height / 2);

        Rect fill = bounds;
        fill.width = static_cast<int>(bounds.width * level);
        if (fill.width > 2)
            window.FillRoundedRect(fill, level > 0.9f ? theme.danger : theme.accent, bounds.height / 2);

        DrawChildren(window);
    }
};

// Drawn via a tiny widget so device rows can place one icon without a
// whole IconView type this toolkit doesn't otherwise need elsewhere.
class IconWidget : public Widget
{
public:
    std::string name;
    void Draw(UiWindow& window) override { window.DrawIcon(bounds, name); DrawChildren(window); }
};

std::string FormatPercent(float linear)
{
    return std::to_string(static_cast<int>(std::lround(linear * 100.0f))) + "%";
}

}

AudioWindow::AudioWindow() : m_settings("kohiko-audio.conf")
{
}

bool AudioWindow::Initialize()
{
    if (!m_instanceLock.AcquireOrListen("kohiko-audio"))
        return false; // another instance is already running - see AppInstanceLock's header comment

    m_instanceLock.SetRaiseHandler([this] { m_window.RaiseAndFocus(); });

    if (!m_window.Create("Kohiko Audio", "kohiko-audio", kWindowWidth, kWindowHeight))
        return false;

    m_notifications.Connect();

    m_pipewire.Connect(); // fine if this fails - RebuildPage() just shows an empty-state message, see its own comment
    m_pipewire.SetChangeHandler([this] { m_pipewireDirty = true; });

    m_currentPage = m_settings.GetString("last_page", "output") == "input" ? Page::Input : Page::Output;

    RebuildChrome();
    RebuildPage();

    m_window.WatchFd(m_instanceLock.Fd(), [this] { m_instanceLock.Dispatch(); });

    if (m_pipewire.Available())
    {
        m_window.WatchFd(m_pipewire.Fd(), [this] { m_pipewire.Iterate(); });
        m_pipewire.StartMetering();
    }

    // Level meters need to keep redrawing on their own schedule (the
    // underlying peak values change far more often than anything a
    // user does), and any pending PipeWireClient change is applied
    // here rather than directly from its callback - deferred, and
    // gated on !IsInteracting(), so a rebuild can never yank the
    // widget tree out from under an in-progress volume-slider drag
    // (see UiWindow::IsInteracting()'s own comment).
    m_window.SetInterval(66, [this]
    {
        if (m_pipewireDirty && !m_window.IsInteracting())
        {
            m_pipewireDirty = false;
            RebuildPage();
        }
        m_window.RequestRedraw();
    });

    return true;
}

void AudioWindow::Run()
{
    m_window.Run();
}

void AudioWindow::RebuildChrome()
{
    auto root = std::make_unique<Widget>();

    auto sidebar = std::make_unique<Sidebar>();
    sidebar->SetItems({
        { "Output", "audio-card" },
        { "Input", "audio-input-microphone" },
    });
    sidebar->SetSelected(m_currentPage == Page::Output ? 0 : 1);
    sidebar->Layout({ kMargin, kMargin, kSidebarWidth, kWindowHeight - kMargin * 2 });
    sidebar->onSelect = [this](int index)
    {
        m_currentPage = index == 0 ? Page::Output : Page::Input;
        m_settings.SetString("last_page", m_currentPage == Page::Output ? "output" : "input");
        m_settings.Save();
        m_headerLabel->text = m_currentPage == Page::Output ? "Output Devices" : "Input Devices";
        RebuildPage();
        m_window.RequestRedraw();
    };
    m_sidebar = static_cast<Sidebar*>(root->AddChild(std::move(sidebar)));

    auto header = std::make_unique<Label>();
    header->text = m_currentPage == Page::Output ? "Output Devices" : "Input Devices";
    header->bounds = { kSidebarWidth + kMargin * 2, kMargin, kWindowWidth - kSidebarWidth - kMargin * 3, 28 };
    m_headerLabel = static_cast<Label*>(root->AddChild(std::move(header)));

    auto scrollView = std::make_unique<ScrollView>();
    scrollView->bounds = {
        kSidebarWidth + kMargin * 2, kMargin + 40,
        kWindowWidth - kSidebarWidth - kMargin * 3, kWindowHeight - kMargin * 2 - 40
    };
    m_scrollView = static_cast<ScrollView*>(root->AddChild(std::move(scrollView)));

    m_window.SetRoot(std::move(root));
}

std::unique_ptr<Widget> AudioWindow::BuildMeterWidget(bool isSource)
{
    auto container = std::make_unique<Widget>();

    auto label = std::make_unique<Label>();
    label->text = isSource ? "Microphone level" : "Output level";
    label->color = UiTheme::Default().muted;
    label->bounds = { 0, 0, 200, 18 };
    container->AddChild(std::move(label));

    auto meter = std::make_unique<LevelMeterWidget>();
    meter->bounds = { 0, 22, m_scrollView->bounds.width, 8 };
    PipeWireClient* pw = &m_pipewire;
    meter->getLevel = isSource
        ? std::function<float()>([pw] { return pw->InputPeakLevel(); })
        : std::function<float()>([pw] { return pw->OutputPeakLevel(); });
    container->AddChild(std::move(meter));

    container->bounds = { 0, 0, m_scrollView->bounds.width, 38 };
    return container;
}

void AudioWindow::RebuildPage()
{
    bool wantSource = (m_currentPage == Page::Input);
    int contentWidth = m_scrollView->bounds.width;

    auto content = std::make_unique<Widget>();
    int y = 0;

    auto meter = BuildMeterWidget(wantSource);
    meter->bounds.y = y;
    y += meter->bounds.height + 12;
    content->AddChild(std::move(meter));

    auto separator = std::make_unique<Separator>();
    separator->bounds = { 0, y, contentWidth, 1 };
    y += 13;
    content->AddChild(std::move(separator));

    for (const AudioNode& node : m_pipewire.Nodes())
    {
        if (node.isSource != wantSource)
            continue;

        const int rowHeight = 78;
        std::uint32_t nodeId = node.id;

        auto row = std::make_unique<ClickableContainer>();
        row->bounds = { 0, y, contentWidth, rowHeight };
        row->selected = node.isDefault;
        row->onClick = [this, nodeId] { m_pipewire.SetDefaultNode(nodeId); };

        auto iconWidget = std::make_unique<IconWidget>();
        iconWidget->name = wantSource ? "audio-input-microphone" : "audio-card";
        iconWidget->bounds = { 14, y + (rowHeight - 32) / 2, 32, 32 };
        row->AddChild(std::move(iconWidget));

        auto title = std::make_unique<Label>();
        title->text = node.description.empty() ? node.name : node.description;
        title->bounds = { 60, y + 10, contentWidth - 260, 20 };
        row->AddChild(std::move(title));

        auto subtitle = std::make_unique<Label>();
        subtitle->text = (node.isDefault ? std::string("Default device \u2022 ") : std::string()) + FormatPercent(node.volume);
        subtitle->color = UiTheme::Default().muted;
        subtitle->bounds = { 60, y + 32, contentWidth - 260, 18 };
        row->AddChild(std::move(subtitle));

        auto slider = std::make_unique<Slider>();
        slider->value = node.volume;
        slider->maxValue = 1.5f;
        slider->bounds = { 60, y + 54, contentWidth - 260, 18 };
        slider->onChange = [this, nodeId](float v) { m_pipewire.SetVolume(nodeId, v); };
        row->AddChild(std::move(slider));

        auto muteToggle = std::make_unique<ToggleSwitch>();
        muteToggle->value = node.muted;
        muteToggle->bounds = { contentWidth - 180, y + (rowHeight - 26) / 2, 46, 26 };
        muteToggle->onChange = [this, nodeId](bool muted) { m_pipewire.SetMute(nodeId, muted); };
        row->AddChild(std::move(muteToggle));

        auto muteLabel = std::make_unique<Label>();
        muteLabel->text = "Mute";
        muteLabel->color = UiTheme::Default().muted;
        muteLabel->align = Label::Align::Right;
        muteLabel->bounds = { contentWidth - 130, y + (rowHeight - 18) / 2, 60, 18 };
        row->AddChild(std::move(muteLabel));

        y += rowHeight + 10;
        content->AddChild(std::move(row));
    }

    if (m_pipewire.Nodes().empty())
    {
        auto empty = std::make_unique<Label>();
        empty->text = m_pipewire.Available()
            ? (wantSource ? "No microphones found." : "No output devices found.")
            : "PipeWire is not available.";
        empty->color = UiTheme::Default().muted;
        empty->bounds = { 0, y, contentWidth, 24 };
        y += 24;
        content->AddChild(std::move(empty));
    }

    content->bounds = { 0, 0, contentWidth, y };
    m_scrollView->SetContent(std::move(content));
}

}
