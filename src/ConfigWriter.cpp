#include "ConfigWriter.h"

#include "Utils.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace Kohiko
{

namespace
{

// Mirrors Config::Load()'s own per-line parsing exactly (trim, skip
// blank/comment lines, split at the first '=') - so a scalar key or a
// repeatable-directive prefix ConfigWriter finds "active" is always
// exactly what Config would actually read as that key's value.
bool SplitActiveLine(
    const std::string& rawLine,
    std::string& outKey,
    std::string& outValue)
{
    std::string trimmed = Utils::Trim(rawLine);

    if (trimmed.empty() || trimmed[0] == '#')
        return false;

    std::size_t pos = trimmed.find('=');

    if (pos == std::string::npos)
        return false;

    outKey   = Utils::Trim(trimmed.substr(0, pos));
    outValue = Utils::Trim(trimmed.substr(pos + 1));

    return !outKey.empty();
}

}

const char* const ConfigWriter::kDefaultMarker = "# --- Added by Kohiko Settings ---";

bool ConfigWriter::Load(
    const std::string& path)
{
    std::ifstream file(path);

    if (!file.is_open())
        return false;

    m_path = path;
    m_lines.clear();

    std::string line;

    while (std::getline(file, line))
        m_lines.push_back(line);

    return true;
}

bool ConfigWriter::Save(
    const std::string& path)
{
    std::string target = path.empty() ? m_path : path;

    if (target.empty())
        return false;

    std::string tempPath = target + ".kohiko-settings.tmp";

    {
        std::ofstream out(tempPath, std::ios::trunc);

        if (!out.is_open())
            return false;

        for (const std::string& line : m_lines)
            out << line << '\n';

        if (!out.good())
        {
            out.close();
            std::remove(tempPath.c_str());
            return false;
        }
    }

    if (std::rename(tempPath.c_str(), target.c_str()) != 0)
    {
        std::remove(tempPath.c_str());
        return false;
    }

    m_path = target;
    return true;
}

std::string ConfigWriter::GetScalar(
    const std::string& key) const
{
    std::string result;

    for (const std::string& rawLine : m_lines)
    {
        std::string lineKey, lineValue;

        if (SplitActiveLine(rawLine, lineKey, lineValue) && lineKey == key)
            result = lineValue; // last one wins, same as Config::GetString()
    }

    return result;
}

bool ConfigWriter::Contains(
    const std::string& key) const
{
    for (const std::string& rawLine : m_lines)
    {
        std::string lineKey, lineValue;

        if (SplitActiveLine(rawLine, lineKey, lineValue) && lineKey == key)
            return true;
    }

    return false;
}

void ConfigWriter::SetScalar(
    const std::string& key,
    const std::string& value,
    const std::string& marker)
{
    std::string newLine = key + "=" + value;

    // Last active occurrence wins for reads, so that's the one
    // updated in place - any earlier, now-shadowed occurrence is left
    // alone rather than also rewritten, same as manual editing would.
    for (std::size_t i = m_lines.size(); i-- > 0;)
    {
        std::string lineKey, lineValue;

        if (SplitActiveLine(m_lines[i], lineKey, lineValue) && lineKey == key)
        {
            m_lines[i] = newLine;
            return;
        }
    }

    std::size_t insertAt = EnsureAddedMarker(marker);
    m_lines.insert(m_lines.begin() + static_cast<std::ptrdiff_t>(insertAt), newLine);
}

std::vector<std::string> ConfigWriter::GetRawBlock(
    const std::string& prefix) const
{
    std::vector<std::string> result;

    for (const std::string& rawLine : m_lines)
    {
        std::string trimmed = Utils::Trim(rawLine);

        if (trimmed.empty() || trimmed[0] == '#')
            continue;

        if (trimmed.rfind(prefix, 0) == 0) // starts with prefix
            result.push_back(trimmed.substr(prefix.size()));
    }

    return result;
}

void ConfigWriter::ReplaceRawBlock(
    const std::string& prefix,
    const std::vector<std::string>& newSuffixes)
{
    std::vector<std::size_t> matched;

    for (std::size_t i = 0; i < m_lines.size(); ++i)
    {
        std::string trimmed = Utils::Trim(m_lines[i]);

        if (!trimmed.empty() && trimmed[0] != '#' && trimmed.rfind(prefix, 0) == 0)
            matched.push_back(i);
    }

    std::size_t insertAt;

    if (!matched.empty())
    {
        insertAt = matched.front();

        // Largest index first, so removing one never shifts the
        // position of any match still waiting to be removed.
        for (auto it = matched.rbegin(); it != matched.rend(); ++it)
            m_lines.erase(m_lines.begin() + static_cast<std::ptrdiff_t>(*it));
    }
    else
    {
        insertAt = EnsureAddedMarker(kDefaultMarker);
    }

    std::vector<std::string> newLines;

    for (const std::string& suffix : newSuffixes)
        newLines.push_back(prefix + suffix);

    m_lines.insert(
        m_lines.begin() + static_cast<std::ptrdiff_t>(insertAt),
        newLines.begin(), newLines.end());
}

std::size_t ConfigWriter::EnsureAddedMarker(
    const std::string& marker)
{
    for (std::size_t i = 0; i < m_lines.size(); ++i)
    {
        if (Utils::Trim(m_lines[i]) == marker)
            return m_lines.size(); // marker exists - new entries still just go at the end, after it and everything already added below it
    }

    if (!m_lines.empty() && !Utils::Trim(m_lines.back()).empty())
        m_lines.push_back("");

    m_lines.push_back(marker);

    return m_lines.size();
}

}
