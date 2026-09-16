#pragma once

#include "ConfigSchema.h"
#include "ConfigWriter.h"
#include "Font.h"
#include "Types.h"

#include <X11/Xlib.h>

#include <string>
#include <vector>

namespace Kohiko
{

// Kohiko Settings: the GUI half of "Configuration GUI" from the spec.
// Unlike every other Kohiko-drawn window (Bar/Launcher/Notepad/
// PowerMenu/LockScreen - all override-redirect windows owned by the
// window manager process itself), this is a completely ordinary
// top-level client application, in its own `kohiko-settings` process,
// that Kohiko (or any other WM) tiles/manages exactly like a
// terminal or browser - see tools/kohiko-settings.cpp and the
// installed .desktop entry. Same plain-Xlib-shapes-plus-Xft-text
// approach as the rest of the project though, and no GTK/Qt/toolkit
// dependency - see the README for why that matters here.
//
// Reads every scalar (single-value) key from ConfigSchema and shows
// it as an editable field grouped by category/group; the four
// repeatable directives (bind=/exec.<name>=/windowrule=/monitor=) and
// the dynamically-numbered workspace<N>= family are each shown as a
// small raw-syntax text block instead of pretending they're
// individually-typed scalar settings - see BuildRawBlocks()/
// BuildWorkspaceFields(). All writes go through ConfigWriter, which
// edits kohiko.conf's existing lines/comments/ordering in place -
// manual editing of the same file remains fully supported before,
// after, or interleaved with using this GUI.
class SettingsWindow
{
public:

    SettingsWindow();
    ~SettingsWindow();

    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    // Opens the display, loads kohiko.conf + the schema, creates the
    // window. Logs why to stderr and returns false if anything
    // essential fails (no DISPLAY, config file missing, ...).
    bool Initialize();

    // Runs the event loop until the window is closed.
    void Run();

private:

    // --- one editable scalar setting -----------------------------------------

    struct Field
    {
        const ConfigOption* option = nullptr;
        std::string loadedValue;  // as last read from disk / last Applied
        std::string currentValue; // live edit buffer - see Dirty()
        std::string error;        // "" = valid; see ValidateField()
        bool infoExpanded = false;

        Rect rowRect{};      // this row's full clickable area, incl. any expanded info panel
        Rect widgetRect{};   // just the checkbox/text field/swatch part
        Rect infoIconRect{};
    };

    // --- a repeatable-directive family (bind=/exec./windowrule=/monitor=) ----

    // Which sub-control of a structured row a raw block's caretRow/
    // caretCol refer to - only meaningful while the focused
    // RawBlockPanel has kind != RawText (see RawBlockPanel::Kind and
    // LayoutAndDrawStructuredBlock()). Action isn't included here:
    // it's a click-to-cycle button (mirroring CycleEnumField()), not
    // a text cell, so it never holds the caret.
    enum class StructuredCell { Class, Instance, Title, Output, Workspace };

    struct RawBlockPanel
    {
        std::string id;         // stable id, e.g. "windowrule"
        std::string category;   // which sidebar Category this appears under
        std::string title;
        std::string helpText;   // short explanation shown above the text area
        std::string prefix;     // e.g. "windowrule=" ("exec." for the named-command family)

        std::vector<std::string> loadedLines; // as last read from disk / last Applied
        std::vector<std::string> lines;        // live edit buffer, one entry per line, prefix stripped
        std::vector<std::string> lineErrors;   // parallel to `lines`; "" = valid

        std::size_t caretRow = 0;

        // Byte offset of the caret - within `lines[caretRow]` itself
        // for a plain (kind == RawText) panel, or within whichever
        // structured sub-field's own text m_focusedCell names for a
        // structured one. Either way, the underlying storage that
        // actually gets validated/saved is always just `lines` -
        // structured editing only changes how a line's text gets
        // read and rewritten, never where it's kept. See
        // StructuredCellText()/SetStructuredCellText().
        std::size_t caretCol = 0;

        // windowrule=/monitor= get a structured row-based editor
        // instead of the plain multi-line textarea every other
        // repeatable directive (bind=/exec.<name>=) still uses - see
        // LayoutAndDrawStructuredBlock(). Set once, in BuildRawBlocks(),
        // and never changed after that.
        enum class Kind { RawText, WindowRule, Monitor };
        Kind kind = Kind::RawText;

