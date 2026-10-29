#include "SettingsWindow.h"
#include "WindowRule.h"

#include "Config.h"
#include "Process.h"
#include "Utils.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace Kohiko
{

namespace
{

constexpr int kWindowDefaultWidth  = 920;
constexpr int kWindowDefaultHeight = 680;
constexpr int kWindowMinWidth      = 560;
constexpr int kWindowMinHeight     = 420;

constexpr int kSidebarWidth     = 190;
constexpr int kTopBarHeight     = 46;
constexpr int kBottomBarHeight  = 54;
constexpr int kRowPaddingX      = 18;
constexpr int kRowHeight        = 38;
constexpr int kGroupHeaderGap   = 26;
constexpr int kWidgetWidth      = 260;
constexpr int kCheckboxSize     = 18;
constexpr int kInfoIconSize     = 16;
constexpr int kRawBlockLineH    = 20;
constexpr int kRawBlockMinLines = 4;
constexpr int kRawBlockMaxLines = 10;

// --- structured windowrule=/monitor= row editor (see LayoutAndDrawStructuredBlock()) ---

constexpr int kStructuredRowH        = 30; // one rule per row, plus a little breathing room
constexpr int kStructuredRowGap      = 6;
constexpr int kStructuredActionW     = 108; // WindowRule's click-to-cycle action button
constexpr int kStructuredRemoveW     = 24;  // the "x" remove button, every row
constexpr int kStructuredWorkspaceW  = 56;  // narrow - just a couple of digits
constexpr int kStructuredWorkspaceLabelW = 104; // a window rule's "Workspace" cell: wide enough for its own placeholder
constexpr int kStructuredChoiceW     = 172; // a window rule's Horizontal/Vertical click-to-cycle buttons
constexpr int kStructuredFieldGap    = 6;
constexpr int kStructuredAddButtonW  = 140;
constexpr int kStructuredAddButtonH  = 26;

// A window rule's click-to-cycle choices, in the order they cycle
// (index 0 is always "Default" = no opinion). The order matches
// WindowRule::Horizontal/Vertical/Mode exactly, so a draft's index IS
// the enum value.
const std::array<const char*, 3> kHorizontalLabels = {"Default", "Left", "Right"};
const std::array<const char*, 3> kHorizontalTokens = {"", "left", "right"};
const std::array<const char*, 3> kVerticalLabels   = {"Default", "Top", "Bottom"};
const std::array<const char*, 3> kVerticalTokens   = {"", "top", "bottom"};
const std::array<const char*, 4> kModeLabels       = {"Default", "Tiled", "Floating", "Fullscreen"};
const std::array<const char*, 4> kModeTokens       = {"", "tiled", "floating", "fullscreen"};

unsigned long ParseColorLiteral(
    const std::string& text,
    unsigned long fallback)
{
    if (text.size() < 3 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X'))
        return fallback;

    for (std::size_t i = 2; i < text.size(); ++i)
    {
        if (!std::isxdigit(static_cast<unsigned char>(text[i])))
            return fallback;
    }

    return std::strtoul(text.c_str(), nullptr, 0);
}

unsigned long BlendPixels(
    unsigned long a,
    unsigned long b,
    float t)
{
    auto channel = [&](int shift) -> unsigned long
    {
        int ca = static_cast<int>((a >> shift) & 0xff);
        int cb = static_cast<int>((b >> shift) & 0xff);
        int result = ca + static_cast<int>((static_cast<float>(cb - ca)) * t);
        result = std::clamp(result, 0, 255);
        return static_cast<unsigned long>(result) << shift;
    };

    return channel(16) | channel(8) | channel(0);
}

bool ContainsCaseInsensitive(
    const std::string& haystack,
    const std::string& needleLower)
{
    if (needleLower.empty())
        return true;

    return Utils::Lower(haystack).find(needleLower) != std::string::npos;
}

}

SettingsWindow::SettingsWindow()
{
}

SettingsWindow::~SettingsWindow()
{
    if (!m_display)
        return;

    if (m_xic)
        XDestroyIC(m_xic);

    if (m_xim)
        XCloseIM(m_xim);

    if (m_xftDraw)
        XftDrawDestroy(m_xftDraw);

    m_font.Unload();
    m_boldFont.Unload();

    if (m_gc)
        XFreeGC(m_display, m_gc);

    if (m_window)
        XDestroyWindow(m_display, m_window);

    XCloseDisplay(m_display);
}

// --- setup --------------------------------------------------------------------

bool SettingsWindow::Initialize()
{
    m_display = XOpenDisplay(nullptr);

    if (!m_display)
    {
        std::fprintf(stderr, "kohiko-settings: could not open X display (is DISPLAY set?)\n");
        return false;
    }

    m_screen = DefaultScreen(m_display);

    const char* home = std::getenv("HOME");
    m_configPath = (home ? std::string(home) : std::string(".")) + "/.config/kohiko/kohiko.conf";

    Config config;

    if (!config.Load(m_configPath))
    {
        std::fprintf(
            stderr,
            "kohiko-settings: couldn't read %s - run Kohiko at least once first "
            "(it creates this file from config/default.conf on first launch).\n",
            m_configPath.c_str());
        return false;
    }

    if (!m_writer.Load(m_configPath))
    {
        std::fprintf(stderr, "kohiko-settings: couldn't open %s for editing\n", m_configPath.c_str());
        return false;
    }

    m_backgroundPixel = ParseColorLiteral(config.GetString("bar.background", "0x1e1e2e"), 0x1e1e2e);
    m_foregroundPixel = ParseColorLiteral(config.GetString("bar.foreground", "0xcdd6f4"), 0xcdd6f4);
    m_accentPixel     = ParseColorLiteral(config.GetString("bar.active", "0x89b4fa"), 0x89b4fa);
    m_fieldPixel      = ParseColorLiteral(config.GetString("lockscreen.field_color", "0x313244"), 0x313244);
    m_errorPixel      = ParseColorLiteral(config.GetString("lockscreen.error_color", "0xf38ba8"), 0xf38ba8);
    m_borderPixel     = ParseColorLiteral(config.GetString("general.border_color_inactive", "0x45475a"), 0x45475a);
    m_panelPixel      = BlendPixels(m_backgroundPixel, m_fieldPixel, 0.5f);
    m_mutedPixel      = BlendPixels(m_foregroundPixel, m_backgroundPixel, 0.45f);

    BuildFields();
    BuildWorkspaceFields();
    BuildRawBlocks();

    CreateWindow();

    m_gc = XCreateGC(m_display, m_window, 0, nullptr);

    std::string fontPattern = config.GetString("general.font", "monospace:pixelsize=14");
    m_font.Load(m_display, m_screen, fontPattern);
    m_boldFont.Load(m_display, m_screen, fontPattern + ":bold");

    m_xftDraw = XftDrawCreate(
        m_display, m_window,
        DefaultVisual(m_display, m_screen),
        DefaultColormap(m_display, m_screen));

    // Same reasoning as Notepad's own XIM/XIC setup - Xutf8LookupString
    // needs it for dead-key/compose sequences and non-ASCII input to
    // work at all in the search box and every text field.
    m_xim = XOpenIM(m_display, nullptr, nullptr, nullptr);

    if (m_xim)
    {
        m_xic = XCreateIC(
            m_xim,
            XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
            XNClientWindow, m_window,
            XNFocusWindow, m_window,
            nullptr);
    }

    XMapWindow(m_display, m_window);
    m_running = true;

    return true;
}

void SettingsWindow::CreateWindow()
{
    m_geometry = Rect{0, 0, kWindowDefaultWidth, kWindowDefaultHeight};

    XSetWindowAttributes attrs{};
    attrs.background_pixel = m_backgroundPixel;
    attrs.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask;

    m_window = XCreateWindow(
        m_display,
        RootWindow(m_display, m_screen),
        m_geometry.x, m_geometry.y,
        static_cast<unsigned int>(m_geometry.width),
        static_cast<unsigned int>(m_geometry.height),
        0,
        DefaultDepth(m_display, m_screen),
        InputOutput,
        DefaultVisual(m_display, m_screen),
        CWBackPixel | CWEventMask,
        &attrs);

    SetWmProperties();
}

void SettingsWindow::SetWmProperties()
{
    // A completely ordinary top-level client window - deliberately
    // NOT override-redirect (unlike every window Kohiko draws for
    // itself) so Kohiko (or any other WM) tiles/manages this exactly
    // like any other application. See this class's own header
    // comment.
    XClassHint classHint;
    classHint.res_name = const_cast<char*>("kohiko-settings");
    classHint.res_class = const_cast<char*>("kohiko-settings");
    XSetClassHint(m_display, m_window, &classHint);

    XStoreName(m_display, m_window, "Kohiko Settings");

    Atom netWmName = XInternAtom(m_display, "_NET_WM_NAME", False);
    Atom utf8String = XInternAtom(m_display, "UTF8_STRING", False);
    const char* title = "Kohiko Settings";
    XChangeProperty(
        m_display, m_window, netWmName, utf8String, 8,
        PropModeReplace,
        reinterpret_cast<const unsigned char*>(title),
        static_cast<int>(std::strlen(title)));

    XSizeHints sizeHints{};
    sizeHints.flags = PMinSize | PSize;
    sizeHints.min_width = kWindowMinWidth;
    sizeHints.min_height = kWindowMinHeight;
    sizeHints.width = kWindowDefaultWidth;
    sizeHints.height = kWindowDefaultHeight;
    XSetWMNormalHints(m_display, m_window, &sizeHints);

    m_wmDeleteWindow = XInternAtom(m_display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(m_display, m_window, &m_wmDeleteWindow, 1);
}

std::string SettingsWindow::HumanizeKey(
    const std::string& key)
{
    // "lockscreen.show_clock" -> "Show clock" - drop the namespace
    // prefix (everything through the last '.' before the final
    // segment - or, for something like "auto_start_programs" with no
    // '.' at all, the whole key), turn '_' into spaces, capitalize
    // the first letter. Good enough for every key this schema
    // actually has; not meant as a general-purpose prettifier.
    std::size_t lastDot = key.find_last_of('.');
    std::string tail = (lastDot == std::string::npos) ? key : key.substr(lastDot + 1);

    std::string result;
    result.reserve(tail.size());

    for (char c : tail)
        result += (c == '_') ? ' ' : c;

    if (!result.empty())
        result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));

    return result;
}

void SettingsWindow::BuildFields()
{
    Config config;
    config.Load(m_configPath);

    // Fixed preferred sidebar order, extended automatically with any
    // ConfigSchema category this list doesn't already know about (see
    // this loop's tail) so a future schema addition never silently
    // fails to show up just because SettingsWindow.cpp wasn't updated
    // in lockstep.
    static const std::vector<std::pair<std::string, bool>> kOrder =
    {
        {"General", false},
        {"Appearance", false},
        {"Launcher", false},
        {"Bar", false},
        {"Input", true},
        {"Monitors", true},
        {"Workspaces", false},
        {"Autostart", false},
        {"Window Rules", true},
        {"Power", false},
        {"Notepad", false},
        {"Scratchpad", false},
        {"Developer", true},
        {"Lock Screen", false},
    };

    for (const auto& [name, hasRawBlock] : kOrder)
        m_categories.push_back({name, hasRawBlock});

    for (const std::string& schemaCategory : ConfigSchema::Categories())
    {
        bool known = std::any_of(
            m_categories.begin(), m_categories.end(),
            [&](const Category& c) { return c.name == schemaCategory; });

        if (!known)
            m_categories.push_back({schemaCategory, false});
    }

    for (const ConfigOption& option : ConfigSchema::All())
    {
        Field field;
        field.option = &option;

        if (option.type == ConfigValueType::Boolean)
        {
            bool defaultBool = (option.defaultValue == "true");
            field.loadedValue = config.GetBool(option.key, defaultBool) ? "true" : "false";
        }
        else
        {
            field.loadedValue = config.GetString(option.key, option.defaultValue);
        }

        field.currentValue = field.loadedValue;
        m_fields.push_back(std::move(field));
    }
}

void SettingsWindow::BuildWorkspaceFields()
{
    m_focusedField = nullptr; // about to invalidate every Field::option this vector's growth might reallocate past
    m_workspaceFields.clear();
    m_workspaceOptionStorage.clear();

    int count = 0;

    for (const Field& field : m_fields)
    {
        if (field.option->key == "workspace.count")
        {
            try { count = std::stoi(field.currentValue); }
            catch (...) { count = 0; }

            break;
        }
    }

    count = std::clamp(count, 0, 64); // sanity ceiling - see workspace.count's own description

    Config config;
    config.Load(m_configPath);

    m_workspaceOptionStorage.reserve(static_cast<std::size_t>(count));

    for (int n = 1; n <= count; ++n)
    {
        ConfigOption option;
        option.category = "Autostart";
        option.group = "Launch on a workspace";
        option.key = "workspace" + std::to_string(n);
        option.type = ConfigValueType::String;
        option.defaultValue = "";
        option.description =
            "Space-separated programs launched once per login session, like "
            "the list above, whose window then opens on workspace " +
            std::to_string(n) + " (once it actually shows up - however long "
            "that takes). A Window Rule for the same application takes "
            "precedence over this. If a program is also in the list above "
            "or has its own ~/.config/autostart entry, it is started only "
            "once, here. Leave empty to launch nothing on this workspace.";

        m_workspaceOptionStorage.push_back(std::move(option));
    }

    for (const ConfigOption& option : m_workspaceOptionStorage)
    {
        Field field;
        field.option = &option;
        field.loadedValue = config.GetString(option.key, "");
        field.currentValue = field.loadedValue;
        m_workspaceFields.push_back(std::move(field));
    }
}

void SettingsWindow::BuildRawBlocks()
{
    struct Spec
    {
        const char* id;
        const char* category;
        const char* title;
        const char* prefix;
        const char* help;
        RawBlockPanel::Kind kind;
    };

    static const Spec kSpecs[] =
    {
        {
            "windowrule", "Window Rules", "Window rules", "windowrule=",
            "You decide where an application's windows go - Kohiko never "
            "learns it from moving windows around. One rule per row: the "
            "application (its WM_CLASS, matched exactly - press Super+Shift+W "
            "on a window to fill one in), then any of Workspace, Horizontal "
            "(Left/Right), Vertical (Top/Bottom) and Mode (Tiled/Floating/"
            "Fullscreen); Default means \"no opinion\" for that choice. "
            "Position moves a newly opened window toward that edge of the "
            "layout (or of the screen, if floating). instance:/title: "
            "narrow a rule to one window of an application; the more "
            "specific rule wins. Rules apply to windows opened afterwards, "
            "and only changing them here, in the Super+Shift+W dialog or "
            "in kohiko.conf ever changes one.",
            RawBlockPanel::Kind::WindowRule
        },
        {
            "monitor", "Monitors", "Monitor rules", "monitor=",
            "One rule per row: pins a specific output to always start on a "
            "given workspace. Run `kohikoctl monitors` (with Kohiko running) "
            "to see your output names.",
            RawBlockPanel::Kind::Monitor
        },
        {
            "bind", "Input", "Keybindings", "bind=",
            "<MODS+KEY> <command> [args] per line. See the README's \"Default "
            "keybindings\" section for the full command list.",
            RawBlockPanel::Kind::RawText
        },
        {
            "exec", "Developer", "Named commands", "exec.",
            "<name>=<command> per line - referenced elsewhere as `exec <name>` "
            "(a bind= line, or `kohikoctl dispatch exec <name>`). \"terminal\" "
            "and \"browser\" are used by Kohiko's default keybindings.",
            RawBlockPanel::Kind::RawText
        },
        {
            "wallpapermonitor", "Wallpaper", "Per-monitor wallpapers", "wallpaper.monitor=",
            "One rule per row: <output name>,path=<file>,mode=<fill|fit|center|"
            "stretch> (mode= is optional - falls back to the Mode setting "
            "above). Run `kohikoctl monitors` to see your output names. A "
            "matching per-workspace rule below always wins over one of these.",
            RawBlockPanel::Kind::RawText
        },
        {
            "wallpaperworkspace", "Wallpaper", "Per-workspace wallpapers", "wallpaper.workspace=",
            "One rule per row: <workspace number>,path=<file>,mode=<fill|fit|"
            "center|stretch> (mode= is optional). Wins over a per-monitor rule "
            "above wherever both could apply to the same screen at once.",
            RawBlockPanel::Kind::RawText
        },
    };

    for (const Spec& spec : kSpecs)
    {
        RawBlockPanel panel;
        panel.id = spec.id;
        panel.category = spec.category;
        panel.title = spec.title;
        panel.prefix = spec.prefix;
        panel.helpText = spec.help;
        panel.kind = spec.kind;
        panel.loadedLines = m_writer.GetRawBlock(spec.prefix);
        panel.lines = panel.loadedLines;
        panel.lineErrors.assign(panel.lines.size(), "");

        for (std::size_t i = 0; i < panel.lines.size(); ++i)
            ValidateRawBlockLine(panel, panel.lines[i], panel.lineErrors[i]);

        m_rawBlocks.push_back(std::move(panel));
    }
}

}

