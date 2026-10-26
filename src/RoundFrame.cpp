#include "RoundFrame.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace
{
constexpr wchar_t kFrameClass[] = L"FastFiles.RoundFrame";
constexpr wchar_t kCapClass[] = L"FastFiles.RoundCap"; // the anti-aliasing patch over the corners
constexpr DWORD kCornerPreference = 33; // DWMWA_WINDOW_CORNER_PREFERENCE (Windows 11)
constexpr int kRound = 2;               // DWMWCP_ROUND

// Signed distance from (x, y) to the rounded rectangle [l, r) x [t, b) with corner radius `radius` (> 0 outside)
double Distance(double x, double y, double l, double t, double r, double b, double radius)
{
    const double qx = std::max(l + radius - x, x - (r - radius)), qy = std::max(t + radius - y, y - (b - radius));
    const double ax = std::max(qx, 0.0), ay = std::max(qy, 0.0);
    return std::sqrt(ax * ax + ay * ay) + std::min(std::max(qx, qy), 0.0) - radius;
}

// Premultiplied BGRA for one pixel at coverage `cv` (0..1, how much of the ideal curve covers it) of `color`
DWORD Premultiplied(COLORREF color, double cv)
{
    const int alpha = static_cast<int>(std::clamp(cv, 0.0, 1.0) * 255.0 + 0.5);
    const auto c = [&](int v) { return static_cast<DWORD>((v * alpha + 127) / 255); };
    return static_cast<DWORD>(alpha) << 24 | c(GetRValue(color)) << 16 | c(GetGValue(color)) << 8 | c(GetBValue(color));
}
}

bool RoundFrame::Windows11()
{
    static const bool is = []
    {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        OSVERSIONINFOW info{ sizeof(info) };
        const auto get = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        return get && get(&info) == 0 && info.dwMajorVersion >= 10 && info.dwBuildNumber >= 22000;
    }();
    return is;
}

RoundFrame::~RoundFrame()
{
    Detach();
}

void RoundFrame::Attach(HWND window, bool resizable)
{
    m_window = window;
    m_resizable = resizable;
    if (Windows11())
    {
        DwmSetWindowAttribute(window, kCornerPreference, &kRound, sizeof(kRound));
        return;
    }
    Update();
}

void RoundFrame::Detach()
{
    if (m_frame)
    {
        SetWindowLongPtrW(m_frame, GWLP_USERDATA, 0);
        DestroyWindow(m_frame);
    }
    if (m_cap)
        DestroyWindow(m_cap);
    if (m_anchor)
        DestroyWindow(m_anchor);
    if (m_capRegion)
        DeleteObject(m_capRegion);
    m_frame = m_anchor = m_cap = nullptr;
    m_capRegion = nullptr;
    m_window = nullptr;
}

void RoundFrame::SetLook(const Look& look)
{
    m_look = look;
    ++m_lookVersion;
    Update();
}

void RoundFrame::SetDpi(UINT dpi)
{
    if (dpi == m_dpi || dpi == 0)
        return;
    m_dpi = dpi;
    ++m_lookVersion;
    m_regionSize = { -1, -1 };
    Update();
}

void RoundFrame::SetActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    Update();
}

void RoundFrame::Suppress(bool on)
{
    m_suppressed = on;
    m_regionSize = { -1, -1 };
    Update();
}

bool RoundFrame::Rounded() const
{
    return Custom() && !m_suppressed && !IsZoomed(m_window) && !IsIconic(m_window);
}

