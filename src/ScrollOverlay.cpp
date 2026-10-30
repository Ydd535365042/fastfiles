#include "ScrollOverlay.h"

namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include <richedit.h>
#include <cmath>

// Timings and sizes from WinUI's ScrollBar_themeresources.xaml (at 96 DPI; the theme scales them)
namespace
{
constexpr int kBar = 12;            // ScrollBarSize: the strip, the arrow buttons
constexpr int kMinThumb = 30;       // ScrollBarVerticalThumbMinHeight / HorizontalThumbMinWidth
constexpr int kInset = 3;           // ScrollBarThumbStrokeThickness 6 (transparent stroke): the fill sits 3 inside the box
constexpr int kThin = 2, kWide = 6; // the visible thumb: Width 8 (collapsed, X +2) / 12 (expanded) less the stroke
constexpr int kExpandDelay = 400, kContractDelay = 500, kExpandMs = 167, kFadeMs = 83, kHideAfter = 2000;
constexpr int kRepeatDelay = 250, kRepeatEvery = 50; // RepeatButton Delay (default) / Interval
}

double ScrollOverlay::Ease::Now() const
{
    if (!at)
        return to;
    const double t = static_cast<double>(GetTickCount64() - at) / ms;
    if (t >= 1)
        return to;
    double f = t;
    if (spline)
    {
        // Cubic Bezier (0,0) (0,0) (0,1) (1,1): x = s^3, y = 3s^2 - 2s^3
        const double s = std::cbrt(t);
        f = 3 * s * s - 2 * s * s * s;
    }
    return from + (to - from) * f;
}

void ScrollOverlay::Ease::Go(double target, int duration, bool eased)
{
    if (target == to)
        return;
    from = Now();
    to = target;
    at = GetTickCount64();
    ms = duration;
    spline = eased;
}

void ScrollOverlay::Attach(HWND hwnd, bool vertical, UINT_PTR timer, const Theme* theme, std::function<void(int)> scroll)
{
    m_hwnd = hwnd;
    m_vertical = vertical;
    m_timer = timer;
    m_theme = theme;
    m_scroll = std::move(scroll);
}

double ScrollOverlay::Expand() const { return std::clamp(m_expand.Now(), 0.0, 1.0); }
double ScrollOverlay::Shown() const { return std::clamp(m_shown.Now(), 0.0, 1.0); }

void ScrollOverlay::Invalidate() const
{
    if (m_hwnd && !IsRectEmpty(&m_strip))
        InvalidateRect(m_hwnd, &m_strip, FALSE);
}

void ScrollOverlay::Kick()
{
    if (m_hwnd)
        SetTimer(m_hwnd, m_timer, 15, nullptr);
}

void ScrollOverlay::Activity()
{
    m_activeAt = GetTickCount64();
    Kick();
}

void ScrollOverlay::Refresh() const
{
    if (Needed() && Shown() > 0)
        Invalidate();
}

void ScrollOverlay::Set(int content, int view, int pos, const RECT& strip, int line, bool activity)
{
    const bool moved = pos != m_pos;
    const bool changed = moved || content != m_content || view != m_view || !EqualRect(&strip, &m_strip);
    const bool visible = Shown() > 0;
    if (changed && visible)
        Invalidate(); // where it was
    m_content = content;
    m_view = view;
    m_pos = pos;
    m_strip = strip;
    m_line = std::max(1, line);
    if (changed && visible)
        Invalidate(); // where it is now
    if (moved && Needed() && activity)
        Activity(); // WinUI shows the indicator while the content scrolls
}

bool ScrollOverlay::Over(POINT pt) const
{
    return Needed() && PtInRect(&m_strip, pt);
}

RECT ScrollOverlay::ArrowRect(bool increase) const
{
    const int a = m_theme->S(kBar);
    if (m_vertical)
        return increase ? RECT{ m_strip.left, m_strip.bottom - a, m_strip.right, m_strip.bottom } : RECT{ m_strip.left, m_strip.top, m_strip.right, m_strip.top + a };
    return increase ? RECT{ m_strip.right - a, m_strip.top, m_strip.right, m_strip.bottom } : RECT{ m_strip.left, m_strip.top, m_strip.left + a, m_strip.bottom };
}

RECT ScrollOverlay::TrackBox() const
{
    const int a = m_theme->S(kBar);
    RECT r = m_strip;
    if (m_vertical)
    {
        r.top += a;
        r.bottom = std::max(r.top, r.bottom - a);
    }
    else
    {
        r.left += a;
        r.right = std::max(r.left, r.right - a);
    }
    return r;
}

