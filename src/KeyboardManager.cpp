#include "KeyboardManager.h"

#include "Config.h"
#include "KeyCombo.h"
#include "Utils.h"
#include "WindowManager.h"
#include "XConnection.h"

#include <cctype>

namespace Kohiko
{

KeyboardManager::KeyboardManager(
    XConnection& connection,
    WindowManager& windowManager)
    :
    m_connection(connection),
    m_windowManager(windowManager)
{
}

void KeyboardManager::Configure(
    const Config& config)
{
    m_bindings.clear();
    m_connection.UngrabAllKeys();

    for (const std::string& entry : config.GetAll("bind"))
    {
        std::string trimmed = Utils::Trim(entry);
        auto spacePos = trimmed.find(' ');

        if (spacePos == std::string::npos)
            continue;

        std::string comboText  = trimmed.substr(0, spacePos);
        std::string actionText = Utils::Trim(trimmed.substr(spacePos + 1));

        ParsedCombo combo = ParseCombo(comboText);

        if (!combo.valid)
            continue;

        KeySym keysym = ResolveKeysym(combo.token);

        if (keysym == NoSymbol)
            continue;

        KeyCode keycode = m_connection.KeysymToKeycode(keysym);

        if (keycode == 0)
            continue;

        Binding binding;
        binding.modifiers = combo.modifiers;
        binding.keycode = keycode;
        binding.command = Command::Parse(actionText);

        m_bindings.push_back(binding);

        for (unsigned int variant : m_connection.LockVariants(combo.modifiers))
            m_connection.GrabKey(keycode, variant);
    }
}

bool KeyboardManager::HandleKeyPress(
    const XKeyEvent& event)
{
    static constexpr unsigned int kRelevantMask =
        ShiftMask | ControlMask | Mod1Mask | Mod4Mask;

    unsigned int modifiers = event.state & kRelevantMask;

    for (const Binding& binding : m_bindings)
    {
        if (binding.keycode == event.keycode && binding.modifiers == modifiers)
        {
            m_windowManager.Execute(binding.command);
            return true;
        }
    }

    return false;
}

KeySym KeyboardManager::ResolveKeysym(
    const std::string& token) const
{
    KeySym sym = XStringToKeysym(token.c_str());

    if (sym != NoSymbol)
        return sym;

    std::string lower = Utils::Lower(token);
    sym = XStringToKeysym(lower.c_str());

    if (sym != NoSymbol)
        return sym;

    std::string title = lower;

    if (!title.empty())
        title[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(title[0])));

    return XStringToKeysym(title.c_str());
}

}
