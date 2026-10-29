// WindowRuleStore: how the per-application rule is found, saved,
// changed and deleted - in memory (lists of `windowrule=` values) and
// through a real config file on disk (ConfigWriter) - plus the
// guarantee the whole redesign rests on: only an explicit edit ever
// changes a rule.

#include "Config.h"
#include "WindowRule.h"
#include "WindowRuleStore.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

using namespace Kohiko;
namespace fs = std::filesystem;
using Lines = std::vector<std::string>;

static int g_pass = 0;
static int g_fail = 0;

static void Check(bool condition, const char* message)
{
    if (condition) { ++g_pass; std::printf("  PASS: %s\n", message); }
    else           { ++g_fail; std::printf("  FAIL: %s\n", message); }
}

static WindowRule Rule(const std::string& app, int ws, WindowRule::Horizontal h = WindowRule::Horizontal::Default,
                       WindowRule::Vertical v = WindowRule::Vertical::Default,
                       WindowRule::Mode m = WindowRule::Mode::Default)
{
    WindowRule r;
    r.appKey = app; r.workspace = ws; r.horizontal = h; r.vertical = v; r.mode = m;
    return r;
}

static std::string Slurp(const fs::path& p)
{
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void Spit(const fs::path& p, const std::string& text)
{
    std::ofstream f(p, std::ios::trunc);
    f << text;
}

// Every line of `text` that is NOT a windowrule= line.
static Lines NonRuleLines(const std::string& text)
{
    Lines out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
    {
        std::string t = line;
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
        if (t.rfind("windowrule=", 0) != 0)
            out.push_back(line);
    }
    return out;
}

int main()
{
    using H = WindowRule::Horizontal;
    using V = WindowRule::Vertical;
    using M = WindowRule::Mode;

    std::printf("FindAppRule():\n");
    {
        Lines lines = { "app:firefox workspace:3", "tile class:tlauncher" };
        Check(!WindowRuleStore::FindAppRule(lines, "kitty").has_value(), "no rule for an application without one");
        auto r = WindowRuleStore::FindAppRule(lines, "firefox");
        Check(r && r->workspace == 3, "finds the saved rule");
        Check(WindowRuleStore::FindAppRule(lines, "FIREFOX").has_value(), "...case-insensitively");
        Check(!WindowRuleStore::FindAppRule(lines, "tlauncher").has_value(),
              "a hand-written class: rule is NOT 'the app rule' for that application");

        Lines multi = { "app:firefox workspace:3 title:private" };
        Check(!WindowRuleStore::FindAppRule(multi, "firefox").has_value(),
              "an app: rule that also has another selector is advanced/hand-written, not the app rule");
    }

    std::printf("\nUpsertAppRule():\n");
    {
        Lines lines = { "tile class:tlauncher" };
        lines = WindowRuleStore::UpsertAppRule(lines, Rule("firefox", 3, H::Right, V::Top, M::Tiled));
        Check(lines.size() == 2 && lines[1] == "app:firefox workspace:3 horizontal:right vertical:top mode:tiled",
              "a new rule is appended in canonical form");
        Check(lines[0] == "tile class:tlauncher", "...and an existing hand-written rule is untouched");

        lines = WindowRuleStore::UpsertAppRule(lines, Rule("telegram-desktop", 2, H::Right, V::Bottom, M::Floating));
        Check(lines.size() == 3, "a second application gets its own, independent line");

        // Modify in place: position among the others must be preserved.
        lines = WindowRuleStore::UpsertAppRule(lines, Rule("firefox", 5));
        Check(lines.size() == 3, "changing a rule doesn't add a line");
        Check(lines[1] == "app:firefox workspace:5", "...it replaces the rule IN PLACE (same position)");
        Check(lines[2].find("telegram-desktop") != std::string::npos && lines[2].find("workspace:2") != std::string::npos,
              "...and the other application's rule is untouched");

        auto f = WindowRuleStore::FindAppRule(lines, "firefox");
        Check(f && f->workspace == 5 && f->horizontal == H::Default && f->mode == M::Default,
              "unchecked fields really are cleared back to Default");

        // Fields coexist independently.
        lines = WindowRuleStore::UpsertAppRule(lines, Rule("foo", 0, H::Left));
        auto g = WindowRuleStore::FindAppRule(lines, "foo");
        Check(g && g->workspace == 0 && g->horizontal == H::Left && g->vertical == V::Default,
              "a horizontal-only rule keeps workspace unset");

        // Duplicates collapse.
        Lines dups = { "app:foo workspace:1", "tile class:x", "app:FOO workspace:2" };
        dups = WindowRuleStore::UpsertAppRule(dups, Rule("foo", 7));
        Check(dups.size() == 2 && dups[0] == "app:foo workspace:7" && dups[1] == "tile class:x",
              "duplicate app rules for one application collapse into the one kept (in the first one's place)");
    }

    std::printf("\nReset / delete:\n");
    {
        Lines lines = { "app:firefox workspace:3", "tile class:tlauncher", "app:kitty mode:floating" };

        Lines afterReset = WindowRuleStore::UpsertAppRule(lines, Rule("firefox", 0));
        Check(afterReset.size() == 2 && !WindowRuleStore::FindAppRule(afterReset, "firefox").has_value(),
              "saving a rule with every field at Default removes it (an empty rule isn't a rule)");

        Lines afterDelete = WindowRuleStore::RemoveAppRule(lines, "firefox");
        Check(afterDelete == afterReset, "RemoveAppRule is the same thing");
        Check(afterDelete[0] == "tile class:tlauncher" && afterDelete[1] == "app:kitty mode:floating",
              "...and only that application's rule goes: everything else stays, in order");

        Check(WindowRuleStore::RemoveAppRule(lines, "nothere") == lines, "deleting a rule that doesn't exist changes nothing");
        Check(WindowRuleStore::RemoveAppRule(Lines{}, "x").empty(), "deleting from an empty list is fine");
    }

    std::printf("\nNothing it doesn't own is ever touched:\n");
    {
        Lines lines = {
            "garbage that is not a rule",
            "app:firefox workspace:3",
            "class:mpv nofullscreen",
            "workspace:0 class:bad",
        };
        Lines out = WindowRuleStore::UpsertAppRule(lines, Rule("firefox", 4));
        Check(out[0] == lines[0] && out[2] == lines[2] && out[3] == lines[3],
              "lines that don't parse, and hand-written rules, pass through verbatim");
        Check(out[1] == "app:firefox workspace:4", "...while the one app rule changes");

        Check(WindowRuleStore::UpsertAppRule(lines, WindowRule{}) == lines, "a rule with no application key is ignored");
    }

    std::printf("\nOtherMatchingRules():\n");
    {
        Lines lines = { "app:firefox workspace:3", "float class:fire", "tile class:mpv", "workspace:9 class:firefox title:private" };
        Lines others = WindowRuleStore::OtherMatchingRules(lines, "firefox", "Navigator", "Mozilla Firefox");
        Check(others.size() == 1 && others[0] == "float class:fire",
              "lists hand-written rules that also match, but not the window's own app rule or non-matching ones");
        others = WindowRuleStore::OtherMatchingRules(lines, "firefox", "Navigator", "Private - Mozilla Firefox private");
        Check(others.size() == 2, "...including a more specific one when its title matches too");
    }

    std::printf("\nConfig file round trip:\n");
    {
        fs::path dir = fs::temp_directory_path() / ("kohiko_wrstore_" + std::to_string(::getpid()));
        fs::create_directories(dir);
        fs::path conf = dir / "kohiko.conf";

        const std::string original =
            "# my config - keep this comment\n"
            "general.gap=6\n"
            "bind=SUPER+RETURN,exec terminal\n"
            "bind=SUPER+Q,close\n"
            "\n"
            "# rules\n"
            "windowrule=tile class:tlauncher\n"
            "exec.terminal=kitty\n"
            "workspace2=discord\n";
        Spit(conf, original);

        Lines lines;
        Check(WindowRuleStore::LoadLines(conf.string(), lines) && lines.size() == 1 && lines[0] == "tile class:tlauncher",
              "LoadLines reads only the windowrule= values");

        // Create.
        lines = WindowRuleStore::UpsertAppRule(lines, Rule("firefox", 3, H::Right, V::Top, M::Tiled));
        Check(WindowRuleStore::SaveLines(conf.string(), lines), "SaveLines succeeds");

        std::string saved = Slurp(conf);
        Check(NonRuleLines(saved) == NonRuleLines(original),
              "every NON-rule line (comments, binds, settings, autostart) is byte-identical after saving a rule");
        Check(saved.find("windowrule=app:firefox workspace:3 horizontal:right vertical:top mode:tiled") != std::string::npos,
              "the rule is in the file in canonical form");
        Check(saved.find("windowrule=tile class:tlauncher") != std::string::npos, "the existing rule survived");

        // Persistence / reload: a fresh Config sees it.
        {
            Config reloaded;
            Check(reloaded.Load(conf.string()), "the saved file loads as a normal config");
            std::vector<WindowRule> rules = LoadWindowRules(reloaded);
            WindowRuleEffect e = ResolveWindowRules(rules, "firefox", "Navigator", "x");
            Check(rules.size() == 2 && e.forceWorkspace == 3 && e.horizontal == H::Right && e.vertical == V::Top && e.forceTile,
                  "reloading yields exactly the saved rule (persistence)");
        }

        // Modify.
        Lines again;
        WindowRuleStore::LoadLines(conf.string(), again);
        again = WindowRuleStore::UpsertAppRule(again, Rule("firefox", 6, H::Left));
        WindowRuleStore::SaveLines(conf.string(), again);
        Lines reread;
        WindowRuleStore::LoadLines(conf.string(), reread);
        auto m = WindowRuleStore::FindAppRule(reread, "firefox");
        Check(m && m->workspace == 6 && m->horizontal == H::Left && m->vertical == V::Default && m->mode == M::Default,
              "a modified rule is what's on disk afterwards");
        Check(NonRuleLines(Slurp(conf)) == NonRuleLines(original), "...still without disturbing any other line");

        // Delete.
        Lines del;
        WindowRuleStore::LoadLines(conf.string(), del);
        del = WindowRuleStore::RemoveAppRule(del, "firefox");
        WindowRuleStore::SaveLines(conf.string(), del);
        Lines gone;
        WindowRuleStore::LoadLines(conf.string(), gone);
        Check(!WindowRuleStore::FindAppRule(gone, "firefox").has_value(), "a deleted rule is gone from the file");
        Check(gone.size() == 1 && gone[0] == "tile class:tlauncher", "...and the other rule is still there");
        Check(NonRuleLines(Slurp(conf)) == NonRuleLines(original), "...and every other line is still byte-identical");

        // Nothing to write, nothing there: the file must not change at all.
        fs::path plain = dir / "plain.conf";
        Spit(plain, "general.gap=6\nbind=SUPER+Q,close\n");
        std::string before = Slurp(plain);
        WindowRuleStore::SaveLines(plain.string(), Lines{});
        Check(Slurp(plain) == before, "saving an empty rule list into a config with no rules leaves the file untouched");

        // Missing file: refuse, and never create one (it would have no bind= lines).
        fs::path missing = dir / "nope.conf";
        Lines tmp;
        Check(!WindowRuleStore::LoadLines(missing.string(), tmp), "a missing config can't be loaded");
        Check(!WindowRuleStore::SaveLines(missing.string(), Lines{ "app:x workspace:1" }), "...nor saved to");
        Check(!fs::exists(missing), "...and no config file is fabricated (it would have no bind= lines and silently remove every keybinding)");

        fs::remove_all(dir);
    }

    std::printf("\nLoadWindowRules() skips bad lines instead of half-applying them:\n");
    {
        fs::path dir = fs::temp_directory_path() / ("kohiko_wrload_" + std::to_string(::getpid()));
        fs::create_directories(dir);
        fs::path conf = dir / "kohiko.conf";
        Spit(conf,
            "windowrule=app:good workspace:2\n"
            "windowrule=app:typo workspce:2\n"
            "windowrule=tile\n"
            "windowrule=float class:alsogood\n");

        Config config;
        config.Load(conf.string());
        std::vector<WindowRule> rules = LoadWindowRules(config);
        Check(rules.size() == 2, "only the two valid rules load");
        Check(ResolveWindowRules(rules, "typo", "", "").forceWorkspace == 0,
              "the typo'd rule does not apply at all (rather than applying without its workspace)");
        fs::remove_all(dir);
    }


    std::printf("\nScoped (role/type) application rules are independent rules:\n");
    {
        RuleScope pipScope;     pipScope.role = "PictureInPicture";
        RuleScope browserScope; browserScope.role = "browser";

        Lines lines = {
            "app:zen workspace:2",
            "app:zen role:browser workspace:3 mode:tiled",
            "app:zen role:PictureInPicture workspace:1 mode:floating",
            "class:zen title:foo workspace:4",
            "app:other workspace:5"
        };

        Check(WindowRuleStore::FindAppRule(lines, "zen")->workspace == 2, "the plain app lookup finds only the whole-application rule");
        Check(WindowRuleStore::FindAppRule(lines, "zen", browserScope)->workspace == 3, "the browser scope finds the browser rule");
        Check(WindowRuleStore::FindAppRule(lines, "zen", pipScope)->workspace == 1, "the PiP scope finds the PiP rule");
        RuleScope other; other.role = "settings";
        Check(!WindowRuleStore::FindAppRule(lines, "zen", other).has_value(), "a scope with no rule finds nothing (no fallback to the whole app)");

        // Editing the plain app rule must leave both scoped rules untouched.
        WindowRule edited; edited.appKey = "zen"; edited.workspace = 4;
        Lines after = WindowRuleStore::UpsertAppRule(lines, edited);
        Check(after.size() == lines.size(), "upserting the plain rule adds/removes nothing");
        Check(after[0] == "app:zen workspace:4", "...the plain rule changed in place");
        Check(after[1] == lines[1] && after[2] == lines[2] && after[3] == lines[3] && after[4] == lines[4],
              "...and every other line, including both scoped rules, is byte-identical");

        // Editing the PiP rule leaves the plain and browser rules alone.
        WindowRule pip; pip.appKey = "zen"; pip.role = "PictureInPicture"; pip.workspace = 2; pip.mode = WindowRule::Mode::Floating;
        pip.horizontal = WindowRule::Horizontal::Right;
        after = WindowRuleStore::UpsertAppRule(lines, pip);
        Check(after[2] == "app:zen role:PictureInPicture workspace:2 horizontal:right mode:floating", "the PiP rule is replaced in place with its role kept");
        Check(after[0] == lines[0] && after[1] == lines[1], "the plain and browser rules are untouched");

        // A scoped rule is created independently when it doesn't exist yet.
        Lines fresh = { "app:zen workspace:2" };
        after = WindowRuleStore::UpsertAppRule(fresh, pip);
        Check(after.size() == 2 && after[0] == fresh[0] && after[1] == "app:zen role:PictureInPicture workspace:2 horizontal:right mode:floating",
              "a new scoped rule is appended; the plain rule is not touched");

        // Deleting one scope deletes only that rule.
        after = WindowRuleStore::RemoveAppRule(lines, "zen", pipScope);
        Check(after.size() == 4 && after[0] == lines[0] && after[1] == lines[1] && after[2] == lines[3],
              "deleting the PiP rule removes exactly that line");
        after = WindowRuleStore::RemoveAppRule(lines, "zen");
        Check(after.size() == 4 && after[0] == lines[1] && after[1] == lines[2],
              "deleting the plain app rule keeps both scoped rules");

        // The chooser's "N other rules also apply" count sees the other scopes.
        WindowIdentity pipWindow;
        pipWindow.className = "zen"; pipWindow.instanceName = "Toolkit"; pipWindow.title = "Picture-in-Picture foo";
        pipWindow.role = "PictureInPicture"; pipWindow.windowType = "utility";
        Lines others = WindowRuleStore::OtherMatchingRules(lines, pipWindow, pipScope);
        Check(others.size() == 2 && others[0] == lines[0] && others[1] == lines[3],
              "editing the PiP rule: the plain app rule and the hand-written class:/title: rule are 'other' rules; the browser rule doesn't match");
        others = WindowRuleStore::OtherMatchingRules(lines, pipWindow, RuleScope());
        Check(others.size() == 2 && others[0] == lines[2],
              "editing the plain app rule: the PiP rule is reported as an 'other' rule that also applies");
    }

    std::printf("\nRole/type rules survive a real config file and a restart:\n");
    {
        fs::path dir = fs::temp_directory_path() / ("kohiko_zenrules_" + std::to_string(::getpid()));
        fs::create_directories(dir);
        fs::path conf = dir / "kohiko.conf";
        Spit(conf,
            "general.inner_gap=6\n"
            "bind=SUPER+Q close\n"
            "windowrule=app:zen workspace:2\n"
            "windowrule=app:zen role:browser type:normal workspace:3 mode:tiled\n"
            "windowrule=app:zen role:PictureInPicture type:utility workspace:1 mode:floating horizontal:right vertical:bottom\n");

        Lines onDisk;
        Check(WindowRuleStore::LoadLines(conf.string(), onDisk) && onDisk.size() == 3, "all three lines load");

        WindowRule pip = *WindowRuleStore::FindAppRule(onDisk, "zen", [] { RuleScope s; s.role = "PictureInPicture"; s.windowType = "utility"; return s; }());
        pip.workspace = 2;
        Lines updated = WindowRuleStore::UpsertAppRule(onDisk, pip);
        Check(WindowRuleStore::SaveLines(conf.string(), updated), "saved");

        Config config;
        config.Load(conf.string());
        std::vector<WindowRule> rules = LoadWindowRules(config);
        Check(rules.size() == 3, "after a 'restart' (reload from disk) all three rules load");

        WindowIdentity browser; browser.className = "zen"; browser.instanceName = "Navigator"; browser.role = "browser"; browser.windowType = "normal";
        WindowIdentity pipWin;  pipWin.className = "zen";  pipWin.instanceName = "Toolkit";   pipWin.role = "PictureInPicture"; pipWin.windowType = "utility";
        WindowRuleEffect b = ResolveWindowRules(rules, browser);
        WindowRuleEffect p = ResolveWindowRules(rules, pipWin);
        Check(b.forceWorkspace == 3 && b.forceTile, "browser: still workspace 3, tiled");
        Check(p.forceWorkspace == 2 && p.forceFloat && p.horizontal == WindowRule::Horizontal::Right, "PiP: the edit (workspace 2) is there, the rest unchanged");

        std::ifstream in(conf);
        std::stringstream text; text << in.rdbuf();
        Check(text.str().find("bind=SUPER+Q close") != std::string::npos && text.str().find("general.inner_gap=6") != std::string::npos,
              "the rest of the config file is untouched");
        fs::remove_all(dir);
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