int ScrollOverlay::ThumbLength() const
{
    const RECT track = TrackBox();
    const int span = m_vertical ? track.bottom - track.top : track.right - track.left;
    if (span <= 0 || m_content <= 0)
        return 0;
    return std::min(span, std::max(m_theme->S(kMinThumb), MulDiv(span, m_view, m_content)));
}

RECT ScrollOverlay::ThumbBox() const
{
    const RECT track = TrackBox();
    const int span = m_vertical ? track.bottom - track.top : track.right - track.left;
    const int len = ThumbLength();
    const int range = m_content - m_view;
    const int start = (m_vertical ? track.top : track.left) + (range > 0 ? MulDiv(span - len, std::clamp(m_pos, 0, range), range) : 0);
    return m_vertical ? RECT{ m_strip.left, start, m_strip.right, start + len } : RECT{ start, m_strip.top, start + len, m_strip.bottom };
}

ScrollOverlay::Part ScrollOverlay::HitTest(POINT pt) const
{
    if (!Over(pt))
        return Part::None;
    // As in WinUI the arrows and the track work whether or not the bar has expanded yet (they are only invisible)
    const RECT dec = ArrowRect(false), inc = ArrowRect(true), thumb = ThumbBox();
    if (PtInRect(&dec, pt))
        return Part::Decrease;
    if (PtInRect(&inc, pt))
        return Part::Increase;
    const int at = m_vertical ? pt.y : pt.x;
    if (at < (m_vertical ? thumb.top : thumb.left))
        return Part::PageDecrease;
    if (at >= (m_vertical ? thumb.bottom : thumb.right))
        return Part::PageIncrease;
    return Part::Thumb;
}

void ScrollOverlay::Step(Part part)
{
    if (!m_scroll)
        return;
    const int range = std::max(0, m_content - m_view);
    int pos = m_pos;
    switch (part)
    {
    case Part::Decrease: pos -= m_line; break;
    case Part::Increase: pos += m_line; break;
    case Part::PageDecrease: pos -= m_view; break; // ScrollViewer: LargeChange = the viewport
    case Part::PageIncrease: pos += m_view; break;
    default: return;
    }
    pos = std::clamp(pos, 0, range);
    if (pos != m_pos)
        m_scroll(pos);
}

bool ScrollOverlay::OnMouseMove(POINT pt, bool)
{
    const ULONGLONG now = GetTickCount64();
    m_activeAt = now; // WinUI's mouse indicator: shown while the pointer moves over the content
    if (m_drag)
    {
        const RECT track = TrackBox();
        const int span = m_vertical ? track.bottom - track.top : track.right - track.left;
        const int avail = span - ThumbLength(), range = m_content - m_view;
        const int offset = (m_vertical ? pt.y - track.top : pt.x - track.left) - m_grab;
        if (avail > 0 && range > 0 && m_scroll)
        {
            const int pos = std::clamp(MulDiv(offset, range, avail), 0, range);
            if (pos != m_pos)
                m_scroll(pos);
        }
        Kick();
        return true;
    }
    if (m_press != Part::None)
    {
        m_pressAt = pt; // the track pages toward where the pointer is now
        Kick();
        return true;
    }
    const bool hover = Over(pt);
    if (hover != m_hover)
    {
        m_hover = hover;
        (hover ? m_hoverSince : m_leftAt) = now;
    }
    const Part hot = hover ? HitTest(pt) : Part::None;
    if (hot != m_hot)
    {
        m_hot = hot;
        Invalidate();
    }
    if (Needed())
        Kick();
    return hover;
}

bool ScrollOverlay::OnButtonDown(POINT pt)
{
    const Part part = HitTest(pt);
    if (part == Part::None)
        return false;
    const ULONGLONG now = GetTickCount64();
    m_activeAt = now;
    if (part == Part::Thumb)
    {
        const RECT box = ThumbBox();
        m_drag = true;
        m_grab = m_vertical ? pt.y - box.top : pt.x - box.left;
    }
    else
    {
        m_press = part;
        m_pressAt = pt;
        Step(part);
        m_nextRepeat = now + kRepeatDelay;
    }
    SetCapture(m_hwnd);
    Invalidate();
    Kick();
    return true;
}