namespace Kohiko
{

// --- event loop -----------------------------------------------------------------

void SettingsWindow::Run()
{
    Redraw();

    while (m_running)
    {
        XEvent event;
        XNextEvent(m_display, &event);
        HandleEvent(event);
    }
}

void SettingsWindow::HandleEvent(
    const XEvent& event)
{
    switch (event.type)
    {
        case Expose:

            if (event.xexpose.count == 0)
                Redraw();

            break;

        case ConfigureNotify:

            if (event.xconfigure.width != m_geometry.width || event.xconfigure.height != m_geometry.height)
            {
                m_geometry.width = event.xconfigure.width;
                m_geometry.height = event.xconfigure.height;
                Redraw();
            }

            break;

        case KeyPress:
            HandleKeyPress(event.xkey);
            break;

        case ButtonPress:
            HandleButtonPress(event.xbutton);
            break;

        case ClientMessage:

            if (static_cast<Atom>(event.xclient.data.l[0]) == m_wmDeleteWindow)
                m_running = false;

            break;

        default:
            break;
    }
}

void SettingsWindow::HandleKeyPress(
    const XKeyEvent& event)
{
    char buffer[32];
    KeySym keysym = NoSymbol;
    XKeyEvent mutableEvent = event; // Xutf8LookupString wants a non-const pointer
    int len;

    if (m_xic)
    {
        Status status;
        len = Xutf8LookupString(m_xic, &mutableEvent, buffer, sizeof(buffer) - 1, &keysym, &status);
    }
    else
    {
        len = XLookupString(&mutableEvent, buffer, sizeof(buffer) - 1, &keysym, nullptr);
    }

    if (len < 0)
        len = 0;

    buffer[len] = '\0';
    std::string typed(buffer, static_cast<std::size_t>(len));

    // Strip control characters (Enter/Backspace/Tab/... already
    // arrive as `keysym`, not as text worth inserting).
    std::string cleanTyped;
    for (char c : typed)
    {
        auto uc = static_cast<unsigned char>(c);
        if (uc >= 0x20 && uc != 0x7f)
            cleanTyped += c;
    }

    if (keysym == XK_Tab)
    {
        // Cycles focus forward: search -> fields/raw blocks in this
        // category (in layout order) -> back to search. A small
        // keyboard-only convenience on top of click-to-focus, not a
        // full tab-order implementation (Shift+Tab isn't handled -
        // going backwards isn't common enough here to be worth the
        // extra bookkeeping for a settings dialog with a handful of
        // fields per screen).
        std::vector<Field*> fields = VisibleFields();
        std::vector<RawBlockPanel*> blocks = VisibleRawBlocks();

        if (m_searchFocused)
        {
            m_searchFocused = false;

            if (!fields.empty())
                FocusField(fields.front(), true);
            else if (!blocks.empty())
                FocusRawBlock(blocks.front());
        }
        else if (m_focusedField)
        {
            auto it = std::find(fields.begin(), fields.end(), m_focusedField);
            std::size_t index = (it == fields.end()) ? fields.size() : static_cast<std::size_t>(it - fields.begin());

            if (index + 1 < fields.size())
                FocusField(fields[index + 1], true);
            else if (!blocks.empty())
                FocusRawBlock(blocks.front());
            else
                ClearFocus();
        }
        else if (m_focusedRawBlock && m_focusedRawBlock->kind != RawBlockPanel::Kind::RawText)
        {
            // A structured windowrule=/monitor= row has its own,
            // finer-grained Tab order across its individual cells -
            // see MoveStructuredFocus(). Only once that runs out (its
            // own last cell, on its own last row) does plain Tab
            // fall through to defocusing, same as every other case
            // here - there's nothing after the raw blocks to tab
            // into.
            MoveStructuredFocus(*m_focusedRawBlock, +1);
        }
        else
        {
            ClearFocus();
        }

        Redraw();
        return;
    }

    if (m_searchFocused)
        HandleSearchKey(event, keysym, cleanTyped);
    else if (m_focusedField)
        HandleFieldKey(event, keysym, cleanTyped);
    else if (m_focusedRawBlock && m_focusedRawBlock->kind != RawBlockPanel::Kind::RawText)
        HandleStructuredBlockKey(event, keysym, cleanTyped);
    else if (m_focusedRawBlock)
        HandleRawBlockKey(event, keysym, cleanTyped);
    else
        return;

    Redraw();
}

void SettingsWindow::HandleButtonPress(
    const XButtonEvent& event)
{
    Point click{event.x, event.y};

    if (event.button == Button4) { HandleScroll(-1); return; }
    if (event.button == Button5) { HandleScroll(1);  return; }

    if (event.button != Button1)
        return;

    // Sidebar
    for (std::size_t i = 0; i < m_categoryRects.size(); ++i)
    {
        if (m_categoryRects[i].Contains(click))
        {
            m_selectedCategory = static_cast<int>(i);
            m_searchText.clear();
            m_searchCaret = 0;
            m_scrollOffset = 0;
            ClearFocus();
            Redraw();
            return;
        }
    }

    // Search box
    if (m_searchRect.Contains(click))
    {
        ClearFocus();
        m_searchFocused = true;
        m_searchCaret = m_searchText.size();
        Redraw();
        return;
    }

    // Bottom bar buttons
    if (m_saveButtonRect.Contains(click))  { Save();  return; }
    if (m_resetButtonRect.Contains(click)) { ResetVisibleToDefault(); return; }

    if (!m_contentRect.Contains(click))
        return;

    for (Field* field : VisibleFields())
    {
        if (field->infoIconRect.Contains(click))
        {
            field->infoExpanded = !field->infoExpanded;
            Redraw();
            return;
        }

        if (field->widgetRect.Contains(click))
        {
            m_searchFocused = false;

            switch (field->option->type)
            {
                case ConfigValueType::Boolean:
                    ToggleBoolField(*field);
                    ClearFocus();
                    break;

                case ConfigValueType::Enum:
                    CycleEnumField(*field);
                    ClearFocus();
                    break;

                default:
                    FocusFieldAtClick(field, click);
                    break;
            }

            Redraw();
            return;
        }
    }

    for (RawBlockPanel* panel : VisibleRawBlocks())
    {
        if (panel->kind != RawBlockPanel::Kind::RawText)
        {
            if (HandleStructuredBlockClick(*panel, click))
            {
                Redraw();
                return;
            }

            continue;
        }

        if (panel->textAreaRect.Contains(click))
        {
            FocusRawBlockAtClick(panel, click);
            Redraw();
            return;
        }
    }
}

void SettingsWindow::HandleScroll(
    int direction)
{
    int maxScroll = std::max(0, m_contentTotalHeight - m_contentRect.height);
    m_scrollOffset = std::clamp(m_scrollOffset + direction * (kRowHeight * 2), 0, maxScroll);
    Redraw();
}


// --- field interaction --------------------------------------------------------

void SettingsWindow::FocusField(
    Field* field,
    bool caretAtEnd)
{
    ClearFocus();
    m_focusedField = field;
    m_fieldCaret = caretAtEnd ? field->currentValue.size() : 0;
}

void SettingsWindow::FocusRawBlock(
    RawBlockPanel* panel)
{
    ClearFocus();
    m_focusedRawBlock = panel;
    panel->caretRow = panel->lines.empty() ? 0 : panel->lines.size() - 1;
    panel->caretCol = panel->lines.empty() ? 0 : panel->lines.back().size();

    if (panel->lines.empty())
        panel->lines.emplace_back();
}

void SettingsWindow::ClearFocus()
{
    m_focusedField = nullptr;
    m_focusedRawBlock = nullptr;
    m_fieldCaret = 0;
    m_searchFocused = false;
}

std::size_t SettingsWindow::CaretIndexForClick(
    const std::string& text,
    int textStartX,
    int clickX) const
{
    std::size_t bestIndex = 0;
    int bestDistance = std::abs(clickX - textStartX);

    for (std::size_t pos = 0; pos <= text.size(); pos = Utils::Utf8NextBoundary(text, pos))
    {
        int width = textStartX + m_font.TextWidth(text.substr(0, pos));
        int distance = std::abs(clickX - width);

        // <= (not <): a click past the last character should land
        // after it, not get stuck on some earlier equally-close
        // boundary from a previous, narrower candidate.
        if (distance <= bestDistance)
        {
            bestDistance = distance;
            bestIndex = pos;
        }

        if (pos == text.size())
            break; // Utf8NextBoundary saturates at size() too, but avoid relying on that to terminate
    }

    return bestIndex;
}

void SettingsWindow::FocusFieldAtClick(
    Field* field,
    const Point& click)
{
    ClearFocus();
    m_focusedField = field;

    // Same left edge DrawField() itself starts text at - see its
    // `int textX = widgetX + 8` (plus the color swatch's extra offset
    // for ConfigValueType::Color fields specifically).
    int textX = field->widgetRect.x + 8;

    if (field->option->type == ConfigValueType::Color)
        textX = field->widgetRect.x + 4 + 18 + 8;

    m_fieldCaret = CaretIndexForClick(field->currentValue, textX, click.x);
}

void SettingsWindow::FocusRawBlockAtClick(
    RawBlockPanel* panel,
    const Point& click)
{
    ClearFocus();
    m_focusedRawBlock = panel;

    if (panel->lines.empty())
        panel->lines.emplace_back();

    // Mirrors LayoutAndDrawRawBlock()'s own line geometry exactly:
    // first line's top at textAreaRect.y + 6, kRawBlockLineH per row.
    int relativeY = click.y - (panel->textAreaRect.y + 6);
    int row = relativeY / kRawBlockLineH;

    panel->caretRow = static_cast<std::size_t>(
        std::clamp(row, 0, static_cast<int>(panel->lines.size()) - 1));

    int textX = panel->textAreaRect.x + 8;
    panel->caretCol = CaretIndexForClick(panel->lines[panel->caretRow], textX, click.x);
}

bool SettingsWindow::HandleStructuredBlockClick(
    RawBlockPanel& panel,
    const Point& click)
{
    if (!panel.rect.Contains(click))
        return false;

    if (panel.addRowButtonRect.Contains(click))
    {
        panel.lines.push_back(DefaultStructuredLine(panel.kind));
        panel.caretRow = panel.lines.size() - 1;
        m_focusedRawBlock = &panel;
        m_focusedCell = StructuredCellOrder(panel, panel.caretRow).front();
        panel.caretCol = 0;

        panel.lineErrors.assign(panel.lines.size(), std::string());

        for (std::size_t i = 0; i < panel.lines.size(); ++i)
            ValidateRawBlockLine(panel, panel.lines[i], panel.lineErrors[i]);

        return true;
    }

    for (std::size_t i = 0; i < panel.rowRects.size() && i < panel.lines.size(); ++i)
    {
        const RawBlockPanel::RowRects& row = panel.rowRects[i];

        if (row.removeRect.Contains(click))
        {
            panel.lines.erase(panel.lines.begin() + static_cast<long>(i));

            if (i < panel.lineErrors.size())
                panel.lineErrors.erase(panel.lineErrors.begin() + static_cast<long>(i));

            if (m_focusedRawBlock == &panel && panel.caretRow == i)
                ClearFocus();
            else if (m_focusedRawBlock == &panel && panel.caretRow > i)
                --panel.caretRow;

            return true;
        }

        if (panel.kind == RawBlockPanel::Kind::WindowRule)
        {
            int which = row.horizontalRect.Contains(click) ? 0
                      : row.verticalRect.Contains(click)   ? 1
                      : row.modeRect.Contains(click)       ? 2 : -1;

            if (which >= 0)
            {
                CycleStructuredChoice(panel, i, which);
                return true;
            }
        }

        struct CellHit { Rect rect; StructuredCell cell; };

        std::vector<CellHit> cellHits;

        if (panel.kind == RawBlockPanel::Kind::WindowRule)
        {
            cellHits.push_back({row.classRect, StructuredCell::Class});
            cellHits.push_back({row.instanceRect, StructuredCell::Instance});
            cellHits.push_back({row.titleRect, StructuredCell::Title});
            cellHits.push_back({row.workspaceRect, StructuredCell::Workspace});
        }
        else
        {
            cellHits.push_back({row.outputRect, StructuredCell::Output});
            cellHits.push_back({row.workspaceRect, StructuredCell::Workspace});
        }

        for (const CellHit& hit : cellHits)
        {
            if (!hit.rect.Contains(click))
                continue;

            ClearFocus();
            m_focusedRawBlock = &panel;
            panel.caretRow = i;
            m_focusedCell = hit.cell;

            std::string cellText = StructuredCellText(panel, i, hit.cell);
            panel.caretCol = CaretIndexForClick(cellText, hit.rect.x + 6, click.x);

            return true;
        }
    }

    return true; // inside the panel but not any one control - see this method's own header comment
}

void SettingsWindow::HandleFieldKey(
    const XKeyEvent& event,
    KeySym keysym,
    const std::string& typed)
{
    Field& field = *m_focusedField;
    std::string& value = field.currentValue;

    switch (keysym)
    {
        case XK_Escape:
            ClearFocus();
            return;

        case XK_Return:
        case XK_KP_Enter:
            ValidateField(field);

            if (field.option->key == "workspace.count")
                BuildWorkspaceFields();

            ClearFocus();
            return;

        case XK_BackSpace:

            if (m_fieldCaret > 0)
            {
                if (event.state & ControlMask)
                {
                    std::size_t start = value.find_last_not_of(' ', m_fieldCaret > 0 ? m_fieldCaret - 1 : 0);
                    start = (start == std::string::npos) ? 0 : value.find_last_of(' ', start);
                    start = (start == std::string::npos) ? 0 : start + 1;
                    value.erase(start, m_fieldCaret - start);
                    m_fieldCaret = start;
                }
                else
                {
                    std::size_t start = Utils::Utf8PrevBoundary(value, m_fieldCaret);
                    value.erase(start, m_fieldCaret - start);
                    m_fieldCaret = start;
                }
            }

            break;

        case XK_Delete:

            if (m_fieldCaret < value.size())
            {
                std::size_t end = Utils::Utf8NextBoundary(value, m_fieldCaret);
                value.erase(m_fieldCaret, end - m_fieldCaret);
            }

            break;

        case XK_Left:

            if (m_fieldCaret > 0)
                m_fieldCaret = Utils::Utf8PrevBoundary(value, m_fieldCaret);

            break;

        case XK_Right:

            if (m_fieldCaret < value.size())
                m_fieldCaret = Utils::Utf8NextBoundary(value, m_fieldCaret);

            break;

        case XK_Home:
            m_fieldCaret = 0;
            break;

        case XK_End:
            m_fieldCaret = value.size();
            break;

        default:

            for (char c : typed)
            {
                value.insert(value.begin() + static_cast<long>(m_fieldCaret), c);
                ++m_fieldCaret;
            }

            break;
    }

    ValidateField(field);
}

void SettingsWindow::HandleRawBlockKey(
    const XKeyEvent&,
    KeySym keysym,
    const std::string& typed)
{
    RawBlockPanel& panel = *m_focusedRawBlock;

    if (panel.lines.empty())
        panel.lines.emplace_back();

    std::string& line = panel.lines[panel.caretRow];

    switch (keysym)
    {
        case XK_Escape:
            ClearFocus();
            return;

        case XK_Return:
        case XK_KP_Enter:
        {
            std::string tail = line.substr(panel.caretCol);
            line.erase(panel.caretCol);
            panel.lines.insert(panel.lines.begin() + static_cast<long>(panel.caretRow) + 1, tail);
            ++panel.caretRow;
            panel.caretCol = 0;
            break;
        }

        case XK_BackSpace:

            if (panel.caretCol > 0)
            {
                std::size_t start = Utils::Utf8PrevBoundary(line, panel.caretCol);
                line.erase(start, panel.caretCol - start);
                panel.caretCol = start;
            }
            else if (panel.caretRow > 0)
            {
                std::size_t previousLen = panel.lines[panel.caretRow - 1].size();
                panel.lines[panel.caretRow - 1] += line;
                panel.lines.erase(panel.lines.begin() + static_cast<long>(panel.caretRow));
                --panel.caretRow;
                panel.caretCol = previousLen;
            }

            break;

        case XK_Delete:

            if (panel.caretCol < line.size())
            {
                std::size_t end = Utils::Utf8NextBoundary(line, panel.caretCol);
                line.erase(panel.caretCol, end - panel.caretCol);
            }
            else if (panel.caretRow + 1 < panel.lines.size())
            {
                line += panel.lines[panel.caretRow + 1];
                panel.lines.erase(panel.lines.begin() + static_cast<long>(panel.caretRow) + 1);
            }

            break;

        case XK_Left:

            if (panel.caretCol > 0)
            {
                panel.caretCol = Utils::Utf8PrevBoundary(line, panel.caretCol);
            }
            else if (panel.caretRow > 0)
            {
                --panel.caretRow;
                panel.caretCol = panel.lines[panel.caretRow].size();
            }

            break;

        case XK_Right:

            if (panel.caretCol < line.size())
            {
                panel.caretCol = Utils::Utf8NextBoundary(line, panel.caretCol);
            }
            else if (panel.caretRow + 1 < panel.lines.size())
            {
                ++panel.caretRow;
                panel.caretCol = 0;
            }

            break;

        case XK_Up:

            if (panel.caretRow > 0)
            {
                --panel.caretRow;
                panel.caretCol = Utils::Utf8ClampToBoundary(panel.lines[panel.caretRow], panel.caretCol);
            }

            break;

        case XK_Down:

            if (panel.caretRow + 1 < panel.lines.size())
            {
                ++panel.caretRow;
                panel.caretCol = Utils::Utf8ClampToBoundary(panel.lines[panel.caretRow], panel.caretCol);
            }

            break;

        case XK_Home:
            panel.caretCol = 0;
            break;

        case XK_End:
            panel.caretCol = panel.lines[panel.caretRow].size();
            break;

        default:

            for (char c : typed)
            {
                panel.lines[panel.caretRow].insert(
                    panel.lines[panel.caretRow].begin() + static_cast<long>(panel.caretCol), c);
                ++panel.caretCol;
            }

            break;
    }

    panel.lineErrors.assign(panel.lines.size(), "");

    for (std::size_t i = 0; i < panel.lines.size(); ++i)
        ValidateRawBlockLine(panel, panel.lines[i], panel.lineErrors[i]);
}

void SettingsWindow::HandleStructuredBlockKey(
    const XKeyEvent&,
    KeySym keysym,
    const std::string& typed)
{
    RawBlockPanel& panel = *m_focusedRawBlock;

    if (panel.lines.empty())
        panel.lines.push_back(DefaultStructuredLine(panel.kind));

    if (panel.caretRow >= panel.lines.size())
        panel.caretRow = panel.lines.size() - 1;

    if (keysym == XK_Escape)
    {
        ClearFocus();
        return;
    }

    if (keysym == XK_Return || keysym == XK_KP_Enter)
    {
        // Mirrors the raw editor's own Enter-adds-a-line behavior,
        // but as a whole new *rule* rather than splitting text - a
        // structured row has no equivalent of "split this class
        // pattern in two".
        panel.lines.insert(
            panel.lines.begin() + static_cast<long>(panel.caretRow) + 1,
            DefaultStructuredLine(panel.kind));

        ++panel.caretRow;
        m_focusedCell = StructuredCellOrder(panel, panel.caretRow).front();
        panel.caretCol = 0;

        panel.lineErrors.assign(panel.lines.size(), std::string());

        for (std::size_t i = 0; i < panel.lines.size(); ++i)
            ValidateRawBlockLine(panel, panel.lines[i], panel.lineErrors[i]);

        return;
    }

    // Tab never reaches here - it's intercepted by HandleKeyPress()'s
    // own top-level dispatcher (see its comment) before this function
    // is even called, exactly like it already was for every other
    // focus target.

    std::string cellText = StructuredCellText(panel, panel.caretRow, m_focusedCell);

    switch (keysym)
    {
        case XK_BackSpace:
            if (panel.caretCol > 0)
            {
                std::size_t start = Utils::Utf8PrevBoundary(cellText, panel.caretCol);
                cellText.erase(start, panel.caretCol - start);
                panel.caretCol = start;
            }
            break;

        case XK_Delete:
            if (panel.caretCol < cellText.size())
            {
                std::size_t end = Utils::Utf8NextBoundary(cellText, panel.caretCol);
                cellText.erase(panel.caretCol, end - panel.caretCol);
            }
            break;

        case XK_Left:
            if (panel.caretCol > 0)
                panel.caretCol = Utils::Utf8PrevBoundary(cellText, panel.caretCol);
            else
                MoveStructuredFocus(panel, -1);
            return; // MoveStructuredFocus already leaves the panel in a fully valid state

        case XK_Right:
            if (panel.caretCol < cellText.size())
                panel.caretCol = Utils::Utf8NextBoundary(cellText, panel.caretCol);
            else
                MoveStructuredFocus(panel, +1);
            return;

        case XK_Home:
            panel.caretCol = 0;
            break;

        case XK_End:
            panel.caretCol = cellText.size();
            break;

        default:
            for (char c : typed)
            {
                // Neither windowrule= nor monitor='s grammar can
                // represent a space inside one whitespace-delimited
                // token, so it's silently swallowed here rather than
                // producing a value that would come back split/
                // mangled the next time this line gets parsed.
                if (c == ' ')
                    continue;

                // The two numeric-only cells (windowrule='s
                // workspace:<N> action and monitor='s workspace=<N>)
                // only ever accept digits.
                if (m_focusedCell == StructuredCell::Workspace &&
                    !std::isdigit(static_cast<unsigned char>(c)))
                    continue;

                cellText.insert(cellText.begin() + static_cast<long>(panel.caretCol), c);
                ++panel.caretCol;
            }
            break;
    }

    SetStructuredCellText(panel, panel.caretRow, m_focusedCell, cellText);

    panel.lineErrors.assign(panel.lines.size(), std::string());

    for (std::size_t i = 0; i < panel.lines.size(); ++i)
        ValidateRawBlockLine(panel, panel.lines[i], panel.lineErrors[i]);
}

void SettingsWindow::HandleSearchKey(
    const XKeyEvent&,
    KeySym keysym,
    const std::string& typed)
{
    switch (keysym)
    {
        case XK_Escape:
            m_searchText.clear();
            m_searchCaret = 0;
            m_searchFocused = false;
            return;

        case XK_Return:
        case XK_KP_Enter:
            m_searchFocused = false;
            return;

        case XK_BackSpace:

            if (m_searchCaret > 0)
            {
                std::size_t start = Utils::Utf8PrevBoundary(m_searchText, m_searchCaret);
                m_searchText.erase(start, m_searchCaret - start);
                m_searchCaret = start;
            }

            break;

        case XK_Delete:

            if (m_searchCaret < m_searchText.size())
            {
                std::size_t end = Utils::Utf8NextBoundary(m_searchText, m_searchCaret);
                m_searchText.erase(m_searchCaret, end - m_searchCaret);
            }

            break;

        case XK_Left:

            if (m_searchCaret > 0)
                m_searchCaret = Utils::Utf8PrevBoundary(m_searchText, m_searchCaret);

            break;

        case XK_Right:

            if (m_searchCaret < m_searchText.size())
                m_searchCaret = Utils::Utf8NextBoundary(m_searchText, m_searchCaret);

            break;

        case XK_Home:
            m_searchCaret = 0;
            break;

        case XK_End:
            m_searchCaret = m_searchText.size();
            break;

        default:

            for (char c : typed)
            {
                m_searchText.insert(m_searchText.begin() + static_cast<long>(m_searchCaret), c);
                ++m_searchCaret;
            }

            m_scrollOffset = 0;
            break;
    }
}

void SettingsWindow::ToggleBoolField(
    Field& field) const
{
    field.currentValue = (field.currentValue == "true") ? "false" : "true";
}

void SettingsWindow::CycleEnumField(
    Field& field) const
{
    const std::vector<std::string>& values = field.option->enumValues;

    if (values.empty())
        return;

    auto it = std::find(values.begin(), values.end(), field.currentValue);
    std::size_t index = (it == values.end()) ? 0 : static_cast<std::size_t>(it - values.begin()) + 1;

    field.currentValue = values[index % values.size()];
}

void SettingsWindow::ValidateField(
    Field& field) const
{
    const std::string& value = field.currentValue;
    field.error.clear();

    switch (field.option->type)
    {
        case ConfigValueType::Int:
        {
            if (value.empty())
            {
                field.error = "Required";
                break;
            }

            std::size_t pos = 0;

            try
            {
                std::stoi(value, &pos);
            }
            catch (...)
            {
                field.error = "Not a whole number";
                break;
            }

            if (pos != value.size())
                field.error = "Not a whole number";

            break;
        }

        case ConfigValueType::Float:
        {
            if (value.empty())
            {
                field.error = "Required";
                break;
            }

            std::size_t pos = 0;

            try
            {
                std::stof(value, &pos);
            }
            catch (...)
            {
                field.error = "Not a number";
                break;
            }

            if (pos != value.size())
                field.error = "Not a number";

            break;
        }

        case ConfigValueType::Percent:
        {
            if (value.empty() || value.back() != '%')
            {
                field.error = "Expected a value like \"70%\"";
                break;
            }

            std::string number = value.substr(0, value.size() - 1);
            std::size_t pos = 0;

            try
            {
                std::stof(number, &pos);
            }
            catch (...)
            {
                field.error = "Expected a value like \"70%\"";
                break;
            }

            if (pos != number.size() || number.empty())
                field.error = "Expected a value like \"70%\"";

            break;
        }

        case ConfigValueType::Color:
        {
            bool ok = value.size() == 8 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X');

            if (ok)
            {
                for (std::size_t i = 2; i < value.size() && ok; ++i)
                    ok = static_cast<bool>(std::isxdigit(static_cast<unsigned char>(value[i])));
            }

            if (!ok)
                field.error = "Expected a value like \"0x89b4fa\"";

            break;
        }

        case ConfigValueType::Boolean:
        case ConfigValueType::Enum:
        case ConfigValueType::String:
        default:
            break; // click-driven (Bool/Enum) or free-form (String) - always valid
    }
}

void SettingsWindow::ValidateRawBlockLine(
    const RawBlockPanel& panel,
    const std::string& rawLine,
    std::string& outError) const
{
    outError.clear();

    std::string line = Utils::Trim(rawLine);

    if (line.empty())
        return; // blank lines are just spacing, not an entry

    if (panel.id == "windowrule")
    {
        // The window manager's own parser decides: whatever it would
        // refuse to load (and log a warning for) is flagged right here,
        // with its own wording, so this editor and Kohiko can never
        // disagree about what a rule means.
        WindowRule parsed;
        std::string error;

        if (!WindowRule::Parse(line, parsed, &error))
            outError = error.empty() ? std::string("Not a valid rule") : error;
    }
    else if (panel.id == "monitor")
    {
        if (line.find(',') == std::string::npos || line.find("workspace=") == std::string::npos)
            outError = "Expected \"<output>,workspace=<N>\"";
    }
    else if (panel.id == "bind")
    {
        std::istringstream stream(line);
        std::string combo, command;
        stream >> combo >> command;

        if (combo.empty() || command.empty())
            outError = "Expected \"<MODS+KEY> <command>\"";
    }
    else if (panel.id == "exec")
    {
        std::size_t eq = line.find('=');

        if (eq == std::string::npos || eq == 0)
            outError = "Expected \"<name>=<command>\"";
    }
}

SettingsWindow::WindowRuleDraft SettingsWindow::ParseWindowRuleDraft(
    const std::string& line) const
{
    WindowRuleDraft draft;

    for (const std::string& token : Utils::SplitWhitespace(line))
    {
        if (token[0] == '#')
            break; // inline comment, same as WindowRule::Parse()

        std::string lower = Utils::Lower(token);

        // Legacy bare words.
        if (lower == "tile")         { draft.modeIndex = 1; continue; }
        if (lower == "float")        { draft.modeIndex = 2; continue; }
        if (lower == "fullscreen")   { draft.modeIndex = 3; continue; }
        if (lower == "nofullscreen") { draft.denyFullscreen = true; continue; }

        std::size_t colon = token.find(':');

        // Anything else this lenient *editing* parser doesn't recognise
        // (mid-edit, or a hand-typed line this editor's never seen)
        // is simply not modelled - strictness is
        // ValidateRawBlockLine()'s job, via WindowRule::Parse().
        if (colon == std::string::npos || colon == 0)
            continue;

        std::string key = Utils::Lower(token.substr(0, colon));
        std::string raw = token.substr(colon + 1);

        // Selector text keeps the case it was typed in (see
        // WindowRuleDraft's comment) and shows %XX escapes decoded.
        if (key == "app")                draft.appText = WindowRule::DecodeValue(raw);
        else if (key == "class")         draft.classText = WindowRule::DecodeValue(raw);
        else if (key == "instance")      draft.instanceText = WindowRule::DecodeValue(raw);
        else if (key == "title")         draft.titleText = WindowRule::DecodeValue(raw);
        else if (key == "role")          draft.roleText = WindowRule::DecodeValue(raw);
        else if (key == "type")          draft.typeText = raw;
        else if (key == "workspace")     draft.workspaceText = raw;
        else if (key == "horizontal")
        {
            std::string v = Utils::Lower(raw);
            for (int i = 1; i < 3; ++i) if (v == kHorizontalTokens[i]) draft.horizontalIndex = i;
        }
        else if (key == "vertical")
        {
            std::string v = Utils::Lower(raw);
            for (int i = 1; i < 3; ++i) if (v == kVerticalTokens[i]) draft.verticalIndex = i;
        }
        else if (key == "mode")
        {
            std::string v = Utils::Lower(raw);
            for (int i = 1; i < 4; ++i) if (v == kModeTokens[i]) draft.modeIndex = i;
        }
    }

    return draft;
}

std::string SettingsWindow::SerializeWindowRuleDraft(
    const WindowRuleDraft& draft) const
{
    std::string line;

    auto add = [&](const std::string& token)
    {
        if (!line.empty())
            line += ' ';

        line += token;
    };

    // Canonical order, identical to WindowRule::Serialize(): selectors,
    // then workspace, horizontal, vertical, mode. Unlike that function
    // this never refuses a half-finished row - it writes whatever is
    // there, and ValidateRawBlockLine() reports what's missing.
    if (!draft.appText.empty())      add("app:" + WindowRule::EncodeValue(draft.appText));
    if (!draft.classText.empty())    add("class:" + WindowRule::EncodeValue(draft.classText));
    if (!draft.instanceText.empty()) add("instance:" + WindowRule::EncodeValue(draft.instanceText));
    if (!draft.roleText.empty())     add("role:" + WindowRule::EncodeValue(draft.roleText));
    if (!draft.typeText.empty())     add("type:" + draft.typeText);
    if (!draft.titleText.empty())    add("title:" + WindowRule::EncodeValue(draft.titleText));

    if (!draft.workspaceText.empty()) add("workspace:" + draft.workspaceText);
    if (draft.horizontalIndex > 0)    add(std::string("horizontal:") + kHorizontalTokens[draft.horizontalIndex]);
    if (draft.verticalIndex > 0)      add(std::string("vertical:") + kVerticalTokens[draft.verticalIndex]);
    if (draft.modeIndex > 0)          add(std::string("mode:") + kModeTokens[draft.modeIndex]);
    if (draft.denyFullscreen)         add("nofullscreen");

    return line;
}

SettingsWindow::MonitorRuleDraft SettingsWindow::ParseMonitorRuleDraft(
    const std::string& line) const
{
    MonitorRuleDraft draft;

    std::size_t comma = line.find(',');

    draft.outputText = Utils::Trim(comma == std::string::npos ? line : line.substr(0, comma));

    if (comma != std::string::npos)
    {
        std::string rest = line.substr(comma + 1);
        std::size_t eq = rest.find("workspace=");

        if (eq != std::string::npos)
            draft.workspaceText = Utils::Trim(rest.substr(eq + std::strlen("workspace=")));
    }

    return draft;
}

std::string SettingsWindow::SerializeMonitorRuleDraft(
    const MonitorRuleDraft& draft) const
{
    return draft.outputText + ",workspace=" + draft.workspaceText;
}

std::string SettingsWindow::StructuredCellText(
    const RawBlockPanel& panel,
    std::size_t row,
    StructuredCell cell) const
{
    if (row >= panel.lines.size())
        return std::string();

    if (panel.kind == RawBlockPanel::Kind::Monitor)
    {
        MonitorRuleDraft draft = ParseMonitorRuleDraft(panel.lines[row]);
        return (cell == StructuredCell::Workspace) ? draft.workspaceText : draft.outputText;
    }

    WindowRuleDraft draft = ParseWindowRuleDraft(panel.lines[row]);

    switch (cell)
    {
        case StructuredCell::Class:     return draft.firstCellIsClass() ? draft.classText : draft.appText;
        case StructuredCell::Instance:  return draft.instanceText;
        case StructuredCell::Title:     return draft.titleText;
        case StructuredCell::Workspace: return draft.workspaceText;
        default:                        return std::string();
    }
}

void SettingsWindow::SetStructuredCellText(
    RawBlockPanel& panel,
    std::size_t row,
    StructuredCell cell,
    const std::string& value)
{
    if (row >= panel.lines.size())
        return;

    if (panel.kind == RawBlockPanel::Kind::Monitor)
    {
        MonitorRuleDraft draft = ParseMonitorRuleDraft(panel.lines[row]);

        if (cell == StructuredCell::Workspace) draft.workspaceText = value;
        else                                   draft.outputText = value;

        panel.lines[row] = SerializeMonitorRuleDraft(draft);
        return;
    }

    WindowRuleDraft draft = ParseWindowRuleDraft(panel.lines[row]);

    switch (cell)
    {
        case StructuredCell::Class:
            // The first cell edits whichever selector this row already
            // uses: the exact `app:` key (the default, and what new rows
            // get) or - only for a hand-written legacy rule - `class:`.
            if (draft.firstCellIsClass()) draft.classText = value;
            else                          draft.appText = value;
            break;
        case StructuredCell::Instance:  draft.instanceText = value;  break;
        case StructuredCell::Title:     draft.titleText = value;     break;
        case StructuredCell::Workspace: draft.workspaceText = value; break;
        default: break;
    }

    panel.lines[row] = SerializeWindowRuleDraft(draft);
}

std::vector<SettingsWindow::StructuredCell> SettingsWindow::StructuredCellOrder(
    const RawBlockPanel& panel,
    std::size_t row) const
{
    if (panel.kind == RawBlockPanel::Kind::Monitor)
        return {StructuredCell::Output, StructuredCell::Workspace};

    (void)row;

    return {StructuredCell::Class, StructuredCell::Instance, StructuredCell::Title, StructuredCell::Workspace};
}

void SettingsWindow::MoveStructuredFocus(
    RawBlockPanel& panel,
    int direction)
{
    std::vector<StructuredCell> order = StructuredCellOrder(panel, panel.caretRow);
    auto it = std::find(order.begin(), order.end(), m_focusedCell);
    std::size_t index = (it != order.end()) ? static_cast<std::size_t>(it - order.begin()) : 0;

    if (direction > 0)
    {
        if (index + 1 < order.size())
        {
            m_focusedCell = order[index + 1];
            panel.caretCol = 0;
            return;
        }

        if (panel.caretRow + 1 < panel.lines.size())
        {
            ++panel.caretRow;
            m_focusedCell = StructuredCellOrder(panel, panel.caretRow).front();
            panel.caretCol = 0;
        }

        return;
    }

    if (index > 0)
    {
        m_focusedCell = order[index - 1];
        panel.caretCol = StructuredCellText(panel, panel.caretRow, m_focusedCell).size();
        return;
    }

    if (panel.caretRow > 0)
    {
        --panel.caretRow;
        m_focusedCell = StructuredCellOrder(panel, panel.caretRow).back();
        panel.caretCol = StructuredCellText(panel, panel.caretRow, m_focusedCell).size();
    }
}

void SettingsWindow::CycleStructuredChoice(
    RawBlockPanel& panel,
    std::size_t row,
    int which)
{
    if (row >= panel.lines.size())
        return;

    WindowRuleDraft draft = ParseWindowRuleDraft(panel.lines[row]);

    if (which == 0)      draft.horizontalIndex = (draft.horizontalIndex + 1) % 3;
    else if (which == 1) draft.verticalIndex   = (draft.verticalIndex + 1) % 3;
    else                 draft.modeIndex       = (draft.modeIndex + 1) % 4;

    panel.lines[row] = SerializeWindowRuleDraft(draft);

    panel.lineErrors.assign(panel.lines.size(), std::string());

    for (std::size_t i = 0; i < panel.lines.size(); ++i)
        ValidateRawBlockLine(panel, panel.lines[i], panel.lineErrors[i]);
}

std::string SettingsWindow::DefaultStructuredLine(
    RawBlockPanel::Kind kind) const
{
    // A new window rule starts with every choice at Default and no
    // application yet - an empty line. (Blank lines are just spacing
    // as far as validation and saving go, so an untouched new row
    // neither errors nor gets written.)
    return (kind == RawBlockPanel::Kind::Monitor) ? std::string(",workspace=1") : std::string();
}

bool SettingsWindow::FieldDirty(
    const Field& field) const
{
    return field.currentValue != field.loadedValue;
}

bool SettingsWindow::RawBlockDirty(
    const RawBlockPanel& panel) const
{
    return panel.lines != panel.loadedLines;
}

bool SettingsWindow::AnyDirty() const
{
    for (const Field& field : m_fields)
        if (FieldDirty(field)) return true;

    for (const Field& field : m_workspaceFields)
        if (FieldDirty(field)) return true;

    for (const RawBlockPanel& panel : m_rawBlocks)
        if (RawBlockDirty(panel)) return true;

    return false;
}


// --- commands -----------------------------------------------------------------

void SettingsWindow::Save()
{
    int applied = 0;
    int skipped = 0;

    auto applyField = [&](Field& field)
    {
        if (!FieldDirty(field))
            return;

        ValidateField(field);

        if (!field.error.empty())
        {
            ++skipped;
            return;
        }

        m_writer.SetScalar(field.option->key, field.currentValue);
        field.loadedValue = field.currentValue;
        ++applied;
    };

    for (Field& field : m_fields)
        applyField(field);

    for (Field& field : m_workspaceFields)
        applyField(field);

    for (RawBlockPanel& panel : m_rawBlocks)
    {
        if (!RawBlockDirty(panel))
            continue;

        bool anyError = std::any_of(
            panel.lineErrors.begin(), panel.lineErrors.end(),
            [](const std::string& e) { return !e.empty(); });

        if (anyError)
        {
            ++skipped;
            continue;
        }

        std::vector<std::string> nonEmpty;

        for (const std::string& line : panel.lines)
            if (!Utils::Trim(line).empty())
                nonEmpty.push_back(line);

        m_writer.ReplaceRawBlock(panel.prefix, nonEmpty);
        panel.loadedLines = panel.lines;
        ++applied;
    }

    if (!m_writer.Save())
    {
        ShowStatus("Couldn't write " + m_configPath + " - check its permissions.");
        return;
    }

    // workspace.count itself may just have been saved - regenerate
    // the per-workspace fields to match before the next redraw.
    BuildWorkspaceFields();

    ReloadRunningKohiko();

    std::ostringstream message;

    if (applied == 0 && skipped == 0)
        message << "Nothing to save.";
    else
    {
        message << "Saved " << applied << " change" << (applied == 1 ? "" : "s") << ".";

        if (skipped > 0)
            message << " " << skipped << " field" << (skipped == 1 ? " has" : "s have")
                    << " an error and " << (skipped == 1 ? "was" : "were") << " skipped.";
    }

    // Deliberately no m_running = false here - Save keeps the window
    // open (see this function's own header-comment: it replaces both
    // the old Apply and Save actions, and the whole point of the
    // merge was that you should never have to leave Settings, or
    // restart Kohiko, just to confirm a change actually took).
    ShowStatus(message.str());
}

void SettingsWindow::ResetVisibleToDefault()
{
    for (Field* field : VisibleFields())
    {
        field->currentValue = field->option->defaultValue;

        if (field->option->type == ConfigValueType::Boolean)
        {
            bool defaultBool = (field->option->defaultValue == "true");
            field->currentValue = defaultBool ? "true" : "false";
        }

        ValidateField(*field);
    }

    for (RawBlockPanel* panel : VisibleRawBlocks())
    {
        panel->lines.clear();
        panel->lineErrors.clear();
    }

    ShowStatus("Reset to defaults - not yet saved. Click Save to write to disk.");
}

void SettingsWindow::ReloadRunningKohiko() const
{
    // Best-effort: kohikoctl silently no-ops (nonzero exit, ignored
    // here) if Kohiko isn't actually running - e.g. someone poking at
    // Kohiko Settings without a Kohiko session up at all. Reusing
    // kohikoctl rather than talking IPCServer's protocol directly
    // keeps this file from needing to know that protocol at all.
    pid_t pid = fork();

    if (pid == 0)
    {
        execlp("kohikoctl", "kohikoctl", "reload", static_cast<char*>(nullptr));
        _exit(127);
    }
    else if (pid > 0)
    {
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

void SettingsWindow::ShowStatus(
    const std::string& message)
{
    m_statusMessage = message;
    Redraw();
}


// --- visible-set helpers --------------------------------------------------------

std::vector<SettingsWindow::Field*> SettingsWindow::VisibleFields()
{
    std::vector<Field*> result;
    std::string needle = Utils::Lower(m_searchText);
    std::string currentCategory = m_categories[static_cast<std::size_t>(m_selectedCategory)].name;

    auto matches = [&](const Field& f) -> bool
    {
        if (needle.empty())
            return f.option->category == currentCategory;

        return ContainsCaseInsensitive(f.option->key, needle) ||
               ContainsCaseInsensitive(HumanizeKey(f.option->key), needle) ||
               ContainsCaseInsensitive(f.option->description, needle) ||
               ContainsCaseInsensitive(f.option->category, needle) ||
               ContainsCaseInsensitive(f.option->group, needle);
    };

    for (Field& f : m_fields)
        if (matches(f)) result.push_back(&f);

    for (Field& f : m_workspaceFields)
        if (matches(f)) result.push_back(&f);

    return result;
}

std::vector<SettingsWindow::RawBlockPanel*> SettingsWindow::VisibleRawBlocks()
{
    std::vector<RawBlockPanel*> result;
    std::string needle = Utils::Lower(m_searchText);
    std::string currentCategory = m_categories[static_cast<std::size_t>(m_selectedCategory)].name;

    for (RawBlockPanel& panel : m_rawBlocks)
    {
        bool show = needle.empty()
            ? panel.category == currentCategory
            : (ContainsCaseInsensitive(panel.category, needle) ||
               ContainsCaseInsensitive(panel.title, needle) ||
               ContainsCaseInsensitive(panel.helpText, needle));

        if (show)
            result.push_back(&panel);
    }

    return result;
}

// --- drawing primitives -------------------------------------------------------

void SettingsWindow::DrawText(
    int x,
    int y,
    const std::string& text,
    unsigned long pixel)
{
    TextColor color(m_display, DefaultVisual(m_display, m_screen), DefaultColormap(m_display, m_screen), pixel);
    m_font.DrawString(m_xftDraw, x, y, text, color.Get());
}

void SettingsWindow::DrawCheckbox(
    const Rect& rect,
    bool checked)
{
    XSetForeground(m_display, m_gc, m_fieldPixel);
    XFillRectangle(m_display, m_window, m_gc, rect.x, rect.y, static_cast<unsigned int>(rect.width), static_cast<unsigned int>(rect.height));

    XSetForeground(m_display, m_gc, checked ? m_accentPixel : m_borderPixel);
    XDrawRectangle(m_display, m_window, m_gc, rect.x, rect.y, static_cast<unsigned int>(rect.width - 1), static_cast<unsigned int>(rect.height - 1));

    if (checked)
    {
        int pad = 4;
        XSetForeground(m_display, m_gc, m_accentPixel);
        XFillRectangle(
            m_display, m_window, m_gc,
            rect.x + pad, rect.y + pad,
            static_cast<unsigned int>(std::max(1, rect.width - 2 * pad)),
            static_cast<unsigned int>(std::max(1, rect.height - 2 * pad)));
    }
}

void SettingsWindow::DrawButton(
    const Rect& rect,
    const std::string& label,
    bool enabled,
    bool primary)
{
    unsigned long background = !enabled ? m_panelPixel : (primary ? m_accentPixel : m_fieldPixel);

    XSetForeground(m_display, m_gc, background);
    XFillRectangle(m_display, m_window, m_gc, rect.x, rect.y, static_cast<unsigned int>(rect.width), static_cast<unsigned int>(rect.height));

    XSetForeground(m_display, m_gc, m_borderPixel);
    XDrawRectangle(m_display, m_window, m_gc, rect.x, rect.y, static_cast<unsigned int>(rect.width - 1), static_cast<unsigned int>(rect.height - 1));

    unsigned long textPixel = !enabled ? m_mutedPixel : (primary ? m_backgroundPixel : m_foregroundPixel);
    int textWidth = m_font.TextWidth(label);
    int tx = rect.x + (rect.width - textWidth) / 2;
    int ty = rect.y + rect.height / 2 + m_font.Ascent() / 2;
    DrawText(tx, ty, label, textPixel);
}

std::vector<std::string> SettingsWindow::WrapText(
    const std::string& text,
    int maxWidth) const
{
    std::vector<std::string> lines;
    std::istringstream words(text);
    std::string word, current;

    while (words >> word)
    {
        std::string candidate = current.empty() ? word : current + " " + word;

        if (!current.empty() && m_font.TextWidth(candidate) > maxWidth)
        {
            lines.push_back(current);
            current = word;
        }
        else
        {
            current = candidate;
        }
    }

    if (!current.empty())
        lines.push_back(current);

    return lines;
}


// --- layout & drawing: whole-window passes --------------------------------------

void SettingsWindow::Redraw()
{
    if (!m_xftDraw || !m_gc)
        return;

    XSetForeground(m_display, m_gc, m_backgroundPixel);
    XFillRectangle(m_display, m_window, m_gc, 0, 0, static_cast<unsigned int>(m_geometry.width), static_cast<unsigned int>(m_geometry.height));

    int sidebarWidth = 0;
    LayoutAndDrawSidebar(sidebarWidth);
    LayoutAndDrawTopBar(sidebarWidth);
    LayoutAndDrawContent(sidebarWidth);
    LayoutAndDrawBottomBar();

    XFlush(m_display);
}

void SettingsWindow::LayoutAndDrawSidebar(
    int& outWidth)
{
    int width = std::clamp(m_geometry.width / 5, 140, 220);
    outWidth = width;

    m_sidebarRect = Rect{0, 0, width, m_geometry.height};

    XSetForeground(m_display, m_gc, m_panelPixel);
    XFillRectangle(m_display, m_window, m_gc, 0, 0, static_cast<unsigned int>(width), static_cast<unsigned int>(m_geometry.height));

    m_categoryRects.clear();

    int y = 12;
    int rowH = 34;

    for (std::size_t i = 0; i < m_categories.size(); ++i)
    {
        m_categoryRects.push_back(Rect{0, y, width, rowH});

        bool selected = (static_cast<int>(i) == m_selectedCategory) && m_searchText.empty();

        if (selected)
        {
            XSetForeground(m_display, m_gc, m_accentPixel);
            XFillRectangle(m_display, m_window, m_gc, 0, y, 3, static_cast<unsigned int>(rowH));
        }

        unsigned long textPixel = selected ? m_foregroundPixel : m_mutedPixel;
        DrawText(16, y + rowH / 2 + m_font.Ascent() / 2, m_categories[i].name, textPixel);

        y += rowH;
    }
}

void SettingsWindow::LayoutAndDrawTopBar(
    int sidebarWidth)
{
    m_searchRect = Rect{sidebarWidth + 16, 10, std::max(100, m_geometry.width - sidebarWidth - 32), kTopBarHeight - 20};

    XSetForeground(m_display, m_gc, m_fieldPixel);
    XFillRectangle(m_display, m_window, m_gc, m_searchRect.x, m_searchRect.y, static_cast<unsigned int>(m_searchRect.width), static_cast<unsigned int>(m_searchRect.height));

    XSetForeground(m_display, m_gc, m_searchFocused ? m_accentPixel : m_borderPixel);
    XDrawRectangle(m_display, m_window, m_gc, m_searchRect.x, m_searchRect.y, static_cast<unsigned int>(m_searchRect.width - 1), static_cast<unsigned int>(m_searchRect.height - 1));

    bool showPlaceholder = m_searchText.empty() && !m_searchFocused;
    std::string display = showPlaceholder ? "Search settings..." : m_searchText;
    unsigned long pixel = showPlaceholder ? m_mutedPixel : m_foregroundPixel;

    int baseline = m_searchRect.y + m_searchRect.height / 2 + m_font.Ascent() / 2;
    DrawText(m_searchRect.x + 10, baseline, display, pixel);

    if (m_searchFocused)
    {
        int caretX = m_searchRect.x + 10 + m_font.TextWidth(m_searchText.substr(0, m_searchCaret));
        XSetForeground(m_display, m_gc, m_foregroundPixel);
        XFillRectangle(m_display, m_window, m_gc, caretX, m_searchRect.y + 6, 2, static_cast<unsigned int>(m_searchRect.height - 12));
    }
}

void SettingsWindow::LayoutAndDrawContent(
    int sidebarWidth)
{
    m_contentRect = Rect{
        sidebarWidth, kTopBarHeight,
        m_geometry.width - sidebarWidth,
        m_geometry.height - kTopBarHeight - kBottomBarHeight};

    XRectangle clip{
        static_cast<short>(m_contentRect.x), static_cast<short>(m_contentRect.y),
        static_cast<unsigned short>(std::max(0, m_contentRect.width)),
        static_cast<unsigned short>(std::max(0, m_contentRect.height))};

    XSetClipRectangles(m_display, m_gc, 0, 0, &clip, 1, Unsorted);
    XftDrawSetClipRectangles(m_xftDraw, 0, 0, &clip, 1);

    int x = m_contentRect.x + kRowPaddingX;
    int width = std::max(100, m_contentRect.width - kRowPaddingX * 2);
    int y = m_contentRect.y - m_scrollOffset + 12;

    bool searching = !m_searchText.empty();
    std::string lastGroup;
    std::string lastCategory;

    std::vector<Field*> fields = VisibleFields();
    std::vector<RawBlockPanel*> blocks = VisibleRawBlocks();

    for (Field* field : fields)
    {
        if (searching && field->option->category != lastCategory)
        {
            lastCategory = field->option->category;
            lastGroup.clear();
            DrawText(x, y + m_boldFont.Ascent(), lastCategory, m_accentPixel);
            y += kGroupHeaderGap;
        }

        if (!field->option->group.empty() && field->option->group != lastGroup)
        {
            lastGroup = field->option->group;
            DrawText(x, y + m_font.Ascent(), lastGroup, m_mutedPixel);
            y += kGroupHeaderGap - 4;
        }
        else if (field->option->group.empty())
        {
            lastGroup.clear();
        }

        int rowHeight = 0;
        LayoutAndDrawField(*field, x, y, width, rowHeight);
        y += rowHeight;
    }

    for (RawBlockPanel* panel : blocks)
    {
        if (searching && panel->category != lastCategory)
        {
            lastCategory = panel->category;
            DrawText(x, y + m_boldFont.Ascent(), lastCategory, m_accentPixel);
            y += kGroupHeaderGap;
        }

        int rowHeight = 0;
        LayoutAndDrawRawBlock(*panel, x, y, width, rowHeight);
        y += rowHeight;
    }

    m_contentTotalHeight = (y + m_scrollOffset) - m_contentRect.y;

    XSetClipMask(m_display, m_gc, None);
    XftDrawSetClip(m_xftDraw, None);
}

void SettingsWindow::LayoutAndDrawField(
    Field& field,
    int x,
    int y,
    int width,
    int& outHeight)
{
    int rowTop = y;
    int rowH = kRowHeight;

    field.infoIconRect = Rect{x, y + (rowH - kInfoIconSize) / 2, kInfoIconSize, kInfoIconSize};

    XSetForeground(m_display, m_gc, m_mutedPixel);
    XFillArc(m_display, m_window, m_gc, field.infoIconRect.x, field.infoIconRect.y, kInfoIconSize, kInfoIconSize, 0, 360 * 64);

    int iw = m_font.TextWidth("i");
    DrawText(
        field.infoIconRect.x + (kInfoIconSize - iw) / 2,
        field.infoIconRect.y + kInfoIconSize / 2 + m_font.Ascent() / 2 - 1,
        "i", m_backgroundPixel);

    int labelX = x + kInfoIconSize + 10;
    bool dirty = FieldDirty(field);

    if (dirty)
    {
        XSetForeground(m_display, m_gc, m_accentPixel);
        XFillArc(m_display, m_window, m_gc, labelX, y + rowH / 2 - 3, 6, 6, 0, 360 * 64);
        labelX += 12;
    }

    DrawText(labelX, y + rowH / 2 + m_font.Ascent() / 2, HumanizeKey(field.option->key), m_foregroundPixel);

    int widgetH = 26;
    int widgetW = kWidgetWidth;
    int widgetX = x + width - widgetW;
    int widgetY = y + (rowH - widgetH) / 2;

    bool isFocused = (m_focusedField == &field);

    if (field.option->type == ConfigValueType::Boolean)
    {
        widgetW = kCheckboxSize;
        widgetH = kCheckboxSize;
        widgetX = x + width - widgetW;
        widgetY = y + (rowH - widgetH) / 2;
        field.widgetRect = Rect{widgetX, widgetY, widgetW, widgetH};
        DrawCheckbox(field.widgetRect, field.currentValue == "true");
    }
    else
    {
        field.widgetRect = Rect{widgetX, widgetY, widgetW, widgetH};

        bool hasError = !field.error.empty();
        XSetForeground(m_display, m_gc, m_fieldPixel);
        XFillRectangle(m_display, m_window, m_gc, widgetX, widgetY, static_cast<unsigned int>(widgetW), static_cast<unsigned int>(widgetH));

        XSetForeground(m_display, m_gc, hasError ? m_errorPixel : (isFocused ? m_accentPixel : m_borderPixel));
        XDrawRectangle(m_display, m_window, m_gc, widgetX, widgetY, static_cast<unsigned int>(widgetW - 1), static_cast<unsigned int>(widgetH - 1));

        int textX = widgetX + 8;

        if (field.option->type == ConfigValueType::Color)
        {
            unsigned long swatch = ParseColorLiteral(field.currentValue, m_borderPixel);
            XSetForeground(m_display, m_gc, swatch);
            XFillRectangle(m_display, m_window, m_gc, widgetX + 4, widgetY + 4, 18, static_cast<unsigned int>(widgetH - 8));
            XSetForeground(m_display, m_gc, m_borderPixel);
            XDrawRectangle(m_display, m_window, m_gc, widgetX + 4, widgetY + 4, 17, static_cast<unsigned int>(widgetH - 9));
            textX = widgetX + 4 + 18 + 8;
        }

        std::string displayValue = field.currentValue;

        if (!isFocused)
        {
            int maxTextWidth = widgetX + widgetW - 8 - textX;

            while (!displayValue.empty() && m_font.TextWidth(displayValue) > maxTextWidth)
                displayValue.pop_back();

            if (displayValue.size() < field.currentValue.size() && displayValue.size() > 3)
                displayValue = displayValue.substr(0, displayValue.size() - 3) + "...";
        }

        DrawText(textX, widgetY + widgetH / 2 + m_font.Ascent() / 2, displayValue, m_foregroundPixel);

        if (field.option->type == ConfigValueType::Enum)
            DrawText(widgetX + widgetW - 16, widgetY + widgetH / 2 + m_font.Ascent() / 2, "v", m_mutedPixel);

        if (isFocused)
        {
            int caretX = textX + m_font.TextWidth(field.currentValue.substr(0, m_fieldCaret));
            XSetForeground(m_display, m_gc, m_foregroundPixel);
            XFillRectangle(m_display, m_window, m_gc, caretX, widgetY + 4, 2, static_cast<unsigned int>(widgetH - 8));
        }
    }

    int consumed = rowH;

    if (!field.error.empty())
    {
        DrawText(x + width - kWidgetWidth, y + rowH + 14, field.error, m_errorPixel);
        consumed += 20;
    }

    if (field.infoExpanded)
    {
        int panelY = rowTop + consumed;

        std::vector<std::string> descLines = WrapText(field.option->description, width - 24);

        std::string defaultText = "Default: " + (field.option->defaultValue.empty() ? std::string("(empty)") : field.option->defaultValue);
        std::vector<std::string> defaultLines = WrapText(defaultText, width - 24);

        std::vector<std::string> allowedLines;

        if (field.option->type == ConfigValueType::Enum)
        {
            std::string allowed = "Allowed values: ";

            for (std::size_t i = 0; i < field.option->enumValues.size(); ++i)
            {
                if (i > 0) allowed += ", ";
                allowed += field.option->enumValues[i];
            }

            allowedLines = WrapText(allowed, width - 24);
        }

        int panelHeight = 12
            + static_cast<int>(descLines.size()) * 18
            + 6 + static_cast<int>(defaultLines.size()) * 18
            + (allowedLines.empty() ? 0 : 6 + static_cast<int>(allowedLines.size()) * 18)
            + 10;

        XSetForeground(m_display, m_gc, m_panelPixel);
        XFillRectangle(m_display, m_window, m_gc, x, panelY, static_cast<unsigned int>(width), static_cast<unsigned int>(panelHeight));

        int ty = panelY + 12 + m_font.Ascent();

        for (const std::string& line : descLines) { DrawText(x + 12, ty, line, m_mutedPixel); ty += 18; }

        ty += 6;
        for (const std::string& line : defaultLines) { DrawText(x + 12, ty, line, m_mutedPixel); ty += 18; }

        if (!allowedLines.empty())
        {
            ty += 6;
            for (const std::string& line : allowedLines) { DrawText(x + 12, ty, line, m_mutedPixel); ty += 18; }
        }

        consumed += panelHeight;
    }

    field.rowRect = Rect{x, rowTop, width, consumed};
    outHeight = consumed + 6;
}

void SettingsWindow::LayoutAndDrawRawBlock(
    RawBlockPanel& panel,
    int x,
    int y,
    int width,
    int& outHeight)
{
    if (panel.kind != RawBlockPanel::Kind::RawText)
    {
        LayoutAndDrawStructuredBlock(panel, x, y, width, outHeight);
        return;
    }

    int titleY = y + m_boldFont.Ascent();
    DrawText(x, titleY, panel.title, m_foregroundPixel);

    if (RawBlockDirty(panel))
    {
        int tw = m_font.TextWidth(panel.title);
        XSetForeground(m_display, m_gc, m_accentPixel);
        XFillArc(m_display, m_window, m_gc, x + tw + 10, y + 4, 6, 6, 0, 360 * 64);
    }

    int helpY = titleY + 20;
    std::vector<std::string> helpLines = WrapText(panel.helpText, width);

    for (const std::string& line : helpLines)
    {
        DrawText(x, helpY, line, m_mutedPixel);
        helpY += 16;
    }

    int textAreaY = helpY + 6;
    int visibleLines = std::clamp(static_cast<int>(panel.lines.size()) + 1, kRawBlockMinLines, kRawBlockMaxLines);
    int textAreaH = visibleLines * kRawBlockLineH + 12;

    panel.textAreaRect = Rect{x, textAreaY, width, textAreaH};

    XSetForeground(m_display, m_gc, m_fieldPixel);
    XFillRectangle(m_display, m_window, m_gc, x, textAreaY, static_cast<unsigned int>(width), static_cast<unsigned int>(textAreaH));

    XSetForeground(m_display, m_gc, (m_focusedRawBlock == &panel) ? m_accentPixel : m_borderPixel);
    XDrawRectangle(m_display, m_window, m_gc, x, textAreaY, static_cast<unsigned int>(width - 1), static_cast<unsigned int>(textAreaH - 1));

    XRectangle innerClip{
        static_cast<short>(x), static_cast<short>(textAreaY),
        static_cast<unsigned short>(width), static_cast<unsigned short>(textAreaH)};
    XftDrawSetClipRectangles(m_xftDraw, 0, 0, &innerClip, 1);

    int ly = textAreaY + 6 + m_font.Ascent();

    for (std::size_t i = 0; i < panel.lines.size() && static_cast<int>(i) < kRawBlockMaxLines; ++i)
    {
        bool lineHasError = i < panel.lineErrors.size() && !panel.lineErrors[i].empty();
        DrawText(x + 8, ly, panel.lines[i], lineHasError ? m_errorPixel : m_foregroundPixel);

        if (m_focusedRawBlock == &panel && panel.caretRow == i)
        {
            int caretX = x + 8 + m_font.TextWidth(panel.lines[i].substr(0, panel.caretCol));
            XSetForeground(m_display, m_gc, m_foregroundPixel);
            XFillRectangle(m_display, m_window, m_gc, caretX, ly - m_font.Ascent(), 2, static_cast<unsigned int>(m_font.Height()));
        }

        ly += kRawBlockLineH;
    }

    // Back to the whole content area's own clip (set by
    // LayoutAndDrawContent, which is still mid-loop at this point) -
    // NOT XftDrawSetClip(..., None), which would remove clipping
    // entirely for every row drawn after this one this pass.
    XRectangle contentClip{
        static_cast<short>(m_contentRect.x), static_cast<short>(m_contentRect.y),
        static_cast<unsigned short>(std::max(0, m_contentRect.width)),
        static_cast<unsigned short>(std::max(0, m_contentRect.height))};
    XftDrawSetClipRectangles(m_xftDraw, 0, 0, &contentClip, 1);

    std::string firstError;

    for (const std::string& error : panel.lineErrors)
    {
        if (!error.empty()) { firstError = error; break; }
    }

    int afterY = textAreaY + textAreaH;

    if (!firstError.empty())
    {
        DrawText(x, afterY + 14, "Line error: " + firstError, m_errorPixel);
        afterY += 20;
    }

    panel.rect = Rect{x, y, width, afterY - y};
    outHeight = (afterY - y) + 20;
}

void SettingsWindow::LayoutAndDrawStructuredBlock(
    RawBlockPanel& panel,
    int x,
    int y,
    int width,
    int& outHeight)
{
    int titleY = y + m_boldFont.Ascent();
    DrawText(x, titleY, panel.title, m_foregroundPixel);

    if (RawBlockDirty(panel))
    {
        int tw = m_font.TextWidth(panel.title);
        XSetForeground(m_display, m_gc, m_accentPixel);
        XFillArc(m_display, m_window, m_gc, x + tw + 10, y + 4, 6, 6, 0, 360 * 64);
    }

    int helpY = titleY + 20;
    std::vector<std::string> helpLines = WrapText(panel.helpText, width);

    for (const std::string& line : helpLines)
    {
        DrawText(x, helpY, line, m_mutedPixel);
        helpY += 16;
    }

    int rowY = helpY + 10;

    panel.rowRects.assign(panel.lines.size(), RawBlockPanel::RowRects{});

    // Draws one labelled text cell, with the caret if this exact
    // (row, cell) is the one currently focused - shared by every
    // text cell below, both WindowRule's and Monitor's.
    auto drawCell = [&](const Rect& rect, const std::string& placeholder, const std::string& value,
                         std::size_t row, StructuredCell cell)
    {
        bool focused = (m_focusedRawBlock == &panel) && (panel.caretRow == row) && (m_focusedCell == cell);

        XSetForeground(m_display, m_gc, m_fieldPixel);
        XFillRectangle(m_display, m_window, m_gc, rect.x, rect.y,
            static_cast<unsigned int>(rect.width), static_cast<unsigned int>(rect.height));
        XSetForeground(m_display, m_gc, focused ? m_accentPixel : m_borderPixel);
        XDrawRectangle(m_display, m_window, m_gc, rect.x, rect.y,
            static_cast<unsigned int>(rect.width - 1), static_cast<unsigned int>(rect.height - 1));

        int textStartX = rect.x + 6;
        bool showingPlaceholder = value.empty() && !focused;

        XRectangle clip{
            static_cast<short>(rect.x), static_cast<short>(rect.y),
            static_cast<unsigned short>(rect.width), static_cast<unsigned short>(rect.height)};
        XftDrawSetClipRectangles(m_xftDraw, 0, 0, &clip, 1);

        DrawText(textStartX, rect.y + rect.height / 2 + m_font.Ascent() / 2,
            showingPlaceholder ? placeholder : value,
            showingPlaceholder ? m_mutedPixel : m_foregroundPixel);

        if (focused)
        {
            int caretX = textStartX + m_font.TextWidth(value.substr(0, panel.caretCol));
            XSetForeground(m_display, m_gc, m_foregroundPixel);
            XFillRectangle(m_display, m_window, m_gc, caretX, rect.y + 4, 2,
                static_cast<unsigned int>(rect.height - 8));
        }

        XRectangle contentClip{
            static_cast<short>(m_contentRect.x), static_cast<short>(m_contentRect.y),
            static_cast<unsigned short>(std::max(0, m_contentRect.width)),
            static_cast<unsigned short>(std::max(0, m_contentRect.height))};
        XftDrawSetClipRectangles(m_xftDraw, 0, 0, &contentClip, 1);
    };

    for (std::size_t i = 0; i < panel.lines.size(); ++i)
    {
        RawBlockPanel::RowRects rowRects;
        int rx = x;

        if (panel.kind == RawBlockPanel::Kind::WindowRule)
        {
            WindowRuleDraft draft = ParseWindowRuleDraft(panel.lines[i]);

            // Line 1: WHICH windows - the application (exact `app:` key,
            // or a legacy `class:` substring), plus the optional
            // instance/title selectors.
            rowRects.removeRect = Rect{x + width - kStructuredRemoveW, rowY, kStructuredRemoveW, kStructuredRowH};

            int fieldsRight = rowRects.removeRect.x - kStructuredFieldGap;
            int fieldsTotalW = std::max(0, fieldsRight - rx);
            int firstW = std::max(100, fieldsTotalW / 2);
            int smallW = std::max(60, (fieldsTotalW - firstW - 2 * kStructuredFieldGap) / 2);

            rowRects.classRect = Rect{rx, rowY, firstW, kStructuredRowH};
            drawCell(rowRects.classRect,
                     draft.firstCellIsClass() ? "class:" : "application, e.g. firefox",
                     draft.firstCellIsClass() ? draft.classText : draft.appText,
                     i, StructuredCell::Class);
            rx += firstW + kStructuredFieldGap;

            rowRects.instanceRect = Rect{rx, rowY, smallW, kStructuredRowH};
            drawCell(rowRects.instanceRect, "instance:", draft.instanceText, i, StructuredCell::Instance);
            rx += smallW + kStructuredFieldGap;

            rowRects.titleRect = Rect{rx, rowY, std::max(60, fieldsRight - rx), kStructuredRowH};
            drawCell(rowRects.titleRect, "title:", draft.titleText, i, StructuredCell::Title);

            DrawButton(rowRects.removeRect, "x", true, false);

            // Line 2: WHAT to do with them - every choice has an
            // explicit Default meaning "no opinion". Indented to line up
            // under the application cell.
            int line2Y = rowY + kStructuredRowH + 4;
            int bx = x;

            rowRects.workspaceRect = Rect{bx, line2Y, kStructuredWorkspaceLabelW, kStructuredRowH};
            drawCell(rowRects.workspaceRect, "Workspace", draft.workspaceText, i, StructuredCell::Workspace);
            bx += kStructuredWorkspaceLabelW + kStructuredFieldGap;

            rowRects.horizontalRect = Rect{bx, line2Y, kStructuredChoiceW, kStructuredRowH};
            DrawButton(rowRects.horizontalRect, std::string("Horizontal: ") + kHorizontalLabels[static_cast<std::size_t>(draft.horizontalIndex)], true, false);
            bx += kStructuredChoiceW + kStructuredFieldGap;

            rowRects.verticalRect = Rect{bx, line2Y, kStructuredChoiceW, kStructuredRowH};
            DrawButton(rowRects.verticalRect, std::string("Vertical: ") + kVerticalLabels[static_cast<std::size_t>(draft.verticalIndex)], true, false);
            bx += kStructuredChoiceW + kStructuredFieldGap;

            rowRects.modeRect = Rect{bx, line2Y, kStructuredChoiceW + 20, kStructuredRowH};
            DrawButton(rowRects.modeRect, std::string("Mode: ") + kModeLabels[static_cast<std::size_t>(draft.modeIndex)], true, false);

            rowY += kStructuredRowH + 4; // the second line (the common bottom of the loop adds the first)
        }
        else // Monitor
        {
            rowRects.removeRect = Rect{x + width - kStructuredRemoveW, rowY, kStructuredRemoveW, kStructuredRowH};
            rowRects.workspaceRect = Rect{
                rowRects.removeRect.x - kStructuredFieldGap - kStructuredWorkspaceW,
                rowY, kStructuredWorkspaceW, kStructuredRowH};

            int outputW = std::max(80, rowRects.workspaceRect.x - kStructuredFieldGap - rx);
            rowRects.outputRect = Rect{rx, rowY, outputW, kStructuredRowH};

            MonitorRuleDraft draft = ParseMonitorRuleDraft(panel.lines[i]);
            drawCell(rowRects.outputRect, "e.g. HDMI-1", draft.outputText, i, StructuredCell::Output);
            drawCell(rowRects.workspaceRect, "WS", draft.workspaceText, i, StructuredCell::Workspace);

            DrawButton(rowRects.removeRect, "x", true, false);
        }

        panel.rowRects[i] = rowRects;
        rowY += kStructuredRowH;

        bool lineHasError = i < panel.lineErrors.size() && !panel.lineErrors[i].empty();

        if (lineHasError)
        {
            DrawText(x, rowY + 14, panel.lineErrors[i], m_errorPixel);
            rowY += 20;
        }

        // A window rule is two lines tall, so leave a clearly bigger gap
        // BETWEEN rules than between a rule's own two lines - otherwise
        // it's impossible to tell which pair of lines belongs together.
        rowY += kStructuredRowGap + (panel.kind == RawBlockPanel::Kind::WindowRule ? 12 : 0);
    }

    panel.addRowButtonRect = Rect{x, rowY, kStructuredAddButtonW, kStructuredAddButtonH};
    DrawButton(panel.addRowButtonRect, (panel.kind == RawBlockPanel::Kind::Monitor) ? "+ Add monitor rule" : "+ Add rule", true, false);
    rowY += kStructuredAddButtonH;

    panel.rect = Rect{x, y, width, rowY - y};
    outHeight = (rowY - y) + 20;
}

void SettingsWindow::LayoutAndDrawBottomBar()
{
    int barY = m_geometry.height - kBottomBarHeight;

    XSetForeground(m_display, m_gc, m_panelPixel);
    XFillRectangle(m_display, m_window, m_gc, 0, barY, static_cast<unsigned int>(m_geometry.width), static_cast<unsigned int>(kBottomBarHeight));

    int buttonW = 100;
    int buttonH = 30;
    int gap = 10;
    int bx = m_geometry.width - gap - buttonW;
    int by = barY + (kBottomBarHeight - buttonH) / 2;

    m_saveButtonRect = Rect{bx, by, buttonW, buttonH};
    bx -= buttonW + gap;
    m_resetButtonRect = Rect{bx, by, buttonW, buttonH};

    DrawButton(m_resetButtonRect, "Reset", true, false);
    DrawButton(m_saveButtonRect, "Save", true, true);

    std::string status = m_statusMessage;

    if (status.empty() && AnyDirty())
        status = "Unsaved changes.";

    if (!status.empty())
        DrawText(m_sidebarRect.width + kRowPaddingX, barY + kBottomBarHeight / 2 + m_font.Ascent() / 2, status, m_mutedPixel);
}

}