void RoundFrame::Update()
{
    if (!Custom() || m_updating)
        return;
    m_updating = true; // (SetWindowRgn / SetWindowPos send WM_WINDOWPOSCHANGED back here)
    RECT rc{};
    GetWindowRect(m_window, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    // The region: rounded unless maximized / full screen (a minimized window keeps the one it had)
    if (!IsIconic(m_window))
    {
        const bool round = !m_suppressed && !IsZoomed(m_window);
        if (round != m_regionRounded || (round && (w != m_regionSize.cx || h != m_regionSize.cy)))
        {
            m_regionRounded = round;
            m_regionSize = { w, h };
            const int d = 2 * Radius();
            SetWindowRgn(m_window, round ? CreateRoundRectRgn(0, 0, w + 1, h + 1, d, d) : nullptr, TRUE);
            // SetWindowRgn takes ownership of that one; keep our own copy to test pixels against (PtInRegion),
            // so the cap knows exactly which ones the hard region actually kept
            if (m_capRegion)
            {
                DeleteObject(m_capRegion);
                m_capRegion = nullptr;
            }
            if (round)
                m_capRegion = CreateRoundRectRgn(0, 0, w + 1, h + 1, d, d);
        }
    }
    if (!Rounded() || !IsWindowVisible(m_window) || w <= 0 || h <= 0)
    {
        if (m_frame && IsWindowVisible(m_frame))
            ShowWindow(m_frame, SW_HIDE);
        if (m_cap && IsWindowVisible(m_cap))
            ShowWindow(m_cap, SW_HIDE);
        m_updating = false;
        return;
    }
    if (!m_frame)
    {
        static const bool registered = []
        {
            WNDCLASSEXW wc{ sizeof(wc) };
            wc.style = CS_DBLCLKS;
            wc.lpfnWndProc = FrameProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = kFrameClass;
            return RegisterClassExW(&wc) != 0;
        }();
        if (registered)
        {
            m_anchor = CreateWindowExW(WS_EX_TOOLWINDOW, kFrameClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            const DWORD ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | (m_resizable ? 0 : WS_EX_TRANSPARENT);
            m_frame = CreateWindowExW(ex, kFrameClass, L"", WS_POPUP, 0, 0, 0, 0, m_anchor, nullptr, GetModuleHandleW(nullptr), nullptr);
            // Owned directly by the window: Windows always keeps an owned window immediately above its owner, so
            // this stays over the jagged region edge without any manual z-order juggling. Click-through always
            // (it never takes the mouse; m_frame behind still does the resize hit-testing).
            // Its own class: nothing for FrameProc to do (no hit testing), and the frame window stays the only one
            // of the frame class
            static const bool capRegistered = []
            {
                WNDCLASSEXW wc{ sizeof(wc) };
                wc.lpfnWndProc = DefWindowProcW;
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.lpszClassName = kCapClass;
                return RegisterClassExW(&wc) != 0;
            }();
            const DWORD capEx = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
            if (capRegistered)
                m_cap = CreateWindowExW(capEx, kCapClass, L"", WS_POPUP, 0, 0, 0, 0, m_window, nullptr, GetModuleHandleW(nullptr), nullptr);
        }
        if (!m_frame)
        {
            m_updating = false;
            return;
        }
        SetWindowLongPtrW(m_frame, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }
    if (m_cap)
    {
        if (w != m_capPainted.cx || h != m_capPainted.cy || m_active != m_capPaintedActive || m_lookVersion != m_capPaintedVersion)
            PaintCap(rc);
        SetWindowPos(m_cap, nullptr, rc.left, rc.top, w, h, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    }
    const int m = Margin();
    const POINT at{ rc.left - m, rc.top - m };
    const SIZE size{ w + 2 * m, h + 2 * m };
    if (size.cx != m_painted.cx || size.cy != m_painted.cy || m_active != m_paintedActive || m_lookVersion != m_paintedVersion)
        Paint(at, size);
    // m_frame isn't owned by m_window (it has to sit behind it, and an owned window is always kept above its
    // owner), so inserting it right after m_window in z-order only stays put within the same topmost/normal band.
    // Across bands SetWindowPos instead knocks hWndInsertAfter (m_window) out of the topmost band to make room.
    // Keep m_frame (and the hidden m_anchor that owns it) in whichever band m_window is actually in before placing
    // it there, so the compact overlay's HWND_TOPMOST isn't undone the next time this runs.
    const bool topmost = (GetWindowLongPtrW(m_window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (topmost != m_frameTopmost)
    {
        m_frameTopmost = topmost;
        const HWND band = topmost ? HWND_TOPMOST : HWND_NOTOPMOST;
        SetWindowPos(m_anchor, band, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
        SetWindowPos(m_frame, band, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    }
    // Just behind the window (the cap stays above it automatically: it's owned by the window, not by m_anchor)
    SetWindowPos(m_frame, m_window, at.x, at.y, size.cx, size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    m_updating = false;
}

// The ring just outside the rounded edge (anti-aliased: it also hides the region's stepped corners) over a shadow
// fading out across the margin; premultiplied BGRA for UpdateLayeredWindow
void RoundFrame::Paint(POINT at, SIZE size)
{
    const int fw = size.cx, fh = size.cy;
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), fw, -fh, 1, 32, BI_RGB };
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits)
    {
        HGDIOBJ old = SelectObject(mem, bmp);
        auto* px = static_cast<DWORD*>(bits);
        const int m = Margin(), r = Radius();
        const double l = m, t = m, rgt = fw - m, btm = fh - m;
        const COLORREF ring = m_active ? m_look.border : m_look.borderInactive;
        const double ringA = (m_active ? m_look.borderAlpha : m_look.borderInactiveAlpha) / 255.0;
        const double shadowA = (m_active ? m_look.shadowAlpha : m_look.shadowInactiveAlpha) / 255.0;
        for (int y = 0; y < fh; ++y)
            for (int x = 0; x < fw; ++x)
            {
                DWORD& p = px[static_cast<size_t>(y) * fw + x];
                // Well inside the edge: the window covers it
                const bool insideX = x >= m + 1 && x < fw - m - 1, insideY = y >= m + 1 && y < fh - m - 1;
                if ((insideX && y >= m + r && y < fh - m - r) || (insideY && x >= m + r && x < fw - m - r))
                {
                    p = 0;
                    continue;
                }
                const double d = Distance(x + 0.5, y + 0.5, l, t, rgt, btm, r);
                if (d <= -0.5)
                {
                    p = 0;
                    continue;
                }
                const double cover = std::clamp(1.0 - std::abs(d - 0.5), 0.0, 1.0) * ringA;
                const double fade = d > 0 ? std::clamp(1.0 - d / m, 0.0, 1.0) : 0.0;
                const double a = cover + shadowA * fade * fade * (1.0 - cover);
                int alpha = static_cast<int>(a * 255.0 + 0.5);
                if (m_resizable && d > 0)
                    alpha = std::max(alpha, 1); // the resize band takes the mouse everywhere
                const auto c = [&](int v) { return static_cast<DWORD>(std::min(alpha, static_cast<int>(v * cover + 0.5))); };
                p = static_cast<DWORD>(alpha) << 24 | c(GetRValue(ring)) << 16 | c(GetGValue(ring)) << 8 | c(GetBValue(ring));
            }
        POINT zero{ 0, 0 };
        BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(m_frame, screen, &at, &size, mem, &zero, 0, &blend, ULW_ALPHA);
        SelectObject(mem, old);
        m_painted = size;
        m_paintedActive = m_active;
        m_paintedVersion = m_lookVersion;
    }
    if (bmp)
        DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

// SetWindowRgn's rounded-rect region is a hard 1-bit clip: at the diagonal it either keeps a pixel the ideal
// curve only partly covers (showing the window's flat corner color squarely, past the curve) or drops one the
// curve partly covers (showing whatever's behind, short of the curve). This window sits directly above the main
// one (owned by it) and repaints only those mismatched pixels along the curve, blending the two looks by how
// much the ideal curve actually covers each one, so the stair steps disappear. Everywhere else: fully transparent.
void RoundFrame::PaintCap(const RECT& rc)
{
    const int fw = rc.right - rc.left, fh = rc.bottom - rc.top;
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), fw, -fh, 1, 32, BI_RGB };
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits)
    {
        HGDIOBJ old = SelectObject(mem, bmp);
        auto* px = static_cast<DWORD*>(bits);
        const int m = Margin(), r = Radius();
        const COLORREF ring = m_active ? m_look.border : m_look.borderInactive;
        const double ringA = (m_active ? m_look.borderAlpha : m_look.borderInactiveAlpha) / 255.0;
        const double shadowA = (m_active ? m_look.shadowAlpha : m_look.shadowInactiveAlpha) / 255.0;
        // Only the four corner boxes can differ from fully transparent; everything else is zeroed up front
        ZeroMemory(bits, static_cast<size_t>(fw) * fh * 4);
        const int box = std::min({ r + 2, fw / 2, fh / 2 });
        for (int cy = 0; cy < 2; ++cy)
            for (int cx = 0; cx < 2; ++cx)
            {
                const int x0 = cx ? fw - box : 0, y0 = cy ? fh - box : 0;
                const COLORREF fill = cy ? m_look.fillBottom : m_look.fillTop;
                for (int y = y0; y < y0 + box; ++y)
                    for (int x = x0; x < x0 + box; ++x)
                    {
                        // Ideal coverage (1 = fully the window's color, 0 = fully outside it), 1px anti-alias band
                        const double d = Distance(x + 0.5, y + 0.5, 0, 0, fw, fh, r);
                        const double cv = std::clamp(0.5 - d, 0.0, 1.0);
                        DWORD& p = px[static_cast<size_t>(y) * fw + x];
                        if (cv >= 1.0 - 1.0 / 255.0)
                            continue; // the region already keeps this pixel fully: nothing to fix
                        const bool kept = m_capRegion && PtInRegion(m_capRegion, x, y);
                        if (kept)
                        {
                            // The region wrongly kept a pixel the curve doesn't fully cover: paint the same
                            // ring-over-shadow look m_frame draws behind the edge over it (identical formula, so
                            // the two windows agree exactly along the seam), at how much it should NOT be the
                            // window's color. ringCover's own color channels are already scaled by ringCover
                            // (how saturated the ring color itself is at this pixel); mix is the overlay's alpha.
                            const double ringCover = std::clamp(1.0 - std::abs(d - 0.5), 0.0, 1.0) * ringA;
                            const double fade = d > 0 ? std::clamp(1.0 - d / m, 0.0, 1.0) : 0.0;
                            const double outsideA = ringCover + shadowA * fade * fade * (1.0 - ringCover);
                            const double mix = (1.0 - cv) * outsideA;
                            const int alpha = static_cast<int>(mix * 255.0 + 0.5);
                            const auto c = [&](int v) { return static_cast<DWORD>(std::min(static_cast<double>(alpha), v * ringCover) + 0.5); };
                            p = static_cast<DWORD>(alpha) << 24 | c(GetRValue(ring)) << 16 | c(GetGValue(ring)) << 8 | c(GetBValue(ring));
                        }
                        else if (cv > 1.0 / 255.0)
                        {
                            // The region wrongly dropped a pixel the curve does partly cover: fill it back in with
                            // the window's own flat corner color, at how much it should be that color
                            p = Premultiplied(fill, cv);
                        }
                    }
            }
        POINT zero{ 0, 0 };
        SIZE size{ fw, fh };
        POINT at{ rc.left, rc.top };
        BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(m_cap, screen, &at, &size, mem, &zero, 0, &blend, ULW_ALPHA);
        SelectObject(mem, old);
        m_capPainted = { fw, fh };
        m_capPaintedActive = m_active;
        m_capPaintedVersion = m_lookVersion;
    }
    if (bmp)
        DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK RoundFrame::FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* self = reinterpret_cast<RoundFrame*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg)
    {
    case WM_NCHITTEST:
    {
        if (!self || !self->m_resizable || !self->m_window || !IsWindowEnabled(self->m_window))
            return HTTRANSPARENT;
        // Which edge of the window (corners: the radius plus the band)
        RECT rc{};
        GetWindowRect(self->m_window, &rc);
        const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        const int corner = self->Radius() + self->Band();
        const bool left = pt.x < rc.left + corner, right = pt.x >= rc.right - corner;
        if (pt.y < rc.top + corner && (pt.y < rc.top || left || right))
            return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
        if (pt.y >= rc.bottom - corner && (pt.y >= rc.bottom || left || right))
            return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
        return pt.x < rc.left ? HTLEFT : pt.x >= rc.right ? HTRIGHT : HTNOWHERE;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCLBUTTONDOWN: case WM_NCLBUTTONDBLCLK:
        // The window's own frame handling: sizing from that edge (double click: the edge's snap)
        if (self && self->m_window && wp >= HTLEFT && wp <= HTBOTTOMRIGHT)
        {
            SetForegroundWindow(self->m_window);
            SendMessageW(self->m_window, msg, wp, lp);
        }
        return 0;
    case WM_NCRBUTTONDOWN: case WM_NCRBUTTONUP:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