bool ScrollOverlay::OnButtonUp()
{
    if (!Captured())
        return false;
    m_drag = false;
    m_press = Part::None;
    if (GetCapture() == m_hwnd)
        ReleaseCapture();
    if (!m_hover)
        m_leftAt = GetTickCount64();
    Invalidate();
    Kick();
    return true;
}

void ScrollOverlay::OnCaptureLost()
{
    if (!Captured())
        return;
    m_drag = false;
    m_press = Part::None;
    m_leftAt = GetTickCount64();
    Invalidate();
    Kick();
}

void ScrollOverlay::OnMouseLeave()
{
    if (Captured())
        return; // still dragging outside the window
    if (m_hover)
        m_leftAt = GetTickCount64();
    m_hover = false;
    if (m_hot != Part::None)
    {
        m_hot = Part::None;
        Invalidate();
    }
    Kick();
}

void ScrollOverlay::OnTimer(UINT_PTR id)
{
    if (id != m_timer)
        return;
    const ULONGLONG now = GetTickCount64();
    // A held arrow / track: RepeatButton's repeat; the track stops once the thumb reaches the pointer
    if (m_press != Part::None && now >= m_nextRepeat)
    {
        m_nextRepeat = now + kRepeatEvery;
        Part part = m_press;
        if (part == Part::PageDecrease || part == Part::PageIncrease)
        {
            const Part under = HitTest(m_pressAt);
            if (under != part)
                part = Part::None;
        }
        Step(part);
    }
    // Expanded (ConsciousStates): after resting 0.4 s on the strip, or at once while it's held; collapsed 0.5 s after
    const bool held = Captured();
    const bool expand = held || (m_hover && now - m_hoverSince >= kExpandDelay) ||
        (Expand() > 0 && !m_hover && now - m_leftAt < kContractDelay);
    // Shown (the mouse indicator): while the pointer moves / it's used / the content scrolls, hidden 2 s after
    const bool show = Needed() && (held || m_hover || expand || now - m_activeAt < kHideAfter);
    m_expand.Go(expand ? 1.0 : 0.0, kExpandMs, true);
    m_shown.Go(show ? 1.0 : 0.0, kFadeMs, false);
    // Compared with what was last painted, not with this tick's start: the frame painted mid-fade must be followed by
    // the settled one (else a half-faded thumb stays on screen)
    const double shownNow = Needed() ? Shown() : 0;
    const bool stale = IsWindowVisible(m_hwnd) && !IsRectEmpty(&m_strip) &&
        (shownNow != m_drawnShown || (shownNow > 0 && Expand() != m_drawnExpand));
    if (m_expand.Running() || m_shown.Running() || stale)
        Invalidate();
    // Keep ticking while it isn't where it will settle: held, animating, waiting to expand under the pointer, not yet
    // painted as it is, or (pointer gone) still showing / expanded; resting expanded under a still pointer, or fully
    // hidden, needs no frames
    const bool pending = held || m_expand.Running() || m_shown.Running() || stale || (m_hover && Expand() < 1.0) ||
        (!m_hover && (Expand() > 0 || Shown() > 0));
    if (!pending)
        KillTimer(m_hwnd, m_timer);
}

namespace
{
Gdiplus::Color Argb(COLORREF c, int alpha)
{
    return Gdiplus::Color(static_cast<BYTE>(std::clamp(alpha, 0, 255)), GetRValue(c), GetGValue(c), GetBValue(c));
}

void AddPill(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r)
{
    r = std::min({ r, w / 2, h / 2 });
    if (r <= 0)
    {
        path.AddRectangle(Gdiplus::RectF(x, y, w, h));
        return;
    }
    const float d = 2 * r;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}
}

