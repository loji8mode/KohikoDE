#include "AppConfigStore.h"
#include "Xdg.h"

#include <fstream>
#include <sstream>

namespace Kohiko
{

AppConfigStore::AppConfigStore(const std::string& fileName)
{
    m_path = (Xdg::ConfigDir() / fileName).string();
    Load();
}

void AppConfigStore::Load()
{
    std::ifstream file(m_path);
    if (!file.is_open())
        return;

    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#')
            continue;

        auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        m_values[key] = value;
    }
}

void AppConfigStore::Save()
{
    std::ofstream file(m_path, std::ios::trunc);
    if (!file.is_open())
        return;

    file << "# Kohiko app settings - generated automatically, safe to delete\n";
    for (auto& [key, value] : m_values)
        file << key << "=" << value << "\n";
}

std::string AppConfigStore::GetString(const std::string& key, const std::string& fallback) const
{
    auto it = m_values.find(key);
    return it != m_values.end() ? it->second : fallback;
}

bool AppConfigStore::GetBool(const std::string& key, bool fallback) const
{
    auto it = m_values.find(key);
    if (it == m_values.end())
        return fallback;

    return it->second == "true" || it->second == "1";
}

int AppConfigStore::GetInt(const std::string& key, int fallback) const
{
    auto it = m_values.find(key);
    if (it == m_values.end())
        return fallback;

    try { return std::stoi(it->second); }
    catch (...) { return fallback; }
}

void AppConfigStore::SetString(const std::string& key, const std::string& value)
{
    m_values[key] = value;
}

void AppConfigStore::SetBool(const std::string& key, bool value)
{
    m_values[key] = value ? "true" : "false";
}

void AppConfigStore::SetInt(const std::string& key, int value)
{
    m_values[key] = std::to_string(value);
}

}
