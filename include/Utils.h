#pragma once

#include <string>
#include <vector>

namespace Kohiko::Utils
{

std::string Trim(
    const std::string& value
);

std::string Lower(
    const std::string& value
);

// Parses a "70%" style string into a 0..1 fraction. Returns `fallback`
// if `value` is empty or doesn't end in '%'.
float ParsePercent(
    const std::string& value,
    float fallback
);

// `text` as ONE word for /bin/sh -c: wrapped in single quotes, with any
// single quote inside it written as '\'' - so a window title or class
// containing spaces, quotes, $, `, ; or anything else reaches the
// program as exactly that text and is never interpreted by the shell.
std::string ShellQuote(
    const std::string& text
);

// Splits `value` on runs of whitespace, dropping empty tokens - used
// for space-separated config lists such as `auto_start_programs=`
// (e.g. "telegram-desktop discord zen-browser" -> 3 tokens).
std::vector<std::string> SplitWhitespace(
    const std::string& value
);

// Splits `value` on every occurrence of `separator`, dropping empty
// tokens and trimming surrounding whitespace from each one. Used for
// freedesktop's semicolon-separated lists (Categories=, Keywords=,
// Inherits=, ...) and for comma-separated config lists - both usually
// end with a trailing separator, which would otherwise produce a
// spurious empty final token.
std::vector<std::string> Split(
    const std::string& value,
    char separator
);

// Byte offset of the start of the UTF-8 codepoint immediately before
// `pos` (clamped to 0 if `pos` is already at/before the start). Lets
// cursor movement, backspace, and delete act on whole characters
// instead of individual UTF-8 continuation bytes.
std::size_t Utf8PrevBoundary(
    const std::string& value,
    std::size_t pos
);

// Byte offset of the start of the UTF-8 codepoint immediately after
// `pos` (clamped to value.size()).
std::size_t Utf8NextBoundary(
    const std::string& value,
    std::size_t pos
);

// Clamps `pos` to value.size() and then, if that lands inside a
// multi-byte UTF-8 character (e.g. a column carried over from a
// different line of different content), backs up to that character's
// start. Used wherever a byte offset from one string is reused
// against another string that may be encoded differently at that
// offset.
std::size_t Utf8ClampToBoundary(
    const std::string& value,
    std::size_t pos
);

// Overwrites `value`'s current buffer with zeros before clearing it,
// so a plaintext password doesn't just sit in still-allocated heap
// memory as ordinary std::string::clear() would leave it (clear()
// only resets size(), it doesn't touch the bytes themselves) -
// used by LockScreen right after every use of the typed password.
// This can only ever be a best-effort mitigation, not a guarantee:
// std::string's short-string optimization, any prior reallocation
// while the password was being typed, and swap space are all outside
// what a single call like this can reach - see LockScreen.cpp's own
// comment at its call sites for what this does and doesn't cover.
void SecureErase(
    std::string& value
);

}