void ScrollOverlay::Paint(HDC hdc, const RECT& clip) const
{
    const double shown = Shown(), expand = Expand();
    RECT hit{};
    if (!IntersectRect(&hit, &clip, &m_strip))
        return;
    m_drawnShown = Needed() ? shown : 0;
    m_drawnExpand = expand;
    if (!Needed() || shown <= 0.004)
        return;
    const Theme& t = *m_theme;
    const float k = t.S(100) / 100.0f; // DPI scale
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const Gdiplus::RectF strip(static_cast<float>(m_strip.left), static_cast<float>(m_strip.top), static_cast<float>(m_strip.right - m_strip.left),
        static_cast<float>(m_strip.bottom - m_strip.top));
    // The track (expanded only): AcrylicInAppFillColorDefault, ScrollBarCornerRadius 3 (x2 on the track rectangle)
    if (expand > 0)
    {
        Gdiplus::GraphicsPath track;
        AddPill(track, strip.X, strip.Y, strip.Width, strip.Height, 6 * k);
        Gdiplus::SolidBrush fill(Argb(t.scrollTrack, static_cast<int>(255 * expand * shown)));
        g.FillPath(&fill, &track);
    }
    // The thumb: 2 px collapsed (offset 2 toward the edge), 6 px expanded; round ends
    const RECT box = ThumbBox();
    const float inset = kInset * k;
    const float thick = static_cast<float>((kThin + (kWide - kThin) * expand) * k);
    const float edge = static_cast<float>(inset); // the fill's outer edge sits 3 inside the strip (the stroke)
    const int thumbAlpha = static_cast<int>(t.scrollThumbAlpha * shown);
    {
        Gdiplus::GraphicsPath thumb;
        if (m_vertical)
            AddPill(thumb, strip.X + strip.Width - edge - thick, static_cast<float>(box.top) + inset, thick,
                static_cast<float>(box.bottom - box.top) - 2 * inset, thick / 2);
        else
            AddPill(thumb, static_cast<float>(box.left) + inset, strip.Y + strip.Height - edge - thick,
                static_cast<float>(box.right - box.left) - 2 * inset, thick, thick / 2);
        Gdiplus::SolidBrush fill(Argb(t.scrollInk, thumbAlpha));
        g.FillPath(&fill, &thumb);
    }
    // The arrows (expanded only): the solid carets, 8 px (ScrollBarButtonArrowIconFontSize), 4 px in from the ends
    if (expand > 0)
        for (const bool increase : { false, true })
        {
            const RECT a = ArrowRect(increase);
            const Part part = increase ? Part::Increase : Part::Decrease;
            const bool hot = m_hot == part || m_press == part;
            const int alpha = static_cast<int>((hot ? t.scrollArrowHotAlpha : t.scrollThumbAlpha) * expand * shown);
            const float scale = m_press == part ? 0.875f : 1.0f; // ScrollBarButtonArrowScalePressed
            const float w = 6 * k * scale, h = 3.5f * k * scale;   // the caret's ink inside the 8 px glyph box
            float cx = (a.left + a.right) / 2.0f, cy = (a.top + a.bottom) / 2.0f;
            const float pad = 2 * k; // Padding 4 on the outer side: the glyph sits 2 toward the thumb
            if (m_vertical)
                cy += increase ? -pad / 2 : pad / 2;
            else
                cx += increase ? -pad / 2 : pad / 2;
            Gdiplus::PointF p[3];
            if (m_vertical)
            {
                const float dir = increase ? 1.0f : -1.0f; // tip down / up
                p[0] = { cx - w / 2, cy - dir * h / 2 };
                p[1] = { cx + w / 2, cy - dir * h / 2 };
                p[2] = { cx, cy + dir * h / 2 };
            }
            else
            {
                const float dir = increase ? 1.0f : -1.0f; // tip right / left
                p[0] = { cx - dir * h / 2, cy - w / 2 };
                p[1] = { cx - dir * h / 2, cy + w / 2 };
                p[2] = { cx + dir * h / 2, cy };
            }
            Gdiplus::SolidBrush fill(Argb(t.scrollInk, alpha));
            g.FillPolygon(&fill, p, 3);
        }
}

// --- NativeBars ---

void NativeBars::Attach(HWND control, Kind kind, const Theme* theme, UINT_PTR timerV, UINT_PTR timerH)
{
    if (!control)
        return;
    m_hwnd = control;
    m_kind = kind;
    m_theme = theme;
    m_timerV = timerV;
    m_timerH = timerH;
    m_v.Attach(control, true, timerV, theme, [this](int pos) { Scroll(true, pos); });
    m_h.Attach(control, false, timerH, theme, [this](int pos) { Scroll(false, pos); });
    SetWindowSubclass(control, Proc, 1, reinterpret_cast<DWORD_PTR>(this));
    Sync(true);
}

void NativeBars::Detach()
{
    if (!m_hwnd)
        return;
    RemoveWindowSubclass(m_hwnd, Proc, 1);
    KillTimer(m_hwnd, m_timerV);
    KillTimer(m_hwnd, m_timerH);
    m_hwnd = nullptr;
}

