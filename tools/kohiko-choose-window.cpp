// kohiko-choose-window - set (or change, or delete) the explicit
// window rule for ONE application: which workspace it opens on, which
// side of the layout, and whether it is tiled, floating or fullscreen.
//
//   kohiko-choose-window --app firefox --class firefox --instance Navigator --title "Mozilla Firefox"
//       opens the dialog for that application (this is what Kohiko runs
//       when you press the `choose_window` key - SUPER+SHIFT+W in the
//       default config - or `kohikoctl dispatch choose_window`, passing
//       the focused window's identity)
//
// and, without any UI (scripting, and so the whole save/load/delete path
// can be tested headlessly):
//
//   kohiko-choose-window --list                       every application rule
//   kohiko-choose-window --app KEY --show             that application's rule
//   kohiko-choose-window --app KEY --set "workspace:3 horizontal:right vertical:top mode:tiled"
//   kohiko-choose-window --app KEY --delete
//
// An application's windows can also be told apart - a browser and its
// Picture-in-Picture overlay share a WM_CLASS but not a WM_WINDOW_ROLE -
// so a rule can be scoped to one kind of window:
//
//   --role R --type T   what the window declared (Kohiko passes these)
//   --scope app|role|type|role+type
//                       which of them the rule is for (default app: the
//                       whole application). `--scope role` needs --role.
//
//   kohiko-choose-window --app zen --role PictureInPicture --scope role --set "mode:floating horizontal:right"
//       writes `windowrule=app:zen role:PictureInPicture mode:floating horizontal:right`,
//       independent of the plain `app:zen` rule and of any other role's rule.
//
// Options:
//   --config PATH   config file to edit (default ~/.config/kohiko/kohiko.conf)
//   --no-reload     don't run `kohikoctl reload` afterwards
//
// A rule is saved as one `windowrule=app:KEY ...` line in kohiko.conf
// (see include/WindowRule.h for the format) and applies to windows
// opened from then on - it never moves a window that is already open.
// This tool and kohiko-settings' Window Rules tab are the only things
// that write rules; moving, dragging, resizing or swapping windows never
// does.

#include "ChooseWindowDialog.h"
#include "ChooseWindowModel.h"
#include "Config.h"
#include "Utils.h"
#include "WindowRule.h"
#include "WindowRuleStore.h"

#include <X11/Xlib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

using namespace Kohiko;

namespace
{

void Usage()
{
    std::fprintf(stderr,
        "usage: kohiko-choose-window --app KEY [--class C] [--instance I] [--title T] [--role R] [--type T] [--scope app|role|type|role+type]\n"
        "       kohiko-choose-window --list\n"
        "       kohiko-choose-window --app KEY (--show | --delete | --set \"workspace:N horizontal:left|right vertical:top|bottom mode:tiled|floating|fullscreen\")\n"
        "options: --config PATH   --no-reload\n");
}

void ReloadRunningKohiko()
{
    // Best-effort, same as kohiko-settings: kohikoctl just fails
    // (ignored here) when Kohiko isn't running.
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

}

int main(int argc, char** argv)
{
    ChooseWindowIdentity identity;
    std::string configPath;
    std::string setTokens;
    std::string scopeName = "app";
    bool haveApp = false, list = false, show = false, doDelete = false, doSet = false, reload = true;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        auto value = [&]() -> std::string { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };

        if      (arg == "--app")      { identity.appKey = value(); haveApp = true; }
        else if (arg == "--class")    identity.className = value();
        else if (arg == "--instance") identity.instanceName = value();
        else if (arg == "--title")    identity.title = value();
        else if (arg == "--role")     identity.role = value();
        else if (arg == "--type")     identity.windowType = WindowRule::NormalizeWindowType(value());
        else if (arg == "--scope")    scopeName = Utils::Lower(value());
        else if (arg == "--config")   configPath = value();
        else if (arg == "--no-reload") reload = false;
        else if (arg == "--list")     list = true;
        else if (arg == "--show")     show = true;
        else if (arg == "--delete")   doDelete = true;
        else if (arg == "--set")      { doSet = true; setTokens = value(); }
        else if (arg == "--help" || arg == "-h") { Usage(); return 0; }
        else { std::fprintf(stderr, "kohiko-choose-window: unknown option %s\n", arg.c_str()); Usage(); return 2; }
    }

    if (configPath.empty())
    {
        const char* home = std::getenv("HOME");
        configPath = (home ? std::string(home) : std::string(".")) + "/.config/kohiko/kohiko.conf";
    }

    if (haveApp && identity.appKey.empty() && !identity.className.empty())
        identity.appKey = WindowRule::AppKeyFor(identity.className, identity.instanceName);

    if (!list && (!haveApp || identity.appKey.empty()))
    {
        std::fprintf(stderr, "kohiko-choose-window: this window has no application identity (empty WM_CLASS), so no rule can match it.\n");
        Usage();
        return 2;
    }

