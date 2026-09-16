// Regression test for kohiko-network's Wi-Fi password prompt gaining
// paste support (Ctrl+V/CLIPBOARD, middle-click/PRIMARY) in 0.20.4 -
// see CHANGELOG.md. This only covers FilterPastedPasswordText(), the
// pure logic behind it (what gets kept vs stripped from whatever
// XConvertSelection() hands back), with no X11 selection round trip -
// PromptForPassword() itself lives in an anonymous namespace in
// NetworkWindow.cpp and needs a live X11 Display/keyboard grab to
// exercise at all, so that side was instead verified by direct
// execution: a live kohiko-network under Xvfb, with a second process
// owning the CLIPBOARD selection, confirming Ctrl+V actually appends
// the real clipboard contents to the field - see the CHANGELOG for
// that verification's specifics.
//
// Also a check on the *other* half of the same "the password field
// appears too restrictive" report - the actual current character
// filter in PromptForPassword()'s own KeyPress handling
// (`len > 0 && buffer[0] >= 0x20`) was checked directly, not assumed:
// it's not numeric-only (that specific claim was already checked and
// found false in an earlier phase - see CHANGELOG.md's "Investigated,
// not changed" section for 0.20.4), and FilterPastedPasswordText()
// below applies the equivalent "any non-control byte" rule to pasted
// text, so both entry paths accept the same thing.

#include "NetworkWindow.h"

#include <cstdio>
#include <cstdlib>

using namespace Kohiko;

namespace
{

int g_pass = 0;

void Check(bool condition, const char* what)
{
    if (condition)
    {
        ++g_pass;
        std::printf("  PASS: %s\n", what);
    }
    else
    {
        std::printf("  FAIL: %s\n", what);
        std::exit(1);
    }
}

}

int main()
{
    std::printf("FilterPastedPasswordText() tests:\n");

    std::printf("-- THE point of this feature: letters, digits, and symbols all pass through --\n");
    {
        // A real-shaped WPA2 passphrase, deliberately containing
        // every character class the original "numeric-only" report
        // was worried about.
        std::string pasted = "Tr0ub4dor&3!Zx#99";
        Check(FilterPastedPasswordText(pasted) == pasted,
              "a password manager's own generated passphrase - letters, digits, and symbols "
              "together - is pasted through completely unchanged, not filtered down to just "
              "the digits");
    }

    std::printf("-- A trailing newline (what copying from a terminal/password manager almost always includes) is stripped --\n");
    {
        Check(FilterPastedPasswordText("correcthorsebatterystaple\n") == "correcthorsebatterystaple",
              "a trailing \\n from the clipboard doesn't become a literal character in the "
              "password - it would otherwise silently make every subsequent connection "
              "attempt fail with a byte-for-byte-correct-looking password");
        Check(FilterPastedPasswordText("correcthorsebatterystaple\r\n") == "correcthorsebatterystaple",
              "a Windows-style \\r\\n line ending is stripped just as completely, not leaving "
              "a stray \\r behind");
    }

    std::printf("-- Embedded control characters (tabs, mid-string newlines) are stripped, not just a trailing one --\n");
    {
        Check(FilterPastedPasswordText("abc\tdef") == "abcdef",
              "a tab pasted in from a spreadsheet cell or similar doesn't survive into the "
              "password");
        Check(FilterPastedPasswordText("line1\nline2") == "line1line2",
              "an embedded newline (a genuinely multi-line clipboard selection, pasted by "
              "accident) is stripped throughout, not just trimmed from one end");
    }

    std::printf("-- Multi-byte UTF-8 characters pass through untouched --\n");
    {
        // Every byte of a non-ASCII character has its high bit set
        // (>= 0x80) - this is the actual regression guard for "only
        // filter C0 control characters, not anything with the high
        // bit set", since a naive \"drop anything >= 0x7f\" filter
        // would silently mangle this instead of passing it through.
        std::string withEmoji = "p\xc3\xa4ssw\xc3\xb6rd\xf0\x9f\x94\x92"; // "pässwörd🔒"
        Check(FilterPastedPasswordText(withEmoji) == withEmoji,
              "accented Latin characters and an emoji - both entirely valid in a WPA "
              "passphrase - survive filtering byte-for-byte");
    }

    std::printf("-- DEL (0x7f) is stripped alongside the C0 control characters --\n");
    {
        std::string withDel = "abc";
        withDel.push_back(static_cast<char>(0x7f));
        withDel += "def";
        Check(FilterPastedPasswordText(withDel) == "abcdef",
              "DEL isn't a printable character NetworkManager would accept either, so it's "
              "filtered the same as the C0 control characters just below it");
    }

    std::printf("-- Empty input stays empty --\n");
    {
        Check(FilterPastedPasswordText("").empty(),
              "an empty clipboard/selection (nothing copied, or a failed conversion that "
              "still reached this far) doesn't crash and produces an empty result");
    }

    std::printf("\nALL %d CHECKS PASSED.\n", g_pass);
    return 0;
}
