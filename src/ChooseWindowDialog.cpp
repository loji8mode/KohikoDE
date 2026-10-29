#include "ChooseWindowDialog.h"

#include "Config.h"
#include "Utils.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace Kohiko
{

namespace
{

constexpr int kWidth = 600;
constexpr int kPad = 20;
constexpr int kLabelWidth = 104;
constexpr int kChipPadX = 12;
constexpr int kChipPadY = 6;
constexpr int kChipGap = 6;
constexpr int kRowGap = 14;
constexpr int kButtonPadX = 18;

unsigned long ParseColor(const std::string& text, unsigned long fallback)
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

unsigned long Blend(unsigned long a, unsigned long b, float t)
{
    auto channel = [&](int shift) -> unsigned long
    {
        int ca = static_cast<int>((a >> shift) & 0xff);
        int cb = static_cast<int>((b >> shift) & 0xff);
        int result = ca + static_cast<int>(static_cast<float>(cb - ca) * t);
        result = std::clamp(result, 0, 255);
        return static_cast<unsigned long>(result) << shift;
    };

    return channel(16) | channel(8) | channel(0);
}

std::string Elide(const Font& font, const std::string& text, int maxWidth)
{
    if (font.TextWidth(text) <= maxWidth)
        return text;

    std::string out = text;

    while (!out.empty() && font.TextWidth(out + "...") > maxWidth)
        out.pop_back();

    return out + "...";
}

}

ChooseWindowDialog::ChooseWindowDialog(
    const Config& config,
    const ChooseWindowIdentity& identity,
    ChooseWindowModel& model,
    int otherRuleCount)
    : m_config(config),
      m_identity(identity),
      m_model(model),
      m_otherRuleCount(otherRuleCount)
{
}

ChooseWindowDialog::~ChooseWindowDialog()
{
    if (m_xftDraw) XftDrawDestroy(m_xftDraw);
    m_font.Unload();
    m_boldFont.Unload();
    if (m_backBuffer) XFreePixmap(m_display, m_backBuffer);
    if (m_gc) XFreeGC(m_display, m_gc);
    if (m_window) XDestroyWindow(m_display, m_window);
    if (m_display) XCloseDisplay(m_display);
}

bool ChooseWindowDialog::AlreadyOpen(Display* display)
{
    Window root = DefaultRootWindow(display);
    Window rootReturn, parentReturn;
    Window* children = nullptr;
    unsigned int count = 0;
    bool found = false;

    if (!XQueryTree(display, root, &rootReturn, &parentReturn, &children, &count))
        return false;

    for (unsigned int i = 0; i < count && !found; ++i)
    {
        XClassHint hint{};

        if (XGetClassHint(display, children[i], &hint))
        {
            found = hint.res_name && std::strcmp(hint.res_name, "kohiko-choose-window") == 0;

            if (hint.res_name)  XFree(hint.res_name);
            if (hint.res_class) XFree(hint.res_class);
        }
    }

    if (children)
        XFree(children);

    return found;
}

void ChooseWindowDialog::FillRect(const Rect& r, unsigned long pixel)
{
    XSetForeground(m_display, m_gc, pixel);
    XFillRectangle(m_display, m_backBuffer, m_gc, r.x, r.y,
                   static_cast<unsigned int>(r.width), static_cast<unsigned int>(r.height));
}

void ChooseWindowDialog::StrokeRect(const Rect& r, unsigned long pixel)
{
    XSetForeground(m_display, m_gc, pixel);
    XDrawRectangle(m_display, m_backBuffer, m_gc, r.x, r.y,
                   static_cast<unsigned int>(r.width - 1), static_cast<unsigned int>(r.height - 1));
}

void ChooseWindowDialog::Text(int x, int baseline, const std::string& text, const XftColor& color, bool bold)
{
    (bold ? m_boldFont : m_font).DrawString(m_xftDraw, x, baseline, text, color);
}

void ChooseWindowDialog::ComputeLayout()
{
    m_hits.clear();
    m_rowLabelY.assign(ChooseWindowModel::kRowCount, 0);

    const int lineHeight = m_font.Height();
    const int chipHeight = lineHeight + kChipPadY * 2;
    const int ascent = m_font.Ascent();

    int y = kPad;

    m_headerBaseline = y + ascent;
    y += lineHeight + 8;

    m_appBaseline = y + ascent;
    y += lineHeight + 2;

    m_detailBaseline = y + ascent;
    y += lineHeight;

    if (m_otherRuleCount > 0)
    {
        y += 4;
        m_noteBaseline = y + ascent;
        y += lineHeight;
    }
    else
        m_noteBaseline = 0;

    y += kRowGap + 6;

    const int chipsLeft = kPad + kLabelWidth;
    const int chipsRight = kWidth - kPad;

    for (int r = 0; r < ChooseWindowModel::kRowCount; ++r)
    {
        auto row = static_cast<ChooseWindowModel::Row>(r);

        m_rowLabelY[static_cast<std::size_t>(r)] = y + kChipPadY + ascent;

        int x = chipsLeft;
        int rowY = y;

        for (int i = 0; i < m_model.OptionCount(row); ++i)
        {
            int w = m_font.TextWidth(m_model.OptionLabel(row, i)) + kChipPadX * 2;

            if (x + w > chipsRight && x > chipsLeft)
            {
                x = chipsLeft;
                rowY += chipHeight + kChipGap;
            }

            Hit hit;
            hit.kind = Hit::Kind::Chip;
            hit.row = row;
            hit.index = i;
            hit.rect = Rect{ x, rowY, w, chipHeight };
            m_hits.push_back(hit);

            x += w + kChipGap;
        }

        y = rowY + chipHeight + kRowGap;
    }

    y += 4;

    // Buttons, right-aligned; Reset sits at the left.
    const int buttonHeight = chipHeight + 4;
    auto buttonWidth = [&](const char* label) { return m_font.TextWidth(label) + kButtonPadX * 2; };

    int saveW = buttonWidth("Save"), cancelW = buttonWidth("Cancel"), resetW = buttonWidth("Reset rule");

    Hit save;   save.kind = Hit::Kind::Save;     save.rect   = Rect{ kWidth - kPad - saveW, y, saveW, buttonHeight };
    Hit cancel; cancel.kind = Hit::Kind::Cancel; cancel.rect = Rect{ save.rect.x - kChipGap - cancelW, y, cancelW, buttonHeight };
    Hit reset;  reset.kind = Hit::Kind::Reset;   reset.rect  = Rect{ kPad, y, resetW, buttonHeight };

    m_hits.push_back(save);
    m_hits.push_back(cancel);
    m_hits.push_back(reset);

    y += buttonHeight + 12;

    m_hintBaseline = y + ascent;
    y += lineHeight + kPad;

    m_height = y;
}

void ChooseWindowDialog::Draw()
{
    FillRect(Rect{ 0, 0, m_width, m_height }, m_background);

    Visual* visual = DefaultVisual(m_display, m_screen);
    Colormap colormap = DefaultColormap(m_display, m_screen);

    TextColor fg(m_display, visual, colormap, m_foreground);
    TextColor accent(m_display, visual, colormap, m_accent);
    TextColor muted(m_display, visual, colormap, m_muted);
    TextColor onAccent(m_display, visual, colormap, m_background);

    const int textWidth = kWidth - kPad * 2;

    Text(kPad, m_headerBaseline, "Window Rules", accent.Get(), true);

    Text(kPad, m_appBaseline,
         Elide(m_font, "Application: " + m_identity.appKey +
             (m_identity.scope.IsApplication() ? std::string() : " (" + m_identity.ScopeLabel() + " only)"), textWidth), fg.Get());

    std::string detail = "class: " + (m_identity.className.empty() ? std::string("-") : m_identity.className) +
                         "   instance: " + (m_identity.instanceName.empty() ? std::string("-") : m_identity.instanceName);
    Text(kPad, m_detailBaseline, Elide(m_font, detail, textWidth), muted.Get());

    if (m_otherRuleCount > 0)
    {
        std::string note = std::to_string(m_otherRuleCount) +
            (m_otherRuleCount == 1 ? " other rule also applies" : " other rules also apply") +
            " (see Settings > Window Rules)";
        Text(kPad, m_noteBaseline, Elide(m_font, note, textWidth), muted.Get());
    }

    // Row labels.
    for (int r = 0; r < ChooseWindowModel::kRowCount; ++r)
    {
        auto row = static_cast<ChooseWindowModel::Row>(r);
        bool current = (row == m_model.CurrentRow());

        Text(kPad, m_rowLabelY[static_cast<std::size_t>(r)],
             ChooseWindowModel::RowLabel(row), current ? accent.Get() : fg.Get(), current);
    }

    // Chips and buttons.
    for (const Hit& hit : m_hits)
    {
        if (hit.kind == Hit::Kind::Chip)
        {
            bool selected = m_model.Selected(hit.row) == hit.index;
            bool currentRow = hit.row == m_model.CurrentRow();
            std::string label = m_model.OptionLabel(hit.row, hit.index);

            FillRect(hit.rect, selected ? m_accent : m_field);
            StrokeRect(hit.rect, (selected || currentRow) ? m_accent : m_border);

            int tx = hit.rect.x + (hit.rect.width - m_font.TextWidth(label)) / 2;
            int ty = hit.rect.y + kChipPadY + m_font.Ascent();
            Text(tx, ty, label, selected ? onAccent.Get() : fg.Get());
            continue;
        }

        const char* label = hit.kind == Hit::Kind::Save ? "Save"
                          : hit.kind == Hit::Kind::Cancel ? "Cancel" : "Reset rule";

        bool disabled = hit.kind == Hit::Kind::Reset && !m_model.HasSavedRule();
        bool primary = hit.kind == Hit::Kind::Save;

        FillRect(hit.rect, primary ? m_accent : m_field);
        StrokeRect(hit.rect, primary ? m_accent : m_border);

        int tx = hit.rect.x + (hit.rect.width - m_font.TextWidth(label)) / 2;
        int ty = hit.rect.y + (hit.rect.height - m_font.Height()) / 2 + m_font.Ascent();
        Text(tx, ty, label, primary ? onAccent.Get() : (disabled ? muted.Get() : fg.Get()), primary);
    }

    Text(kPad, m_hintBaseline,
         Elide(m_font, "hjkl/arrows  1-9 workspace  Enter save  Del reset  Esc cancel", textWidth),
         muted.Get());

    XCopyArea(m_display, m_backBuffer, m_window, m_gc, 0, 0,
              static_cast<unsigned int>(m_width), static_cast<unsigned int>(m_height), 0, 0);
    XFlush(m_display);
}

ChooseWindowModel::Action ChooseWindowDialog::HandleKeyPress(XKeyEvent& event)
{
    KeySym sym = XLookupKeysym(&event, 0);
    bool shift = (event.state & ShiftMask) != 0;

    ChooseWindowModel::Key key;

    switch (sym)
    {
        case XK_Up:    case XK_k: key.kind = ChooseWindowModel::Key::Kind::Up; break;
        case XK_Down:  case XK_j: key.kind = ChooseWindowModel::Key::Kind::Down; break;
        case XK_Left:  case XK_h: key.kind = ChooseWindowModel::Key::Kind::Left; break;
        case XK_Right: case XK_l: key.kind = ChooseWindowModel::Key::Kind::Right; break;
        case XK_Tab:   key.kind = shift ? ChooseWindowModel::Key::Kind::BackTab : ChooseWindowModel::Key::Kind::Tab; break;
        case XK_ISO_Left_Tab: key.kind = ChooseWindowModel::Key::Kind::BackTab; break;
        case XK_Return: case XK_KP_Enter: key.kind = ChooseWindowModel::Key::Kind::Enter; break;
        case XK_Escape: key.kind = ChooseWindowModel::Key::Kind::Escape; break;
        case XK_Delete: case XK_BackSpace: key.kind = ChooseWindowModel::Key::Kind::Delete; break;
        default:
            if (sym >= XK_0 && sym <= XK_9)
            {
                key.kind = ChooseWindowModel::Key::Kind::Digit;
                key.digit = static_cast<int>(sym - XK_0);
            }
            else if (sym >= XK_KP_0 && sym <= XK_KP_9)
            {
                key.kind = ChooseWindowModel::Key::Kind::Digit;
                key.digit = static_cast<int>(sym - XK_KP_0);
            }
            else
                return ChooseWindowModel::Action::Continue;
    }

    return m_model.HandleKey(key);
}

ChooseWindowModel::Action ChooseWindowDialog::HandleClick(int x, int y)
{
    for (const Hit& hit : m_hits)
    {
        if (!hit.rect.Contains(Point{ x, y }))
            continue;

        switch (hit.kind)
        {
            case Hit::Kind::Chip:
                m_model.SetCurrentRow(hit.row);
                m_model.Select(hit.row, hit.index);
                return ChooseWindowModel::Action::Continue;

            case Hit::Kind::Save:   return ChooseWindowModel::Action::Save;
            case Hit::Kind::Cancel: return ChooseWindowModel::Action::Cancel;
            case Hit::Kind::Reset:
                return m_model.HasSavedRule() ? ChooseWindowModel::Action::Reset
                                              : ChooseWindowModel::Action::Continue;
        }
    }

    return ChooseWindowModel::Action::Continue;
}

ChooseWindowModel::Action ChooseWindowDialog::Run()
{
    m_display = XOpenDisplay(nullptr);

    if (!m_display)
        return ChooseWindowModel::Action::Cancel;

    m_screen = DefaultScreen(m_display);

    m_background = ParseColor(m_config.GetString("bar.background", "0x1e1e2e"), 0x1e1e2e);
    m_foreground = ParseColor(m_config.GetString("bar.foreground", "0xcdd6f4"), 0xcdd6f4);
    m_accent     = ParseColor(m_config.GetString("bar.active", "0x89b4fa"), 0x89b4fa);
    m_field      = ParseColor(m_config.GetString("lockscreen.field_color", "0x313244"), 0x313244);
    m_border     = ParseColor(m_config.GetString("general.border_color_inactive", "0x45475a"), 0x45475a);
    m_panel      = Blend(m_background, m_field, 0.5f);
    m_muted      = Blend(m_foreground, m_background, 0.45f);

    std::string fontPattern = m_config.GetString("general.font", "monospace:pixelsize=14");
    m_font.Load(m_display, m_screen, fontPattern);
    m_boldFont.Load(m_display, m_screen, fontPattern + ":bold");

    m_width = kWidth;
    ComputeLayout();

    XSetWindowAttributes attrs{};
    attrs.background_pixel = m_background;
    attrs.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask;

    m_window = XCreateWindow(
        m_display, RootWindow(m_display, m_screen), 0, 0,
        static_cast<unsigned int>(m_width), static_cast<unsigned int>(m_height), 0,
        DefaultDepth(m_display, m_screen), InputOutput, DefaultVisual(m_display, m_screen),
        CWBackPixel | CWEventMask, &attrs);

    XClassHint classHint;
    classHint.res_name = const_cast<char*>("kohiko-choose-window");
    classHint.res_class = const_cast<char*>("Kohiko-choose-window");
    XSetClassHint(m_display, m_window, &classHint);

    std::string title = "Window Rules - " + (m_identity.appKey.empty() ? std::string("window") : m_identity.appKey) +
        (m_identity.scope.IsApplication() ? std::string() : " [" + m_identity.ScopeLabel() + "]");
    XStoreName(m_display, m_window, title.c_str());

    Atom netWmName = XInternAtom(m_display, "_NET_WM_NAME", False);
    Atom utf8 = XInternAtom(m_display, "UTF8_STRING", False);
    XChangeProperty(m_display, m_window, netWmName, utf8, 8, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(title.c_str()), static_cast<int>(title.size()));

    // A dialog: Kohiko floats and centres these instead of tiling them.
    Atom type = XInternAtom(m_display, "_NET_WM_WINDOW_TYPE", False);
    Atom dialog = XInternAtom(m_display, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    XChangeProperty(m_display, m_window, type, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(&dialog), 1);

    // Fixed size - the content is laid out for exactly this.
    XSizeHints hints{};
    hints.flags = PMinSize | PMaxSize | PSize;
    hints.min_width = hints.max_width = hints.width = m_width;
    hints.min_height = hints.max_height = hints.height = m_height;
    XSetWMNormalHints(m_display, m_window, &hints);

    Atom wmDelete = XInternAtom(m_display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(m_display, m_window, &wmDelete, 1);

    m_gc = XCreateGC(m_display, m_window, 0, nullptr);
    m_backBuffer = XCreatePixmap(m_display, m_window,
        static_cast<unsigned int>(m_width), static_cast<unsigned int>(m_height),
        static_cast<unsigned int>(DefaultDepth(m_display, m_screen)));
    m_xftDraw = XftDrawCreate(m_display, m_backBuffer,
        DefaultVisual(m_display, m_screen), DefaultColormap(m_display, m_screen));

    XMapWindow(m_display, m_window);
    XFlush(m_display);

    for (;;)
    {
        XEvent event;
        XNextEvent(m_display, &event);

        ChooseWindowModel::Action action = ChooseWindowModel::Action::Continue;

        switch (event.type)
        {
            case Expose:
                if (event.xexpose.count == 0)
                    Draw();
                break;

            case KeyPress:
                action = HandleKeyPress(event.xkey);
                break;

            case ButtonPress:
                if (event.xbutton.button == Button1)
                    action = HandleClick(event.xbutton.x, event.xbutton.y);
                break;

            case ClientMessage:
                if (static_cast<Atom>(event.xclient.data.l[0]) == wmDelete)
                    action = ChooseWindowModel::Action::Cancel;
                break;

            default:
                break;
        }

        if (action != ChooseWindowModel::Action::Continue)
            return action;

        if (event.type == KeyPress || event.type == ButtonPress)
            Draw();
    }
}

}
