#include "AudioWindow.h"
#include "UiListRow.h"

#include <algorithm>
#include <cmath>

namespace Kohiko
{

namespace
{

constexpr int kWindowWidth = 760;
constexpr int kWindowHeight = 560;
constexpr int kMargin = 24;
constexpr int kCardGap = 16;

// Past this, content stops growing wider and just gets centered with
// extra breathing room on either side - a fullscreen/ultra-wide
// window should feel spacious, not stretch every row into one
// enormous line (see the "no wasted space, but don't just stretch
// widgets either" usability requirement).
constexpr int kMaxContentWidth = 900;

// Below this, a device row switches from one line (icon, name, badge,
// percent, slider, mute button all side by side) to two (name/badge
// on top, slider/percent/mute below) - see BuildDeviceRow(). Chosen
// so the single-line shape never has to squeeze a slider down to
// nothing to make room for everything else.
constexpr int kNarrowBreakpoint = 560;

constexpr int kRowHeightWide = 72;
constexpr int kRowHeightNarrow = 104;

// A thin animated bar reflecting a live 0.0-1.0 level - what backs
// both the "output level meter" and "microphone level meter" on the
// Advanced Settings page. Reads its value through a callback (rather
// than storing one directly) so it always shows PipeWireClient's
// *current* peak even though this widget itself is only rebuilt when
// the device list changes, not on every one of PipeWireClient's much
// more frequent level updates.
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
// than fixed at construction - the live "42%" peak/volume readout
// needs to track PipeWireClient's current value on the same schedule
// LevelMeterWidget does, without forcing a full page rebuild (and the
// dragged-slider hazard that would come with one - see
// UiWindow::IsInteracting()) on every single meter tick.
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

    int available = std::max(1, m_window.Width() - kMargin * 2);
    int contentWidth = std::min(available, kMaxContentWidth);
    int marginX = kMargin + (available - contentWidth) / 2;

    auto scrollView = std::make_unique<ScrollView>();
    scrollView->bounds = { marginX, kMargin, contentWidth, std::max(1, m_window.Height() - kMargin * 2) };
    m_scrollView = static_cast<ScrollView*>(root->AddChild(std::move(scrollView)));

    m_window.SetRoot(std::move(root));

