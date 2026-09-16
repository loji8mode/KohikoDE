#pragma once

#include <cstdint>

namespace Kohiko
{

// Plain 0xRRGGBB colors, same convention as every other color value
// in Kohiko's own config (bar.background=, general.border_color_active=,
// ...) - see Config.h. Kept as a simple struct with sensible built-in
// defaults rather than parsed out of Kohiko's main config file: these
// three apps are meant to look like natural Kohiko citizens without
// coupling their own layout to config keys the window manager's own
// bar/border config was never meant to serve (a config key named
// `bar.background` visually driving a control-panel window body would
// be a coincidence, not a real shared setting) - a real themeable
// palette (see the spec's own "future additions" framing) is exactly
// the kind of thing that can be layered on top of this struct later
// without changing anything that reads from it.
struct UiTheme
{
    std::uint32_t background = 0x1e1e24;
    std::uint32_t surface = 0x282830;
    std::uint32_t surfaceHover = 0x32323c;
    std::uint32_t surfaceActive = 0x3c3c48;
    std::uint32_t border = 0x3a3a44;
    std::uint32_t foreground = 0xe8e8ec;
    std::uint32_t muted = 0x9a9aa4;
    std::uint32_t accent = 0x5b8cff;
    std::uint32_t accentForeground = 0xffffff;
    std::uint32_t danger = 0xe0554f;
    std::uint32_t success = 0x4fbf7a;

    static const UiTheme& Default()
    {
        static UiTheme theme;
        return theme;
    }
};

}
