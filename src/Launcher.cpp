#include "Launcher.h"

#include "AppIndex.h"
#include "Config.h"
#include "IconResolver.h"
#include "LauncherScoring.h"
#include "Utils.h"
#include "XConnection.h"

#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <Imlib2.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "AppRatings.h"

#include <filesystem>
#include <fstream>

namespace Kohiko
{

namespace
{

int GetPopularityScore(
    const IndexedApp& app)
{
    for (const auto& entry : GlobalPopularity)
    {
        bool desktopMatch =
            !entry.desktopId.empty() &&
            app.desktopId == entry.desktopId;

        bool execMatch =
            !entry.execContains.empty() &&
            app.exec.find(entry.execContains) != std::string::npos;

        if (desktopMatch || execMatch)
            return entry.popularity;
    }

    for (const auto& entry : PenaltyPrograms)
    {
        bool desktopMatch =
            !entry.desktopId.empty() &&
            app.desktopId == entry.desktopId;

        bool execMatch =
            !entry.execContains.empty() &&
            app.exec.find(entry.execContains) != std::string::npos;

        if (desktopMatch || execMatch)
            return entry.popularity;
    }

    return 0;
}

// Bullet 12, "Recently launched bonus" - a simple tiered boost on top
// of HistoryStore's own continuous favoriteScore, so something
// launched a few minutes ago visibly jumps to the top even before
// its favoriteScore has had a chance to accumulate from repeat use.
int RecencyBonus(
    long long lastLaunchEpoch)
{
    if (lastLaunchEpoch <= 0)
        return 0;

    long long now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    long long secondsAgo = now - lastLaunchEpoch;

    if (secondsAgo < 3600) return 150;         // last hour
    if (secondsAgo < 86400) return 80;         // last day
    if (secondsAgo < 7 * 86400) return 30;     // last week

    return 0;
}

// --- CONFIGURABLE INTERNET SEARCH -------------------------------
//
// Every engine Kohiko knows a URL template for out of the box, plus
// "custom" (handled separately in BuildSearchUrl - it reads
// `launcher.custom_search_url` instead of one of these). Which engine
// actually gets used - default/fallback, or a smart prefix like
// "yt " - is entirely config-driven; nothing here is hardcoded as
// *the* search engine, just as one of several options.

struct SearchEngine
{
    const char* id;
    const char* displayName;
    const char* urlTemplate; // "{q}" is replaced with the URL-encoded query
};

constexpr std::array<SearchEngine, 7> kSearchEngines =
{{
    {"google",     "Google",       "https://www.google.com/search?q={q}"},
    {"duckduckgo", "DuckDuckGo",   "https://duckduckgo.com/?q={q}"},
    {"brave",      "Brave Search", "https://search.brave.com/search?q={q}"},
    {"wikipedia",  "Wikipedia",    "https://en.wikipedia.org/w/index.php?search={q}"},
    {"archwiki",   "Arch Wiki",    "https://wiki.archlinux.org/index.php?search={q}"},
    {"github",     "GitHub",       "https://github.com/search?q={q}"},
    {"youtube",    "YouTube",      "https://www.youtube.com/results?search_query={q}"},
}};

// SMART SEARCH: a recognized leading word always routes to that exact
// engine, regardless of `launcher.default_search_engine` - typing
// "yt cats" means YouTube, not whatever the default happens to be.
struct SmartPrefix
{
    const char* prefix;
    const char* engineId;
};

constexpr std::array<SmartPrefix, 4> kSmartPrefixes =
{{
    {"yt", "youtube"},
    {"wiki", "wikipedia"},
    {"arch", "archwiki"},
    {"gh", "github"},
}};

std::string UrlEncode(
    const std::string& text)
{
    static constexpr char kHex[] = "0123456789ABCDEF";

    std::string encoded;
    encoded.reserve(text.size() * 3);

    for (unsigned char c : text)
    {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            encoded += static_cast<char>(c);
        else
        {
            encoded += '%';
            encoded += kHex[c >> 4];
            encoded += kHex[c & 0x0F];
        }
    }

    return encoded;
}

const SearchEngine* FindEngine(
    const std::string& id)
{
    std::string lower = Utils::Lower(id);

    for (const auto& engine : kSearchEngines)
    {
        if (lower == engine.id)
            return &engine;
    }

    return nullptr;
}

// Builds the full URL for `engineId` searching `query`, honouring
// `customUrl` (`launcher.custom_search_url`) when engineId is
// "custom". Returns "" for an unrecognized engine id (e.g. a typo in
// default_search_engine=) rather than guessing - BuildInternetMatches()
// just skips adding a row for it in that case.
std::string BuildSearchUrl(
    const std::string& engineId,
    const std::string& query,
    const std::string& customUrl)
{
    std::string encoded = UrlEncode(query);

    if (Utils::Lower(engineId) == "custom")
    {
        if (customUrl.empty())
            return "";

        std::size_t placeholder = customUrl.find("{query}");

        if (placeholder == std::string::npos)
            return customUrl + encoded; // no placeholder given - just append, e.g. a URL already ending in "?q="

        std::string url = customUrl;
        url.replace(placeholder, std::string("{query}").size(), encoded);
        return url;
    }

    const SearchEngine* engine = FindEngine(engineId);

    if (!engine)
        return "";

    std::string url = engine->urlTemplate;
    std::size_t placeholder = url.find("{q}");

    if (placeholder != std::string::npos)
        url.replace(placeholder, 3, encoded);

    return url;
}

std::string DisplayNameForEngine(
    const std::string& engineId)
{
    if (Utils::Lower(engineId) == "custom")
        return "the web";

    const SearchEngine* engine = FindEngine(engineId);
    return engine ? engine->displayName : engineId;
}

// $HOME file index - unchanged in spirit from the previous
// implementation (a flat recursive walk, matched by name only), just
// extracted into its own function so it can run on the background
// thread instead of blocking Configure()/ReloadDesktopEntries().
std::vector<FileEntry> ScanHomeFiles()
{
    namespace fs = std::filesystem;

    std::vector<FileEntry> files;

    const char* home = std::getenv("HOME");

    if (!home)
        return files;

    try
    {
        for (const auto& entry : fs::recursive_directory_iterator(home))
        {
            if (!entry.is_regular_file() && !entry.is_directory())
                continue;

            FileEntry file;
            file.path = entry.path().string();
            file.name = entry.path().filename().string();
            file.isDirectory = entry.is_directory();
            file.nameLower = Utils::Lower(file.name);
            file.nameWords = Scoring::SplitWords(file.nameLower);

            files.push_back(std::move(file));
        }
    }
    catch (...)
    {
        // A permission-denied subtree, a symlink loop, ... - keep
        // whatever was collected before the exception rather than
        // discarding the whole scan.
    }

    return files;
}

}

Launcher::Launcher(
    XConnection& connection)
    :
    m_connection(connection)
{
}

Launcher::~Launcher()
{
    // The background thread never touches X11/Imlib2 (see
    // RebuildIndexAndFiles) - joining here just waits for any in-
    // flight index/file rebuild to finish, which in the overwhelming
    // majority of cases has already happened long before Kohiko
    // shuts down.
    if (m_buildThread.joinable())
        m_buildThread.join();

    Display* display = m_connection.GetDisplay();

    if (!display)
        return;

    for (auto& [path, pixmap] : m_iconCache)
    {
        if (pixmap)
            XFreePixmap(display, pixmap);
    }

    for (auto& [path, mask] : m_iconMaskCache)
    {
        if (mask)
            XFreePixmap(display, mask);
    }

    if (m_xic)
        XDestroyIC(m_xic);

    if (m_xim)
        XCloseIM(m_xim);

    if (m_xftDraw)
        XftDrawDestroy(m_xftDraw);

    m_font.Unload();

    if (m_gc)
        XFreeGC(display, m_gc);

    if (m_window)
        XDestroyWindow(display, m_window);
}

Rect Launcher::ComputeGeometry(
    const Rect& monitorGeometry) const
{
    // "About 1/12 of the screen" read as *area*, held at a wide,
    // single-line-input aspect ratio (a literal 1/12-area square would
    // be far taller than a one-line box needs), then clamped to sane
    // pixel bounds so it stays sensible on very small or very large
    // monitors.
    constexpr float kAreaFraction = 1.0f / 12.0f;
    constexpr float kAspect = 1.5f; // width : height

    float targetArea = static_cast<float>(monitorGeometry.width) *
                        static_cast<float>(monitorGeometry.height) *
                        kAreaFraction;

    int height = static_cast<int>(std::sqrt(targetArea / kAspect));
    int width  = static_cast<int>(static_cast<float>(height) * kAspect);

    height = std::clamp(height, 180, 350);
    width  = std::clamp(width, 320, static_cast<int>(static_cast<float>(monitorGeometry.width) * 0.9f));

    Rect geometry;
    geometry.width  = width;
    geometry.height = 300;
    geometry.x = monitorGeometry.x + (monitorGeometry.width  - width)  / 2;
    geometry.y = monitorGeometry.y + (monitorGeometry.height - height) / 2.5;
    return geometry;
}

void Launcher::Reposition(
    const Rect& monitorGeometry)
{
    m_geometry = ComputeGeometry(monitorGeometry);

    if (m_window != 0)
        m_connection.MoveResizeWindow(m_window, m_geometry);
}

void Launcher::Configure(
    const Config& config,
    const Rect& monitorGeometry)
{
    m_geometry = ComputeGeometry(monitorGeometry);

    Display* display = m_connection.GetDisplay();
    int screen = m_connection.Screen();

    // Reuses the bar's palette plus the same accent already used for a
    // focused window's border, so the launcher looks like part of
    // Kohiko rather than a bolted-on extra widget - no new config keys
    // needed for it to fit in.
    m_backgroundPixel  = std::strtoul(config.GetString("bar.background", "0x1e1e2e").c_str(), nullptr, 0);
    m_foregroundPixel  = std::strtoul(config.GetString("bar.foreground", "0xcdd6f4").c_str(), nullptr, 0);
    m_borderPixel      = std::strtoul(config.GetString("general.border_color_active", "0x89b4fa").c_str(), nullptr, 0);
    m_placeholderPixel = std::strtoul(config.GetString("general.border_color_inactive", "0x45475a").c_str(), nullptr, 0);

    // --- SEARCH ENGINE / DESKTOP FILE QUALITY / CONFIGURABLE
    // INTERNET SEARCH config - see default.conf's LAUNCHER section. ---
    m_showHiddenApps        = config.GetBool("launcher.show_hidden", false);
    m_strictFiltering       = config.GetBool("launcher.strict_filtering", true);
    m_internetSearchEnabled = config.GetBool("launcher.internet_search", true);
    m_searchWhenNoResults   = config.GetBool("launcher.search_when_no_results", true);
    m_defaultSearchEngine   = config.GetString("launcher.default_search_engine", "duckduckgo");
    m_fallbackSearchEngine  = config.GetString("launcher.fallback_search_engine", "google");
    m_customSearchUrl       = config.GetString("launcher.custom_search_url", "");

    if (m_window == 0)
    {
        XSetWindowAttributes attrs{};
        attrs.override_redirect = True; // this is *our* window - never redirect it back to ourselves as a MapRequest
        attrs.background_pixel = m_backgroundPixel;
        attrs.border_pixel = m_borderPixel;
        attrs.event_mask = ExposureMask | KeyPressMask | ButtonPressMask;

        m_window = XCreateWindow(
            display,
            m_connection.Root(),
            m_geometry.x, m_geometry.y,
            static_cast<unsigned int>(m_geometry.width),
            static_cast<unsigned int>(m_geometry.height),
            2,
            DefaultDepth(display, screen),
            InputOutput,
            DefaultVisual(display, screen),
            CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWEventMask,
            &attrs
        );

        m_gc = XCreateGC(display, m_window, 0, nullptr);

        m_font.Load(
            display,
            screen,
            config.GetString("general.font", "monospace:pixelsize=14"));

        m_xftDraw = XftDrawCreate(
            display,
            m_window,
            DefaultVisual(display, screen),
            DefaultColormap(display, screen));

        // Xutf8LookupString (used in HandleKeyPress below) needs an
        // actual input method + input context - without this m_xic
        // stays null and every keypress would either crash or have to
        // be special-cased, and dead-key/compose sequences for
        // non-ASCII input wouldn't work at all.
        m_xim = XOpenIM(display, nullptr, nullptr, nullptr);

        if (m_xim)
        {
            m_xic = XCreateIC(
                m_xim,
                XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                XNClientWindow, m_window,
                XNFocusWindow, m_window,
                nullptr);
        }

    }
    else
    {
        XMoveResizeWindow(
            display,
            m_window,
            m_geometry.x, m_geometry.y,
            static_cast<unsigned int>(m_geometry.width),
            static_cast<unsigned int>(m_geometry.height));

        XSetWindowBackground(display, m_window, m_backgroundPixel);
        XSetWindowBorder(display, m_window, m_borderPixel);
    }

    if (!m_indexRequested)
    {
        m_indexRequested = true;
        StartIndexRebuild();
    }
}

void Launcher::Open()
{
    if (m_window == 0)
        return;

    m_query.clear();
    m_submitted = LaunchRequest{};
    m_selectedIndex = 0;
    m_scrollOffset = 0;

    UpdateMatches();
    m_cursor = 0;
    m_caretOn = true;
    m_open = true;

    m_connection.MapWindow(m_window);
    m_connection.Raise(m_window);

    Redraw();
}

void Launcher::Close()
{
    if (m_window == 0 || !m_open)
        return;

    m_open = false;
    m_connection.UnmapWindow(m_window);
}

bool Launcher::IsOpen() const
{
    return m_open;
}

::Window Launcher::WindowId() const
{
    return m_window;
}

const std::string& Launcher::Query() const
{
    return m_query;
}

std::string Launcher::SelectedCommand()
{
    if (m_matches.empty())
        return m_query;

    if (m_selectedIndex >= m_matches.size())
        return "";

    const MatchResult& match = m_matches[m_selectedIndex];

    if (match.type == MatchType::File)
        return "";

    if (match.type == MatchType::Application)
        m_history.RecordLaunch(match.desktopId);

    return match.command;
}

LaunchRequest Launcher::SnapshotSelection()
{
    LaunchRequest request;

    if (SelectedIsFile())
    {
        request.kind = LaunchRequest::Kind::RevealFile;
        request.path = SelectedPath();
        return request;
    }

    // SelectedCommand() is also where a launched application is
    // recorded in the history - which belongs to the moment of
    // submission, so it happens here, exactly once.
    std::string command = SelectedCommand();

    if (!command.empty())
    {
        request.kind = LaunchRequest::Kind::Command;
        request.command = std::move(command);
    }

    return request;
}

LaunchRequest Launcher::TakeSubmitted()
{
    LaunchRequest request = std::move(m_submitted);
    m_submitted = LaunchRequest{};
    return request;
}

bool Launcher::SelectedIsFile() const
{
    if (m_matches.empty() || m_selectedIndex >= m_matches.size())
        return false;

    return m_matches[m_selectedIndex].type == MatchType::File;
}

std::string Launcher::SelectedPath()
{
    if (m_matches.empty() || m_selectedIndex >= m_matches.size())
        return "";

    const MatchResult& match = m_matches[m_selectedIndex];

    if (match.type != MatchType::File)
        return "";

    return match.path;
}

LauncherResult Launcher::HandleKeyPress(
    const XKeyEvent& event)
{
    char buffer[32];
    KeySym keysym = NoSymbol;

    XKeyEvent mutableEvent = event; // Xutf8LookupString wants a non-const pointer
    Status status;

    int len;

    if (m_xic)
    {
        len = Xutf8LookupString(
            m_xic,
            &mutableEvent,
            buffer,
            sizeof(buffer) - 1,
            &keysym,
            &status);
    }
    else
    {
        // No input method could be opened (e.g. none configured on
        // this system) - fall back to plain, non-UTF-8 lookup rather
        // than passing a null XIC to Xutf8LookupString.
        len = XLookupString(
            &mutableEvent,
            buffer,
            sizeof(buffer) - 1,
            &keysym,
            nullptr);
    }

    if (len < 0)
        len = 0;

    switch (keysym)
    {
        case XK_Up:
        {
            if (!m_matches.empty() && m_selectedIndex > 0)
            {
                --m_selectedIndex;

                if (m_selectedIndex < m_scrollOffset)
                    m_scrollOffset = m_selectedIndex;
            }

            Redraw();
            return LauncherResult::Editing;
        }

        case XK_Down:
        {
            if (!m_matches.empty() && m_selectedIndex + 1 < m_matches.size())
            {
                ++m_selectedIndex;

                if (m_selectedIndex >= m_scrollOffset + static_cast<std::size_t>(VisibleRows()))
                    ++m_scrollOffset;
            }

            Redraw();
            return LauncherResult::Editing;
        }

        case XK_Escape:
            return LauncherResult::Cancelled;

        case XK_Return:
        case XK_KP_Enter:
            // Freeze what is being submitted RIGHT NOW. Nothing the
            // user does after this point - paging, typing, moving the
            // selection - can change the launch (see LaunchRequest).
            m_submitted = SnapshotSelection();
            return LauncherResult::Confirmed;

        case XK_BackSpace:
            if (m_cursor > 0)
            {
                // Erase the whole UTF-8 codepoint before the cursor,
                // not just its last byte - otherwise a single
                // backspace on a multi-byte character (e.g. Cyrillic,
                // accented Latin) leaves the string as invalid UTF-8
                // and takes 2-4 presses to actually remove.
                std::size_t start = Utils::Utf8PrevBoundary(m_query, m_cursor);
                m_query.erase(start, m_cursor - start);
                m_cursor = start;
            }
            UpdateMatches();
            Redraw();
            return LauncherResult::Editing;

        case XK_Delete:
            if (m_cursor < m_query.size())
            {
                std::size_t end = Utils::Utf8NextBoundary(m_query, m_cursor);
                m_query.erase(m_cursor, end - m_cursor);
            }
            UpdateMatches();
            Redraw();
            return LauncherResult::Editing;

        case XK_Left:
            if (m_cursor > 0)
                m_cursor = Utils::Utf8PrevBoundary(m_query, m_cursor);
            Redraw();
            return LauncherResult::Editing;

        case XK_Right:
            if (m_cursor < m_query.size())
                m_cursor = Utils::Utf8NextBoundary(m_query, m_cursor);
            Redraw();
            return LauncherResult::Editing;

        case XK_Home:
            m_cursor = 0;
            Redraw();
            return LauncherResult::Editing;

        case XK_End:
            m_cursor = m_query.size();
            Redraw();
            return LauncherResult::Editing;

        case XK_u:

            // Ctrl+U: clear the line - a small, well-known readline-ism
            // that's handy given there's no undo otherwise. Only fires
            // with Control actually held, so plain "u" still types.
            if (event.state & ControlMask)
            {
                m_query.clear();
                m_cursor = 0;
                Redraw();
                return LauncherResult::Editing;
            }
            break;

        default:
            break;
    }

    // Anything else that produced printable text - Xutf8LookupString
    // already resolves Shift/level via the active keyboard mapping.
    for (int i = 0; i < len; ++i)
    {
        unsigned char c = static_cast<unsigned char>(buffer[i]);

        if (c < 0x20 || c == 0x7f) // control character, e.g. an unhandled Ctrl+combo - not text
            continue;

        m_query.insert(m_query.begin() + static_cast<long>(m_cursor), static_cast<char>(c));
        ++m_cursor;
    }
    UpdateMatches();
    Redraw();
    return LauncherResult::Editing;
}

void Launcher::HandleExpose()
{
    Redraw();
}

void Launcher::HandleButtonPress(
    const XButtonEvent& event)
{
    if (event.button == Button4)
    {
        if (m_scrollOffset > 0)
        {
            --m_scrollOffset;
            Redraw();
        }
    }
    else if (event.button == Button5)
    {
        std::size_t maxOffset = 0;

        if (m_matches.size() > static_cast<std::size_t>(VisibleRows()))
            maxOffset = m_matches.size() - VisibleRows();

        if (m_scrollOffset < maxOffset)
        {
            ++m_scrollOffset;
            Redraw();
        }
    }
}

void Launcher::Blink()
{
    if (!m_open)
        return;

    m_caretOn = !m_caretOn;
    Redraw();
}

void Launcher::Redraw()
{
    if (!m_open || m_window == 0)
        return;

    Display* display = m_connection.GetDisplay();

    XClearWindow(display, m_window);

    int padding = 16;
    int fontHeight = 18;
    int baseline = padding + fontHeight;

    Visual* visual = DefaultVisual(display, m_connection.Screen());
    Colormap colormap = DefaultColormap(display, m_connection.Screen());

    if (m_query.empty())
    {
        static const std::string placeholder =
            "Type a command and press Enter...";

        if (m_xftDraw)
        {
            TextColor color(display, visual, colormap, m_placeholderPixel);
            m_font.DrawString(m_xftDraw, padding, baseline, placeholder, color.Get());
        }
    }
    else
    {
        std::string visible = m_query;

        if (m_caretOn)
            visible.insert(visible.begin() + static_cast<long>(m_cursor), '|');

        if (m_xftDraw)
        {
            TextColor color(display, visual, colormap, m_foregroundPixel);
            m_font.DrawString(m_xftDraw, padding, baseline, visible, color.Get());
        }
    }

    XSetForeground(display, m_gc, m_placeholderPixel);

    XDrawLine(
        display,
        m_window,
        m_gc,
        padding,
        baseline + 15,
        m_geometry.width - padding,
        baseline + 15);

    int y = baseline + 40;
    int lineHeight = 20;
    int bottomPadding = -100;

    int visibleRows = (m_geometry.height - y - bottomPadding) / lineHeight;

    std::size_t lastVisibleIndex = std::min(
        m_matches.size(),
        m_scrollOffset + static_cast<std::size_t>(visibleRows));

    for (std::size_t i = m_scrollOffset; i < lastVisibleIndex; ++i)
    {
        int drawY = y + static_cast<int>(i - m_scrollOffset) * lineHeight;

        // Selected rows draw their label in the background colour
        // (for contrast against the highlight rectangle below) - the
        // old code achieved this by setting the GC's foreground and
        // letting Xutf8DrawString read it back; Xft takes an explicit
        // XftColor per call instead, so that same choice is now
        // captured here and threaded through to the draw call below.
        unsigned long textPixel = m_foregroundPixel;

        if (i == m_selectedIndex)
        {
            XSetForeground(display, m_gc, m_borderPixel);

            XFillRectangle(
                display,
                m_window,
                m_gc,
                8,
                drawY - 14,
                m_geometry.width - 16,
                20);

            textPixel = m_backgroundPixel;
        }

        const MatchResult& match = m_matches[i];

        // One lookup path for every row type: Application rows carry
        // their app's resolved icon path, File rows carry the shared
        // folder/generic-file path, Internet rows carry "" - which
        // ResolveIconPixmap() turns into {0, 0}, so this simply draws
        // nothing for them without needing a type check here at all.
        Pixmap iconMask = 0;
        Pixmap iconPixmap = ResolveIconPixmap(match.iconPath, iconMask);

        if (iconPixmap)
        {
            int iconX = 12;
            int iconY = drawY - 14;

            if (iconMask)
            {
                XSetClipMask(display, m_gc, iconMask);
                XSetClipOrigin(display, m_gc, iconX, iconY);
            }

            XCopyArea(
                display,
                iconPixmap,
                m_window,
                m_gc,
                0,
                0,
                20,
                20,
                iconX,
                iconY);

            if (iconMask)
                XSetClipMask(display, m_gc, None);
        }

        if (m_xftDraw)
        {
            TextColor color(display, visual, colormap, textPixel);
            m_font.DrawString(m_xftDraw, padding + 28, drawY, match.label, color.Get());
        }
    }

    XFlush(display);
}

void Launcher::UpdateMatches()
{
    m_matches.clear();
    m_scrollOffset = 0;

    std::string queryLower = Utils::Lower(m_query);

    {
        std::lock_guard<std::mutex> lock(m_indexMutex);

        for (const auto& app : m_index)
        {
            Scoring::Bonuses bonuses;
            bonuses.popularityBonus = GetPopularityScore(app);

            if (const HistoryRecord* history = m_history.Find(app.desktopId))
            {
                // Bullet 10, "Launch history bonus".
                bonuses.launchCountBonus = std::min(300, history->launchCount * 15);

                // HistoryStore's blended frecency signal, folded in as
                // its own small bonus (see HistoryStore::RecordLaunch).
                bonuses.favoriteBonus = static_cast<int>(std::min(150.0, history->favoriteScore * 10.0));

                bonuses.recencyBonus = RecencyBonus(history->lastLaunchEpoch);
            }

            int score = Scoring::ScoreApp(app, queryLower, bonuses);

            if (score == Scoring::kNoMatch)
                continue;

            MatchResult match;
            match.type = MatchType::Application;
            match.label = app.name;
            match.command = app.exec;
            match.desktopId = app.desktopId;
            match.iconPath = app.iconPath;
            match.score = score;
            m_matches.push_back(std::move(match));
        }

        // Preserves the previous behaviour exactly: files only ever
        // show up once something has actually been typed, never as
        // part of the empty-query "list everything" view.
        if (!m_query.empty())
        {
            for (const auto& file : m_files)
            {
                int score = Scoring::BestFieldMatch(queryLower, file.nameLower, file.nameWords);

                if (score == Scoring::kNoMatch)
                    continue;

                MatchResult match;
                match.type = MatchType::File;
                match.label = file.name;
                match.path = file.path;
                match.isDirectory = file.isDirectory;
                match.iconPath = file.isDirectory ? m_folderIconPath : m_fileIconPath;
                match.score = score;
                m_matches.push_back(std::move(match));
            }
        }
    }

    BuildInternetMatches(!m_matches.empty());

    std::sort(
        m_matches.begin(),
        m_matches.end(),
        [](const MatchResult& a, const MatchResult& b)
        {
            // A recognized smart-search prefix ("yt cats") is an
            // explicit, unambiguous request - it always wins, even
            // over an Application match.
            if (a.priority != b.priority)
                return a.priority;

            // Otherwise: Application, then File, then Internet - the
            // enum is declared in exactly that order for this reason.
            if (a.type != b.type)
                return static_cast<int>(a.type) < static_cast<int>(b.type);

            return a.score > b.score;
        });

    if (m_selectedIndex >= m_matches.size())
    {
        m_selectedIndex = 0;
        m_scrollOffset = 0;
    }
}

void Launcher::BuildInternetMatches(
    bool haveLocalResults)
{
    if (!m_internetSearchEnabled || m_query.empty())
        return;

    // SMART SEARCH: a recognized leading word ("yt cats", "wiki x11",
    // ...) always offers that engine, whether or not local results
    // exist - it's a deliberate request, not a fallback.
    std::size_t space = m_query.find(' ');

    if (space != std::string::npos && space > 0)
    {
        std::string prefix = Utils::Lower(m_query.substr(0, space));
        std::string rest = Utils::Trim(m_query.substr(space + 1));

        if (!rest.empty())
        {
            for (const auto& mapping : kSmartPrefixes)
            {
                if (prefix != mapping.prefix)
                    continue;

                AddInternetMatch(mapping.engineId, rest, /*priority=*/true);
                return; // unambiguous - no need to also offer a fallback suggestion below
            }
        }
    }

    // Otherwise: only offer to search the web when there's nothing
    // useful to show locally, and only if configured to do so.
    if (haveLocalResults || !m_searchWhenNoResults)
        return;

    AddInternetMatch(m_defaultSearchEngine, m_query, false);

    if (!m_fallbackSearchEngine.empty() && m_fallbackSearchEngine != m_defaultSearchEngine)
        AddInternetMatch(m_fallbackSearchEngine, m_query, false);
}

void Launcher::AddInternetMatch(
    const std::string& engineId,
    const std::string& queryText,
    bool priority)
{
    std::string url = BuildSearchUrl(engineId, queryText, m_customSearchUrl);

    if (url.empty())
        return; // unrecognized engine id and no usable custom URL - nothing sensible to offer

    MatchResult match;
    match.type = MatchType::Internet;
    match.label = "Search " + DisplayNameForEngine(engineId) + " for \"" + queryText + "\"";

    // xdg-open (not a hardcoded browser) - whatever the person has
    // set as their default handler for http(s) URLs opens it, exactly
    // like clicking a link anywhere else on their system would.
    // Wrapped in single quotes (sh disables all expansion inside
    // them) rather than double - UrlEncode() never leaves a literal
    // single quote in the URL, so this can't be broken out of.
    match.command = "xdg-open '" + url + "'";

    match.priority = priority;
    match.score = priority ? 1000 : 0;

    m_matches.push_back(std::move(match));
}

void Launcher::StartIndexRebuild()
{
    // In practice this is always already finished by the time a
    // second rebuild is requested (ReloadDesktopEntries() is a rare,
    // deliberate action) - join() just makes that a guarantee instead
    // of an assumption, so there's never more than one thread writing
    // m_index/m_files at once.
    if (m_buildThread.joinable())
        m_buildThread.join();

    // Captured by value rather than read from `this` inside the
    // thread - see RebuildIndexAndFiles()'s declaration for why.
    bool includeHidden = m_showHiddenApps;
    bool strictFiltering = m_strictFiltering;

    m_buildThread = std::thread([this, includeHidden, strictFiltering]()
    {
        RebuildIndexAndFiles(includeHidden, strictFiltering);
    });
}

void Launcher::RebuildIndexAndFiles(
    bool includeHidden,
    bool strictFiltering)
{
    std::vector<IndexedApp> apps;
    std::vector<AppIndex::DirStamp> stamps;

    // APPLICATION INDEX: only actually re-scan/re-parse every .desktop
    // file when the cache is missing or one of the application
    // directories has changed since it was written - otherwise this
    // is just reading one small flat file.
    if (AppIndex::LoadCache(apps, stamps) && AppIndex::IsFresh(stamps))
    {
        // cache hit - nothing to rescan
    }
    else
    {
        apps = AppIndex::Build(includeHidden, strictFiltering);
        AppIndex::SaveCache(apps, AppIndex::CurrentDirStamps());
    }

    Scoring::PrepareForSearch(apps);

    std::vector<FileEntry> files = ScanHomeFiles();

    // A short-lived resolver is enough here - it's only ever asked
    // for these same two generic icon names, and re-loading the
    // theme chain once per (rare) rebuild costs nothing worth caching
    // across calls for.
    IconResolver iconResolver;
    std::string folderIconPath = iconResolver.Resolve("folder");
    std::string fileIconPath = iconResolver.Resolve("text-x-generic");

    std::lock_guard<std::mutex> lock(m_indexMutex);
    m_index = std::move(apps);
    m_files = std::move(files);
    m_folderIconPath = std::move(folderIconPath);
    m_fileIconPath = std::move(fileIconPath);
}

Pixmap Launcher::LoadIcon(
    const std::string& path,
    Pixmap& maskOut)
{
    maskOut = 0;

    if (path.empty())
        return 0;

    Display* display = m_connection.GetDisplay();

    Visual* visual = DefaultVisual(display, m_connection.Screen());
    Colormap colormap = DefaultColormap(display, m_connection.Screen());

    imlib_context_set_display(display);
    imlib_context_set_visual(visual);
    imlib_context_set_colormap(colormap);
    imlib_context_set_drawable(m_window);

    Imlib_Image image = imlib_load_image(path.c_str());

    if (!image)
        return 0;

    imlib_context_set_image(image);

    // Most icon themes hand out PNGs with an alpha channel (rounded
    // corners, transparent padding around a smaller glyph, etc).
    // Rendering straight onto a freshly created Pixmap ignores that
    // alpha entirely and paints over whatever the Pixmap's memory
    // happened to already contain, which reads back as solid black:
    // every icon would show up with a black square behind it instead
    // of blending into the launcher's own (dark) background.
    //
    // Asking Imlib2 for a 1-bit shape mask alongside the pixmap - not
    // just a flat-rendered pixmap - lets Redraw() clip its XCopyArea
    // to only the icon's own opaque pixels via XSetClipMask, so
    // whatever is already drawn underneath (the plain background, or
    // the selected-row highlight) shows through everywhere else. Real
    // per-pixel transparency without needing a compositor or an ARGB
    // visual, which a plain core-Xlib window like this one doesn't have.
    Pixmap pixmap = 0;
    Pixmap mask = 0;

    imlib_render_pixmaps_for_whole_image_at_size(&pixmap, &mask, 20, 20);
    imlib_free_image();

    maskOut = mask;
    return pixmap;
}

Pixmap Launcher::ResolveIconPixmap(
    const std::string& iconPath,
    Pixmap& maskOut)
{
    maskOut = 0;

    if (iconPath.empty())
        return 0;

    if (auto it = m_iconCache.find(iconPath); it != m_iconCache.end())
    {
        maskOut = m_iconMaskCache[iconPath];
        return it->second;
    }

    // First time this exact path has been drawn - decode/rasterize it
    // now (this is the one piece of icon work that has to happen
    // synchronously on the UI thread: Imlib2/X11 need a live Display
    // connection, so it can't run on m_buildThread) and cache the
    // result so every later row - this one redrawn again, or a
    // different app that happens to share the icon - is free.
    Pixmap mask = 0;
    Pixmap pixmap = LoadIcon(iconPath, mask);

    m_iconCache[iconPath] = pixmap;
    m_iconMaskCache[iconPath] = mask;

    maskOut = mask;
    return pixmap;
}

int Launcher::VisibleRows() const
{
    int lineHeight = 20;
    int startY = 50;

    return (m_geometry.height - startY - 10) / lineHeight;
}

void Launcher::ReloadDesktopEntries()
{
    // Runs on the background thread, same as the initial build - the
    // currently-open (if any) result list keeps showing today's data
    // until the next keystroke or the next Open(), at which point
    // UpdateMatches() picks up whatever RebuildIndexAndFiles() has
    // published by then. Simpler and just as correct as trying to
    // force an immediate redraw the moment the background thread
    // finishes, which would mean touching X11 from that thread.
    StartIndexRebuild();
    UpdateMatches();
}

}