        // Per-row control rects for a structured panel, rebuilt every
        // draw - parallel to `lines` (rowRects[i] belongs to lines[i]).
        // Unused (left empty) for a RawText panel.
        struct RowRects
        {
            Rect actionRect{};     // WindowRule only
            Rect classRect{};      // WindowRule only
            Rect instanceRect{};   // WindowRule only
            Rect titleRect{};      // WindowRule only
            Rect outputRect{};     // Monitor only
            Rect workspaceRect{};  // WindowRule (only when its action is Workspace) and Monitor
            Rect removeRect{};
        };
        std::vector<RowRects> rowRects;
        Rect addRowButtonRect{}; // structured panels only

        Rect rect{}; // the whole panel, including title/help/textarea
        Rect textAreaRect{}; // RawText panels only
    };

    // A windowrule= line's structured fields, parsed leniently from
    // (and re-serialized exactly back to) one RawBlockPanel::lines
    // entry - see ParseWindowRuleDraft()/SerializeWindowRuleDraft().
    // Deliberately its own lightweight, tolerant-of-partial-input type
    // rather than reusing WindowRule::Parse() directly: that parser
    // lowercases pattern values (correct for matching at runtime, but
    // it would silently mangle whatever case the user is mid-typing
    // here) and rejects anything incomplete outright, whereas a row
    // being actively edited is expected to pass through invalid
    // in-between states (e.g. no selector at all yet).
    struct WindowRuleDraft
    {
        int actionIndex = 1; // index into kWindowRuleActionTokens; 1 = "tile"
        std::string workspaceText; // digits typed after "workspace:" - only meaningful when actionIndex names the Workspace action
        std::string classText;
        std::string instanceText;
        std::string titleText;
    };

    // A monitor= line's structured fields - see WindowRuleDraft's
    // comment above for why this exists instead of MonitorRule::Parse().
    struct MonitorRuleDraft
    {
        std::string outputText;
        std::string workspaceText;
    };

    // --- a sidebar entry --------------------------------------------------------

    struct Category
    {
        std::string name;
        bool hasRawBlock = false; // Window Rules / Monitors / Developer / Input's Keybindings group
    };

private:

    // --- setup ------------------------------------------------------------------

    void BuildFields();
    void BuildRawBlocks();
    void BuildWorkspaceFields(); // workspace<N>= - regenerated whenever workspace.count changes

    void CreateWindow();
    void SetWmProperties();

    // --- event loop ---------------------------------------------------------------

    void HandleEvent(const XEvent& event);
    void HandleKeyPress(const XKeyEvent& event);
    void HandleButtonPress(const XButtonEvent& event);
    void HandleScroll(int direction);

    // --- field interaction --------------------------------------------------------

    void FocusField(Field* field, bool caretAtEnd);
    void FocusRawBlock(RawBlockPanel* panel);
    void ClearFocus();

    // Click-to-position variants of the two above: focuses the field/
    // block exactly like the plain versions, but places the caret at
    // the character boundary closest to the actual click point
    // instead of always jumping to one end. See CaretIndexForClick().
    void FocusFieldAtClick(Field* field, const Point& click);
    void FocusRawBlockAtClick(RawBlockPanel* panel, const Point& click);

    // Hit-tests `click` against a structured panel's own per-row
    // controls (action button, each text cell, remove button, the
    // "+ Add rule" button) built by the most recent
    // LayoutAndDrawStructuredBlock() call, and acts on whichever one
    // it lands in - focusing a cell (with the caret placed exactly
    // where clicked, same as FocusFieldAtClick()), cycling an action,
    // removing a row, or appending a fresh one. Returns false, having
    // touched nothing, for a click inside the panel's overall `rect`
    // but outside every individual control (e.g. the padding between
    // rows) - the caller (HandleButtonPress()) treats that exactly
    // like missing the panel entirely.
    bool HandleStructuredBlockClick(RawBlockPanel& panel, const Point& click);

