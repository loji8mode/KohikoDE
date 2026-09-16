#pragma once

#include "Types.h"

#include <algorithm>
#include <chrono>

namespace Kohiko
{

// The arithmetic behind NotificationPopup/NotificationCenter's on-
// screen placement - deliberately its own header, with zero X11
// dependency, so it can be unit-tested directly (see
// tests/test_notificationlayout.cpp) the same way WindowPlacementNotice.h
// and EventLoop.h's own TickSchedule namespace already are: the
// *decision* (where exactly should the third stacked toast on a
// 1920x1080 monitor land?) is what regressions actually happen in,
// not the X11 calls that later paint a rectangle at whatever this
// says - so that's what gets a fast, no-display-required test, with
// the live-X11 test (test_notificationcenter.cpp) left to confirm the
// window this geometry ends up applied to really behaves the way the
// spec requires (override-redirect, right type, no focus stolen).
namespace NotificationLayout
{

// Bottom-right placement, per the spec ("Bottom-right placement with
// a sensible margin from the screen edges") - kMargin is that margin,
// on both edges. kGap is the vertical breathing room between two
// stacked cards - without it, a full stack would read as one solid
// block rather than several distinct notifications, undermining the
// "similar to Telegram's notification popups" ask.
constexpr int kMargin = 16;
constexpr int kGap = 8;

// Padding inside the card, around the (auto-sized) text - "readable
// for longer device names" (the spec's own phrasing) is what
// kMaxWidth is for: past this width, text wraps to the row height
// this doesn't grow for. In practice every real caller (a device
// name) comfortably fits on one line under this cap; a name long
// enough not to isn't a placement-math regression, it's a rendering
// truncation NotificationPopup's own Redraw() is responsible for, not
// this file.
constexpr int kHorizontalPadding = 16;
constexpr int kVerticalPadding = 12;
constexpr int kMinWidth = 180;
constexpr int kMaxWidth = 360;

// However many notifications are already stacked at once, force the
// oldest one out rather than let the stack grow without bound - see
// NotificationCenter::Post()'s own comment for why this is "handled
// cleanly", not merely "doesn't crash".
constexpr int kMaxStacked = 5;

// "Automatically disappear after exactly 2.5 seconds", per the spec -
// named here (rather than a bare `std::chrono::milliseconds(2500)`
// literal sitting in NotificationCenter::Post()'s default argument)
// specifically so that exact figure has its own line a fast, no-X11
// unit test (see tests/test_notificationlayout.cpp) can assert on
// directly, instead of the only coverage for "is it really 2.5
// seconds" being a live test that has to either wait out a real 2.5s
// timeout or can't actually observe this particular value at all
// (NotificationCenter::Post() accepts a lifetime override precisely
// so *that* test can use a short one instead - see its own comment).
constexpr std::chrono::milliseconds kDefaultLifetime{2500};

// The card's own on-screen width, auto-sized to `textWidth` (from
// Font::TextWidth() against whatever text this notification actually
// shows) but clamped to a sane range - never so narrow the card looks
// broken, never so wide one long device name spans half the screen.
inline int ComputeWidth(int textWidth)
{
    int width = textWidth + kHorizontalPadding * 2;
    return std::clamp(width, kMinWidth, kMaxWidth);
}

// The card's own on-screen height - auto-sized to content per the
// spec ("Automatically size to the content"), which for a single-line
// toast just means the font's own line height plus padding; nothing
// here assumes a fixed height the way a bar row does.
inline int ComputeHeight(int fontHeight)
{
    return fontHeight + kVerticalPadding * 2;
}

// Where a card of `width`x`height` belongs, bottom-right of
// `monitorGeometry`, given that `stackOffsetFromBottom` pixels of that
// same corner are already occupied by other notifications currently
// stacked above it (0 for the first/only one on this monitor) - see
// NotificationCenter::RestackFromBottom() for how that offset is
// actually accumulated across several active notifications "without
// overlap" (the spec's own requirement). ClampedTo() at the end is
// what keeps a notification wider or taller than the monitor itself
// (a tiny/odd monitor, or an unreasonably long piece of text that
// still slipped past kMaxWidth's row-wrap-not-truncate boundary) from
// ever landing partly or fully off-screen, the same guarantee
// PowerMenu::Open() already relies on this exact method for.
inline Rect ComputeRect(
    const Rect& monitorGeometry,
    int width,
    int height,
    int stackOffsetFromBottom)
{
    Rect rect;
    rect.width = width;
    rect.height = height;
    rect.x = monitorGeometry.Right() - kMargin - width;
    rect.y = monitorGeometry.Bottom() - kMargin - height - stackOffsetFromBottom;

    return rect.ClampedTo(monitorGeometry, 0);
}

}

}
