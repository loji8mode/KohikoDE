// WindowRuleDraft: the editing model behind kohiko-settings' Window Rules
// rows. The properties that matter to the user:
//
//   - the role and type of a rule (its "window kind") can be created,
//     shown and changed in Settings;
//   - editing ANY other cell of a row - application, instance, title,
//     workspace, Horizontal/Vertical/Mode - never erases or broadens
//     them (that would turn a Picture-in-Picture rule into a rule for
//     every window of the application);
//   - each row is its own rule: a row's edit is a function of that row's
//     line alone.
//
// Pure logic, no X11. (The Settings window's drawing and clicking are
// not exercised here - see docs/ARCHITECTURE.md "Testing and its limits".)

#include "WindowRuleDraft.h"

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
    {
        ++g_fail;
        std::printf("  FAIL: (unexpected parse failure for [%s]: %s)\n", text.c_str(), error.c_str());
    }
    return rule;
}

// The three rules a Zen user wants, exactly as they are serialised.
static const std::string kGeneric = "app:zen workspace:2";
static const std::string kBrowser = "app:zen role:browser workspace:3 horizontal:left mode:tiled";
static const std::string kPip     = "app:zen role:PictureInPicture type:utility workspace:5 horizontal:right mode:floating";

int main()
{
    std::printf("Parse / serialize round trips (byte for byte):\n");
    {
        for (const std::string& line : {kGeneric, kBrowser, kPip,
             std::string("app:firefox workspace:3 horizontal:right vertical:top mode:tiled"),
             std::string("app:zen role:My%20Role workspace:2"),
             std::string("class:tlauncher instance:x title:y mode:floating nofullscreen")})
        {
            Check(SerializeWindowRuleDraft(ParseWindowRuleDraft(line)) == line, ("round trip: " + line).c_str());
        }

        // Parsed fields.
        WindowRuleDraft pip = ParseWindowRuleDraft(kPip);
        Check(pip.appText == "zen" && pip.roleText == "PictureInPicture" && pip.typeText == "utility" &&
              pip.workspaceText == "5" && pip.horizontalIndex == 2 && pip.modeIndex == 2,
              "the PiP row parses into app, role, type, workspace, horizontal and mode");
        Check(ParseWindowRuleDraft("app:zen role:My%20Role workspace:2").roleText == "My Role", "role: shows %XX escapes decoded");
        Check(ParseWindowRuleDraft(kGeneric).roleText.empty() && ParseWindowRuleDraft(kGeneric).typeText.empty(), "a generic row has no role or type");

        // The draft's output parses to the rule it was shown as.
        Check(P(SerializeWindowRuleDraft(ParseWindowRuleDraft(kPip))) == P(kPip), "the serialized row is the same rule as the one that was loaded");
    }

    std::printf("\nWindow kind in words (the Settings caption):\n");
    {
        Check(ParseWindowRuleDraft(kGeneric).KindDescription() == "All windows", "generic: All windows");
        Check(ParseWindowRuleDraft(kBrowser).KindDescription() == "role browser", "role only");
        Check(ParseWindowRuleDraft(kPip).KindDescription() == "role PictureInPicture + type utility", "role + type");
        Check(ParseWindowRuleDraft("app:zen type:utility workspace:1").KindDescription() == "type utility", "type only");
    }

    std::printf("\nEditing other cells keeps the window kind:\n");
    {
        // What SettingsWindow does for each cell: parse the line, change the one field, serialize.
        WindowRuleDraft d = ParseWindowRuleDraft(kPip);
        d.workspaceText = "2";
        Check(SerializeWindowRuleDraft(d) == "app:zen role:PictureInPicture type:utility workspace:2 horizontal:right mode:floating",
              "workspace edited: role and type kept");

        d = ParseWindowRuleDraft(kPip);
        d.horizontalIndex = (d.horizontalIndex + 1) % 3; // Right -> Default
        d.verticalIndex   = (d.verticalIndex + 1) % 3;   // Default -> Top
        d.modeIndex       = (d.modeIndex + 1) % 4;       // Floating -> Fullscreen
        Check(SerializeWindowRuleDraft(d) == "app:zen role:PictureInPicture type:utility workspace:5 vertical:top mode:fullscreen",
              "Horizontal/Vertical/Mode cycled: role and type kept");

        d = ParseWindowRuleDraft(kPip);
        d.appText = "firefox";
        Check(SerializeWindowRuleDraft(d).find("role:PictureInPicture type:utility") != std::string::npos, "application renamed: role and type kept");

        d = ParseWindowRuleDraft(kBrowser);
        d.instanceText = "Navigator";
        d.titleText = "Zen Browser";
        std::string line = SerializeWindowRuleDraft(d);
        Check(line.find("role:browser") != std::string::npos && line.find("instance:Navigator") != std::string::npos &&
              line.find("title:Zen%20Browser") != std::string::npos,
              "instance and title added: role kept");

        // The generic row stays generic when its cells are edited.
        d = ParseWindowRuleDraft(kGeneric);
        d.workspaceText = "4";
        Check(SerializeWindowRuleDraft(d) == "app:zen workspace:4", "editing the generic row's workspace does not give it a role");
    }

    std::printf("\nCreating and changing the window kind:\n");
    {
        // + Add rule: an empty row; fill it as a Picture-in-Picture rule.
        WindowRuleDraft d = ParseWindowRuleDraft("");
        d.appText = "zen";
        d.roleText = "PictureInPicture";
        d.workspaceText = "5";
        d.horizontalIndex = 2;
        d.modeIndex = 2;
        Check(SerializeWindowRuleDraft(d) == "app:zen role:PictureInPicture workspace:5 horizontal:right mode:floating",
              "a new row filled in as a PiP rule serialises with its role");
        Check(P(SerializeWindowRuleDraft(d)).Scope().role == "PictureInPicture", "and the window manager's parser reads it as a PiP-scoped rule");

        // Setting a role on a generic row narrows it.
        d = ParseWindowRuleDraft(kGeneric);
        d.roleText = "browser";
        Check(SerializeWindowRuleDraft(d) == "app:zen role:browser workspace:2", "typing a role into a generic row narrows it to that kind");

        // Clearing the role is an explicit edit of THAT cell only.
        d = ParseWindowRuleDraft(kPip);
        d.roleText.clear();
        Check(SerializeWindowRuleDraft(d) == "app:zen type:utility workspace:5 horizontal:right mode:floating",
              "clearing the role cell removes the role and nothing else (type stays)");

        // Type button: Any -> normal -> dialog -> utility -> ... -> dock -> Any.
        std::string t;
        std::vector<std::string> seen;
        for (int i = 0; i < 15; ++i) { t = NextWindowTypeText(t); seen.push_back(t); }
        Check(seen.size() == 15 && seen[0] == "normal" && seen[1] == "dialog" && seen[2] == "utility" &&
              seen[13] == "dock" && seen[14].empty(),
              "the type button cycles Any -> normal -> dialog -> utility -> ... -> dock -> Any");
        bool allValid = true;
        for (const std::string& v : seen)
            allValid = allValid && (v.empty() || !WindowRule::NormalizeWindowType(v).empty());
        Check(allValid, "every value it produces is accepted by the rule parser");
        Check(NextWindowTypeText("bogus").empty(), "an unrecognised hand-typed type moves to Any");
        Check(NextWindowTypeText("UTILITY") == "toolbar", "a type typed in another case still cycles from its place");

        // Cycling the type of a PiP row keeps the role.
        d = ParseWindowRuleDraft("app:zen role:PictureInPicture workspace:5");
        d.typeText = NextWindowTypeText(d.typeText);
        Check(SerializeWindowRuleDraft(d) == "app:zen role:PictureInPicture type:normal workspace:5", "cycling Type on a role row keeps the role");
    }

    std::printf("\nThree rows, three independent rules:\n");
    {
        std::vector<std::string> rows = {kGeneric, kBrowser, kPip};

        // Edit the middle row (as Settings does: only that row's line).
        WindowRuleDraft d = ParseWindowRuleDraft(rows[1]);
        d.workspaceText = "4";
        rows[1] = SerializeWindowRuleDraft(d);

        Check(rows[0] == kGeneric && rows[2] == kPip, "editing the browser row leaves the generic and PiP rows byte-identical");
        Check(P(rows[1]).role == "browser" && P(rows[1]).workspace == 4, "and the browser row is still a browser rule, now on workspace 4");

        // Delete the PiP row (the row's 'x'): the other two remain, in order.
        rows.erase(rows.begin() + 2);
        Check(rows.size() == 2 && rows[0] == kGeneric && P(rows[1]).role == "browser", "deleting the PiP row leaves the generic row and the browser row intact");

        // Delete the browser row: the generic row remains and still has no role.
        rows.erase(rows.begin() + 1);
        Check(rows.size() == 1 && rows[0] == kGeneric && P(rows[0]).Scope().IsApplication(), "deleting a specific row leaves the generic rule intact (and still application-wide)");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
