#pragma once

#include <filesystem>

#include "AppIndex.h"
#include "Font.h"
#include "HistoryStore.h"
#include "Types.h"

#include <X11/Xlib.h>

#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Kohiko
{

class XConnection;
class Config;

// What happened to one XKeyEvent fed into a currently-open Launcher.
enum class LauncherResult
{
    Editing,    // consumed - the query text changed, just redraw
    Confirmed,  // Enter - Query() is what should be run, then close
    Cancelled   // Escape - close without running anything
};

// Kohiko's native Super+D launcher, replacing the old
// `exec.launcher=dmenu_run` default: a small centered input box that
// "instantly opens, cursor already in the input field, type, Enter,
// program starts, launcher closes" - no results list, no external
// process, no toolkit. Rendered the same way as Bar: plain Xlib
// shapes plus Xft text (see Font.h), for the same "own bar, no
// toolkit" reason.
//
// Deliberately never uses an active keyboard grab (XGrabKeyboard) -
// WindowManager just gives it ordinary X input focus and, while it's
// open, routes every KeyPress to it directly (see
// WindowManager::HandleModalKeyPress). That keeps this feature from
// ever being the reason Kohiko's global hotkeys stop responding - the
// exact failure mode the Super+Q hardening in WindowManager::Manage()
// guards against.
//
// Search architecture: the actual application list lives in
// AppIndex.h/LauncherScoring.h (parsing, deduplication, icon
// resolution, ranking - all pure, X11-free code, independently
// testable - see tests/test_launcherscoring.cpp). Launcher itself is
// just the X11 front end: it owns the in-memory index, rebuilds it on
// a background thread so opening/typing never blocks on the
// filesystem, and turns each keystroke's ranked results into rows on
// screen.

struct FileEntry
{
    std::string name;
    std::string path;
    bool isDirectory = false;

    // Precomputed once by ScanHomeFiles(), not per keystroke - see
    // Scoring::SplitWords()/BestFieldMatch() in LauncherScoring.h.
    std::string nameLower;
    std::vector<std::string> nameWords;
};

// An immutable record of exactly what the user submitted with Enter.
//
// The Launcher's selection (m_matches / m_selectedIndex) is live,
// mutable state: typing, paging and arrow keys all change it, and
// index rebuilds can reshuffle it. What gets LAUNCHED must not be
// read from that state after the fact. When Enter is accepted,
// HandleKeyPress() captures one of these - the actual command line
// (executable and arguments) or file path, resolved right then - and
// everything downstream (WindowManager::CloseLauncher()) launches
// from the snapshot only, so no later input, however fast, can change
// WHICH application was submitted.
//
// Deliberately contains NO workspace. Which workspace a launched
// window lands on is decided when its window actually appears (see
// WindowManager::Manage()), from the user's latest workspace context -
// freezing the Launcher's workspace here would be exactly the stale
// context bug this snapshot must not reintroduce.
struct LaunchRequest
{
    enum class Kind
    {
        Nothing,    // nothing to launch (empty selection / empty query)
        Command,    // run `command` through /bin/sh -c
        RevealFile  // open `path`'s folder in the file manager
    };

    Kind kind = Kind::Nothing;
    std::string command; // Kind::Command: executable + arguments, exactly as submitted
    std::string path;    // Kind::RevealFile: the selected file

    bool Empty() const { return kind == Kind::Nothing; }
};

class Launcher
{
public:

    explicit Launcher(
        XConnection& connection
    );

    ~Launcher();

    void Configure(
        const Config& config,
        const Rect& monitorGeometry
    );

    // Re-centers the (already-created) window on `monitorGeometry`
    // without touching anything Configure() reads from Config - just
    // the same size/position math Configure() itself runs, followed
    // by an XMoveResizeWindow if the window already exists. Called by
    // WindowManager right before Open(), so the launcher always opens
    // centered on whichever monitor currently has the cursor rather
    // than staying pinned to wherever it first opened. A no-op before
    // the first Configure() (m_window still 0).
    void Reposition(
        const Rect& monitorGeometry
    );

    // Opens the box empty, mapped, raised - WindowManager still has to
    // give it real input focus afterwards (it owns focus policy).
    void Open();

    // Unmaps the box. Safe to call even when already closed.
    void Close();

    bool IsOpen() const;

    ::Window WindowId() const;

    // The currently typed line - only meaningful while IsOpen().
    const std::string& Query() const;

    // The snapshot taken when Enter was last accepted (empty if Enter
    // has not been accepted since the Launcher opened). See
    // LaunchRequest. Consuming it clears it, so one Enter can launch
    // at most once.
    LaunchRequest TakeSubmitted();

    // Read-only state, for the `kohikoctl launcher` introspection
    // verb and tests.
    std::size_t SelectedIndex() const { return m_selectedIndex; }
    const Rect& Geometry() const { return m_geometry; }
    std::string SelectedCommand();
    std::string SelectedPath();
    bool SelectedIsFile() const;

    LauncherResult HandleKeyPress(
        const XKeyEvent& event
    );

    void HandleExpose();
    void HandleButtonPress(const XButtonEvent& event);
    // Called on Kohiko's ~1s idle heartbeat while open, to blink the
    // caret - cheap enough not to need a dedicated timer.
    void Blink();

    // Rebuilds the application index (from .desktop files, via
    // AppIndex::Build()) and the file index (from $HOME) on a
    // background thread, so newly-installed programs and newly-
    // created/removed files show up without restarting Kohiko. Not
    // called automatically after the first Configure() - wired up to
    // CommandType::LauncherReload (default bind Super+Shift+D) and
    // the `reloadlauncher` IPC verb, so it happens on demand instead
    // of on every single Open().
    void ReloadDesktopEntries();

private:

    // The "about 1/12 of the screen area, clamped" size/position math
    // shared by Configure() (first-ever placement) and Reposition()
    // (re-centering on a different monitor later) - see either call
    // site for why the two need to be kept in sync.
    Rect ComputeGeometry(
        const Rect& monitorGeometry
    ) const;

    // Resolves the current selection into a LaunchRequest, once.
    LaunchRequest SnapshotSelection();

    void Redraw();
    void UpdateMatches();

    // Appends a "Search <engine> for '<query>'" row to m_matches when
    // appropriate - either a recognized smart prefix ("yt cats") or,
    // when `haveLocalResults` is false, the configured default/
    // fallback engines. See LAUNCHER-SEARCH ENGINES in default.conf.
    void BuildInternetMatches(
        bool haveLocalResults
    );

    void AddInternetMatch(
        const std::string& engineId,
        const std::string& queryText,
        bool priority
    );

    // Joins any previous build (a no-op if it already finished, which
    // in practice it always has by the time this runs again) and
    // starts a fresh one on m_buildThread.
    void StartIndexRebuild();

    // Runs entirely on m_buildThread: builds/loads the application
    // index and scans $HOME for the file index, then publishes both
    // into m_index/m_files under m_indexMutex. Touches only the
    // filesystem (via AppIndex/IconResolver, both X11-free) - never
    // X11, Imlib2, or Config - so it's safe to run concurrently with
    // the UI thread. `includeHidden`/`strictFiltering` are captured by
    // value from Config at the point the thread is started, rather
    // than read from `this` here, so a later Configure() call can
    // never race this thread's reads of them.
    void RebuildIndexAndFiles(
        bool includeHidden,
        bool strictFiltering
    );

    // Returns the icon scaled to a 20x20 Pixmap and, via maskOut, the
    // 1-bit shape mask that goes with it (see the .cpp for why both
    // are needed to avoid a black box around every icon). Low-level -
    // always decodes/rasterizes; callers want ResolveIconPixmap()
    // below instead, which caches this by path.
    Pixmap LoadIcon(
        const std::string& path,
        Pixmap& maskOut);

    // LoadIcon(), but only once per distinct `iconPath` - every
    // subsequent call (whether for the same application redrawn again,
    // or a different application that happens to share an icon)
    // reuses the cached Pixmap/mask instead of re-decoding the file.
    // Empty `iconPath` (e.g. an Internet-search row, or an app whose
    // icon genuinely couldn't be resolved) returns {0, 0} - Redraw()
    // just skips drawing an icon for that row.
    Pixmap ResolveIconPixmap(
        const std::string& iconPath,
        Pixmap& maskOut);

private:

    XConnection& m_connection;

    ::Window m_window = 0;
    GC m_gc = nullptr;
    Font m_font;
    XftDraw* m_xftDraw = nullptr;

    XIM m_xim = nullptr;
    XIC m_xic = nullptr;

    Rect m_geometry;
    bool m_open = false;

    std::string m_query;
    LaunchRequest m_submitted; // set only by an accepted Enter; see LaunchRequest
    std::size_t m_cursor = 0;
    bool m_caretOn = true;

    unsigned long m_backgroundPixel = 0;
    unsigned long m_foregroundPixel = 0;
    unsigned long m_borderPixel = 0;
    unsigned long m_placeholderPixel = 0;

    // --- search index - written only by m_buildThread, read only from
    // inside UpdateMatches() while holding m_indexMutex. Nothing else
    // (Redraw(), SelectedCommand(), ...) touches these directly -
    // MatchResult below carries its own copy of whatever a row needs
    // to display/launch, precisely so the lock only ever has to cover
    // UpdateMatches() itself. ---
    std::mutex m_indexMutex;
    std::vector<IndexedApp> m_index;
    std::vector<FileEntry> m_files;
    std::string m_folderIconPath; // resolved once per rebuild, for FileEntry rows
    std::string m_fileIconPath;

    std::thread m_buildThread;
    bool m_indexRequested = false; // Configure() only kicks off the first background build once

    HistoryStore m_history;

    // Resolved Pixmap/mask for a given icon path, shared by
    // application, folder and generic-file icons alike - most systems
    // have far fewer distinct icon files than distinct applications
    // (many apps fall back to the same generic icon), so keying by
    // path here also naturally deduplicates that decoding work.
    std::unordered_map<std::string, Pixmap> m_iconCache;
    std::unordered_map<std::string, Pixmap> m_iconMaskCache;

    enum class MatchType
    {
        Application,
        File,
        Internet
    };

    // Self-contained: everything Redraw()/SelectedCommand()/
    // SelectedPath() need for one result row, copied out of
    // m_index/m_files (or built directly, for Internet rows) while
    // UpdateMatches() holds m_indexMutex - see the comment on
    // m_indexMutex above for why.
    struct MatchResult
    {
        MatchType type;
        std::string label;       // what Redraw() draws
        std::string command;     // Application/Internet - what SelectedCommand() returns
        std::string path;        // File - what SelectedPath() returns
        std::string desktopId;   // Application - what gets passed to HistoryStore::RecordLaunch()
        std::string iconPath;    // resolved icon path, or "" for no icon
        bool isDirectory = false;
        int score = 0;
        bool priority = false;   // Internet only - a recognized smart prefix always sorts first
    };

    std::vector<MatchResult> m_matches;
    std::size_t m_selectedIndex = 0;
    std::size_t m_scrollOffset = 0;

    // launcher.* config, resolved once in Configure() - see
    // default.conf's LAUNCHER section for what each one does.
    bool m_internetSearchEnabled = true;
    bool m_searchWhenNoResults = true;
    std::string m_defaultSearchEngine = "duckduckgo";
    std::string m_fallbackSearchEngine = "google";
    std::string m_customSearchUrl;
    bool m_showHiddenApps = false;
    bool m_strictFiltering = true;

    int VisibleRows() const;
};

}
