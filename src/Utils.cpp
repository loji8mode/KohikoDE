#include "Utils.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace Kohiko::Utils
{

std::string Trim(
    const std::string& value)
{
    std::size_t first = value.find_first_not_of(" \t\r\n");

    if (first == std::string::npos)
        return "";

    std::size_t last = value.find_last_not_of(" \t\r\n");

    return value.substr(first, last - first + 1);
}

std::string Lower(
    const std::string& value)
{
    std::string result = value;

    std::transform(
        result.begin(),
        result.end(),
        result.begin(),
        [](unsigned char c)
        {
            return static_cast<char>(std::tolower(c));
        });

    return result;
}

float ParsePercent(
    const std::string& value,
    float fallback)
{
    if (value.empty() || value.back() != '%')
        return fallback;

    try
    {
        float percent = std::stof(value.substr(0, value.size() - 1));
        return percent / 100.0f;
    }
    catch (...)
    {
        return fallback;
    }
}

std::string ShellQuote(
    const std::string& text)
{
    std::string quoted = "'";

    for (char c : text)
    {
        if (c == '\'')
            quoted += "'\\''";
        else
            quoted += c;
    }

    quoted += "'";
    return quoted;
}

std::vector<std::string> SplitWhitespace(
    const std::string& value)
{
    std::vector<std::string> tokens;

    std::size_t pos = 0;

    while (pos < value.size())
    {
        std::size_t start = value.find_first_not_of(" \t\r\n", pos);

        if (start == std::string::npos)
            break;

        std::size_t end = value.find_first_of(" \t\r\n", start);

        if (end == std::string::npos)
            end = value.size();

        tokens.push_back(value.substr(start, end - start));
        pos = end;
    }

    return tokens;
}

namespace
{

// UTF-8 continuation bytes all match binary 10xxxxxx - everything
// else (ASCII bytes and multi-byte lead bytes) starts a new codepoint.
bool IsUtf8Continuation(unsigned char c)
{
    return (c & 0xC0) == 0x80;
}

}

std::vector<std::string> Split(
    const std::string& value,
    char separator)
{
    std::vector<std::string> tokens;

    std::size_t pos = 0;

    while (pos <= value.size())
    {
        std::size_t end = value.find(separator, pos);

        if (end == std::string::npos)
            end = value.size();

        std::string token = Trim(value.substr(pos, end - pos));

        if (!token.empty())
            tokens.push_back(std::move(token));

        pos = end + 1;
    }

    return tokens;
}

std::size_t Utf8PrevBoundary(
    const std::string& value,
    std::size_t pos)
{
    if (pos == 0)
        return 0;

    --pos;

    while (pos > 0 && IsUtf8Continuation(static_cast<unsigned char>(value[pos])))
        --pos;

    return pos;
}

std::size_t Utf8NextBoundary(
    const std::string& value,
    std::size_t pos)
{
    if (pos >= value.size())
        return value.size();

    ++pos;

    while (pos < value.size() && IsUtf8Continuation(static_cast<unsigned char>(value[pos])))
        ++pos;

    return pos;
}

std::size_t Utf8ClampToBoundary(
    const std::string& value,
    std::size_t pos)
{
    pos = std::min(pos, value.size());

    while (pos > 0 && IsUtf8Continuation(static_cast<unsigned char>(value[pos])))
        --pos;

    return pos;
}

void SecureErase(
    std::string& value)
{
    // volatile so the optimizer can't reason "this store is dead,
    // nothing reads `value` again before clear()/destruction" and
    // drop it - the same reasoning explicit_bzero()/SecureZeroMemory()
    // exist for on other platforms. Plain std::memset(value.data(), ...)
    // followed immediately by clear() would be legal for the compiler
    // to elide entirely under the as-if rule.
    if (!value.empty())
    {
        volatile char* data = const_cast<volatile char*>(value.data());

        for (std::size_t i = 0; i < value.size(); ++i)
            data[i] = '\0';
    }

    value.clear();
    value.shrink_to_fit(); // drop the (now-zeroed) buffer instead of leaving it allocated for reuse
}

}
