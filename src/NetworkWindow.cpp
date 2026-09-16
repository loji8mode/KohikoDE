#include "NetworkWindow.h"
#include "UiListRow.h"

#include <X11/Xatom.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>

namespace Kohiko
{

std::string FilterPastedPasswordText(const std::string& raw)
{
    std::string filtered;
    filtered.reserve(raw.size());

    for (unsigned char c : raw)
    {
        if (c < 0x20 || c == 0x7f)
            continue; // every C0 control character, newline/tab included, plus DEL

        filtered.push_back(static_cast<char>(c));
    }

    return filtered;
}

namespace
{

constexpr int kWindowWidth = 820;
constexpr int kWindowHeight = 580;
constexpr int kMargin = 24;
constexpr int kCardGap = 16;

// Same "cap and center past a comfortable width" treatment as
// AudioWindow - see kMaxContentWidth's comment there.
constexpr int kMaxContentWidth = 960;

// Below this, the network list and its details panel stack into one
// column instead of sitting side by side.
constexpr int kSplitBreakpoint = 720;

// Below this, the toolbar (Wi-Fi toggle / VPN status / refresh
// button) wraps onto a second line rather than clipping.
constexpr int kToolbarNarrowBreakpoint = 560;

std::string SignalQuality(std::uint8_t strength)
{
    if (strength >= 80) return "Excellent signal";
    if (strength >= 55) return "Good signal";
    if (strength >= 30) return "Fair signal";
    return "Weak signal";
}

std::string WiFiIconFor(std::uint8_t strength)
{
    const char* level = strength >= 80 ? "excellent" : strength >= 55 ? "good" : strength >= 30 ? "ok" : "weak";
    return std::string("network-wireless-signal-") + level;
}

std::string ToLower(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

// A small caption-over-value field, e.g. "GATEWAY" / "192.168.1.1" -
// what BuildEthernetCard() arranges into a 1-, 2-, or 4-column grid
// depending on how much width is available, so a wide window shows
// Address/Gateway/DNS/MAC as a proper labeled grid instead of the
// single cramped summary line a narrow one falls back to.
std::unique_ptr<Widget> BuildInfoField(const Rect& rect, const std::string& caption, const std::string& value)
{
    auto field = std::make_unique<Widget>();
    field->bounds = rect;

    auto captionLabel = std::make_unique<Label>();
    captionLabel->text = caption;
    captionLabel->color = UiTheme::Default().muted;
    captionLabel->bounds = { rect.x, rect.y, rect.width, 16 };
    field->AddChild(std::move(captionLabel));

    auto valueLabel = std::make_unique<Label>();
    valueLabel->text = value.empty() ? "\u2014" : value;
    valueLabel->bounds = { rect.x, rect.y + 18, rect.width, 20 };
    field->AddChild(std::move(valueLabel));

    return field;
}

// A small self-contained modal password prompt - not part of the
// shared UI toolkit (see NetworkWindow.h's comment on
// PromptAndConnect()), since connecting to a secured Wi-Fi network is
// the one place across all three new apps that needs a *modal*
// text prompt (kohiko-network's live search field is a different,
// non-modal use of the toolkit's own TextField). Structured the same
// way PopupMenu is: an override-redirect window, an exclusive grab, a
// small self-contained XNextEvent loop that returns once the user is
// done.
std::optional<std::string> PromptForPassword(
    Display* display, int screen, Visual* visual, Colormap colormap,
    Font& font, const UiTheme& theme, const std::string& ssid)
{
    const int width = 340;
    const int height = 132;

    int screenWidth = DisplayWidth(display, screen);
    int screenHeight = DisplayHeight(display, screen);

    XSetWindowAttributes attrs{};
    attrs.override_redirect = True;
    attrs.background_pixel = theme.surface;
    attrs.event_mask = ExposureMask | KeyPressMask | ButtonPressMask;

    Window win = XCreateWindow(
        display, RootWindow(display, screen),
        (screenWidth - width) / 2, (screenHeight - height) / 2, width, height, 0,
        DefaultDepth(display, screen), InputOutput, visual,
        CWOverrideRedirect | CWBackPixel | CWEventMask, &attrs
    );

    XMapRaised(display, win);
    XFlush(display);

    XGrabKeyboard(display, win, False, GrabModeAsync, GrabModeAsync, CurrentTime);
    XGrabPointer(display, win, False, ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None, CurrentTime);

    // Paste support (Ctrl+V from CLIPBOARD, middle-click from PRIMARY -
    // the two conventional X11 paste gestures) - added alongside the
    // "accepts pasted text" requirement in CHANGELOG.md's 0.20.4 entry.
    // Neither XGrabKeyboard/XGrabPointer above nor this window's own
    // event_mask needs to change for SelectionNotify specifically: X11
    // delivers it straight to whichever window issued the matching
    // XConvertSelection() call, the same way SelectionClear/
    // SelectionRequest target a specific window regardless of that
    // window's own selected event mask. Only UTF8_STRING is
    // requested - virtually universal on anything a password would
    // plausibly be copied from on a modern desktop (a password
    // manager, a browser, a terminal) - with no XA_STRING fallback
    // attempted if a selection owner doesn't support it.
    Atom clipboardAtom = XInternAtom(display, "CLIPBOARD", False);
    Atom utf8Atom = XInternAtom(display, "UTF8_STRING", False);
    Atom pasteTargetAtom = XInternAtom(display, "KOHIKO_NETWORK_PASSWORD_PASTE", False);

    GC gc = XCreateGC(display, win, 0, nullptr);
    XftDraw* xftDraw = XftDrawCreate(display, win, visual, colormap);

    std::string password;
    bool confirmed = false;
    bool done = false;

    Rect fieldRect{ 20, 62, width - 40, 32 };

    auto redraw = [&]()
    {
        XSetForeground(display, gc, theme.surface);
        XFillRectangle(display, win, gc, 0, 0, width, height);
        XSetForeground(display, gc, theme.border);
        XDrawRectangle(display, win, gc, 0, 0, width - 1, height - 1);

        TextColor fg(display, visual, colormap, theme.foreground);
        TextColor muted(display, visual, colormap, theme.muted);

        std::string title = "Password for \"" + ssid + "\"";
        font.DrawString(xftDraw, 20, 30, title, fg.Get());
        font.DrawString(xftDraw, 20, 50, "Press Enter to connect, Escape to cancel", muted.Get());

        XSetForeground(display, gc, theme.surfaceActive);
        XFillRectangle(display, win, gc, fieldRect.x, fieldRect.y, fieldRect.width, fieldRect.height);

        std::string masked(password.size(), '*');
        font.DrawString(xftDraw, fieldRect.x + 8, fieldRect.y + 22, masked, fg.Get());

        XFlush(display);
    };

    // Reads back whatever XConvertSelection() converted into
    // `pasteTargetAtom` on our own window, filters it the same way
    // typed input already is, appends it, and cleans the property up
    // - shared by both the Ctrl+V and middle-click paste gestures
    // below, since they differ only in which selection they request.
    auto handleSelectionNotify = [&](const XSelectionEvent& sel)
    {
        if (sel.property == None)
            return; // conversion failed, or the owner has nothing in this format - nothing to paste

        Atom actualType;
        int actualFormat;
        unsigned long itemCount, bytesAfter;
        unsigned char* data = nullptr;

        if (XGetWindowProperty(display, win, sel.property, 0, 65536, False, AnyPropertyType,
                &actualType, &actualFormat, &itemCount, &bytesAfter, &data) == Success && data)
        {
            if (actualFormat == 8)
                password += FilterPastedPasswordText(std::string(reinterpret_cast<char*>(data), itemCount));

            XFree(data);
        }

        XDeleteProperty(display, win, sel.property);
        redraw();
    };

    redraw();

    while (!done)
    {
        XEvent event;
        XNextEvent(display, &event);

        switch (event.type)
        {
            case Expose:
                redraw();
                break;

            case ButtonPress:
                if (event.xbutton.x < 0 || event.xbutton.x >= width || event.xbutton.y < 0 || event.xbutton.y >= height)
                    done = true; // clicked outside - same "outside == dismiss" convention as PopupMenu
                else if (event.xbutton.button == Button2)
                    XConvertSelection(display, XA_PRIMARY, utf8Atom, pasteTargetAtom, win, event.xbutton.time);
                break;

            case KeyPress:
            {
                char buffer[32];
                KeySym keysym;
                int len = XLookupString(&event.xkey, buffer, sizeof(buffer) - 1, &keysym, nullptr);

                if (keysym == XK_Return || keysym == XK_KP_Enter)
                {
                    confirmed = true;
                    done = true;
                }
                else if (keysym == XK_Escape)
                {
                    done = true;
                }
                else if (keysym == XK_BackSpace)
                {
                    if (!password.empty())
                        password.pop_back();
                    redraw();
                }
                else if ((keysym == XK_v || keysym == XK_V) && (event.xkey.state & ControlMask))
                {
                    XConvertSelection(display, clipboardAtom, utf8Atom, pasteTargetAtom, win, event.xkey.time);
                }
                else if (len > 0 && static_cast<unsigned char>(buffer[0]) >= 0x20)
                {
                    password.append(buffer, len);
                    redraw();
                }
                break;
            }

            case SelectionNotify:
                handleSelectionNotify(event.xselection);
                break;

            default:
                break;
        }
    }

    XUngrabKeyboard(display, CurrentTime);
    XUngrabPointer(display, CurrentTime);
    XftDrawDestroy(xftDraw);
    XFreeGC(display, gc);
    XDestroyWindow(display, win);
    XFlush(display);

    if (!confirmed)
        return std::nullopt;

    return password;
}

}

NetworkWindow::NetworkWindow() : m_settings("kohiko-network.conf")
{
}

bool NetworkWindow::Initialize()
{
    if (!m_instanceLock.AcquireOrListen("kohiko-network"))
        return false;

    m_instanceLock.SetRaiseHandler([this] { m_window.RaiseAndFocus(); });

    if (!m_window.Create("Kohiko Network", "kohiko-network", kWindowWidth, kWindowHeight))
        return false;

    m_notifications.Connect();

    m_networkManager.Connect(); // fine if this fails - RebuildPage() just shows an unavailable message, see its own comment
    m_networkManager.SetChangeHandler([this] { m_networkDirty = true; });

    RebuildChrome();

    m_window.SetResizeHandler([this](int, int) { RebuildChrome(); });

    m_window.WatchFd(m_instanceLock.Fd(), [this] { m_instanceLock.Dispatch(); });

    if (m_networkManager.Available())
    {
        m_window.WatchFd(m_networkManager.Fd(), [this] { m_networkManager.Dispatch(); });
        m_networkManager.RequestWiFiScan();
    }

    // Same throttled-rebuild shape as AudioWindow's interval, guarded
    // by !IsInteracting() so a saved-connection change landing mid-
    // search-typing (or mid Ethernet/VPN toggle drag) doesn't yank
    // the tree out from under it - see IsInteracting()'s own comment.
    m_window.SetInterval(300, [this]
    {
        if (m_networkDirty && !m_window.IsInteracting())
        {
            m_networkDirty = false;
            RebuildPage();
            m_window.RequestRedraw();
        }
    });

    return true;
}

void NetworkWindow::Run()
{
    m_window.Run();
}

void NetworkWindow::RebuildChrome()
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

int NetworkWindow::AppendToolbar(Widget& content, int y, int contentWidth)
{
    bool available = m_networkManager.Available();
    bool wirelessEnabled = available && m_networkManager.WirelessEnabled();

    bool vpnActive = false;
    for (auto& conn : m_networkManager.SavedConnections())
        if (conn.isVpn && conn.isActive) { vpnActive = true; break; }

    bool narrow = contentWidth < kToolbarNarrowBreakpoint;
    const int rowHeight = 40;

    Rect row1{ 0, y, contentWidth, rowHeight };

    auto wifiLabel = std::make_unique<Label>();
    wifiLabel->text = "Wi-Fi";
    wifiLabel->bounds = { row1.x, row1.y, 46, rowHeight };
    content.AddChild(std::move(wifiLabel));

    auto wifiToggle = std::make_unique<ToggleSwitch>();
    wifiToggle->value = wirelessEnabled;
    wifiToggle->enabled = available;
    wifiToggle->bounds = { row1.x + 50, row1.y + (rowHeight - 26) / 2, 46, 26 };
    wifiToggle->onChange = [this](bool enabled) { m_networkManager.SetWirelessEnabled(enabled); };
    content.AddChild(std::move(wifiToggle));

    int cursorX = row1.x + 50 + 46 + 20;

    if (!narrow)
    {
        auto divider = std::make_unique<Separator>();
        divider->bounds = { cursorX, row1.y + 6, 1, rowHeight - 12 };
        content.AddChild(std::move(divider));
        cursorX += 20;

        auto shieldIcon = std::make_unique<IconView>();
        shieldIcon->name = "network-vpn";
        shieldIcon->bounds = { cursorX, row1.y + (rowHeight - 20) / 2, 20, 20 };
        content.AddChild(std::move(shieldIcon));
        cursorX += 20 + 8;

        auto vpnLabel = std::make_unique<Label>();
        vpnLabel->text = "VPN";
        vpnLabel->bounds = { cursorX, row1.y, 34, rowHeight };
        content.AddChild(std::move(vpnLabel));
        cursorX += 34 + 8;

        auto vpnState = std::make_unique<Label>();
        vpnState->text = vpnActive ? "On" : "Off";
        vpnState->color = vpnActive ? UiTheme::Default().accent : UiTheme::Default().muted;
        vpnState->bounds = { cursorX, row1.y, 40, rowHeight };
        content.AddChild(std::move(vpnState));
    }

    auto refresh = std::make_unique<IconButton>();
    refresh->iconName = "view-refresh";
    refresh->enabled = available;
    refresh->bounds = { row1.Right() - rowHeight, row1.y, rowHeight, rowHeight };
    refresh->onClick = [this]
    {
        m_networkManager.RequestWiFiScan();
        m_networkManager.Refresh();
        RebuildPage();
        m_window.RequestRedraw();
    };
    content.AddChild(std::move(refresh));

    y += rowHeight;

    if (narrow)
    {
        const int row2Height = 26;
        Rect row2{ 0, y, contentWidth, row2Height };

        auto shieldIcon = std::make_unique<IconView>();
        shieldIcon->name = "network-vpn";
        shieldIcon->bounds = { row2.x, row2.y + (row2Height - 16) / 2, 16, 16 };
        content.AddChild(std::move(shieldIcon));

        auto vpnRow = std::make_unique<Label>();
        vpnRow->text = std::string("VPN \u2022 ") + (vpnActive ? "On" : "Off");
        vpnRow->color = vpnActive ? UiTheme::Default().accent : UiTheme::Default().muted;
        vpnRow->bounds = { row2.x + 16 + 8, row2.y, contentWidth - 24, row2Height };
        content.AddChild(std::move(vpnRow));

        y += row2Height;
    }

    y += kCardGap;
    return y;
}

std::unique_ptr<Widget> NetworkWindow::BuildNetworkRow(const WiFiAccessPoint& ap, const std::string& devicePath, const Rect& rowBounds, bool showDivider, bool showPercent)
{
    (void)devicePath; // kept in the signature for symmetry with BuildDeviceRow-style builders; connecting happens from the details panel, not the row itself

    auto row = std::make_unique<ListRow>();
    row->flat = true;
    row->showDivider = showDivider;
    row->selected = (ap.objectPath == m_selectedApPath);
    row->iconName = "network-wireless";
    row->title = ap.ssid.empty() ? "(Hidden Network)" : ap.ssid;
    row->subtitle = SignalQuality(ap.strength);

    std::string apPath = ap.objectPath;
    row->onClick = [this, apPath] { m_selectedApPath = apPath; RebuildPage(); m_window.RequestRedraw(); };

    const int gap = 10;
    const int connectedW = ap.isActiveConnection ? Badge::MeasureWidth(m_window, "Connected") : 0;
    const int lockW = ap.secured ? 18 : 0;
    const int barsW = showPercent ? 22 : 0;

    int trailingWidth = barsW + (lockW > 0 ? lockW + gap : 0) + (connectedW > 0 ? connectedW + gap : 0);
    if (trailingWidth > 0)
        trailingWidth += 0; // already includes internal gaps; margin is added by Layout() itself
    row->Layout(rowBounds, nullptr, trailingWidth);

    int cursorRight = rowBounds.Right() - 14;

    if (showPercent)
    {
        Rect barsRect{ cursorRight - barsW, rowBounds.y + (rowBounds.height - 20) / 2, barsW, 20 };
        auto bars = std::make_unique<IconView>();
        bars->name = WiFiIconFor(ap.strength);
        bars->bounds = barsRect;
        row->AddChild(std::move(bars));
        cursorRight = barsRect.x;
    }

    if (ap.secured)
    {
        cursorRight -= gap;
        Rect lockRect{ cursorRight - lockW, rowBounds.y + (rowBounds.height - 18) / 2, lockW, 18 };
        auto lock = std::make_unique<IconView>();
        lock->name = "network-wireless-encrypted";
        lock->bounds = lockRect;
        row->AddChild(std::move(lock));
        cursorRight = lockRect.x;
    }

    if (ap.isActiveConnection)
    {
        cursorRight -= gap;
        Rect badgeRect{ cursorRight - connectedW, rowBounds.y + (rowBounds.height - 24) / 2, connectedW, 24 };
        auto badge = std::make_unique<Badge>();
        badge->text = "Connected";
        badge->tone = Badge::Tone::Positive;
        badge->bounds = badgeRect;
        row->AddChild(std::move(badge));
    }

    return row;
}

std::unique_ptr<Widget> NetworkWindow::BuildDetailsPanel(const NetworkDevice& wifiDevice, const WiFiAccessPoint& ap, const Rect& panelBounds)
{
    auto card = std::make_unique<Card>();
    card->bounds = panelBounds;

    int x = panelBounds.x + 18;
    int w = panelBounds.width - 36;
    int y = panelBounds.y + 16;

    auto title = std::make_unique<Label>();
    title->text = ap.ssid.empty() ? "(Hidden Network)" : ap.ssid;
    title->bounds = { x, y, w, 24 };
    card->AddChild(std::move(title));
    y += 24 + 14;

    bool connected = ap.isActiveConnection;
    const UiTheme& theme = UiTheme::Default();

    card->AddChild(MakeDetailRow({ x, y, w, 22 }, "Status", connected ? "Connected" : "Not connected", connected ? theme.accent : 0));
    y += 30;

    card->AddChild(MakeDetailRow({ x, y, w, 22 }, "Signal Strength", std::to_string(ap.strength) + "%"));
    y += 30;

    if (connected)
    {
        card->AddChild(MakeDetailRow({ x, y, w, 22 }, "IPv4 Address", wifiDevice.ipv4.address.empty() ? "\u2014" : wifiDevice.ipv4.address));
        y += 30;

        card->AddChild(MakeDetailRow({ x, y, w, 22 }, "Gateway", wifiDevice.ipv4.gateway.empty() ? "\u2014" : wifiDevice.ipv4.gateway));
        y += 30;

        card->AddChild(MakeDetailRow({ x, y, w, 22 }, "DNS", wifiDevice.ipv4.dnsServers.empty() ? "\u2014" : wifiDevice.ipv4.dnsServers.front()));
        y += 30;
    }

    y += 10;

    // Both actions always shown side by side (matching the mockup),
    // but only the one that applies to the current state is enabled -
    // clearer than swapping which button is present, and avoids the
    // ambiguity of a "Connect" that would do nothing on an already-
    // active network.
    const int buttonHeight = 44;
    const int buttonGap = 12;
    int buttonWidth = (w - buttonGap) / 2;

    std::string devicePath = wifiDevice.objectPath;
    std::string apPath = ap.objectPath;
    std::string ssid = ap.ssid;
    bool secured = ap.secured;

    auto disconnectBtn = std::make_unique<Button>();
    disconnectBtn->label = "Disconnect";
    disconnectBtn->tone = Button::Tone::Danger;
    disconnectBtn->enabled = connected;
    disconnectBtn->bounds = { x, y, buttonWidth, buttonHeight };
    disconnectBtn->onClick = [this, devicePath] { m_networkManager.DisconnectDevice(devicePath); };
    card->AddChild(std::move(disconnectBtn));

    auto connectBtn = std::make_unique<Button>();
    connectBtn->label = "Connect";
    connectBtn->tone = Button::Tone::Accent;
    connectBtn->enabled = !connected;
    connectBtn->bounds = { x + buttonWidth + buttonGap, y, buttonWidth, buttonHeight };
    connectBtn->onClick = [this, devicePath, apPath, ssid, secured]
    {
        // Only the *first* half of this condition used to exist: any
        // secured network unconditionally went to PromptAndConnect(),
        // regardless of whether NetworkManager already had a saved,
        // working secret for this exact SSID - forcing a password
        // prompt on literally every manual connect to a known
        // network, typed-in text or not. HasUsableSavedSecret() (see
        // its own comment) is the actual "is there something to
        // reuse" check; when it's true, this takes the same path an
        // open network already did - straight to
        // ConnectToAccessPoint() with an empty password, which its
        // own "reuse an existing saved profile" branch (the one
        // 0.20.3's BytesToString() fix made reachable at all) already
        // correctly treats as "activate with whatever's already
        // saved, don't touch it" rather than as "the user wants no
        // password".
        if (!secured || m_networkManager.HasUsableSavedSecret(ssid))
            m_networkManager.ConnectToAccessPoint(devicePath, apPath, ssid, "");
        else
            PromptAndConnect(devicePath, apPath, ssid, secured);
    };
    card->AddChild(std::move(connectBtn));

    return card;
}

void NetworkWindow::EnsureSelection(const std::vector<const WiFiAccessPoint*>& visibleAccessPoints)
{
    for (auto* ap : visibleAccessPoints)
        if (ap->objectPath == m_selectedApPath)
            return;

    for (auto* ap : visibleAccessPoints)
    {
        if (ap->isActiveConnection)
        {
            m_selectedApPath = ap->objectPath;
            return;
        }
    }

    m_selectedApPath = visibleAccessPoints.empty() ? std::string() : visibleAccessPoints.front()->objectPath;
}

int NetworkWindow::AppendNetworkSection(Widget& content, int y, int contentWidth)
{
    auto header = std::make_unique<SectionHeader>();
    header->text = "AVAILABLE NETWORKS";
    header->bounds = { 0, y, contentWidth, 18 };
    content.AddChild(std::move(header));
    y += 18 + 10;

    bool available = m_networkManager.Available();
    bool wirelessEnabled = available && m_networkManager.WirelessEnabled();

    if (!available || !wirelessEnabled)
    {
        const int emptyHeight = 64;
        auto card = std::make_unique<Card>();
        card->bounds = { 0, y, contentWidth, emptyHeight };

        auto label = std::make_unique<Label>();
        label->text = !available ? "NetworkManager is not available." : "Turn Wi-Fi on to see nearby networks.";
        label->color = UiTheme::Default().muted;
        label->bounds = { 20, y, contentWidth - 40, emptyHeight };
        card->AddChild(std::move(label));

        content.AddChild(std::move(card));
        return y + emptyHeight;
    }

    const NetworkDevice* wifiDevice = nullptr;
    for (auto& device : m_networkManager.Devices())
        if (device.kind == NetworkDeviceKind::WiFi) { wifiDevice = &device; break; }

    if (!wifiDevice)
    {
        const int emptyHeight = 64;
        auto card = std::make_unique<Card>();
        card->bounds = { 0, y, contentWidth, emptyHeight };

        auto label = std::make_unique<Label>();
        label->text = "No Wi-Fi adapter found.";
        label->color = UiTheme::Default().muted;
        label->bounds = { 20, y, contentWidth - 40, emptyHeight };
        card->AddChild(std::move(label));

        content.AddChild(std::move(card));
        return y + emptyHeight;
    }

    std::string query = ToLower(m_searchQuery);
    std::vector<const WiFiAccessPoint*> visible;
    for (auto& ap : wifiDevice->accessPoints)
        if (query.empty() || ToLower(ap.ssid).find(query) != std::string::npos)
            visible.push_back(&ap);

    EnsureSelection(visible);

    bool split = contentWidth >= kSplitBreakpoint;
    int listWidth = split ? (contentWidth - kCardGap) * 56 / 100 : contentWidth;
    const int rowHeight = 64;

    int listCardHeight = visible.empty() ? 64 : rowHeight * static_cast<int>(visible.size());

    auto listCard = std::make_unique<Card>();
    listCard->bounds = { 0, y, listWidth, listCardHeight };

    bool showPercent = listWidth >= 320;

    if (visible.empty())
    {
        auto label = std::make_unique<Label>();
        label->text = wifiDevice->accessPoints.empty() ? "No networks found yet." : "No networks match your search.";
        label->color = UiTheme::Default().muted;
        label->bounds = { 20, y, listWidth - 40, listCardHeight };
        listCard->AddChild(std::move(label));
    }
    else
    {
        std::string devicePath = wifiDevice->objectPath;
        for (std::size_t i = 0; i < visible.size(); ++i)
        {
            Rect rowBounds{ 0, y + static_cast<int>(i) * rowHeight, listWidth, rowHeight };
            bool showDivider = (i + 1 != visible.size());
            listCard->AddChild(BuildNetworkRow(*visible[i], devicePath, rowBounds, showDivider, showPercent));
        }
    }

    content.AddChild(std::move(listCard));

    int bottomY = y + listCardHeight;

    const WiFiAccessPoint* selectedAp = nullptr;
    for (auto* ap : visible)
        if (ap->objectPath == m_selectedApPath) { selectedAp = ap; break; }

    if (selectedAp)
    {
        int detailsX = split ? listWidth + kCardGap : 0;
        int detailsY = split ? y : y + listCardHeight + kCardGap;
        int detailsWidth = split ? contentWidth - listWidth - kCardGap : contentWidth;

        bool connected = selectedAp->isActiveConnection;
        int detailRows = connected ? 5 : 2;
        int detailsHeight = 24 + 14 + detailRows * 30 + 10 + 44;

        Rect panelBounds{ detailsX, detailsY, detailsWidth, detailsHeight };
        content.AddChild(BuildDetailsPanel(*wifiDevice, *selectedAp, panelBounds));

        bottomY = std::max(bottomY, detailsY + detailsHeight);
    }

    return bottomY;
}

std::unique_ptr<Widget> NetworkWindow::BuildEthernetCard(const NetworkDevice& device, const Rect& cardBounds)
{
    int width = cardBounds.width;
    int columns = width >= 760 ? 4 : width >= 520 ? 2 : 1;

    auto card = std::make_unique<Card>();
    card->bounds = cardBounds;

    auto icon = std::make_unique<IconView>();
    icon->name = "network-wired";
    icon->bounds = { cardBounds.x + 16, cardBounds.y + 16, 30, 30 };
    card->AddChild(std::move(icon));

    int badgeWidth = Badge::MeasureWidth(m_window, device.connected ? "Connected" : "Not connected");

    auto title = std::make_unique<Label>();
    title->text = device.interfaceName.empty() ? "Ethernet" : device.interfaceName;
    title->bounds = { cardBounds.x + 58, cardBounds.y + 16, width - 58 - badgeWidth - 28, 22 };
    card->AddChild(std::move(title));

    auto badge = std::make_unique<Badge>();
    badge->text = device.connected ? "Connected" : "Not connected";
    badge->tone = device.connected ? Badge::Tone::Positive : Badge::Tone::Neutral;
    badge->bounds = { cardBounds.Right() - 16 - badgeWidth, cardBounds.y + 18, badgeWidth, 22 };
    card->AddChild(std::move(badge));

    if (columns == 1)
    {
        std::ostringstream summary;
        summary << (device.ipv4.address.empty() ? "No address" : device.ipv4.address + "/" + std::to_string(device.ipv4.prefixLength));
        if (!device.macAddress.empty())
            summary << "  \u2022  " << device.macAddress;

        auto summaryLabel = std::make_unique<Label>();
        summaryLabel->text = summary.str();
        summaryLabel->color = UiTheme::Default().muted;
        summaryLabel->bounds = { cardBounds.x + 58, cardBounds.y + 40, width - 74, 18 };
        card->AddChild(std::move(summaryLabel));
    }
    else
    {
        struct Field { std::string caption, value; };
        std::vector<Field> fields = {
            { "ADDRESS", device.ipv4.address.empty() ? "" : device.ipv4.address + "/" + std::to_string(device.ipv4.prefixLength) },
            { "GATEWAY", device.ipv4.gateway },
            { "DNS", device.ipv4.dnsServers.empty() ? "" : device.ipv4.dnsServers.front() },
            { "MAC ADDRESS", device.macAddress },
        };

        int rows = (4 + columns - 1) / columns;
        (void)rows;
        int fieldWidth = (width - 32 - (columns - 1) * kCardGap) / columns;
        int gridTop = cardBounds.y + 58;

        for (std::size_t i = 0; i < fields.size(); ++i)
        {
            int col = static_cast<int>(i) % columns;
            int row = static_cast<int>(i) / columns;
            Rect fieldRect{ cardBounds.x + 16 + col * (fieldWidth + kCardGap), gridTop + row * 44, fieldWidth, 40 };
            card->AddChild(BuildInfoField(fieldRect, fields[i].caption, fields[i].value));
        }
    }

    return card;
}

int NetworkWindow::RebuildMainPage(Widget& content, int contentWidth)
{
    int y = AppendToolbar(content, 0, contentWidth);

    const int searchHeight = 44;
    auto search = std::make_unique<TextField>();
    search->iconName = "edit-find";
    search->placeholder = "Search networks...";
    search->text = m_searchQuery;
    search->bounds = { 0, y, contentWidth, searchHeight };
    search->onChange = [this](const std::string& text)
    {
        m_searchQuery = text;
        RebuildPage();
        m_window.RequestRedraw();
    };
    m_searchField = static_cast<TextField*>(content.AddChild(std::move(search)));
    y += searchHeight + kCardGap;

    y = AppendNetworkSection(content, y, contentWidth);
    y += kCardGap;

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

int NetworkWindow::RebuildAdvancedPage(Widget& content, int contentWidth)
{
    m_searchField = nullptr;

    int y = 0;

    auto backRow = std::make_unique<ListRow>();
    backRow->flat = true;
    backRow->showDivider = true;
    backRow->title = "\u2039  Back to Kohiko Network";
    backRow->onClick = [this] { m_page = Page::Main; RebuildPage(); m_window.RequestRedraw(); };
    backRow->Layout({ 0, y, contentWidth, 44 });
    content.AddChild(std::move(backRow));
    y += 44 + kCardGap;

    const int tabHeight = 40;
    int tabWidth = (contentWidth - 12) / 2;

    auto ethernetTab = std::make_unique<Button>();
    ethernetTab->label = "Ethernet";
    ethernetTab->primary = (m_advancedTab == AdvancedTab::Ethernet);
    ethernetTab->bounds = { 0, y, tabWidth, tabHeight };
    ethernetTab->onClick = [this] { m_advancedTab = AdvancedTab::Ethernet; RebuildPage(); m_window.RequestRedraw(); };
    content.AddChild(std::move(ethernetTab));

    auto vpnTab = std::make_unique<Button>();
    vpnTab->label = "VPN";
    vpnTab->primary = (m_advancedTab == AdvancedTab::Vpn);
    vpnTab->bounds = { tabWidth + 12, y, tabWidth, tabHeight };
    vpnTab->onClick = [this] { m_advancedTab = AdvancedTab::Vpn; RebuildPage(); m_window.RequestRedraw(); };
    content.AddChild(std::move(vpnTab));

    y += tabHeight + kCardGap;

    bool available = m_networkManager.Available();

    if (m_advancedTab == AdvancedTab::Ethernet)
    {
        auto header = std::make_unique<SectionHeader>();
        header->text = "ETHERNET";
        header->bounds = { 0, y, contentWidth, 18 };
        content.AddChild(std::move(header));
        y += 18 + 10;

        if (!available)
        {
            auto label = std::make_unique<Label>();
            label->text = "NetworkManager is not available.";
            label->color = UiTheme::Default().muted;
            label->bounds = { 0, y, contentWidth, 24 };
            content.AddChild(std::move(label));
            y += 24;
        }
        else
        {
            bool any = false;
            for (auto& device : m_networkManager.Devices())
            {
                if (device.kind != NetworkDeviceKind::Ethernet)
                    continue;

                any = true;
                int columns = contentWidth >= 760 ? 4 : contentWidth >= 520 ? 2 : 1;
                int rows = columns == 1 ? 1 : (4 + columns - 1) / columns;
                int gridHeight = columns == 1 ? 20 : rows * 44;
                int cardHeight = 64 + gridHeight;

                content.AddChild(BuildEthernetCard(device, { 0, y, contentWidth, cardHeight }));
                y += cardHeight + kCardGap;
            }

            if (!any)
            {
                auto label = std::make_unique<Label>();
                label->text = "No Ethernet devices found.";
                label->color = UiTheme::Default().muted;
                label->bounds = { 0, y, contentWidth, 24 };
                content.AddChild(std::move(label));
                y += 24;
            }
        }
    }
    else
    {
        auto header = std::make_unique<SectionHeader>();
        header->text = "VPN CONNECTIONS";
        header->bounds = { 0, y, contentWidth, 18 };
        content.AddChild(std::move(header));
        y += 18 + 10;

        if (!available)
        {
            auto label = std::make_unique<Label>();
            label->text = "NetworkManager is not available.";
            label->color = UiTheme::Default().muted;
            label->bounds = { 0, y, contentWidth, 24 };
            content.AddChild(std::move(label));
            y += 24;
        }
        else
        {
            std::vector<const SavedConnection*> vpns;
            for (auto& conn : m_networkManager.SavedConnections())
                if (conn.isVpn)
                    vpns.push_back(&conn);

            if (vpns.empty())
            {
                auto label = std::make_unique<Label>();
                label->text = "No VPN connections configured. Add one with nm-connection-editor or your VPN provider's setup tool.";
                label->color = UiTheme::Default().muted;
                label->bounds = { 0, y, contentWidth, 48 };
                content.AddChild(std::move(label));
                y += 48;
            }
            else
            {
                const int rowHeight = 72;
                auto card = std::make_unique<Card>();
                card->bounds = { 0, y, contentWidth, rowHeight * static_cast<int>(vpns.size()) };

                for (std::size_t i = 0; i < vpns.size(); ++i)
                {
                    const SavedConnection& conn = *vpns[i];
                    Rect rowBounds{ 0, y + static_cast<int>(i) * rowHeight, contentWidth, rowHeight };

                    auto row = std::make_unique<ListRow>();
                    row->flat = true;
                    row->showDivider = (i + 1 != vpns.size());
                    row->iconName = "network-vpn";
                    row->title = conn.id;
                    row->subtitle = conn.isActive ? "Connected" : "Not connected";
                    row->selected = conn.isActive;

                    std::string connPath = conn.objectPath;
                    bool isActive = conn.isActive;

                    auto toggle = std::make_unique<ToggleSwitch>();
                    toggle->value = isActive;
                    toggle->bounds = { 0, 0, 0, 26 };
                    toggle->onChange = [this, connPath](bool enable)
                    {
                        if (enable)
                            m_networkManager.ActivateSavedConnection(connPath, "/");
                        else
                            m_networkManager.DeactivateConnection(connPath);
                    };

                    row->Layout(rowBounds, std::move(toggle), 46);
                    card->AddChild(std::move(row));
                }

                y += rowHeight * static_cast<int>(vpns.size());
                content.AddChild(std::move(card));
            }
        }
    }

    return y;
}

void NetworkWindow::RebuildPage()
{
    bool searchWasFocused = m_searchField && m_searchField->focused;

    m_window.ClearFocus();

    int contentWidth = m_scrollView->bounds.width;
    auto content = std::make_unique<Widget>();

    int y = (m_page == Page::Advanced)
        ? RebuildAdvancedPage(*content, contentWidth)
        : RebuildMainPage(*content, contentWidth);

    content->bounds = { 0, 0, contentWidth, y };
    m_scrollView->SetContent(std::move(content));

    if (searchWasFocused && m_searchField)
        m_window.SetFocus(m_searchField);
}

void NetworkWindow::PromptAndConnect(const std::string& devicePath, const std::string& apPath, const std::string& ssid, bool /*secured*/)
{
    Display* display = m_window.GetDisplay();
    int screen = DefaultScreen(display);

    auto password = PromptForPassword(
        display, screen,
        DefaultVisual(display, screen), DefaultColormap(display, screen),
        m_window.GetFont(), m_window.Theme(), ssid
    );

    if (password.has_value())
        m_networkManager.ConnectToAccessPoint(devicePath, apPath, ssid, *password);
}

}
