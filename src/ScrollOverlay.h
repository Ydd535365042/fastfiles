#pragma once
#include "Theme.h"
#include <functional>

// WinUI's ScrollBar (ScrollBar_themeresources.xaml) drawn over a window's own content, for the lists that have no XAML
// ScrollViewer: the mouse indicator is a thin rounded thumb at the edge (2 px, fading in while the pointer moves or the
// content scrolls, out 2 s after); resting on the 12 px strip expands it after 0.4 s (167 ms, KeySpline 0,0,0,1) to a
// 6 px thumb on the AcrylicInAppFillColorDefault track with the caret arrows, contracting 0.5 s after leaving. The
// arrows step a line, the track pages toward the pointer (RepeatButton: 250 ms delay, every 50 ms), the thumb drags.
// The colors follow the theme (Theme::scroll*), so dark mode never shows the system's light scroll bar.
// The owner calls Set after each layout / scroll, routes its mouse messages here first and paints it last.
class ScrollOverlay
{
public:
    // `scroll` moves the owner's content to an absolute position (it then calls Set with the new one)
    void Attach(HWND hwnd, bool vertical, UINT_PTR timer, const Theme* theme, std::function<void(int)> scroll);
    // content / view lengths along the axis, the position, the strip it lives in (client coordinates), a line's step.
    // activity: a moved position shows the indicator (false: new content, back at the top without a flash)
    void Set(int content, int view, int pos, const RECT& strip, int line, bool activity = true);
    bool Needed() const { return m_content > m_view && m_view > 0; }
    void Refresh() const; // repaint the strip if it shows (its owner drew over it outside WM_PAINT)
    bool Showing() const { return Needed() && Shown() > 0; }

    // Mouse (client coordinates). True when the overlay took the message (the owner skips it).
    bool OnMouseMove(POINT pt, bool buttonDown);
    bool OnButtonDown(POINT pt);
    bool OnButtonUp();
    void OnMouseLeave();
    void OnCaptureLost();
    void OnTimer(UINT_PTR id); // the owner forwards WM_TIMER with this overlay's id
    void Activity();           // the content scrolled another way (wheel, keys): the indicator shows for a while
    bool Captured() const { return m_drag || m_press != Part::None; }
    bool Over(POINT pt) const;  // the point is on the strip (while it can be used)

    void Paint(HDC hdc, const RECT& clip) const; // last, over everything else; antialiased (GDI+), alpha-blended

private:
    enum class Part { None, Decrease, Increase, PageDecrease, PageIncrease, Thumb };
    Part HitTest(POINT pt) const;
    RECT ArrowRect(bool increase) const;
    RECT ThumbBox() const;   // the thumb's layout box along the track (full strip width)
    RECT TrackBox() const;   // between the arrows
    int ThumbLength() const;
    void Step(Part part);    // one arrow / page step
    void Invalidate() const;
    void Kick();             // keep the frame timer running while anything is in motion or pending
    double Expand() const;   // 0 collapsed .. 1 expanded, now
    double Shown() const;    // 0 hidden .. 1 shown, now

    HWND m_hwnd = nullptr;
    bool m_vertical = true;
    UINT_PTR m_timer = 0;
    const Theme* m_theme = nullptr;
    std::function<void(int)> m_scroll;
    int m_content = 0, m_view = 0, m_pos = 0, m_line = 1;
    RECT m_strip{};
    bool m_hover = false;          // the pointer is over the strip
    bool m_drag = false;
    int m_grab = 0;                // where on the thumb it was taken
    Part m_press = Part::None, m_hot = Part::None;
    POINT m_pressAt{};
    ULONGLONG m_nextRepeat = 0;
    // Animations: a target and when it was set, eased toward over the duration
    struct Ease
    {
        double from = 0, to = 0;
        ULONGLONG at = 0;
        int ms = 167;
        bool spline = true; // KeySpline 0,0,0,1 (the thumb's width); false: linear (the opacity animations)
        double Now() const;
        bool Running() const { return at && GetTickCount64() - at < static_cast<ULONGLONG>(ms); }
        void Go(double target, int duration, bool eased);
    };
    Ease m_expand, m_shown;
    mutable double m_drawnExpand = 0, m_drawnShown = 0; // what the last paint of the strip showed (the timer runs until it's final)
    ULONGLONG m_hoverSince = 0, m_leftAt = 0, m_activeAt = 0;
};

// The same bars over a native control that scrolls itself (Edit, RichEdit, report ListView: the details pane's
// previews). The control keeps its system scroll bars, so its own scrolling (wheel, keys, selection, smooth scroll)
// is untouched; the owner places it a scroll bar's width past the right / bottom edge of a clipping parent so they're
// never seen. A subclass reads the control's scroll state after each message and draws the overlay last.
class NativeBars
{
public:
    enum class Kind { Edit, Rich, List };
    // timers: two WM_TIMER ids the control itself doesn't use
    void Attach(HWND control, Kind kind, const Theme* theme, UINT_PTR timerV, UINT_PTR timerH);
    void Detach();
    void Sync(); // the scroll state may have changed (content set, resized)

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void Scroll(bool vertical, int pos);
    bool Info(int bar, SCROLLINFO& si) const;

    void Sync(bool quiet);

    HWND m_hwnd = nullptr;
    Kind m_kind = Kind::Edit;
    const Theme* m_theme = nullptr;
    ScrollOverlay m_v, m_h;
    UINT_PTR m_timerV = 0, m_timerH = 0;
    int m_posV = 0, m_posH = 0;
    bool m_syncing = false;
};
