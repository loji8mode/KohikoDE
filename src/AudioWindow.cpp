#include "AudioWindow.h"

#include <algorithm>
#include <cmath>

namespace Kohiko
{

namespace
{

constexpr int kWindowWidth = 760;
constexpr int kWindowHeight = 560;
constexpr int kSidebarWidth = 200;
constexpr int kMargin = 24;
constexpr int kCardGap = 16;

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

// A text label whose content is computed fresh every Draw() rather
// than fixed at construction - the live "42%" peak readout next to
// each meter needs to track PipeWireClient's current value on the
// same schedule LevelMeterWidget does, without forcing a full page
// rebuild (and the dragged-slider hazard that would come with one -
// see UiWindow::IsInteracting()) on every single meter tick.
class LiveValueLabel : public Widget
{
public:

    std::function<std::string()> getText;
    Label::Align align = Label::Align::Left;
    std::uint32_t color = 0;

    void Draw(UiWindow& window) override
    {
        std::string text = getText ? getText() : std::string();
        window.DrawTextClipped(bounds, text, color != 0 ? color : window.Theme().foreground, align);
        DrawChildren(window);
    }
};

std::string FormatPercent(float linear)
{
    return std::to_string(static_cast<int>(std::lround(linear * 100.0f))) + "%";
}

// How many columns a device grid should use for `width` px of
// available content width and `itemCount` cards to place in it.
// Capped at 3: each card is control-dense (icon, name, badge,
// percentage, slider, mute switch), so beyond three columns a card
// stops having room for any of that comfortably - a very wide
// monitor is better served by *wider, more generous* cards than by
// ever-narrower ones. Never uses more columns than there are cards to
// fill them with, so a single device never ends up as a lone
// half-width card floating next to empty space.
int ComputeColumns(int width, int itemCount)
{
    int columns = 1;
    if (width >= 620) columns = 2;
    if (width >= 1000) columns = 3;
    return std::max(1, std::min(columns, std::max(1, itemCount)));
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

    m_window.SetResizeHandler([this](int, int) { RebuildChrome(); });

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
    sidebar->Layout({ kMargin, kMargin, kSidebarWidth, m_window.Height() - kMargin * 2 });
    sidebar->onSelect = [this](int index)
    {
        m_currentPage = index == 0 ? Page::Output : Page::Input;
        m_settings.SetString("last_page", m_currentPage == Page::Output ? "output" : "input");
        m_settings.Save();
        RebuildPage();
        m_window.RequestRedraw();
    };
    m_sidebar = static_cast<Sidebar*>(root->AddChild(std::move(sidebar)));

    auto scrollView = std::make_unique<ScrollView>();
    scrollView->bounds = {
        kSidebarWidth + kMargin * 2, kMargin,
        m_window.Width() - kSidebarWidth - kMargin * 3, m_window.Height() - kMargin * 2
    };
    m_scrollView = static_cast<ScrollView*>(root->AddChild(std::move(scrollView)));

    m_window.SetRoot(std::move(root));

    RebuildPage();
}

std::unique_ptr<Widget> AudioWindow::BuildMeterCard(bool isSource, int width)
{
    auto card = std::make_unique<ClickableContainer>();
    card->bounds = { 0, 0, width, 76 };

    auto title = std::make_unique<Label>();
    title->text = isSource ? "Microphone Level" : "Output Level";
    title->bounds = { 20, 14, width - 100, 20 };
    card->AddChild(std::move(title));

    PipeWireClient* pw = &m_pipewire;

    auto readout = std::make_unique<LiveValueLabel>();
    readout->align = Label::Align::Right;
    readout->color = UiTheme::Default().muted;
    readout->bounds = { width - 80, 14, 60, 20 };
    readout->getText = isSource
        ? std::function<std::string()>([pw] { return FormatPercent(pw->InputPeakLevel()); })
        : std::function<std::string()>([pw] { return FormatPercent(pw->OutputPeakLevel()); });
    card->AddChild(std::move(readout));

    auto meter = std::make_unique<LevelMeterWidget>();
    meter->bounds = { 20, 44, width - 40, 12 };
    meter->getLevel = isSource
        ? std::function<float()>([pw] { return pw->InputPeakLevel(); })
        : std::function<float()>([pw] { return pw->OutputPeakLevel(); });
    card->AddChild(std::move(meter));

    return card;
}

std::unique_ptr<Widget> AudioWindow::BuildDeviceCard(const AudioNode& node, int width)
{
    const int height = 112;
    std::uint32_t nodeId = node.id;

    auto card = std::make_unique<ClickableContainer>();
    card->bounds = { 0, 0, width, height };
    card->selected = node.isDefault;
    card->onClick = [this, nodeId] { m_pipewire.SetDefaultNode(nodeId); };

    auto icon = std::make_unique<IconView>();
    icon->name = node.isSource ? "audio-input-microphone" : "audio-card";
    icon->bounds = { 16, 16, 30, 30 };
    card->AddChild(std::move(icon));

    int badgeWidth = node.isDefault ? Badge::MeasureWidth(m_window, "Default") : 0;

    auto title = std::make_unique<Label>();
    title->text = node.description.empty() ? node.name : node.description;
    title->bounds = { 58, 16, width - 58 - badgeWidth - 28, 24 };
    card->AddChild(std::move(title));

    if (node.isDefault)
    {
        auto badge = std::make_unique<Badge>();
        badge->text = "Default";
        badge->tone = Badge::Tone::Accent;
        badge->bounds = { width - 16 - badgeWidth, 18, badgeWidth, 22 };
        card->AddChild(std::move(badge));
    }

    PipeWireClient* pw = &m_pipewire;

    auto percent = std::make_unique<LiveValueLabel>();
    percent->color = UiTheme::Default().muted;
    percent->bounds = { 16, 58, 46, 34 };
    percent->getText = [pw, nodeId]
    {
        for (auto& n : pw->Nodes())
            if (n.id == nodeId)
                return FormatPercent(n.volume);
        return std::string();
    };
    card->AddChild(std::move(percent));

    auto slider = std::make_unique<Slider>();
    slider->value = node.volume;
    slider->maxValue = 1.5f;
    slider->bounds = { 66, 66, width - 66 - 74, 18 };
    slider->onChange = [this, nodeId](float v) { m_pipewire.SetVolume(nodeId, v); };
    card->AddChild(std::move(slider));

    auto muteToggle = std::make_unique<ToggleSwitch>();
    muteToggle->value = node.muted;
    muteToggle->bounds = { width - 62, 60, 46, 26 };
    muteToggle->onChange = [this, nodeId](bool muted) { m_pipewire.SetMute(nodeId, muted); };
    card->AddChild(std::move(muteToggle));

    return card;
}

std::unique_ptr<Widget> AudioWindow::BuildOptionsCard(int width)
{
    auto card = std::make_unique<ClickableContainer>();
    card->bounds = { 0, 0, width, 64 };

    auto label = std::make_unique<Label>();
    label->text = "Notify when the default device changes";
    label->bounds = { 20, 21, width - 90, 22 };
    card->AddChild(std::move(label));

    auto toggle = std::make_unique<ToggleSwitch>();
    toggle->value = m_settings.GetBool("notify_default_change", false);
    toggle->bounds = { width - 66, 19, 46, 26 };
    toggle->onChange = [this](bool enabled)
    {
        m_settings.SetBool("notify_default_change", enabled);
        m_settings.Save();
    };
    card->AddChild(std::move(toggle));

    return card;
}

void AudioWindow::MaybeNotifyDefaultChanged()
{
    if (!m_settings.GetBool("notify_default_change", false) || !m_notifications.Available())
        return;

    const AudioNode* defaultOutput = nullptr;
    for (auto& node : m_pipewire.Nodes())
        if (!node.isSource && node.isDefault) { defaultOutput = &node; break; }

    std::string currentName = defaultOutput ? defaultOutput->description : std::string();

    if (m_haveSeenDefaultOutput && currentName != m_lastDefaultOutputName && !currentName.empty())
    {
        m_notifications.Notify("Kohiko Audio", "audio-card", "Default output changed", currentName);
    }

    m_lastDefaultOutputName = currentName;
    m_haveSeenDefaultOutput = true;
}

void AudioWindow::RebuildPage()
{
    bool wantSource = (m_currentPage == Page::Input);
    int contentWidth = m_scrollView->bounds.width;

    auto content = std::make_unique<Widget>();
    int y = 0;

    std::vector<const AudioNode*> devices;
    for (const AudioNode& node : m_pipewire.Nodes())
        if (node.isSource == wantSource)
            devices.push_back(&node);

    const AudioNode* defaultDevice = nullptr;
    for (auto* d : devices)
        if (d->isDefault) { defaultDevice = d; break; }

    std::string headerTitle = wantSource ? "Input Devices" : "Output Devices";
    std::string headerSubtitle;
    if (!m_pipewire.Available())
        headerSubtitle = "PipeWire is not available.";
    else if (defaultDevice)
        headerSubtitle = "Default: " + (defaultDevice->description.empty() ? defaultDevice->name : defaultDevice->description)
            + "  \u2022  " + FormatPercent(defaultDevice->volume) + (defaultDevice->muted ? " (muted)" : "");
    else
        headerSubtitle = devices.empty() ? "No devices found." : "No default device selected.";

    auto header = MakePageHeader({ 0, y, contentWidth, 52 }, headerTitle, headerSubtitle);
    y += 52 + kCardGap;
    content->AddChild(std::move(header));

    auto meterCard = BuildMeterCard(wantSource, contentWidth);
    y += meterCard->bounds.height + kCardGap;
    meterCard->bounds.y = y - meterCard->bounds.height - kCardGap;
    content->AddChild(std::move(meterCard));

    if (devices.empty())
    {
        auto empty = std::make_unique<Label>();
        empty->text = m_pipewire.Available()
            ? (wantSource ? "No microphones found." : "No output devices found.")
            : "Connect to PipeWire to manage devices.";
        empty->color = UiTheme::Default().muted;
        empty->bounds = { 0, y, contentWidth, 24 };
        y += 24 + kCardGap;
        content->AddChild(std::move(empty));
    }
    else
    {
        int columns = ComputeColumns(contentWidth, static_cast<int>(devices.size()));
        int cardWidth = columns > 1 ? (contentWidth - (columns - 1) * kCardGap) / columns : contentWidth;
        int cardHeight = 112;
        int gridTop = y;

        for (std::size_t i = 0; i < devices.size(); ++i)
        {
            int col = static_cast<int>(i) % columns;
            int row = static_cast<int>(i) / columns;

            auto card = BuildDeviceCard(*devices[i], cardWidth);
            card->bounds.x = col * (cardWidth + kCardGap);
            card->bounds.y = gridTop + row * (cardHeight + kCardGap);
            content->AddChild(std::move(card));
        }

        int rows = (static_cast<int>(devices.size()) + columns - 1) / columns;
        y = gridTop + rows * (cardHeight + kCardGap);
    }

    auto optionsCard = BuildOptionsCard(contentWidth);
    y += optionsCard->bounds.height;
    optionsCard->bounds.y = y - optionsCard->bounds.height;
    content->AddChild(std::move(optionsCard));

    content->bounds = { 0, 0, contentWidth, y };
    m_scrollView->SetContent(std::move(content));

    MaybeNotifyDefaultChanged();
}

}
