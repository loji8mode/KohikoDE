#include "WindowRule.h"

#include "Config.h"
#include "Logger.h"
#include "Utils.h"

#include <algorithm>
#include <cctype>

namespace Kohiko
{

namespace
{

bool ContainsInsensitive(
    const std::string& haystack,
    const std::string& pattern)
{
    return Utils::Lower(haystack).find(Utils::Lower(pattern)) != std::string::npos;
}

bool AllDigits(const std::string& text)
{
    return !text.empty() &&
        std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

int HexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

const char* HorizontalName(WindowRule::Horizontal h)
{
    return h == WindowRule::Horizontal::Left ? "left" : "right";
}

const char* VerticalName(WindowRule::Vertical v)
{
    return v == WindowRule::Vertical::Top ? "top" : "bottom";
}

const char* ModeName(WindowRule::Mode m)
{
    switch (m)
    {
        case WindowRule::Mode::Tiled:      return "tiled";
        case WindowRule::Mode::Floating:   return "floating";
        case WindowRule::Mode::Fullscreen: return "fullscreen";
        default:                           return "";
    }
}

const char* const kWindowTypes[] =
{
    "normal", "dialog", "utility", "toolbar", "menu", "splash",
    "dropdown_menu", "popup_menu", "tooltip", "notification",
    "combo", "dnd", "desktop", "dock"
};

bool EqualsInsensitive(const std::string& a, const std::string& b)
{
    return Utils::Lower(a) == Utils::Lower(b);
}

bool Fail(std::string* error, const std::string& message)
{
    if (error)
        *error = message;

    return false;
}

}

std::string WindowRule::AppKeyFor(
    const std::string& className,
    const std::string& instanceName)
{
    return className.empty() ? instanceName : className;
}

bool RuleScope::operator==(const RuleScope& other) const
{
    return EqualsInsensitive(role, other.role) &&
           EqualsInsensitive(windowType, other.windowType);
}

std::string WindowRule::NormalizeWindowType(
    const std::string& text)
{
    static const std::string kPrefix = "_net_wm_window_type_";

    std::string lower = Utils::Lower(text);

    if (lower.rfind(kPrefix, 0) == 0)
        lower = lower.substr(kPrefix.size());

    for (const char* known : kWindowTypes)
    {
        if (lower == known)
            return lower;
    }

    return std::string();
}

int WindowRule::SelectorCount() const
{
    return static_cast<int>(!appKey.empty()) +
           static_cast<int>(!classPattern.empty()) +
           static_cast<int>(!instancePattern.empty()) +
           static_cast<int>(!role.empty()) +
           static_cast<int>(!windowType.empty()) +
           static_cast<int>(!titlePattern.empty());
}

bool WindowRule::HasProperty() const
{
    return workspace > 0 ||
           horizontal != Horizontal::Default ||
           vertical != Vertical::Default ||
           mode != Mode::Default ||
           denyFullscreen;
}

bool WindowRule::IsAppRule() const
{
    return IsScopedAppRule() && role.empty() && windowType.empty();
}

bool WindowRule::IsScopedAppRule() const
{
    return !appKey.empty() &&
           classPattern.empty() &&
           instancePattern.empty() &&
           titlePattern.empty();
}

RuleScope WindowRule::Scope() const
{
    RuleScope scope;
    scope.role = role;
    scope.windowType = windowType;
    return scope;
}

bool WindowRule::Matches(
    const std::string& className,
    const std::string& instanceName,
    const std::string& title) const
{
    WindowIdentity window;
    window.className = className;
    window.instanceName = instanceName;
    window.title = title;

    return Matches(window);
}

bool WindowRule::Matches(
    const WindowIdentity& window) const
{
    if (!HasSelector())
        return false;

    if (!appKey.empty() &&
        !EqualsInsensitive(AppKeyFor(window.className, window.instanceName), appKey))
        return false;

    if (!classPattern.empty() && !ContainsInsensitive(window.className, classPattern))
        return false;

    if (!instancePattern.empty() && !ContainsInsensitive(window.instanceName, instancePattern))
        return false;

    // Exact, and a missing role/type never matches: a rule that asks for
    // a specific kind of window must not fall through to "any window of
    // this application" just because the window didn't say what it is.
    if (!role.empty() && !EqualsInsensitive(window.role, role))
        return false;

    if (!windowType.empty() && !EqualsInsensitive(window.windowType, windowType))
        return false;

    if (!titlePattern.empty() && !ContainsInsensitive(window.title, titlePattern))
        return false;

    return true;
}

std::string WindowRule::EncodeValue(const std::string& value)
{
    static const char* kHex = "0123456789ABCDEF";
    std::string out;

    for (unsigned char c : value)
    {
        if (c == ' ' || c == '\t' || c == '%' || c < 0x20 || c == 0x7f)
        {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0f];
        }
        else
            out += static_cast<char>(c);
    }