    // Resolve which windows of the application the rule is for. A scope
    // the window cannot satisfy is an error rather than a quiet fallback
    // to the whole application - that would write a broader rule than
    // the one that was asked for.
    if (scopeName == "role" || scopeName == "role+type")
    {
        if (identity.role.empty())
        {
            std::fprintf(stderr, "kohiko-choose-window: --scope %s needs --role (this window has no WM_WINDOW_ROLE)\n", scopeName.c_str());
            return 2;
        }
        identity.scope.role = identity.role;
    }

    if (scopeName == "type" || scopeName == "role+type")
    {
        if (identity.windowType.empty())
        {
            std::fprintf(stderr, "kohiko-choose-window: --scope %s needs a known --type (this window declares none)\n", scopeName.c_str());
            return 2;
        }
        identity.scope.windowType = identity.windowType;
    }

    if (scopeName != "app" && scopeName != "role" && scopeName != "type" && scopeName != "role+type")
    {
        std::fprintf(stderr, "kohiko-choose-window: unknown --scope \"%s\" (use app, role, type or role+type)\n", scopeName.c_str());
        return 2;
    }

    std::vector<std::string> lines;

    // Never create the config: one made from scratch has no bind= lines,
    // which would silently remove every keybinding (see WindowRuleStore).
    if (!WindowRuleStore::LoadLines(configPath, lines))
    {
        std::fprintf(stderr,
            "kohiko-choose-window: couldn't read %s - run Kohiko at least once first "
            "(it creates this file from config/default.conf on first launch).\n",
            configPath.c_str());
        return 1;
    }

    if (list)
    {
        for (const std::string& line : lines)
        {
            WindowRule rule;

            if (WindowRule::Parse(line, rule) && rule.IsScopedAppRule())
                std::printf("%s\n", rule.Serialize().c_str());
        }
        return 0;
    }

    if (show)
    {
        if (auto rule = WindowRuleStore::FindAppRule(lines, identity.appKey, identity.scope))
            std::printf("%s\n", rule->Serialize().c_str());
        return 0;
    }

    if (doDelete || doSet)
    {
        std::vector<std::string> updated;

        if (doDelete)
            updated = WindowRuleStore::RemoveAppRule(lines, identity.appKey, identity.scope);
        else
        {
            WindowRule rule;
            std::string error;

            std::string scopeTokens;
            if (!identity.scope.role.empty())       scopeTokens += " role:" + WindowRule::EncodeValue(identity.scope.role);
            if (!identity.scope.windowType.empty()) scopeTokens += " type:" + identity.scope.windowType;

            if (!WindowRule::Parse("app:" + WindowRule::EncodeValue(identity.appKey) + scopeTokens + " " + setTokens, rule, &error))
            {
                std::fprintf(stderr, "kohiko-choose-window: bad --set value: %s\n", error.c_str());
                return 2;
            }

            // --set must not widen or narrow the scope that was asked for.
            if (!(rule.Scope() == identity.scope))
            {
                std::fprintf(stderr, "kohiko-choose-window: --set may not contain role:/type: - choose the scope with --scope\n");
                return 2;
            }

            updated = WindowRuleStore::UpsertAppRule(lines, rule);
        }

        if (!WindowRuleStore::SaveLines(configPath, updated))
        {
            std::fprintf(stderr, "kohiko-choose-window: couldn't write %s\n", configPath.c_str());
            return 1;
        }

        if (reload)
            ReloadRunningKohiko();

        return 0;
    }

    // --- interactive ---------------------------------------------------

    Config config;
    config.Load(configPath);

    // Opened by pressing the key twice? Don't stack a second dialog.
    if (Display* probe = XOpenDisplay(nullptr))
    {
        bool open = ChooseWindowDialog::AlreadyOpen(probe);
        XCloseDisplay(probe);

        if (open)
            return 0;
    }

    int workspaceCount = config.GetInt("workspace.count", 4);

    ChooseWindowModel model(workspaceCount, WindowRuleStore::FindAppRule(lines, identity.appKey, identity.scope));

    WindowIdentity windowIdentity;
    windowIdentity.className = identity.className;
    windowIdentity.instanceName = identity.instanceName;
    windowIdentity.title = identity.title;
    windowIdentity.role = identity.role;
    windowIdentity.windowType = identity.windowType;

    int others = static_cast<int>(
        WindowRuleStore::OtherMatchingRules(lines, windowIdentity, identity.scope).size());

    ChooseWindowDialog dialog(config, identity, model, others);

    ChooseWindowModel::Action action = dialog.Run();

    std::vector<std::string> updated;

    switch (action)
    {
        case ChooseWindowModel::Action::Save:
            // All-Default is not a rule: Upsert removes it, so "Save"
            // with nothing chosen is the same as deleting.
            updated = WindowRuleStore::UpsertAppRule(lines, model.ToRule(identity.appKey, identity.scope));
            break;

        case ChooseWindowModel::Action::Reset:
            updated = WindowRuleStore::RemoveAppRule(lines, identity.appKey, identity.scope);
            break;

        default:
            return 0; // Cancel: change nothing, touch nothing.
    }

    if (!WindowRuleStore::SaveLines(configPath, updated))
    {
        std::fprintf(stderr, "kohiko-choose-window: couldn't write %s\n", configPath.c_str());
        return 1;
    }

    if (reload)
        ReloadRunningKohiko();

    return 0;
}
