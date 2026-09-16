#pragma once

#include "UiWidget.h"

namespace Kohiko
{

// A clipped, mouse-wheel-scrollable viewport onto a single content
// widget taller than it - what every device/network/connection list
// in kohiko-audio/network/bluetooth sits inside, since none of them
// can assume their whole list fits in a fixed-height window (a
// dozen paired Bluetooth devices, or an office's worth of visible
// Wi-Fi networks, easily won't).
class ScrollView : public Widget
{
public:

    // `content`'s own bounds.height is taken as the total scrollable
    // content height - set it to the sum of everything laid out
    // inside before calling this (see AudioWindow::RebuildOutputPage()
    // for the usual pattern: lay out rows starting at y=0 inside a
    // plain Widget, set that widget's bounds.height to the final y,
    // then hand it here).
    void SetContent(std::unique_ptr<Widget> content);

    void Draw(UiWindow& window) override;
    bool WantsInput() const override { return true; }
    bool ClipsHitTesting() const override { return true; }
    void OnScroll(Point p, int delta) override;

private:

    Widget* m_content = nullptr;
    int m_scrollOffset = 0;

    // The *previous* content, kept alive one extra generation rather
    // than destroyed immediately - see SetContent()'s comment.
    std::unique_ptr<Widget> m_retiredContent;

    static void Translate(Widget* widget, int dx, int dy);
};

}