    return out;
}

std::string WindowRule::DecodeValue(const std::string& value)
{
    std::string out;

    for (std::size_t i = 0; i < value.size(); ++i)
    {
        if (value[i] == '%' && i + 2 < value.size() &&
            HexValue(value[i + 1]) >= 0 && HexValue(value[i + 2]) >= 0)
        {
            out += static_cast<char>(HexValue(value[i + 1]) * 16 + HexValue(value[i + 2]));
            i += 2;
        }
        else
            out += value[i];
    }

    return out;
}

bool WindowRule::Parse(
    const std::string& text,
    WindowRule& out,
    std::string* error)
{
    WindowRule parsed;
    bool haveMode = false;
    bool haveWorkspace = false;
    bool haveHorizontal = false;
    bool haveVertical = false;

    auto setMode = [&](Mode mode) -> bool
    {
        if (haveMode && parsed.mode != mode)
            return false;

        parsed.mode = mode;
        haveMode = true;
        return true;
    };

    for (const std::string& token : Utils::SplitWhitespace(text))
    {
        if (token[0] == '#')
            break; // inline comment - config comments are otherwise full-line only

        std::string lower = Utils::Lower(token);

        // Legacy bare-word actions.
        if (lower == "tile")
        {
            if (!setMode(Mode::Tiled)) return Fail(error, "conflicting modes");
            continue;
        }
        if (lower == "float")
        {
            if (!setMode(Mode::Floating)) return Fail(error, "conflicting modes");
            continue;
        }
        if (lower == "fullscreen")
        {
            if (!setMode(Mode::Fullscreen)) return Fail(error, "conflicting modes");
            continue;
        }
        if (lower == "nofullscreen")
        {
            parsed.denyFullscreen = true;
            continue;
        }

        std::size_t colon = token.find(':');

        if (colon == std::string::npos || colon == 0)
            return Fail(error, "unknown token \"" + token + "\"");

        std::string key = Utils::Lower(token.substr(0, colon));
        std::string rawValue = token.substr(colon + 1);

        // A selector with nothing after the colon is simply not
        // there (the Settings editor writes only the cells you filled
        // in, and this has always been lenient about the rest).
        if (key == "app" || key == "class" || key == "instance" || key == "title" || key == "role")
        {
            std::string value = DecodeValue(rawValue);

            // An empty app:/class:/instance:/title: is simply not there
            // (see above) - but an empty role: is refused: dropping it
            // would silently turn a rule written for one kind of window
            // into one for every window of the application.
            if (value.empty())
            {
                if (key == "role")
                    return Fail(error, "role: needs a value (the window's WM_WINDOW_ROLE)");

                continue;
            }

            if      (key == "app")      parsed.appKey = value;
            else if (key == "class")    parsed.classPattern = value;
            else if (key == "instance") parsed.instancePattern = value;
            else if (key == "role")     parsed.role = value;
            else                        parsed.titlePattern = value;

            continue;
        }

        if (key == "type")
        {
            std::string value = DecodeValue(rawValue);

            if (value.empty())
                return Fail(error, "type: needs a value (for example utility)");

            std::string normalised = NormalizeWindowType(value);

            if (normalised.empty())
                return Fail(error, "type must be one of normal, dialog, utility, toolbar, menu, splash, "
                                   "dropdown_menu, popup_menu, tooltip, notification, combo, dnd, desktop or dock, "
                                   "got \"" + rawValue + "\"");

            parsed.windowType = normalised;
            continue;
        }

        if (key == "workspace")
        {
            if (haveWorkspace)
                return Fail(error, "workspace given twice");

            if (!AllDigits(rawValue) || rawValue.size() > 4 || std::stoi(rawValue) < 1)
                return Fail(error, "workspace needs a number of 1 or more, got \"" + rawValue + "\"");

            parsed.workspace = std::stoi(rawValue);
            haveWorkspace = true;
            continue;
        }

        std::string value = Utils::Lower(rawValue);

        if (key == "horizontal")
        {
            if (haveHorizontal)
                return Fail(error, "horizontal given twice");

            if (value == "left")       parsed.horizontal = Horizontal::Left;
            else if (value == "right") parsed.horizontal = Horizontal::Right;
            else return Fail(error, "horizontal must be left or right, got \"" + rawValue + "\"");

            haveHorizontal = true;
            continue;
        }

        if (key == "vertical")
        {
            if (haveVertical)
                return Fail(error, "vertical given twice");

            if (value == "top")         parsed.vertical = Vertical::Top;
            else if (value == "bottom") parsed.vertical = Vertical::Bottom;
            else return Fail(error, "vertical must be top or bottom, got \"" + rawValue + "\"");

            haveVertical = true;
            continue;
        }

        if (key == "mode")
        {
            Mode mode;

            if (value == "tiled")           mode = Mode::Tiled;
            else if (value == "floating")   mode = Mode::Floating;
            else if (value == "fullscreen") mode = Mode::Fullscreen;
            else return Fail(error, "mode must be tiled, floating or fullscreen, got \"" + rawValue + "\"");

            if (!setMode(mode))
                return Fail(error, "conflicting modes");

            continue;
        }

        return Fail(error, "unknown key \"" + key + ":\"");
    }

    if (!parsed.HasSelector())
        return Fail(error, "needs at least one of app:/class:/instance:/role:/type:/title:");

    if (!parsed.HasProperty())
        return Fail(error, "needs at least one of workspace:/horizontal:/vertical:/mode:");

    out = parsed;
    return true;
}

std::string WindowRule::Serialize() const
{
    std::string line;

    auto add = [&](const std::string& token)
    {
        if (!line.empty())
            line += ' ';

        line += token;
    };

    if (!appKey.empty())          add("app:" + EncodeValue(appKey));
    if (!classPattern.empty())    add("class:" + EncodeValue(classPattern));
    if (!instancePattern.empty()) add("instance:" + EncodeValue(instancePattern));
    if (!role.empty())            add("role:" + EncodeValue(role));
    if (!windowType.empty())      add("type:" + windowType);
    if (!titlePattern.empty())    add("title:" + EncodeValue(titlePattern));

    if (workspace > 0)                  add("workspace:" + std::to_string(workspace));
    if (horizontal != Horizontal::Default) add(std::string("horizontal:") + HorizontalName(horizontal));
    if (vertical != Vertical::Default)     add(std::string("vertical:") + VerticalName(vertical));
    if (mode != Mode::Default)             add(std::string("mode:") + ModeName(mode));
    if (denyFullscreen)                    add("nofullscreen");

    return line;
}

bool WindowRule::operator==(const WindowRule& other) const
{
    return Utils::Lower(appKey) == Utils::Lower(other.appKey) &&
           Utils::Lower(classPattern) == Utils::Lower(other.classPattern) &&
           Utils::Lower(instancePattern) == Utils::Lower(other.instancePattern) &&
           Utils::Lower(role) == Utils::Lower(other.role) &&
           Utils::Lower(windowType) == Utils::Lower(other.windowType) &&
           Utils::Lower(titlePattern) == Utils::Lower(other.titlePattern) &&
           workspace == other.workspace &&
           horizontal == other.horizontal &&
           vertical == other.vertical &&
           mode == other.mode &&
           denyFullscreen == other.denyFullscreen;
}

WindowRuleEffect ResolveWindowRules(
    const std::vector<WindowRule>& rules,
    const std::string& className,
    const std::string& instanceName,
    const std::string& title)
{
    WindowIdentity window;
    window.className = className;
    window.instanceName = instanceName;
    window.title = title;

    return ResolveWindowRules(rules, window);
}

WindowRuleEffect ResolveWindowRules(
    const std::vector<WindowRule>& rules,
    const WindowIdentity& window)
{
    std::vector<const WindowRule*> matching;

    for (const WindowRule& rule : rules)
    {
        if (rule.Matches(window))
            matching.push_back(&rule);
    }

    // Least specific first; stable, so equally specific rules keep file
    // order and the later line overrides the earlier one.
    std::stable_sort(matching.begin(), matching.end(),
        [](const WindowRule* a, const WindowRule* b)
        {
            return a->SelectorCount() < b->SelectorCount();
        });

    WindowRule::Mode winningMode = WindowRule::Mode::Default;
    WindowRuleEffect effect;

    for (const WindowRule* rule : matching)
    {
        if (rule->workspace > 0)
            effect.forceWorkspace = rule->workspace;

        if (rule->horizontal != WindowRule::Horizontal::Default)
            effect.horizontal = rule->horizontal;

        if (rule->vertical != WindowRule::Vertical::Default)
            effect.vertical = rule->vertical;

        if (rule->mode != WindowRule::Mode::Default)
            winningMode = rule->mode;

        if (rule->denyFullscreen)
            effect.denyFullscreen = true;
    }

    effect.forceTile       = (winningMode == WindowRule::Mode::Tiled);
    effect.forceFloat      = (winningMode == WindowRule::Mode::Floating);
    effect.forceFullscreen = (winningMode == WindowRule::Mode::Fullscreen);

    return effect;
}

std::vector<WindowRule> LoadWindowRules(
    const Config& config)
{
    std::vector<WindowRule> rules;

    for (const std::string& entry : config.GetAll("windowrule"))
    {
        WindowRule rule;
        std::string error;

        if (WindowRule::Parse(entry, rule, &error))
            rules.push_back(rule);
        else
            Logger::Warning("ignoring windowrule=" + entry + " - " + error);
    }

    return rules;
}

}