bool NativeBars::Info(int bar, SCROLLINFO& si) const
{
    // A bar the control hid (nothing to scroll that way) keeps stale numbers: only a shown bar counts
    const LONG_PTR style = GetWindowLongPtrW(m_hwnd, GWL_STYLE);
    if (!(style & (bar == SB_VERT ? WS_VSCROLL : WS_HSCROLL)))
        return false;
    si = { sizeof(si), SIF_ALL };
    return GetScrollInfo(m_hwnd, bar, &si) && si.nPage > 0 && si.nMax - si.nMin + 1 > static_cast<int>(si.nPage);
}

void NativeBars::Sync()
{
    Sync(true);
}

// The control's own scroll units: Edit lines down / pixels across, RichEdit pixels, ListView rows down / pixels across
void NativeBars::Sync(bool quiet)
{
    if (!m_hwnd || m_syncing)
        return;
    m_syncing = true;
    RECT client{};
    GetClientRect(m_hwnd, &client);
    SCROLLINFO v{}, h{};
    const bool hasV = Info(SB_VERT, v), hasH = Info(SB_HORZ, h);
    const int bar = m_theme->S(12);
    int top = 0; // a report list's header stays above the bar (the rows scroll under it)
    if (m_kind == Kind::List)
        if (HWND header = ListView_GetHeader(m_hwnd); header && IsWindowVisible(header))
        {
            RECT hr{};
            GetWindowRect(header, &hr);
            top = hr.bottom - hr.top;
        }
    const RECT stripV{ client.right - bar, top, client.right, client.bottom - (hasH ? bar : 0) };
    const RECT stripH{ 0, client.bottom - bar, client.right - (hasV ? bar : 0), client.bottom };
    const int posV = hasV ? v.nPos - v.nMin : 0, posH = hasH ? h.nPos - h.nMin : 0;
    const bool moved = posV != m_posV || posH != m_posH;
    const bool shown = m_v.Showing() || m_h.Showing();
    // An arrow steps a line: Edit / ListView count in lines / rows, RichEdit in pixels (about a line of body text)
    m_v.Set(hasV ? v.nMax - v.nMin + 1 : 0, hasV ? static_cast<int>(v.nPage) : 0, posV, stripV, m_kind == Kind::Rich ? m_theme->S(20) : 1, !quiet);
    m_h.Set(hasH ? h.nMax - h.nMin + 1 : 0, hasH ? static_cast<int>(h.nPage) : 0, posH, stripH, m_theme->S(24), !quiet);
    m_posV = posV;
    m_posH = posH;
    // The control scrolled its pixels (ScrollWindowEx) and the bars' with them, into the content: all of it again
    if (moved && shown)
        InvalidateRect(m_hwnd, nullptr, FALSE);
    m_syncing = false;
}

void NativeBars::Scroll(bool vertical, int pos)
{
    SCROLLINFO si{};
    if (!Info(vertical ? SB_VERT : SB_HORZ, si))
        return;
    const int target = si.nMin + pos;
    if (m_kind == Kind::List)
    {
        // LVM_SCROLL takes pixels (rows: a row's height each)
        int dy = 0, dx = 0;
        if (vertical)
        {
            RECT row{};
            ListView_GetItemRect(m_hwnd, 0, &row, LVIR_BOUNDS);
            dy = (target - si.nPos) * std::max<int>(1, row.bottom - row.top);
        }
        else
            dx = target - si.nPos;
        ListView_Scroll(m_hwnd, dx, dy);
    }
    else if (m_kind == Kind::Rich)
    {
        POINT p{};
        SendMessageW(m_hwnd, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&p));
        (vertical ? p.y : p.x) = target;
        SendMessageW(m_hwnd, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&p));
    }
    else
        SendMessageW(m_hwnd, vertical ? WM_VSCROLL : WM_HSCROLL, MAKEWPARAM(SB_THUMBPOSITION, std::clamp(target, 0, 0xFFFF)), 0);
    Sync(false);
}

LRESULT CALLBACK NativeBars::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref)
{
    auto* self = reinterpret_cast<NativeBars*>(ref);
    if (msg == WM_NCDESTROY)
    {
        RemoveWindowSubclass(hwnd, Proc, 1);
        self->m_hwnd = nullptr;
        return DefSubclassProc(hwnd, msg, wp, lp);
    }
    return self->Handle(msg, wp, lp);
}