    // Maps a click's X coordinate back to a byte offset into `text`,
    // assuming `text` is drawn left-aligned starting at `textStartX`
    // with this window's own m_font - i.e. the exact inverse of how
    // every caret-drawing call site here computes `caretX` from a
    // byte offset. Always lands on a UTF-8 character boundary, and
    // clamps to the nearest end for a click before/after the text.
    std::size_t CaretIndexForClick(
        const std::string& text,
        int textStartX,
        int clickX) const;

    void InsertCodepoint(const std::string& utf8Codepoint);
    void HandleFieldKey(const XKeyEvent& event, KeySym keysym, const std::string& typed);
    void HandleRawBlockKey(const XKeyEvent& event, KeySym keysym, const std::string& typed);
    void HandleSearchKey(const XKeyEvent& event, KeySym keysym, const std::string& typed);

    // Structured windowrule=/monitor= editing (panel.kind != RawText)
    // - see RawBlockPanel::Kind's comment and LayoutAndDrawStructuredBlock().
    void HandleStructuredBlockKey(const XKeyEvent& event, KeySym keysym, const std::string& typed);

    // Reads/rewrites one cell of `panel.lines[row]` by parsing the
    // whole line into a WindowRuleDraft/MonitorRuleDraft, touching
    // just the one field named by `cell`, and re-serializing the
    // *entire* line back - `lines` stays the sole source of truth
    // (see RawBlockPanel::caretCol's comment), these never keep their
    // own separate copy of a row's state.
    std::string StructuredCellText(const RawBlockPanel& panel, std::size_t row, StructuredCell cell) const;
    void SetStructuredCellText(RawBlockPanel& panel, std::size_t row, StructuredCell cell, const std::string& value);

    // The text-cell Tab order for `panel.lines[row]` - Class/
    // Instance/Title (+ Workspace, only when that row's action is
    // "Workspace") for a WindowRule panel, or Output/Workspace for a
    // Monitor one. Used by both MoveStructuredFocus() and mouse-click
    // handling to agree on the same set of tabbable cells.
    std::vector<StructuredCell> StructuredCellOrder(const RawBlockPanel& panel, std::size_t row) const;

    // Moves the caret to the next (direction > 0) or previous
    // (direction < 0) cell in StructuredCellOrder(), crossing into
    // the adjacent row when it runs off either end of the current one.
    void MoveStructuredFocus(RawBlockPanel& panel, int direction);

    // Advances a row's action to the next entry in kWindowRuleActionTokens
    // (WindowRule panels only) - the click-to-cycle button mirrors
    // CycleEnumField()'s own interaction model.
    void CycleStructuredAction(RawBlockPanel& panel, std::size_t row);

    // A single freshly-created row for "+ Add rule"/"+ Add monitor
    // rule" - "tile" with no selector yet for WindowRule (matching
    // this class's own default action), or a blank output name for
    // Monitor - either way intentionally invalid until the user
    // actually fills in a field, exactly like an empty line typed
    // into the old raw-text editor was.
    std::string DefaultStructuredLine(RawBlockPanel::Kind kind) const;

    WindowRuleDraft ParseWindowRuleDraft(const std::string& line) const;
    std::string SerializeWindowRuleDraft(const WindowRuleDraft& draft) const;
    MonitorRuleDraft ParseMonitorRuleDraft(const std::string& line) const;
    std::string SerializeMonitorRuleDraft(const MonitorRuleDraft& draft) const;

    void ToggleBoolField(Field& field) const;
    void CycleEnumField(Field& field) const;

    void ValidateField(Field& field) const;
    void ValidateRawBlockLine(const RawBlockPanel& panel, const std::string& line, std::string& outError) const;

    bool FieldDirty(const Field& field) const;
    bool RawBlockDirty(const RawBlockPanel& panel) const;
    bool AnyDirty() const;

    // --- commands -----------------------------------------------------------------

    // Validates every dirty field/raw block, writes kohiko.conf (via
    // ConfigWriter, editing existing lines in place), immediately
    // tells any running `kohiko` to reload its config, and reports
    // the result via ShowStatus() - all without closing this window,
    // so there's never a reason to leave Settings just to see whether
    // a change actually applied. Fields with a validation error are
    // left exactly as typed (and still flagged) rather than being
    // silently dropped or blocking the fields that *did* validate.
    void Save();
    void ResetVisibleToDefault();
    void ReloadRunningKohiko() const;
    void ShowStatus(const std::string& message);

