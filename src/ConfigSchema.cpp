#include "ConfigSchema.h"

#include <algorithm>

namespace Kohiko
{

const std::vector<ConfigOption>& ConfigSchema::All()
{
    static const std::vector<ConfigOption> options =
    {
        // ==================================================================
        // General
        // ==================================================================
        {
            "General", "", "general.focus_follows_mouse", ConfigValueType::Boolean, "true",
            "Moving the mouse onto a window focuses it automatically, without "
            "needing to click. Recommended on for a tiling-style workflow; "
            "turn off if you prefer click-to-focus.",
            {}
        },
        {
            "General", "", "general.tiling_misbehavior_threshold", ConfigValueType::Int, "3",
            "How many times in a row a tiled window can fight its assigned "
            "geometry before Kohiko gives up and applies "
            "general.tiling_misbehavior_fallback instead. Recommended: 3 - "
            "low enough to catch a genuinely misbehaving app quickly, high "
            "enough that one ordinary resize during normal startup never "
            "trips it.",
            {}
        },
        {
            "General", "", "general.tiling_misbehavior_fallback", ConfigValueType::Enum, "floating",
            "Where a window that keeps fighting its assigned tile geometry "
            "(see general.tiling_misbehavior_threshold) gets moved to instead. "
            "'floating' keeps it visible alongside everything else; "
            "'new_workspace' moves it out of the way entirely. Recommended: "
            "floating, unless the misbehaving app is something you'd rather "
            "not see at all until you switch to it.",
            {"floating", "new_workspace"}
        },
        {
            "General", "", "general.min_tile_width", ConfigValueType::Int, "100",
            "No tile is ever shrunk narrower than this, for itself or any "
            "neighbour it displaces. Kohiko opens a window floating instead "
            "of forcing it under this width - it never goes off-screen or "
            "becomes unusably small.",
            {}
        },
        {
            "General", "", "general.min_tile_height", ConfigValueType::Int, "60",
            "Same floor as general.min_tile_width, for height.",
            {}
        },
        {
            "General", "Autostart", "auto_start_programs", ConfigValueType::String, "Telegram discord zen-browser flameshot",
            "Space-separated programs launched once, automatically, right "
            "after Kohiko finishes starting up - no keybind needed. Each runs "
            "exactly like an exec.<name>= command (through /bin/sh -c, on "
            "this session's DISPLAY). Leave empty to autostart nothing. Want "
            "one of these on a specific workspace instead of wherever's "
            "current? Use the Workspaces category's per-workspace autostart "
            "fields instead of (not in addition to) this one.",
            {}
        },
        {
            "General", "Session Restore", "session.restore_priority", ConfigValueType::Enum, "config",
            "When a window's saved session state and a matching windowrule= "
            "disagree (e.g. it was on workspace 1 last time Kohiko exited, "
            "but a windowrule now pins it to workspace 3), which one wins. "
            "'config' keeps your windowrule=s authoritative; 'session' prefers "
            "whatever was actually true last time you used it. Recommended: "
            "config, unless you're actively relying on session restore to "
            "remember placements you're not ready to write permanent rules "
            "for yet.",
            {"config", "session"}
        },
        {
            "General", "Session Restore", "general.adaptive_placement", ConfigValueType::Boolean, "true",
            "Learns, per application, which workspace and which side of the "
            "tiling layout you repeatedly move its windows to by hand, and "
            "starts placing new windows of that application there "
            "automatically once the pattern is clear and consistent - during "
            "an ordinary session, not just after a restart. Never overrides "
            "an explicit windowrule= or a precise Session Restore match; only "
            "fills in when neither has an opinion. Turn off to open every "
            "new window using only the ordinary placement rules, with no "
            "learned habits at all.",
            {}
        },

        // ==================================================================
        // Appearance
        // ==================================================================
        {
            "Appearance", "Layout", "general.inner_gap", ConfigValueType::Int, "6",
            "Pixel gap between tiled windows. 0 disables gaps entirely.",
            {}
        },
        {
            "Appearance", "Layout", "general.outer_gap", ConfigValueType::Int, "8",
            "Pixel gap between the outermost tiles and the edge of the "
            "monitor's usable area. 0 disables it entirely.",
            {}
        },
        {
            "Appearance", "Layout", "general.border_size", ConfigValueType::Int, "2",
            "Pixel width of the border drawn around every tiled/floating "
            "window. 0 disables borders entirely.",
            {}
        },
        {
            "Appearance", "Layout", "general.smart_gaps", ConfigValueType::Boolean, "true",
            "Gaps collapse to 0 while a workspace has only one tiled window "
            "on it - there's nothing for a gap to separate it from yet. "
            "Recommended on.",
            {}
        },
        {
            "Appearance", "Layout", "general.smart_borders", ConfigValueType::Boolean, "true",
            "Same idea as general.smart_gaps, for borders: no border drawn "
            "while a workspace has only one tiled window. Recommended on.",
            {}
        },
        {
            "Appearance", "Colors", "general.border_color_active", ConfigValueType::Color, "0x89b4fa",
            "Border color of the currently-focused window.",
            {}
        },
        {
            "Appearance", "Colors", "general.border_color_inactive", ConfigValueType::Color, "0x45475a",
            "Border color of every window that isn't currently focused.",
            {}
        },
        {
            "Appearance", "Colors", "bar.background", ConfigValueType::Color, "0x1e1e2e",
            "The bar's background color.",
            {}
        },
        {
            "Appearance", "Colors", "bar.foreground", ConfigValueType::Color, "0xcdd6f4",
            "The bar's default text color.",
            {}
        },
        {
            "Appearance", "Colors", "bar.active", ConfigValueType::Color, "0x89b4fa",
            "Accent color the bar uses for whichever workspace/element is "
            "currently active.",
            {}
        },
        {
            "Appearance", "Text", "general.font", ConfigValueType::String, "monospace:pixelsize=14",
            "Fontconfig pattern (e.g. \"JetBrains Mono:pixelsize=13\"), NOT an "
            "XLFD name, used for the bar/launcher/notepad's own text. "
            "Automatically falls back to any other installed font that has a "
            "given glyph, so scripts this font doesn't cover (Cyrillic, CJK, "
            "...) still render instead of showing missing-glyph boxes. See "
            "\"Fonts and languages\" in the README.",
            {}
        },

        // ==================================================================
        // Launcher
        // ==================================================================
        {
            "Launcher", "", "launcher.file_manager", ConfigValueType::String, "pcmanfm",
            "File manager command the launcher opens a folder result with.",
            {}
        },
        {
            "Launcher", "", "launcher.show_hidden", ConfigValueType::Boolean, "false",
            "Show .desktop entries marked Hidden=true/NoDisplay=true too, "
            "instead of skipping them.",
            {}
        },
        {
            "Launcher", "", "launcher.strict_filtering", ConfigValueType::Boolean, "true",
            "On top of Hidden=/NoDisplay=, skips a handful of well-known "
            "non-app helper entries (MIME handlers, updater UIs, "
            "uninstallers, ...) that don't set either. Turn off if it ever "
            "hides something you actually wanted to see.",
            {}
        },
        {
            "Launcher", "Web search", "launcher.internet_search", ConfigValueType::Boolean, "true",
            "Offer web-search suggestions from the launcher at all.",
            {}
        },
        {
            "Launcher", "Web search", "launcher.search_when_no_results", ConfigValueType::Boolean, "true",
            "Only actually show a web-search suggestion once nothing local "
            "matches what's typed, rather than alongside every result.",
            {}
        },
        {
            "Launcher", "Web search", "launcher.default_search_engine", ConfigValueType::Enum, "duckduckgo",
            "Which engine an unprefixed web search uses.",
            {"google", "duckduckgo", "brave", "wikipedia", "archwiki", "github", "youtube", "custom"}
        },
        {
            "Launcher", "Web search", "launcher.fallback_search_engine", ConfigValueType::Enum, "google",
            "Backup engine, used if the default one above is 'custom' but "
            "launcher.custom_search_url is empty.",
            {"google", "duckduckgo", "brave", "wikipedia", "archwiki", "github", "youtube", "custom"}
        },
        {
            "Launcher", "Web search", "launcher.custom_search_url", ConfigValueType::String, "",
            "Only used when default/fallback_search_engine=custom. \"{query}\" "
            "is replaced with the URL-encoded search text, or appended to the "
            "end if no \"{query}\" placeholder is present.",
            {}
        },

        // ==================================================================
        // Bar
        // ==================================================================
        {
            "Bar", "", "general.bar_height", ConfigValueType::Int, "26",
            "Pixel height of the bar shown on every monitor.",
            {}
        },

        // ==================================================================
        // Input
        // ==================================================================
        {
            "Input", "Mouse", "mouse.swap", ConfigValueType::String, "SUPER+BTN1",
            "Modifier+button that picks up and drags a window to swap places "
            "with whatever's under the cursor when released (Hyprland-style "
            "BSP dragging - geometry never changes, only which window sits "
            "in which tile does). Format: MODS+BTN1/BTN2/BTN3.",
            {}
        },
        {
            "Input", "Mouse", "mouse.resize", ConfigValueType::String, "SUPER+BTN3",
            "Modifier+button that resizes the grabbed window in the drag "
            "direction, its neighbour adjusting to match automatically.",
            {}
        },
        {
            "Input", "Keyboard", "keyboard.layouts", ConfigValueType::String, "us, ua",
            "Comma-separated XKB layouts (whatever `setxkbmap -layout` "
            "accepts - \"us\", \"ua\", \"de\", \"ru\", ...), applied at "
            "startup and on reload. Kohiko's own keybinds are unaffected "
            "either way - they're grabbed by physical keycode, not by which "
            "layout is active.",
            {}
        },
        {
            "Input", "Keyboard", "keyboard.layout_toggle", ConfigValueType::String, "grp:alt_shift_toggle",
            "How to switch between the layouts listed above, once more than "
            "one is listed - any setxkbmap \"grp:\" option works. Ignored "
            "while only one layout is configured.",
            {}
        },

        // ==================================================================
        // Workspaces
        // ==================================================================
        {
            "Workspaces", "", "workspace.count", ConfigValueType::Int, "4",
            "How many workspaces Kohiko creates. Changing this and applying "
            "adds or removes the corresponding per-workspace autostart fields "
            "further down this page.",
            {}
        },

        // ==================================================================
        // Power
        // ==================================================================
        {
            "Power", "Commands", "power.shutdown_command", ConfigValueType::String, "systemctl poweroff",
            "Shell command run for the power menu's Shutdown row. Swap in "
            "whatever your init system/session actually uses if it's not "
            "systemd (e.g. loginctl poweroff).",
            {}
        },
        {
            "Power", "Commands", "power.restart_command", ConfigValueType::String, "systemctl reboot",
            "Shell command run for the power menu's Restart row.",
            {}
        },
        {
            "Power", "Commands", "power.suspend_command", ConfigValueType::String, "systemctl suspend",
            "Shell command run for the power menu's Suspend row. Whether "
            "Kohiko locks the screen first is governed by Suspend "
            "Integration's lockscreen.after, just below.",
            {}
        },
        {
            "Power", "Suspend Integration", "lockscreen.after", ConfigValueType::Enum, "suspend",
            "When the native lock screen engages automatically. 'never' - "
            "not even the manual lock command/keybind will lock (a "
            "deliberate full opt-out). 'manual' - only locks on request "
            "(bind=/`kohikoctl dispatch lock`); Suspend does not lock "
            "automatically. 'suspend' - also locks right before Suspend. "
            "'always' - also locks once at Kohiko startup, on top of "
            "'suspend'. Recommended: suspend, unless another locker is "
            "already handling this, or you deliberately don't want one.",
            {"never", "manual", "suspend", "always"}
        },
        {
            "Power", "Suspend Integration", "lockscreen.idle_timeout_minutes", ConfigValueType::Int, "0",
            "Automatically locks the screen after this many minutes with no "
            "keyboard/mouse input anywhere on the display, independent of "
            "lockscreen.after's own Suspend/startup triggers above. 0 "
            "disables idle-timeout locking entirely (the default). Still "
            "fully subject to lockscreen.after=never, which disables "
            "locking altogether, on-demand or automatic.",
            {}
        },
        {
            "Power", "Display Sleep", "general.inhibit_sleep_during_playback", ConfigValueType::Boolean, "true",
            "Keeps the display from powering off via DPMS while there's a "
            "reason it shouldn't: primarily, any application actively "
            "holding a standard D-Bus screen-inhibit request (most video/"
            "media players and browsers - a YouTube tab, VLC, mpv - and "
            "presentation software use this whether or not their window is "
            "fullscreen), with a plain X11 check for a visible fullscreen "
            "window as a fallback for content that doesn't make that "
            "request itself. Never disables DPMS itself, and never "
            "prevents the display from sleeping once genuinely idle - "
            "turn off only if you'd rather the display's own configured "
            "timeout applied unconditionally, even during playback.",
            {}
        },

        // ==================================================================
        // Notepad
        // ==================================================================
        {
            "Notepad", "", "notepad.width", ConfigValueType::Percent, "40%",
            "Width of the notepad popup (Super+N), as a percentage of the "
            "monitor it opens on.",
            {}
        },
        {
            "Notepad", "", "notepad.height", ConfigValueType::Percent, "50%",
            "Height of the notepad popup, as a percentage of the monitor.",
            {}
        },

        // ==================================================================
        // Scratchpad
        // ==================================================================
        {
            "Scratchpad", "", "scratchpad.width", ConfigValueType::Percent, "70%",
            "Width the scratchpad (Super+F1) opens at, as a percentage of "
            "the monitor.",
            {}
        },
        {
            "Scratchpad", "", "scratchpad.height", ConfigValueType::Percent, "70%",
            "Height the scratchpad opens at, as a percentage of the monitor.",
            {}
        },

        // ==================================================================
        // Lock Screen
        // ==================================================================
        {
            "Lock Screen", "Appearance", "lockscreen.background_color", ConfigValueType::Color, "0x1e1e2e",
            "Solid background color, used wherever lockscreen.background_image "
            "isn't set (or fails to load).",
            {}
        },
        {
            "Lock Screen", "Appearance", "lockscreen.foreground", ConfigValueType::Color, "0xcdd6f4",
            "Text and password-dot color.",
            {}
        },
        {
            "Lock Screen", "Appearance", "lockscreen.field_color", ConfigValueType::Color, "0x313244",
            "The password field's own background color.",
            {}
        },
        {
            "Lock Screen", "Appearance", "lockscreen.error_color", ConfigValueType::Color, "0xf38ba8",
            "Color for the brief \"Wrong password\" message and field border "
            "shown after a failed attempt.",
            {}
        },
        {
            "Lock Screen", "Appearance", "lockscreen.font", ConfigValueType::String, "",
            "Fontconfig pattern used for every label on the lock screen. "
            "Leave empty to fall back to general.font.",
            {}
        },
        {
            "Lock Screen", "Appearance", "lockscreen.background_image", ConfigValueType::String, "",
            "Optional path to an image, stretched to fill each monitor. Leave "
            "empty to just use lockscreen.background_color.",
            {}
        },
        {
            "Lock Screen", "Appearance", "lockscreen.logo", ConfigValueType::String, "",
            "Optional path to a logo image, drawn at a fixed size centered "
            "above the password field on every monitor. Leave empty for no "
            "logo.",
            {}
        },
        {
            "Lock Screen", "Clock", "lockscreen.show_clock", ConfigValueType::Boolean, "true",
            "Show the current time on the lock screen.",
            {}
        },
        {
            "Lock Screen", "Clock", "lockscreen.clock_format", ConfigValueType::String, "%H:%M",
            "strftime() pattern for the clock. Ignored while "
            "lockscreen.show_clock is off.",
            {}
        },
        {
            "Lock Screen", "Clock", "lockscreen.show_date", ConfigValueType::Boolean, "true",
            "Show the current date on the lock screen, under the clock.",
            {}
        },
        {
            "Lock Screen", "Clock", "lockscreen.date_format", ConfigValueType::String, "%A, %B %d",
            "strftime() pattern for the date. Ignored while "
            "lockscreen.show_date is off.",
            {}
        },
        {
            "Lock Screen", "Identity", "lockscreen.show_username", ConfigValueType::Boolean, "true",
            "Show your username above the password field.",
            {}
        },
        {
            "Lock Screen", "Identity", "lockscreen.show_hostname", ConfigValueType::Boolean, "false",
            "Show this machine's hostname above the password field, next to "
            "the username (as \"user@host\") if both are on, or alone if "
            "lockscreen.show_username is off.",
            {}
        },
    };

    return options;
}

std::vector<std::string> ConfigSchema::Categories()
{
    std::vector<std::string> categories;

    for (const ConfigOption& option : All())
    {
        if (std::find(categories.begin(), categories.end(), option.category) == categories.end())
            categories.push_back(option.category);
    }

    return categories;
}

}
