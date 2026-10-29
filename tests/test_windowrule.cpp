// WindowRule: parsing (new grammar AND every legacy form that existed
// before 0.22.0), matching, serialisation round trips, and
// ResolveWindowRules() - the single place that turns "all the rules
// that match this window" into one deterministic effect.
//
// Pure logic, no X11 and no files: every rule is a string in, a
// decision out.

#include "WindowRule.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace Kohiko;

static int g_pass = 0;
static int g_fail = 0;

static void Check(bool condition, const char* message)
{
    if (condition) { ++g_pass; std::printf("  PASS: %s\n", message); }
    else           { ++g_fail; std::printf("  FAIL: %s\n", message); }
}

static WindowRule P(const std::string& text)
{
    WindowRule rule;
    std::string error;
    if (!WindowRule::Parse(text, rule, &error))
        std::printf("  (unexpected parse failure for [%s]: %s)\n", text.c_str(), error.c_str());
    return rule;
}

static bool Rejects(const std::string& text)
{
    WindowRule rule;
    return !WindowRule::Parse(text, rule);
}

using H = WindowRule::Horizontal;
using V = WindowRule::Vertical;
using M = WindowRule::Mode;

int main()
{
    std::printf("Legacy single-action grammar still parses exactly as before:\n");
    {
        WindowRule r = P("tile class:tlauncher");
        Check(r.mode == M::Tiled && r.classPattern == "tlauncher" && r.workspace == 0, "tile class:tlauncher");

        r = P("float class:pavucontrol");
        Check(r.mode == M::Floating && r.classPattern == "pavucontrol", "float class:pavucontrol");

        r = P("fullscreen class:flameshot");
        Check(r.mode == M::Fullscreen, "fullscreen class:flameshot");

        r = P("nofullscreen class:mpv");
        Check(r.denyFullscreen && r.mode == M::Default, "nofullscreen class:mpv is a deny flag, not a mode");

        r = P("workspace:8 class:telegram title:photo");
        Check(r.workspace == 8 && r.classPattern == "telegram" && r.titlePattern == "photo",
              "workspace:8 class:telegram title:photo");

        r = P("TILE Class:TLauncher");
        Check(r.mode == M::Tiled && r.classPattern == "TLauncher", "keywords are case-insensitive; selector text keeps its case");

        r = P("tile class:tlauncher          # never let it float at its own size");
        Check(r.mode == M::Tiled && r.classPattern == "tlauncher", "a trailing inline # comment (as the old docs showed) is ignored");

        r = P("float instance:pavucontrol title:volume");
        Check(r.instancePattern == "pavucontrol" && r.titlePattern == "volume", "instance: and title: selectors");
    }

    std::printf("\nNew grammar: every property, any order, each optional:\n");
    {
        WindowRule r = P("app:firefox workspace:3 horizontal:right vertical:top mode:tiled");
        Check(r.appKey == "firefox" && r.workspace == 3 && r.horizontal == H::Right &&
              r.vertical == V::Top && r.mode == M::Tiled, "the full example from the design");

        r = P("mode:floating vertical:bottom app:telegram-desktop horizontal:right workspace:2");
        Check(r.appKey == "telegram-desktop" && r.workspace == 2 && r.horizontal == H::Right &&
              r.vertical == V::Bottom && r.mode == M::Floating, "same fields in a different order");

        r = P("app:kitty workspace:1 mode:fullscreen");
        Check(r.horizontal == H::Default && r.vertical == V::Default && r.mode == M::Fullscreen,
              "unset fields stay Default (workspace + mode only)");

        r = P("app:foo workspace:3");
        Check(r.workspace == 3 && r.horizontal == H::Default && r.vertical == V::Default && r.mode == M::Default,
              "workspace-only: position and mode untouched");

        r = P("app:foo horizontal:left");
        Check(r.workspace == 0 && r.horizontal == H::Left && r.vertical == V::Default && r.mode == M::Default,
              "horizontal-only: workspace and the rest untouched");

        r = P("app:foo vertical:bottom");
        Check(r.vertical == V::Bottom && r.horizontal == H::Default && r.workspace == 0, "vertical-only");

        r = P("app:foo mode:floating");
        Check(r.mode == M::Floating && r.workspace == 0, "mode-only: workspace untouched");

        r = P("app:foo tile");
        Check(r.mode == M::Tiled, "legacy bare word is accepted alongside the new grammar");

        r = P("app:foo tile mode:tiled");
        Check(r.mode == M::Tiled, "a legacy word and the same mode: spelling agree");
    }

    std::printf("\nStrictness: a rule that can't be applied exactly as written is rejected, not half-applied:\n");
    {
        Check(Rejects("tile"), "no selector at all (would match every window)");
        Check(Rejects("class:foo"), "selector but no property");
        Check(Rejects("app:foo"), "app selector but no property");
        Check(Rejects("workspace:3"), "property but no selector");
        Check(Rejects("app:foo workspace:0"), "workspace:0");
        Check(Rejects("app:foo workspace:-2"), "negative workspace");
        Check(Rejects("app:foo workspace:abc"), "non-numeric workspace");
        Check(Rejects("app:foo workspace:"), "empty workspace value");
        Check(Rejects("app:foo horizontal:middle"), "horizontal:middle");
        Check(Rejects("app:foo vertical:center"), "vertical:center");
        Check(Rejects("app:foo mode:maximised"), "unknown mode");
        Check(Rejects("app:foo tile mode:floating"), "conflicting modes");
        Check(Rejects("app:foo float fullscreen"), "conflicting legacy modes");
        Check(Rejects("app:foo workspace:2 workspace:3"), "workspace given twice");
        Check(Rejects("app:foo horizontal:left horizontal:right"), "horizontal given twice");
        Check(Rejects("app:foo workspce:3"), "a typo'd key (would otherwise silently drop the property)");
        Check(Rejects("class:foo pid:123 tile"), "an unsupported selector (would silently BROADEN the rule)");
        // `role:` used to be this test's example of an unsupported selector;
        // it is a real, exact-match selector now (see the Zen section below).
        Check(Rejects("app:foo bogus"), "a stray bare word");
        Check(Rejects(""), "empty rule");
        Check(Rejects("   "), "whitespace-only rule");

        WindowRule untouched = P("tile class:keep");
        WindowRule scratch = untouched;
        WindowRule::Parse("this is garbage", scratch);
        Check(scratch == untouched, "a failed Parse() leaves its output argument untouched");
    }

    std::printf("\nSerialisation round trip (Parse(Serialize(r)) == r):\n");
    {
        const char* samples[] = {
            "app:firefox workspace:3 horizontal:right vertical:top mode:tiled",
            "app:telegram-desktop workspace:2 horizontal:right vertical:bottom mode:floating",
            "app:kitty workspace:1 mode:fullscreen",
            "app:foo horizontal:left",
            "class:tlauncher mode:tiled",
            "class:telegram title:photo workspace:8",
            "class:mpv nofullscreen",
            "app:Foo instance:bar title:baz mode:floating",
        };

        bool allOk = true;
        for (const char* sample : samples)
        {
            WindowRule a = P(sample);
            WindowRule b = P(a.Serialize());
            if (!(a == b)) { allOk = false; std::printf("  (round trip differs for [%s] -> [%s])\n", sample, a.Serialize().c_str()); }
        }
        Check(allOk, "every sample survives a serialise/parse round trip");

        Check(P("mode:floating workspace:3 app:foo").Serialize() == "app:foo workspace:3 mode:floating",
              "serialisation is canonical regardless of input order");
        Check(P("float class:pavucontrol").Serialize() == "class:pavucontrol mode:floating",
              "a legacy rule serialises into the new canonical form");
        Check(P("app:Foo workspace:2").Serialize() == "app:Foo workspace:2", "the application key keeps its case when written back");
    }

    std::printf("\n%%XX escapes (application keys / titles with spaces):\n");
    {
        Check(WindowRule::EncodeValue("My App") == "My%20App", "a space is escaped");
        Check(WindowRule::EncodeValue("100%") == "100%25", "a literal percent is escaped");
        Check(WindowRule::DecodeValue("My%20App") == "My App", "...and decoded");
        Check(WindowRule::DecodeValue("100%25") == "100%", "...a literal percent too");
        Check(WindowRule::DecodeValue("100%") == "100%", "a lone % is kept literally");
        Check(WindowRule::DecodeValue("a%zzb") == "a%zzb", "an invalid escape is kept literally");

        WindowRule r = P("app:My%20App workspace:2");
        Check(r.appKey == "My App", "app:My%20App parses to \"My App\"");
        Check(r.Matches("My App", "", ""), "...and matches a window whose class really contains a space");
        Check(P(r.Serialize()).appKey == "My App", "...and round-trips");
    }

    std::printf("\nMatching:\n");
    {
        WindowRule r = P("app:firefox workspace:3");
        Check(r.Matches("firefox", "Navigator", "Mozilla Firefox"), "app: matches the class");
        Check(r.Matches("Firefox", "x", "y"), "app: is case-insensitive");
        Check(!r.Matches("firefox-developer-edition", "", ""), "app: is EXACT - not a substring of a longer class");
        Check(!r.Matches("xfirefox", "", ""), "app: is exact at the start too");
        Check(!r.Matches("kitty", "firefox", "firefox"), "app: does not match on the instance or title when a class exists");
        Check(r.Matches("", "firefox", "whatever"), "app: falls back to the instance only when the class is empty");

        WindowRule c = P("tile class:tele");
        Check(c.Matches("TelegramDesktop", "", ""), "class: stays a case-insensitive substring match (legacy)");
        Check(!c.Matches("kitty", "", ""), "...and doesn't match unrelated classes");

        WindowRule both = P("workspace:8 class:telegram title:photo");
        Check(both.Matches("TelegramDesktop", "", "Photo viewer"), "all given selectors must match (AND)");
        Check(!both.Matches("TelegramDesktop", "", "Chats"), "...one failing selector fails the rule");

        Check(WindowRule::AppKeyFor("Class", "inst") == "Class", "AppKeyFor prefers the class");
        Check(WindowRule::AppKeyFor("", "inst") == "inst", "AppKeyFor falls back to the instance");
        Check(WindowRule::AppKeyFor("", "") == "", "AppKeyFor of nothing is empty (nothing to match)");

        WindowRule none;
        Check(!none.Matches("anything", "anything", "anything"), "a rule with no selector never matches");
    }

    std::printf("\nResolveWindowRules():\n");
    {
        std::vector<WindowRule> none;
        WindowRuleEffect e = ResolveWindowRules(none, "firefox", "", "");
        Check(!e.forceFloat && !e.forceTile && !e.forceFullscreen && !e.denyFullscreen &&
              e.forceWorkspace == 0 && e.horizontal == H::Default && e.vertical == V::Default,
              "no rules at all: no opinion on anything (normal placement)");

        std::vector<WindowRule> rules = { P("app:firefox workspace:3") };
        e = ResolveWindowRules(rules, "firefox", "", "");
        Check(e.forceWorkspace == 3 && !e.forceFloat && !e.forceTile && !e.forceFullscreen &&
              e.horizontal == H::Default && e.vertical == V::Default, "workspace-only rule: only the workspace is set");

        e = ResolveWindowRules(rules, "kitty", "", "");
        Check(e.forceWorkspace == 0, "a window the rule doesn't match is untouched");

        rules = { P("app:foo horizontal:right") };
        e = ResolveWindowRules(rules, "foo", "", "");
        Check(e.horizontal == H::Right && e.forceWorkspace == 0 && e.vertical == V::Default, "horizontal-only");

        rules = { P("app:foo vertical:top") };
        e = ResolveWindowRules(rules, "foo", "", "");
        Check(e.vertical == V::Top && e.forceWorkspace == 0 && e.horizontal == H::Default, "vertical-only");

        rules = { P("app:foo mode:floating") };
        e = ResolveWindowRules(rules, "foo", "", "");
        Check(e.forceFloat && !e.forceTile && !e.forceFullscreen && e.forceWorkspace == 0, "mode-only: floating");
        rules = { P("app:foo mode:tiled") };
        e = ResolveWindowRules(rules, "foo", "", "");
        Check(e.forceTile && !e.forceFloat && !e.forceFullscreen, "mode-only: tiled");
        rules = { P("app:foo mode:fullscreen") };
        e = ResolveWindowRules(rules, "foo", "", "");
        Check(e.forceFullscreen && !e.forceFloat && !e.forceTile, "mode-only: fullscreen");

        rules = { P("app:firefox workspace:3 horizontal:right vertical:top mode:tiled") };
        e = ResolveWindowRules(rules, "firefox", "", "");
        Check(e.forceWorkspace == 3 && e.horizontal == H::Right && e.vertical == V::Top && e.forceTile,
              "combined rule: every field applied together");

        // Independence between applications.
        rules = { P("app:firefox workspace:3"), P("app:telegram-desktop workspace:2 mode:floating") };
        WindowRuleEffect ff = ResolveWindowRules(rules, "firefox", "", "");
        WindowRuleEffect tg = ResolveWindowRules(rules, "telegram-desktop", "", "");
        Check(ff.forceWorkspace == 3 && !ff.forceFloat, "app A's effect is its own rule only");
        Check(tg.forceWorkspace == 2 && tg.forceFloat, "app B's effect is its own rule only");
    }

    std::printf("\nPrecedence when several rules match one window:\n");
    {
        // The motivating case: a general app rule, plus a hand-written
        // more specific rule for one window of that app.
        std::vector<WindowRule> rules = {
            P("app:telegramdesktop workspace:2 mode:floating"),
            P("class:telegram title:photo workspace:8"),
        };
        WindowRuleEffect main = ResolveWindowRules(rules, "TelegramDesktop", "", "Telegram");
        WindowRuleEffect media = ResolveWindowRules(rules, "TelegramDesktop", "", "Photo viewer");
        Check(main.forceWorkspace == 2 && main.forceFloat, "the general app rule applies to the main window");
        Check(media.forceWorkspace == 8, "the more specific (2-selector) rule wins the workspace for its window...");
        Check(media.forceFloat, "...while the app rule's other properties still apply (mode)");

        // Specificity beats file order.
        rules = { P("class:telegram title:photo workspace:8"), P("app:telegramdesktop workspace:2") };
        media = ResolveWindowRules(rules, "TelegramDesktop", "", "Photo viewer");
        Check(media.forceWorkspace == 8, "specificity beats file order (the specific rule listed FIRST still wins)");

        // Same specificity: later wins.
        rules = { P("app:foo workspace:2"), P("app:foo workspace:5") };
        Check(ResolveWindowRules(rules, "foo", "", "").forceWorkspace == 5, "equally specific rules: the later line wins");
        rules = { P("app:foo workspace:5"), P("app:foo workspace:2") };
        Check(ResolveWindowRules(rules, "foo", "", "").forceWorkspace == 2, "...in either order (it really is file order)");

        // Legacy conflict: float vs tile on equal footing.
        WindowRuleEffect e;
        rules = { P("float class:foo"), P("tile class:foo") };
        e = ResolveWindowRules(rules, "foo", "", "");
        Check(e.forceTile && !e.forceFloat, "conflicting legacy modes: the later one wins, and only one mode is ever set");
        rules = { P("tile class:foo"), P("float class:foo") };
        e = ResolveWindowRules(rules, "foo", "", "");
        Check(e.forceFloat && !e.forceTile, "...and reversing them reverses the outcome");

        // A property one rule doesn't set is not erased by another.
        rules = { P("app:foo workspace:4"), P("class:foo vertical:top") };
        e = ResolveWindowRules(rules, "foo", "", "");
        Check(e.forceWorkspace == 4 && e.vertical == V::Top, "rules setting different properties both apply");

        rules = { P("app:mpv mode:fullscreen"), P("class:mpv nofullscreen") };
        e = ResolveWindowRules(rules, "mpv", "", "");
        Check(e.denyFullscreen && e.forceFullscreen, "nofullscreen is an independent deny flag (Manage() lets deny win)");
    }


    // ------------------------------------------------------------------
    // Window-specific rules: WM_WINDOW_ROLE / window type (Zen Browser).
    //
    // The two window kinds below are the property sets observed on a
    // real running Zen Browser (Arch Linux / Kohiko); they share a PID
    // and a WM_CLIENT_LEADER, which is why neither is a usable identity.
    // ------------------------------------------------------------------
    std::printf("\nZen Browser: the exact observed property combinations:\n");
    {
        WindowIdentity browser;
        browser.className = "zen"; browser.instanceName = "Navigator";
        browser.role = "browser";  browser.windowType = "normal";
        browser.title = "KohikoDE Session Launch \xE2\x80\x94 Zen Browser";

        WindowIdentity pip;
        pip.className = "zen"; pip.instanceName = "Toolkit";
        pip.role = "PictureInPicture"; pip.windowType = "utility";
        pip.title = "Picture-in-Picture";

        WindowRule generic     = P("app:zen workspace:2");
        WindowRule browserRule = P("app:zen role:browser workspace:3 mode:tiled");
        WindowRule pipRule     = P("app:zen role:PictureInPicture workspace:1 mode:floating horizontal:right vertical:bottom");

        Check(generic.Matches(browser) && generic.Matches(pip), "the generic app:zen rule matches BOTH window kinds");
        Check(browserRule.Matches(browser), "role:browser matches the normal browser window");
        Check(!browserRule.Matches(pip), "role:browser does NOT match the Picture-in-Picture window");
        Check(pipRule.Matches(pip), "role:PictureInPicture matches the Picture-in-Picture window");
        Check(!pipRule.Matches(browser), "role:PictureInPicture does NOT match the normal browser window");

        // Both together, with the type as an additional property.
        WindowRule pipTyped = P("app:zen role:PictureInPicture type:utility workspace:1");
        Check(pipTyped.Matches(pip), "role + type utility matches the PiP window");
        Check(!pipTyped.Matches(browser), "...and not the browser");
        WindowRule browserTyped = P("app:zen role:browser type:normal workspace:3");
        Check(browserTyped.Matches(browser), "role + type normal matches the browser window");
        Check(!browserTyped.Matches(pip), "...and not the PiP window");

        // Type alone distinguishes them too, but only as an extra property.
        Check(P("app:zen type:utility mode:floating").Matches(pip) && !P("app:zen type:utility mode:floating").Matches(browser),
              "type:utility alone separates them as well");

        // The instance differs but is optional additional information.
        Check(P("app:zen instance:Navigator workspace:3").Matches(browser) && !P("app:zen instance:Navigator workspace:3").Matches(pip),
              "instance:Navigator is usable as an extra selector");
        Check(P("app:zen instance:Toolkit workspace:1").Matches(pip) && !P("app:zen instance:Toolkit workspace:1").Matches(browser),
              "instance:Toolkit likewise");

        // Neither PID, client leader, window id nor title is part of the rule format.
        Check(Rejects("app:zen pid:2077 workspace:1"), "pid: is not a selector (shared by both windows)");
        Check(Rejects("app:zen leader:0xe00001 workspace:1"), "client-leader is not a selector");
        Check(Rejects("app:zen id:0x400001 workspace:1"), "a window id is not a selector (ids are recycled)");

        // The title is not what identifies a window: the browser's current title is launch metadata.
        WindowIdentity retitled = browser;
        retitled.title = "Some other page \xE2\x80\x94 Zen Browser";
        Check(browserRule.Matches(retitled) && pipRule.Matches(pip),
              "role rules do not depend on the (volatile) window title");

        std::printf("\nZen Browser: precedence - the more specific rule beats the generic one:\n");
        std::vector<WindowRule> rules = { generic, browserRule, pipRule };

        WindowRuleEffect b = ResolveWindowRules(rules, browser);
        WindowRuleEffect p = ResolveWindowRules(rules, pip);
        Check(b.forceWorkspace == 3 && b.forceTile && !b.forceFloat, "browser: workspace 3, tiled (specific beats generic's workspace 2)");
        Check(p.forceWorkspace == 1 && p.forceFloat && p.horizontal == H::Right && p.vertical == V::Bottom,
              "PiP: workspace 1, floating, right/bottom - independent of the browser's rule");

        // The order the lines are written in must not matter: specificity decides.
        rules = { pipRule, browserRule, generic };
        b = ResolveWindowRules(rules, browser);
        p = ResolveWindowRules(rules, pip);
        Check(b.forceWorkspace == 3 && p.forceWorkspace == 1, "...even when the generic rule is listed LAST");

        // A role-specific rule alone does not leak onto the other window; the generic rule still applies there.
        rules = { generic, pipRule };
        b = ResolveWindowRules(rules, browser);
        Check(b.forceWorkspace == 2 && !b.forceFloat && b.horizontal == H::Default,
              "only the PiP rule exists besides the generic one: the browser gets just the generic rule");
        rules = { pipRule };
        b = ResolveWindowRules(rules, browser);
        Check(b.forceWorkspace == 0 && !b.forceFloat, "with no generic rule, the browser window gets nothing from the PiP rule");

        // Independent properties per window kind.
        rules = { P("app:zen role:browser workspace:3"), P("app:zen role:browser horizontal:left"), P("app:zen role:PictureInPicture vertical:top"), P("app:zen role:PictureInPicture mode:floating") };
        b = ResolveWindowRules(rules, browser);
        p = ResolveWindowRules(rules, pip);
        Check(b.forceWorkspace == 3 && b.horizontal == H::Left && b.vertical == V::Default && !b.forceFloat,
              "browser gets its own workspace + horizontal, nothing from the PiP rules");
        Check(p.forceWorkspace == 0 && p.vertical == V::Top && p.forceFloat && p.horizontal == H::Default,
              "PiP gets its own vertical + mode, nothing from the browser rules");
    }

    std::printf("\nMissing or mismatched role/type never broadens a specific rule:\n");
    {
        WindowIdentity noRole;      // a window that declares nothing
        noRole.className = "zen"; noRole.instanceName = "Navigator"; noRole.title = "x";
        WindowIdentity wrongType;
        wrongType.className = "zen"; wrongType.role = "browser"; wrongType.windowType = "dialog";
        WindowIdentity noType;
        noType.className = "zen"; noType.role = "browser";

        Check(!P("app:zen role:browser workspace:3").Matches(noRole), "a role rule does not match a window with no WM_WINDOW_ROLE");
        Check(!P("app:zen role:PictureInPicture workspace:1").Matches(noRole), "...whichever role it asks for");
        Check(!P("app:zen type:normal workspace:3").Matches(noRole), "a type rule does not match a window with no declared type (type is never inferred)");
        Check(!P("app:zen type:normal workspace:3").Matches(noType), "...even though EWMH would default it to normal");
        Check(!P("app:zen role:browser type:normal workspace:3").Matches(wrongType), "role right but type wrong: no match");
        Check(P("app:zen role:browser workspace:3").Matches(wrongType), "role-only rule ignores the type");
        Check(!P("app:zen role:browser workspace:3").Matches("zen", "Navigator", "x"),
              "the legacy 3-argument form knows no role, so a role rule cannot match it");

        // The role comparison is EXACT: no substring, no prefix.
        WindowIdentity popup; popup.className = "zen"; popup.role = "browser-popup";
        Check(!P("app:zen role:browser workspace:3").Matches(popup), "role:browser does not match role \"browser-popup\"");
        WindowIdentity upper; upper.className = "zen"; upper.role = "BROWSER";
        Check(P("app:zen role:browser workspace:3").Matches(upper), "role is compared case-insensitively");

        // A rule is refused - not half-applied - when its role/type can't be honoured.
        Check(Rejects("app:zen role: workspace:3"), "an empty role: is a parse error (it must not become an app-wide rule)");
        Check(Rejects("app:zen type: workspace:3"), "an empty type: is a parse error");
        Check(Rejects("app:zen type:bogus workspace:3"), "an unknown type name is a parse error");
        Check(P("app:zen type:_NET_WM_WINDOW_TYPE_UTILITY mode:floating").windowType == "utility", "the _NET_WM_WINDOW_TYPE_ prefix is accepted and normalised");
        Check(P("app:zen type:UTILITY mode:floating").windowType == "utility", "type names are case-insensitive");
        Check(P("role:PictureInPicture mode:floating").HasSelector(), "role: alone is a selector (applies to that role in any application)");

        // Role with a space survives serialisation.
        WindowRule spaced = P("app:zen role:My%20Role workspace:2");
        Check(spaced.role == "My Role" && spaced.Serialize() == "app:zen role:My%20Role workspace:2", "role values use %XX escapes like the other selectors");
    }

    std::printf("\nExisting application-wide rules and the config format stay compatible:\n");
    {
        WindowRule r = P("app:firefox workspace:3 horizontal:right vertical:top mode:tiled");
        Check(r.IsAppRule() && r.IsScopedAppRule() && r.Scope().IsApplication(), "a plain app rule is still an app rule with the whole-application scope");
        Check(r.Serialize() == "app:firefox workspace:3 horizontal:right vertical:top mode:tiled", "and serialises byte-for-byte as before");
        Check(P("class:tlauncher tile").Serialize() == "class:tlauncher mode:tiled", "legacy forms serialise as before");

        WindowRule scoped = P("app:zen role:browser type:normal workspace:3");
        Check(!scoped.IsAppRule() && scoped.IsScopedAppRule(), "a role/type rule is a scoped app rule, not THE app rule");
        Check(scoped.SelectorCount() == 3, "app + role + type = 3 selectors (so it outranks a 1-selector app rule)");
        Check(scoped.Serialize() == "app:zen role:browser type:normal workspace:3", "canonical order: app, class, instance, role, type, title");
        Check(P(scoped.Serialize()) == scoped, "Parse(Serialize(rule)) round-trips");

        WindowRule withTitle = P("app:zen role:browser title:foo workspace:3");
        Check(!withTitle.IsScopedAppRule(), "a rule that also has a title selector stays a hand-written rule");

        Check(P("app:zen role:browser workspace:3") == P("APP:ZEN ROLE:BROWSER workspace:3"), "case of keys and values is irrelevant to equality");
        Check(!(P("app:zen role:browser workspace:3") == P("app:zen role:PictureInPicture workspace:3")), "rules differing only in role are different rules");
        Check(!(P("app:zen workspace:3") == P("app:zen role:browser workspace:3")), "a scoped rule is not equal to the generic one");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
