#include "WindowRuleDraft.h"

#include "Utils.h"
#include "WindowRule.h"

#include <array>

namespace Kohiko
{

namespace
{

const std::array<const char*, 3> kHorizontalTokens = {"", "left", "right"};
const std::array<const char*, 3> kVerticalTokens   = {"", "top", "bottom"};
const std::array<const char*, 4> kModeTokens       = {"", "tiled", "floating", "fullscreen"};

}

std::string WindowRuleDraft::KindDescription() const
{
    RuleScope scope;
    scope.role = roleText;
    scope.windowType = typeText;
    return scope.Describe();
}

WindowRuleDraft ParseWindowRuleDraft(const std::string& line)
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
        // (mid-edit, or a hand-typed line this editor's never seen) is
        // simply not modelled - strictness is the validator's job, via
        // WindowRule::Parse().
        if (colon == std::string::npos || colon == 0)
            continue;

        std::string key = Utils::Lower(token.substr(0, colon));
        std::string raw = token.substr(colon + 1);

        // Selector text keeps the case it was typed in and shows %XX
        // escapes decoded.
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
            for (int i = 1; i < 3; ++i) if (v == kHorizontalTokens[static_cast<std::size_t>(i)]) draft.horizontalIndex = i;
        }
        else if (key == "vertical")
        {
            std::string v = Utils::Lower(raw);
            for (int i = 1; i < 3; ++i) if (v == kVerticalTokens[static_cast<std::size_t>(i)]) draft.verticalIndex = i;
        }
        else if (key == "mode")
        {
            std::string v = Utils::Lower(raw);
            for (int i = 1; i < 4; ++i) if (v == kModeTokens[static_cast<std::size_t>(i)]) draft.modeIndex = i;
        }
    }

    return draft;
}

std::string SerializeWindowRuleDraft(const WindowRuleDraft& draft)
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
    // there, and the validator reports what's missing.
    if (!draft.appText.empty())      add("app:" + WindowRule::EncodeValue(draft.appText));
    if (!draft.classText.empty())    add("class:" + WindowRule::EncodeValue(draft.classText));
    if (!draft.instanceText.empty()) add("instance:" + WindowRule::EncodeValue(draft.instanceText));
    if (!draft.roleText.empty())     add("role:" + WindowRule::EncodeValue(draft.roleText));
    if (!draft.typeText.empty())     add("type:" + draft.typeText);
    if (!draft.titleText.empty())    add("title:" + WindowRule::EncodeValue(draft.titleText));

    if (!draft.workspaceText.empty()) add("workspace:" + draft.workspaceText);
    if (draft.horizontalIndex > 0)    add(std::string("horizontal:") + kHorizontalTokens[static_cast<std::size_t>(draft.horizontalIndex)]);
    if (draft.verticalIndex > 0)      add(std::string("vertical:") + kVerticalTokens[static_cast<std::size_t>(draft.verticalIndex)]);
    if (draft.modeIndex > 0)          add(std::string("mode:") + kModeTokens[static_cast<std::size_t>(draft.modeIndex)]);
    if (draft.denyFullscreen)         add("nofullscreen");

    return line;
}

std::string NextWindowTypeText(const std::string& current)
{
    const std::vector<std::string>& types = WindowRule::KnownWindowTypes();

    if (current.empty())
        return types.empty() ? std::string() : types.front();

    for (std::size_t i = 0; i < types.size(); ++i)
    {
        if (types[i] == WindowRule::NormalizeWindowType(current))
            return (i + 1 < types.size()) ? types[i + 1] : std::string();
    }

    return std::string();
}

}