LRESULT NativeBars::Handle(UINT msg, WPARAM wp, LPARAM lp)
{
    const HWND hwnd = m_hwnd;
    const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    switch (msg)
    {
    case WM_TIMER:
        if (wp == m_timerV || wp == m_timerH)
        {
            (wp == m_timerV ? m_v : m_h).OnTimer(wp);
            return 0;
        }
        break;
    case WM_PAINT:
    {
        // The control paints itself; the bars go over what it just painted (clipped to that, so a strip it left
        // alone isn't blended twice)
        HRGN update = CreateRectRgn(0, 0, 0, 0);
        const bool any = GetUpdateRgn(hwnd, update, FALSE) != NULLREGION;
        const LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        if (any && (m_v.Needed() || m_h.Needed()))
        {
            HDC dc = GetDC(hwnd);
            SelectClipRgn(dc, update);
            RECT box{};
            GetRgnBox(update, &box);
            m_v.Paint(dc, box);
            m_h.Paint(dc, box);
            ReleaseDC(hwnd, dc);
        }
        DeleteObject(update);
        return r;
    }
    case WM_PRINTCLIENT:
    {
        const LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        RECT client{};
        GetClientRect(hwnd, &client);
        m_v.Paint(reinterpret_cast<HDC>(wp), client);
        m_h.Paint(reinterpret_cast<HDC>(wp), client);
        return r;
    }
    case WM_MOUSEMOVE:
        if (GetCapture() != hwnd || m_v.Captured() || m_h.Captured()) // (not while the control drags a selection)
        {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            const bool held = m_v.Captured() || m_h.Captured();
            const bool onV = m_v.OnMouseMove(pt, (wp & MK_LBUTTON) != 0);
            const bool onH = !m_v.Captured() && m_h.OnMouseMove(pt, (wp & MK_LBUTTON) != 0);
            if (held || onV || onH)
                return 0;
        }
        break;
    case WM_MOUSELEAVE:
        m_v.OnMouseLeave();
        m_h.OnMouseLeave();
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        if (m_v.Over(pt) || m_h.Over(pt))
        {
            if (msg != WM_RBUTTONDOWN && msg != WM_RBUTTONUP && !m_v.OnButtonDown(pt))
                m_h.OnButtonDown(pt);
            return 0; // the strip: no caret, no selection, no menu
        }
        break;
    case WM_LBUTTONUP:
        if (m_v.OnButtonUp() || m_h.OnButtonUp())
            return 0;
        break;
    case WM_CAPTURECHANGED:
        m_v.OnCaptureLost();
        m_h.OnCaptureLost();
        break;
    case WM_CONTEXTMENU:
        if (POINT c{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; c.x != -1 && ScreenToClient(hwnd, &c) && (m_v.Over(c) || m_h.Over(c)))
            return 0;
        break;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT)
        {
            POINT c{};
            GetCursorPos(&c);
            ScreenToClient(hwnd, &c);
            if (m_v.Over(c) || m_h.Over(c) || m_v.Captured() || m_h.Captured())
            {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
        }
        break;
    }
    const LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
    if (!m_hwnd)
        return r;
    switch (msg)
    {
    // Nothing scrolls on these (and some come often)
    case WM_PAINT: case WM_NCPAINT: case WM_ERASEBKGND: case WM_NCHITTEST: case WM_SETCURSOR: case WM_GETTEXT:
    case WM_GETTEXTLENGTH: case WM_GETDLGCODE: case WM_GETOBJECT: case WM_NOTIFY: case WM_CTLCOLOREDIT: case WM_CTLCOLORSTATIC:
    case WM_NCCALCSIZE: case WM_GETFONT: case WM_PRINTCLIENT: case WM_PRINT:
    case EM_GETSCROLLPOS: case EM_GETLINECOUNT: case EM_GETFIRSTVISIBLELINE: case LVM_GETITEMRECT: case LVM_GETITEMCOUNT:
        break;
    // New content / size: the bars follow without the indicator flashing
    case WM_SETTEXT: case EM_STREAMIN: case EM_SETTEXTEX: case EM_SETTARGETDEVICE: case WM_SETFONT: case WM_SIZE:
    case WM_WINDOWPOSCHANGED: case LVM_SETITEMCOUNT: case LVM_DELETEALLITEMS: case WM_SHOWWINDOW:
        Sync(true);
        break;
    default:
        Sync(false);
        // Drawn outside WM_PAINT (a selection drag, the caret's line): the bars go back over it
        if ((msg == WM_MOUSEMOVE && GetCapture() == hwnd) || msg == WM_KEYDOWN || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)
        {
            m_v.Refresh();
            m_h.Refresh();
        }
        break;
    }
    return r;
}