    RebuildPage();
}

std::unique_ptr<Widget> AudioWindow::BuildMeterCard(bool isSource, const Rect& cardBounds)
{
    auto card = std::make_unique<Card>();
    card->bounds = cardBounds;

    auto title = std::make_unique<Label>();
    title->text = isSource ? "Microphone Level" : "Output Level";
    title->bounds = { cardBounds.x + 20, cardBounds.y + 14, cardBounds.width - 100, 20 };
    card->AddChild(std::move(title));

    PipeWireClient* pw = &m_pipewire;

    auto readout = std::make_unique<LiveValueLabel>();
    readout->align = Label::Align::Right;
    readout->color = UiTheme::Default().muted;
    readout->bounds = { cardBounds.Right() - 80, cardBounds.y + 14, 60, 20 };
    readout->getText = isSource
        ? std::function<std::string()>([pw] { return FormatPercent(pw->InputPeakLevel()); })
        : std::function<std::string()>([pw] { return FormatPercent(pw->OutputPeakLevel()); });
    card->AddChild(std::move(readout));

    auto meter = std::make_unique<LevelMeterWidget>();
    meter->bounds = { cardBounds.x + 20, cardBounds.y + 44, cardBounds.width - 40, 12 };
    meter->getLevel = isSource
        ? std::function<float()>([pw] { return pw->InputPeakLevel(); })
        : std::function<float()>([pw] { return pw->OutputPeakLevel(); });
    card->AddChild(std::move(meter));

    return card;
}

std::unique_ptr<Widget> AudioWindow::BuildDeviceRow(const AudioNode& node, const Rect& rowBounds, bool narrow, bool showDivider)
{
    const int gap = 14;
    const int muteW = 78;
    const int percentW = 42;
    const int wideSliderW = 150;

    const int badgeW = node.isDefault ? Badge::MeasureWidth(m_window, "Default") : 0;

    auto row = std::make_unique<ListRow>();
    row->flat = true;
    row->showDivider = showDivider;
    row->iconName = node.isSource ? "audio-input-microphone" : "audio-card";
    row->title = node.description.empty() ? node.name : node.description;
    row->subtitle = (node.perChannelVolumes.size() >= 2) ? "Stereo" : "Mono";

    std::uint32_t nodeId = node.id;
    row->onClick = [this, nodeId] { m_pipewire.SetDefaultNode(nodeId); };

    PipeWireClient* pw = &m_pipewire;

    if (!narrow)
    {
        // Single line: icon | title/subtitle | [Default] percent slider mute
        int trailingWidth = (badgeW > 0 ? badgeW + gap : 0) + percentW + gap + wideSliderW + gap + muteW;
        row->Layout(rowBounds, nullptr, trailingWidth);

        int cursorRight = rowBounds.Right() - gap;

        Rect muteRect{ cursorRight - muteW, rowBounds.y + (rowBounds.height - 32) / 2, muteW, 32 };
        cursorRight = muteRect.x - gap;

        Rect sliderRect{ cursorRight - wideSliderW, rowBounds.y + (rowBounds.height - 18) / 2, wideSliderW, 18 };
        cursorRight = sliderRect.x - gap;

        Rect percentRect{ cursorRight - percentW, rowBounds.y, percentW, rowBounds.height };
        cursorRight = percentRect.x - gap;

        if (badgeW > 0)
        {
            Rect badgeRect{ cursorRight - badgeW, rowBounds.y + (rowBounds.height - 24) / 2, badgeW, 24 };
            auto badge = std::make_unique<Badge>();
            badge->text = "Default";
            badge->tone = Badge::Tone::Accent;
            badge->bounds = badgeRect;
            row->AddChild(std::move(badge));
        }

        auto percent = std::make_unique<LiveValueLabel>();
        percent->align = Label::Align::Right;
        percent->color = UiTheme::Default().muted;
        percent->bounds = percentRect;
        percent->getText = [pw, nodeId]
        {
            for (auto& n : pw->Nodes())
                if (n.id == nodeId)
                    return FormatPercent(n.volume);
            return std::string();
        };
        row->AddChild(std::move(percent));

        auto slider = std::make_unique<Slider>();
        slider->value = node.volume;
        slider->maxValue = 1.5f;
        slider->bounds = sliderRect;
        slider->onChange = [this, nodeId](float v) { m_pipewire.SetVolume(nodeId, v); };
        row->AddChild(std::move(slider));

        auto mute = std::make_unique<Button>();
        mute->label = node.muted ? "Unmute" : "Mute";
        mute->tone = node.muted ? Button::Tone::Danger : Button::Tone::Neutral;
        mute->bounds = muteRect;
        mute->onClick = [this, nodeId, muted = node.muted] { m_pipewire.SetMute(nodeId, !muted); };
        row->AddChild(std::move(mute));
    }
    else
    {
        // Two lines: [icon title/subtitle .......... Default]
        //            [ percent  ========slider========  mute ]
        const int topHeight = 52;
        Rect topRect{ rowBounds.x, rowBounds.y, rowBounds.width, topHeight };
        row->Layout(topRect, nullptr, badgeW > 0 ? badgeW : 0);
        row->bounds.height = rowBounds.height; // extend so the divider/selection chrome covers both lines

        if (badgeW > 0)
        {
            Rect badgeRect{ topRect.Right() - gap - badgeW, topRect.y + (topHeight - 24) / 2, badgeW, 24 };
            auto badge = std::make_unique<Badge>();
            badge->text = "Default";
            badge->tone = Badge::Tone::Accent;
            badge->bounds = badgeRect;
            row->AddChild(std::move(badge));
        }

        Rect bottomRect{ rowBounds.x, rowBounds.y + topHeight, rowBounds.width, rowBounds.height - topHeight };
        int leftX = bottomRect.x + gap;
        int rightX = bottomRect.Right() - gap;

        Rect percentRect{ leftX, bottomRect.y, percentW, bottomRect.height };
        Rect muteRect{ rightX - muteW, bottomRect.y + (bottomRect.height - 32) / 2, muteW, 32 };
        int sliderLeft = percentRect.Right() + gap;
        int sliderRight = muteRect.x - gap;
        Rect sliderRect{ sliderLeft, bottomRect.y + (bottomRect.height - 18) / 2, std::max(40, sliderRight - sliderLeft), 18 };

        auto percent = std::make_unique<LiveValueLabel>();
        percent->color = UiTheme::Default().muted;
        percent->bounds = percentRect;
        percent->getText = [pw, nodeId]
        {
            for (auto& n : pw->Nodes())
                if (n.id == nodeId)
                    return FormatPercent(n.volume);
            return std::string();
        };
        row->AddChild(std::move(percent));

        auto slider = std::make_unique<Slider>();
        slider->value = node.volume;
        slider->maxValue = 1.5f;
        slider->bounds = sliderRect;
        slider->onChange = [this, nodeId](float v) { m_pipewire.SetVolume(nodeId, v); };
        row->AddChild(std::move(slider));

        auto mute = std::make_unique<Button>();
        mute->label = node.muted ? "Unmute" : "Mute";
        mute->tone = node.muted ? Button::Tone::Danger : Button::Tone::Neutral;
        mute->bounds = muteRect;
        mute->onClick = [this, nodeId, muted = node.muted] { m_pipewire.SetMute(nodeId, !muted); };
        row->AddChild(std::move(mute));
    }

    return row;
}

int AudioWindow::AppendDeviceSection(Widget& content, int y, int contentWidth, const std::string& headerText, bool wantSource)
{
    std::vector<const AudioNode*> devices;
    for (const AudioNode& node : m_pipewire.Nodes())
        if (node.isSource == wantSource)
            devices.push_back(&node);

    auto header = std::make_unique<SectionHeader>();
    header->text = headerText;
    header->bounds = { 0, y, contentWidth, 18 };
    content.AddChild(std::move(header));
    y += 18 + 10;

    if (devices.empty())
    {
        const int emptyHeight = 64;
        auto card = std::make_unique<Card>();
        card->bounds = { 0, y, contentWidth, emptyHeight };

        auto empty = std::make_unique<Label>();
        empty->text = !m_pipewire.Available()
            ? "Connect to PipeWire to manage devices."
            : (wantSource ? "No microphones found." : "No output devices found.");
        empty->color = UiTheme::Default().muted;
        empty->bounds = { 20, y, contentWidth - 40, emptyHeight };
        card->AddChild(std::move(empty));

        content.AddChild(std::move(card));
        y += emptyHeight + kCardGap;
        return y;
    }

    bool narrow = contentWidth < kNarrowBreakpoint;
    const int rowHeight = narrow ? kRowHeightNarrow : kRowHeightWide;
    const int cardHeight = rowHeight * static_cast<int>(devices.size());

    auto card = std::make_unique<Card>();
    card->bounds = { 0, y, contentWidth, cardHeight };

    for (std::size_t i = 0; i < devices.size(); ++i)
    {
        Rect rowBounds{ 0, y + static_cast<int>(i) * rowHeight, contentWidth, rowHeight };
        bool showDivider = (i + 1 != devices.size());
        card->AddChild(BuildDeviceRow(*devices[i], rowBounds, narrow, showDivider));
    }

    content.AddChild(std::move(card));
    y += cardHeight + kCardGap;
    return y;
}

int AudioWindow::RebuildMainPage(Widget& content, int contentWidth)
{
    int y = 0;

    y = AppendDeviceSection(content, y, contentWidth, "OUTPUT DEVICES", false);
    y += 8;
    y = AppendDeviceSection(content, y, contentWidth, "INPUT DEVICES", true);
    y += 8;

    // "Advanced Settings" link - where the level meters and the
    // "notify on default change" option (neither part of the mockup)
    // still live, rather than being dropped - see RebuildAdvancedPage().
    const int advancedHeight = 56;
    auto advancedCard = std::make_unique<Card>();
    advancedCard->bounds = { 0, y, contentWidth, advancedHeight };

    auto advancedRow = std::make_unique<ListRow>();
    advancedRow->flat = true;
    advancedRow->showDivider = false;
    advancedRow->title = "Advanced Settings";
    advancedRow->onClick = [this] { m_page = Page::Advanced; RebuildPage(); m_window.RequestRedraw(); };

    auto chevron = std::make_unique<Label>();
    chevron->text = "\u203a";
    chevron->align = Label::Align::Center;
    chevron->color = UiTheme::Default().muted;
    chevron->bounds = { 0, 0, 20, 20 };
    advancedRow->Layout({ 0, y, contentWidth, advancedHeight }, std::move(chevron), 20);

    advancedCard->AddChild(std::move(advancedRow));
    content.AddChild(std::move(advancedCard));
    y += advancedHeight;

    return y;
}

int AudioWindow::RebuildAdvancedPage(Widget& content, int contentWidth)
{
    int y = 0;

    auto backRow = std::make_unique<ListRow>();
    backRow->flat = true;
    backRow->showDivider = true;
    backRow->title = "\u2039  Back to Kohiko Audio";
    backRow->onClick = [this] { m_page = Page::Main; RebuildPage(); m_window.RequestRedraw(); };
    backRow->Layout({ 0, y, contentWidth, 44 });
    content.AddChild(std::move(backRow));
    y += 44 + kCardGap;

    auto meterHeader = std::make_unique<SectionHeader>();
    meterHeader->text = "LEVEL METERS";
    meterHeader->bounds = { 0, y, contentWidth, 18 };
    content.AddChild(std::move(meterHeader));
    y += 18 + 10;

    auto outputMeter = BuildMeterCard(false, { 0, y, contentWidth, 76 });
    y += 76 + kCardGap;
    content.AddChild(std::move(outputMeter));

    auto inputMeter = BuildMeterCard(true, { 0, y, contentWidth, 76 });
    y += 76 + kCardGap;
    content.AddChild(std::move(inputMeter));

    y += 8;

    auto notifyHeader = std::make_unique<SectionHeader>();
    notifyHeader->text = "NOTIFICATIONS";
    notifyHeader->bounds = { 0, y, contentWidth, 18 };
    content.AddChild(std::move(notifyHeader));
    y += 18 + 10;

    const int optionsHeight = 64;
    auto optionsCard = std::make_unique<Card>();
    optionsCard->bounds = { 0, y, contentWidth, optionsHeight };

    auto toggle = std::make_unique<ToggleSwitch>();
    toggle->value = m_settings.GetBool("notify_default_change", false);
    toggle->bounds = { 0, 0, 0, 26 };

    Rect settingsRowBounds{ optionsCard->bounds.x + 20, optionsCard->bounds.y, optionsCard->bounds.width - 40, optionsHeight };
    Widget* toggleRaw = MakeSettingsRow(*optionsCard, settingsRowBounds, "Notify when the default device changes", std::move(toggle), 46);
    static_cast<ToggleSwitch*>(toggleRaw)->onChange = [this](bool enabled)
    {
        m_settings.SetBool("notify_default_change", enabled);
        m_settings.Save();
    };

    content.AddChild(std::move(optionsCard));
    y += optionsHeight;

    return y;
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
    m_window.ClearFocus();

    int contentWidth = m_scrollView->bounds.width;
    auto content = std::make_unique<Widget>();

    int y = (m_page == Page::Advanced)
        ? RebuildAdvancedPage(*content, contentWidth)
        : RebuildMainPage(*content, contentWidth);

    content->bounds = { 0, 0, contentWidth, y };
    m_scrollView->SetContent(std::move(content));

    MaybeNotifyDefaultChanged();
}

}
