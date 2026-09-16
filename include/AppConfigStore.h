#pragma once

#include <map>
#include <string>

namespace Kohiko
{

// A deliberately tiny read/write key=value store, distinct from the
// existing read-only IniFile (see include/IniFile.h, whose own
// comment explains it's shaped around parsing other people's
// freedesktop-format files, never writing them). What kohiko-audio/
// network/bluetooth need to persist is a short flat list of small UI
// preferences - which page was last open, whether the tray widget
// should show notifications, and similar - not sectioned config with
// someone else's escaping rules to preserve, so this intentionally
// doesn't try to be a general-purpose ini reader/writer: one
// "key=value" pair per line, '#'-prefixed lines and blank lines
// ignored, no sections, no quoting.
class AppConfigStore
{
public:

    // `fileName` is just the file name (e.g. "kohiko-audio.conf") -
    // it's read from and written to under Xdg::ConfigDir().
    explicit AppConfigStore(const std::string& fileName);

    std::string GetString(const std::string& key, const std::string& fallback = "") const;
    bool GetBool(const std::string& key, bool fallback = false) const;
    int GetInt(const std::string& key, int fallback = 0) const;

    // Every setter stages the change in memory; call Save() to
    // persist. Not auto-saved on every call since a settings page
    // often changes several keys in response to one user action (e.g.
    // switching pages might update both "last_page" and a scroll
    // position) - callers batch those into one Save().
    void SetString(const std::string& key, const std::string& value);
    void SetBool(const std::string& key, bool value);
    void SetInt(const std::string& key, int value);

    void Save();

private:

    std::string m_path;
    std::map<std::string, std::string> m_values;

    void Load();
};

}
