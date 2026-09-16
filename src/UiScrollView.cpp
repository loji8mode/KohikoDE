#include "UiScrollView.h"
#include "UiWindow.h"

#include <algorithm>

namespace Kohiko
{

void ScrollView::Translate(Widget* widget, int dx, int dy)
{
    widget->bounds.x += dx;
    widget->bounds.y += dy;

    for (auto& child : widget->Children())
        Translate(child.get(), dx, dy);
}

void ScrollView::SetContent(std::unique_ptr<Widget> content)
{
    // The content retired by the *previous* SetContent() call is only
    // actually freed now, at the start of a fresh call - never
    // immediately when it stops being current. SetContent() is
    // typically invoked from inside a click/keypress callback
    // belonging to a widget *in* the tree being replaced (a row's
    // onClick calling straight back into RebuildPage(), say) -
    // destroying that widget (and the std::function currently
    // executing on its behalf) while its own call stack is still
    // unwinding is a real use-after-free in practice, not just in
    // theory (caught by AddressSanitizer during development). Deferring
    // the actual free to the next call - by which point we're always
    // back at the top of a fresh, unrelated event - avoids that
    // without requiring every onClick/onChange call site to know to
    // defer anything itself.
    m_retiredContent.reset();

    if (m_content)
        m_retiredContent = ReleaseChild(m_content);

    m_scrollOffset = 0;

    // Callers build `content` using local coordinates starting at
    // (0, 0) - see e.g. AudioWindow::RebuildPage() - on the
    // assumption that (0, 0) is this ScrollView's own top-left.
    // Reconcile the two coordinate spaces in one place: shift
    // `content` itself AND every descendant (rows, cards, sliders,
    // ...) by this ScrollView's actual on-screen position, the same
    // way scrolling later re-shifts everything by the wheel offset.
    // (Previously only `content`'s own bounds were pinned here, which
    // left every child still sitting at its original local (0, 0)-
    // relative position - i.e. rendered at the window's absolute
    // top-left instead of inside this ScrollView.)
    const int dx = bounds.x - content->bounds.x;
    const int dy = bounds.y - content->bounds.y;

    content->bounds.width = bounds.width;

    m_content = AddChild(std::move(content));

    if (dx != 0 || dy != 0)
        Translate(m_content, dx, dy);
}

Rect ScrollView::ComputeThumbRect(Rect viewport, int contentHeight, int scrollOffset)
{
    if (contentHeight <= viewport.height)
        return Rect{ 0, 0, 0, 0 };

    const int trackHeight = viewport.height;
    const int thumbHeight = std::max(24, trackHeight * viewport.height / contentHeight);
    const int maxOffset = contentHeight - viewport.height;
    const int maxThumbTravel = trackHeight - thumbHeight;
    const int thumbY = viewport.y + (maxOffset > 0 ? (scrollOffset * maxThumbTravel / maxOffset) : 0);

    const int thumbWidth = 4;
    const int thumbX = viewport.Right() - thumbWidth - 3;

    return Rect{ thumbX, thumbY, thumbWidth, thumbHeight };
}

void ScrollView::Draw(UiWindow& window)
{
    window.SetClip(bounds);
    DrawChildren(window);
    window.ClearClip();

    if (!m_content)
        return;

    // A thin overlay thumb - shown only when there's actually
    // something to scroll, drawn last so it sits above the content -
    // is the only visual cue that a short window is hiding rows below
    // the fold. Without it, resizing down toward the documented
    // ~420x360 floor (see AudioWindow.h) silently swaps "everything
    // is visible" for "the rest doesn't exist" from the user's point
    // of view: the content is one wheel-scroll away, but nothing on
    // screen suggests that, and there's no drag handle (deliberately -
    // this is a cue, not a second scrolling mechanism to keep in sync
    // with OnScroll()'s own clamping).
    const Rect thumb = ComputeThumbRect(bounds, m_content->bounds.height, m_scrollOffset);
    if (thumb.height > 0)
        window.FillRoundedRect(thumb, window.Theme().border, thumb.width / 2);
}

void ScrollView::OnScroll(Point, int delta)
{
    if (!m_content)
        return;

    const int step = 48;
    int maxOffset = std::max(0, m_content->bounds.height - bounds.height);
    int newOffset = std::clamp(m_scrollOffset - delta * step, 0, maxOffset);

    int diff = m_scrollOffset - newOffset;
    if (diff != 0)
        Translate(m_content, 0, diff);

    m_scrollOffset = newOffset;
}

}
