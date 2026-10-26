#include "Ui.h"
#include <algorithm>
#include <cmath>

void Ui::Paint(HDC target)
{
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0)
        return;
    // Top-down 32-bit frame: its alpha is set to opaque at the end (DWM reads it where the frame is extended)
    BITMAPINFO bi{ { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB } };
    void* bits = nullptr;
    HDC hdc = CreateCompatibleDC(target);
    HBITMAP frame = CreateDIBSection(target, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldFrame = SelectObject(hdc, frame);

    const double t = m_animations ? std::clamp((GetTickCount64() - m_pageStart) / static_cast<double>(kSlideMs), 0.0, 1.0) : 1.0;
    const int top = S(kTitle);
    if (m_old && t < 1.0)
    {
        // Page transition: the old page slides left and fades, the new one slides in from the right
        const double e = EaseOut(t);
        const int shift = static_cast<int>(std::lround(S(28) * e)), enter = static_cast<int>(std::lround(S(28) * (1 - e)));
        FillRect(hdc, { 0, 0, w, h - S(kFooter) }, m_p.content);
        FillRect(hdc, { 0, h - S(kFooter), w, h }, m_p.footer);
        HDC src = CreateCompatibleDC(target);
        HGDIOBJ oldSrc = SelectObject(src, m_old);
        const BLENDFUNCTION fadeOut{ AC_SRC_OVER, 0, static_cast<BYTE>(std::lround(255 * (1 - e))), 0 };
        AlphaBlend(hdc, -shift, top, w, h - top, src, 0, top, w, h - top, fadeOut);
        // The new page into a scratch bitmap, then blended in
        HBITMAP fresh = CreateCompatibleBitmap(target, w, h);
        SelectObject(src, fresh);
        PaintPage(src, m_page);
        const BLENDFUNCTION fadeIn{ AC_SRC_OVER, 0, static_cast<BYTE>(std::lround(255 * e)), 0 };
        AlphaBlend(hdc, enter, top, w - enter, h - top, src, 0, top, w - enter, h - top, fadeIn);
        SelectObject(src, oldSrc);
        DeleteDC(src);
        DeleteObject(fresh);
    }
    else
    {
        if (m_old)
        {
            DeleteObject(m_old);
            m_old = nullptr;
        }
        PaintPage(hdc, m_page);
    }
    PaintTitle(hdc);

    GdiFlush();
    auto* px = static_cast<DWORD*>(bits);
    for (int i = 0, n = w * h; i < n; ++i)
        px[i] |= 0xFF000000;
    BitBlt(target, 0, 0, w, h, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, oldFrame);
    DeleteObject(frame);
    DeleteDC(hdc);
}

// The current page as a bitmap, for the transition to the next one
void Ui::Snapshot()
{
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    if (m_old)
        DeleteObject(m_old);
    HDC screen = GetDC(m_hwnd);
    m_old = CreateCompatibleBitmap(screen, rc.right, rc.bottom);
    HDC hdc = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(hdc, m_old);
    PaintPage(hdc, m_page);
    SelectObject(hdc, old);
    DeleteDC(hdc);
    ReleaseDC(m_hwnd, screen);
}