    // --- layout & drawing -----------------------------------------------------------

    void Redraw();
    void LayoutAndDrawSidebar(int& outWidth);
    void LayoutAndDrawTopBar(int sidebarWidth);
    void LayoutAndDrawContent(int sidebarWidth);
    void LayoutAndDrawField(Field& field, int x, int y, int width, int& outHeight);
    void LayoutAndDrawRawBlock(RawBlockPanel& panel, int x, int y, int width, int& outHeight);

    // windowrule=/monitor='s own row-based renderer - called from
    // LayoutAndDrawRawBlock() itself (branching on panel.kind), never
    // needs a separate call site of its own.
    void LayoutAndDrawStructuredBlock(RawBlockPanel& panel, int x, int y, int width, int& outHeight);

    void LayoutAndDrawBottomBar();

    void DrawText(int x, int y, const std::string& text, unsigned long pixel);
    void DrawCheckbox(const Rect& rect, bool checked);
    void DrawButton(const Rect& rect, const std::string& label, bool enabled, bool primary);
    std::vector<std::string> WrapText(const std::string& text, int maxWidth) const;

    // Every Field currently on screen, in on-screen order - either the
    // selected category's fields, or (while m_searchText is non-empty)
    // every field/panel anywhere matching it. What Reset to Default
    // and the layout pass both iterate.
    std::vector<Field*> VisibleFields();
    std::vector<RawBlockPanel*> VisibleRawBlocks();

    static std::string HumanizeKey(const std::string& key);

private:

    // --- X plumbing -----------------------------------------------------------------

    Display* m_display = nullptr;
    int m_screen = 0;
    ::Window m_window = 0;
    GC m_gc = nullptr;
    Font m_font;
    Font m_boldFont; // section/category headings - see Initialize()
    XftDraw* m_xftDraw = nullptr;
    XIM m_xim = nullptr;
    XIC m_xic = nullptr;
    Atom m_wmDeleteWindow = 0;

    Rect m_geometry;
    bool m_running = false;

    // --- config -----------------------------------------------------------------

    std::string m_configPath;
    ConfigWriter m_writer;

    // --- content -----------------------------------------------------------------

    std::vector<Category> m_categories;
    std::vector<Field> m_fields;
    std::vector<RawBlockPanel> m_rawBlocks;

    // workspace<N>= fields are regenerated (not fixed ConfigSchema
    // entries - N depends on the live value of workspace.count), so
    // their backing ConfigOptions have to live somewhere Field can
    // safely point into; this is that storage. See BuildWorkspaceFields().
    std::vector<ConfigOption> m_workspaceOptionStorage;
    std::vector<Field> m_workspaceFields;

    int m_selectedCategory = 0;
    std::string m_searchText;
    bool m_searchFocused = false;
    std::size_t m_searchCaret = 0;

    int m_scrollOffset = 0;
    int m_contentTotalHeight = 0;

    Field* m_focusedField = nullptr;
    std::size_t m_fieldCaret = 0;
    RawBlockPanel* m_focusedRawBlock = nullptr;

    // Which sub-field of m_focusedRawBlock->caretRow is focused -
    // only meaningful while m_focusedRawBlock's own kind != RawText.
    StructuredCell m_focusedCell = StructuredCell::Class;

    std::string m_statusMessage;

    // --- layout (recomputed every Redraw()) -----------------------------------------

    Rect m_sidebarRect{};
    Rect m_searchRect{};
    Rect m_contentRect{};
    Rect m_saveButtonRect{};
    Rect m_resetButtonRect{};
    std::vector<Rect> m_categoryRects; // parallel to m_categories

    // --- colors (same palette convention as Bar/Notepad/LockScreen) ----------------

    unsigned long m_backgroundPixel = 0;
    unsigned long m_panelPixel = 0;
    unsigned long m_foregroundPixel = 0;
    unsigned long m_mutedPixel = 0;
    unsigned long m_accentPixel = 0;
    unsigned long m_fieldPixel = 0;
    unsigned long m_errorPixel = 0;
    unsigned long m_borderPixel = 0;

};

}