void Ui::PaintTitle(HDC hdc)
{
    const int w = S(kW), th = S(kTitle);
    FillRect(hdc, { 0, 0, w, th }, m_p.title);
    const int icon = S(16);
    DrawAppIcon(hdc, S(12), (th - icon) / 2, icon);
    const bool active = GetActiveWindow() == m_hwnd;
    Text(hdc, m_f.caption, m_o.uninstall ? L"卸载 FastFiles" : L"FastFiles 安装", { S(12) + icon + S(10), 0, w - S(100), th },
        active ? m_p.ink : m_p.muted);
    // Caption buttons (46 wide like the system's): subtle fill on hover, red for close
    for (int ctl : { CtlMin, CtlClose })
    {
        const RECT& r = m_rc[ctl];
        const double hv = m_hover[ctl].value;
        const bool pressed = m_pressed == ctl;
        if (hv > 0.01 && Enabled(ctl))
        {
            const COLORREF fill = ctl == CtlClose ? m_p.closeHover : Mix(m_p.title, m_p.ink, pressed ? 0.12 : 0.07);
            FillRect(hdc, r, Mix(m_p.title, fill, hv));
        }
        COLORREF glyph = Enabled(ctl) ? (active ? m_p.ink : m_p.muted) : m_p.disabled;
        if (ctl == CtlClose && Enabled(ctl))
            glyph = Mix(glyph, m_p.highlight, hv);
        Text(hdc, m_f.icons, ctl == CtlMin ? L"\xE921" : L"\xE8BB", r, glyph, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}
std::wstring Ui::PrimaryText() const
{
    switch (m_page)
    {
    case Page::Ready: return m_o.uninstall ? L"卸载" : !m_previous.empty() ? L"更新" : L"安装";
    case Page::Done: return L"完成";
    default: return L"关闭";
    }
}

void Ui::PaintPage(HDC hdc, Page page)
{
    const int w = S(kW), h = S(kH), foot = h - S(kFooter), left = S(24), right = w - S(24);
    FillRect(hdc, { 0, 0, w, foot }, m_p.content);
    FillRect(hdc, { 0, foot, w, h }, m_p.footer);
    FillRect(hdc, { 0, foot, w, foot + std::max(1, S(1)) }, m_p.divider);
    const std::wstring version = L"版本 " + std::wstring(kVersion);
    auto heading = [&](const std::wstring& title, const std::wstring& sub)
    {
        Text(hdc, m_f.title, title, { left + S(76), S(64), right, S(100) }, m_p.ink);
        Text(hdc, m_f.body, sub, { left + S(76), S(100), right, S(124) }, m_p.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    };
    switch (page)
    {
    case Page::Splash:
    {
        // Launch animation: the icon scales in with a slight overshoot, the name fades in just after
        const double age = m_animations ? (GetTickCount64() - m_pageStart) / 1.0 : 1e6;
        const double grow = EaseOutBack(age / 420.0);
        const double nameT = EaseOut((age - 160) / 420.0);
        const int baseSize = S(72);
        // Rounded to 2px: the growing icon only touches a handful of cached HICON sizes, not one per frame
        const int size = 2 * static_cast<int>(std::lround(baseSize * std::clamp(grow, 0.0, 1.15) / 2.0));
        const int cx = w / 2, cy = foot / 2 - S(18);
        if (size > 1)
            DrawAppIcon(hdc, cx - size / 2, cy - size / 2, size);
        if (nameT > 0.01)
        {
            const COLORREF ink = Mix(m_p.content, m_p.ink, std::clamp(nameT, 0.0, 1.0));
            Text(hdc, m_f.title, kAppName, { 0, cy + baseSize / 2 + S(16), w, cy + baseSize / 2 + S(56) }, ink,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        break;
    }
    case Page::Ready:
        DrawAppIcon(hdc, left, S(60), S(64));
        if (m_o.uninstall)
        {
            heading(L"卸载 FastFiles", version);
            Text(hdc, m_f.body, L"将从这台电脑上删除 FastFiles 及其快捷方式。你的设置和固定的文件夹会保留。", { left, S(160), right, S(220) },
                m_p.ink, DT_LEFT | DT_WORDBREAK);
            PaintCheck(hdc, L"同时删除我的设置和数据", m_o.deleteData);
        }
        else
        {
            heading(!m_previous.empty() ? L"更新 FastFiles" : L"安装 FastFiles",
                !m_previous.empty() && m_previous != kVersion ? L"版本 " + m_previous + L" → " + kVersion : version);
            Text(hdc, m_f.caption, L"安装位置", { left, S(162), right, S(182) }, m_p.muted);
            // Read-only path box (TextBox look) and the 更改 button
            const RECT box{ left, S(188), m_rc[CtlBrowse].left - S(8), S(220) };
            FillRound(hdc, box, S(4), m_p.control);
            StrokeRound(hdc, box, S(4), m_p.stroke);
            Text(hdc, m_f.body, m_o.dir, { box.left + S(11), box.top, box.right - S(11), box.bottom }, m_p.ink,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
            PaintButton(hdc, CtlBrowse, L"更改…", false);
            PaintCheck(hdc, L"创建桌面快捷方式", m_o.desktopIcon);
        }
        PaintButton(hdc, CtlPrimary, PrimaryText(), true);
        PaintButton(hdc, CtlSecondary, L"取消", false);
        break;
    case Page::Working:
    {
        DrawAppIcon(hdc, left, S(60), S(64));
        // Indeterminate ring around the icon (WinUI ProgressRing-ish): a short arc sweeps around it while busy
        if (m_animations)
            PaintRing(hdc, left + S(32), S(60) + S(32), S(40));
        heading(m_o.uninstall ? L"正在卸载" : !m_previous.empty() ? L"正在更新" : L"正在安装", version);
        // Status text cross-fades between steps instead of popping
        const double statusAge = m_animations ? (GetTickCount64() - m_statusStart) / 220.0 : 1e6;
        const double statusT = EaseOut(statusAge);
        const RECT statusRc{ left, S(176), right - S(60), S(200) };
        if (statusT < 1.0 && !m_statusFrom.empty())
        {
            // GDI text has no alpha: faded toward the page background instead, same trick as a hover fill mix
            const double t = std::clamp(statusT, 0.0, 1.0);
            Text(hdc, m_f.body, m_statusFrom, statusRc, Mix(m_p.muted, m_p.content, t));
            Text(hdc, m_f.body, m_status, statusRc, Mix(m_p.content, m_p.muted, t));
        }
        else
            Text(hdc, m_f.body, m_status, statusRc, m_p.muted);
        Text(hdc, m_f.body, std::to_wstring(static_cast<int>(std::lround(m_shown))) + L"%", { right - S(60), S(176), right, S(200) },
            m_p.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        // ProgressBar: 4px rounded track, accent fill, a soft highlight sweeping across while working
        const RECT track{ left, S(210), right, S(214) };
        FillRound(hdc, { track.left, track.top + S(1), track.right, track.bottom - S(1) }, S(1), m_p.track);
        const int fill = track.left + static_cast<int>((track.right - track.left) * std::clamp(m_shown, 0.0, 100.0) / 100.0);
        if (fill > track.left + S(2))
        {
            FillRound(hdc, { track.left, track.top, fill, track.bottom }, S(2), m_p.accent);
            if (m_animations)
            {
                const double phase = std::fmod(GetTickCount64() / 1100.0, 1.0);
                const int sw = S(80), x = track.left - sw + static_cast<int>((fill - track.left + sw) * phase);
                const RECT sheen{ std::max<int>(x, track.left), track.top, std::min(x + sw, fill), track.bottom };
                if (sheen.right > sheen.left)
                    FillRound(hdc, sheen, S(2), m_p.highlight, m_p.dark ? 60 : 90);
            }
        }
        break;
    }
    case Page::Done:
        PaintBadge(hdc, left + S(32), S(92), true);
        heading(m_o.uninstall ? L"卸载完成" : !m_previous.empty() ? L"更新完成" : L"安装完成",
            m_o.uninstall ? L"FastFiles 已从这台电脑上删除" : L"FastFiles " + std::wstring(kVersion) + L" 已可以使用");
        if (!m_o.uninstall)
            PaintCheck(hdc, L"启动 FastFiles", m_o.launch);
        PaintButton(hdc, CtlPrimary, PrimaryText(), true);
        break;
    case Page::Failed:
        PaintBadge(hdc, left + S(32), S(92), false);
        heading(m_o.uninstall ? L"卸载失败" : L"安装失败", version);
        Text(hdc, m_f.body, m_error, { left, S(160), right, foot - S(16) }, m_p.ink, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
        PaintButton(hdc, CtlPrimary, PrimaryText(), true);
        break;
    }
}

void Ui::PaintBadge(HDC hdc, int cx, int cy, bool ok)
{
    // The circle pops in, then the mark draws itself
    const double age = m_animations ? (GetTickCount64() - m_pageStart) / 1.0 : 1e6;
    const double grow = EaseOutBack((age - 120) / 320.0), draw = EaseOut((age - 360) / 360.0);
    const double radius = S(28) * std::max(0.0, grow);
    const COLORREF color = ok ? m_p.success : m_p.error;
    if (radius > 0.5)
        FillCircle(hdc, cx, cy, radius, color);
    const COLORREF mark = m_p.onAccent;
    const double k = S(1);
    if (ok)
    {
        const double xy[] = { cx - 11 * k, cy + 1 * k, cx - 3.5 * k, cy + 8.5 * k, cx + 12 * k, cy - 8 * k };
        StrokePath(hdc, xy, 3, draw, mark, 3.2 * k);
    }
    else
    {
        const double a[] = { cx - 8 * k, cy - 8 * k, cx + 8 * k, cy + 8 * k };
        const double b[] = { cx + 8 * k, cy - 8 * k, cx - 8 * k, cy + 8 * k };
        StrokePath(hdc, a, 2, draw * 2, mark, 3.2 * k);
        StrokePath(hdc, b, 2, draw * 2 - 1, mark, 3.2 * k);
    }
}

void Ui::PaintRing(HDC hdc, int cx, int cy, double radius)
{
    // WinUI ProgressRing, indeterminate: a short arc continuously sweeps around a faint full track
    const double width = S(3);
    StrokeArc(hdc, cx, cy, radius, 0, 360, m_p.track, width, m_p.dark ? 90 : 110);
    const double period = 1600.0, phase = std::fmod(static_cast<double>(GetTickCount64()), period) / period;
    const double start = phase * 360.0, sweep = 32 + 46 * (0.5 - 0.5 * std::cos(phase * 2 * 3.14159265358979));
    StrokeArc(hdc, cx, cy, radius, start, sweep, m_p.accent, width);
}

void Ui::PaintButton(HDC hdc, int ctl, const std::wstring& text, bool accent)
{
    const RECT r = m_rc[ctl];
    if (IsRectEmpty(&r))
        return;
    const double hv = m_hover[ctl].value;
    const bool pressed = m_pressed == ctl && m_hot == ctl;
    COLORREF fill, ink;
    if (accent)
    {
        fill = pressed ? m_p.accentPressed : Mix(m_p.accent, m_p.accentHover, hv);
        ink = pressed ? Mix(m_p.onAccent, fill, 0.2) : m_p.onAccent;
    }
    else
    {
        fill = pressed ? m_p.controlPressed : Mix(m_p.control, m_p.controlHover, hv);
        ink = pressed ? m_p.muted : m_p.ink;
    }
    FillRound(hdc, r, S(4), fill);
    if (!accent)
        StrokeRound(hdc, r, S(4), m_p.stroke);
    else
        StrokeRound(hdc, r, S(4), m_p.highlight, 1.0, m_p.dark ? 20 : 36); // ControlElevationBorder highlight
    Text(hdc, m_f.button, text, r, ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (m_focus == ctl && m_keyboard)
        StrokeRound(hdc, { r.left - S(3), r.top - S(3), r.right + S(3), r.bottom + S(3) }, S(7), m_p.ink, 2.0 * m_dpi / 96);
}

void Ui::PaintCheck(HDC hdc, const std::wstring& text, bool on)
{
    const RECT& r = m_rc[CtlCheck];
    if (IsRectEmpty(&r))
        return;
    const int box = S(20), y = (r.top + r.bottom - box) / 2;
    const RECT b{ r.left, y, r.left + box, y + box };
    const double fill = m_checkFill.value, hv = m_hover[CtlCheck].value;
    // Unchecked: stroked box; checked: accent fill with a check that draws in
    FillRound(hdc, b, S(4), Mix(m_p.control, m_p.controlHover, hv));
    StrokeRound(hdc, b, S(4), m_p.strokeStrong, 1.0, static_cast<int>(255 * (1 - fill)));
    if (fill > 0.01)
    {
        FillRound(hdc, b, S(4), Mix(m_p.accent, m_p.accentHover, hv), static_cast<int>(255 * fill));
        const double k = S(1), cx = b.left + box / 2.0, cy = b.top + box / 2.0;
        const double xy[] = { cx - 5 * k, cy + 0.5 * k, cx - 1.5 * k, cy + 4 * k, cx + 5.5 * k, cy - 4 * k };
        StrokePath(hdc, xy, 3, on ? fill : 0, m_p.onAccent, 1.6 * k);
    }
    Text(hdc, m_f.body, text, { b.right + S(8), r.top, r.right, r.bottom }, m_p.ink);
    if (m_focus == CtlCheck && m_keyboard)
        StrokeRound(hdc, { b.left - S(3), b.top - S(3), b.right + S(3), b.bottom + S(3) }, S(7), m_p.ink, 2.0 * m_dpi / 96);
}
