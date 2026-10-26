#include "Draw.h"
#include "Setup.h"
#include "DWriteText.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <objidl.h>
#include <gdiplus.h>

namespace
{
Gdiplus::Color Argb(COLORREF c, int alpha) { return Gdiplus::Color(static_cast<BYTE>(alpha), GetRValue(c), GetGValue(c), GetBValue(c)); }

void RoundPath(Gdiplus::GraphicsPath& p, const RECT& r, double radius)
{
    const float x = static_cast<float>(r.left), y = static_cast<float>(r.top);
    const float w = static_cast<float>(r.right - r.left), h = static_cast<float>(r.bottom - r.top);
    const float d = static_cast<float>(std::min<double>(radius * 2, std::min(w, h)));
    if (d <= 0.5f)
    {
        p.AddRectangle(Gdiplus::RectF(x, y, w, h));
        return;
    }
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

HFONT MakeFont(const wchar_t* face, int px, int weight)
{
    LOGFONTW lf{};
    lf.lfHeight = -px;
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, face);
    return CreateFontIndirectW(&lf);
}

bool FontExists(const wchar_t* face)
{
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, face);
    bool found = false;
    HDC hdc = GetDC(nullptr);
    EnumFontFamiliesExW(hdc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) { *reinterpret_cast<bool*>(p) = true; return 0; },
        reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, hdc);
    return found;
}
}

COLORREF Mix(COLORREF a, COLORREF b, double t)
{
    t = std::clamp(t, 0.0, 1.0);
    auto ch = [t](int x, int y) { return static_cast<BYTE>(std::lround(x + (y - x) * t)); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}

bool SystemDark()
{
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
        RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

Palette MakePalette(bool dark)
{
    Palette p;
    p.dark = dark;
    if (dark)
    {
        p.content = RGB(0x2B, 0x2B, 0x2B);  // ContentDialog background (SolidBackgroundFillColorBase + layer)
        p.footer = RGB(0x20, 0x20, 0x20);   // SolidBackgroundFillColorBase
        p.title = RGB(0x2B, 0x2B, 0x2B);
        p.ink = RGB(0xFF, 0xFF, 0xFF);
        p.muted = RGB(0xC5, 0xC5, 0xC5);
        p.disabled = RGB(0x78, 0x78, 0x78);
        p.accent = RGB(0x4C, 0xC2, 0xFF);   // AccentFillColorDefault (dark, default blue)
        p.onAccent = RGB(0x00, 0x00, 0x00);
        p.control = RGB(0x37, 0x37, 0x37);  // ControlFillColorDefault #0FFFFFFF over the dialog
        p.controlHover = RGB(0x3C, 0x3C, 0x3C);
        p.controlPressed = RGB(0x32, 0x32, 0x32);
        p.stroke = RGB(0x3E, 0x3E, 0x3E);
        p.strokeStrong = RGB(0x9A, 0x9A, 0x9A);
        p.divider = RGB(0x1C, 0x1C, 0x1C);
        p.track = RGB(0x5A, 0x5A, 0x5A);
        p.error = RGB(0xFF, 0x99, 0xA4);
    }
    else
    {
        p.content = RGB(0xFF, 0xFF, 0xFF);
        p.footer = RGB(0xF3, 0xF3, 0xF3);
        p.title = RGB(0xFF, 0xFF, 0xFF);
        p.ink = RGB(0x1B, 0x1B, 0x1B);
        p.muted = RGB(0x5F, 0x5F, 0x5F);
        p.disabled = RGB(0xA0, 0xA0, 0xA0);
        p.accent = RGB(0x00, 0x5F, 0xB8);   // AccentFillColorDefault (light, default blue)
        p.onAccent = RGB(0xFF, 0xFF, 0xFF);
        p.control = RGB(0xFB, 0xFB, 0xFB);
        p.controlHover = RGB(0xF6, 0xF6, 0xF6);
        p.controlPressed = RGB(0xF0, 0xF0, 0xF0);
        p.stroke = RGB(0xE0, 0xE0, 0xE0);
        p.strokeStrong = RGB(0x8A, 0x8A, 0x8A);
        p.divider = RGB(0xE5, 0xE5, 0xE5);
        p.track = RGB(0xD6, 0xD6, 0xD6);
        p.error = RGB(0xC4, 0x2B, 0x1C);
    }
    p.accentHover = Mix(p.accent, dark ? RGB(0, 0, 0) : p.content, 0.1);
    p.accentPressed = Mix(p.accent, dark ? RGB(0, 0, 0) : p.content, 0.2);
    p.closeHover = RGB(0xC4, 0x2B, 0x1C);
    p.frame = dark ? RGB(0x40, 0x40, 0x40) : RGB(0xC8, 0xC8, 0xC8);
    p.frameAlpha = 255;
    p.frameInactiveAlpha = dark ? 200 : 170;
    p.shadowAlpha = dark ? 100 : 56;
    p.shadowInactiveAlpha = dark ? 70 : 36;
    p.highlight = RGB(0xFF, 0xFF, 0xFF);
    p.success = dark ? RGB(0x6C, 0xCB, 0x5F) : RGB(0x0F, 0x7B, 0x0F);
    return p;
}

void Fonts::Build(UINT dpi)
{
    Release();
    static const wchar_t* text = FontExists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Microsoft YaHei UI";
    static const wchar_t* display = FontExists(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Microsoft YaHei UI";
    static const wchar_t* glyphs = FontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    auto s = [dpi](int v) { return MulDiv(v, static_cast<int>(dpi), 96); };
    title = MakeFont(display, s(24), FW_SEMIBOLD); // TitleTextBlockStyle-ish (dialog title 20-28)
    body = MakeFont(text, s(14), FW_NORMAL);
    caption = MakeFont(text, s(12), FW_NORMAL);
    button = MakeFont(text, s(14), FW_NORMAL);
    icons = MakeFont(glyphs, s(10), FW_NORMAL);
}

void Fonts::Release()
{
    for (HFONT* f : { &title, &body, &caption, &icons, &button })
        if (*f)
        {
            DeleteObject(*f);
            *f = nullptr;
        }
}

void FillRound(HDC hdc, const RECT& r, double radius, COLORREF color, int alpha)
{
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::GraphicsPath p;
    RoundPath(p, r, radius);
    Gdiplus::SolidBrush b(Argb(color, alpha));
    g.FillPath(&b, &p);
}

void StrokeRound(HDC hdc, const RECT& r, double radius, COLORREF color, double width, int alpha)
{
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath p;
    const float inset = static_cast<float>(width / 2);
    Gdiplus::RectF rf(r.left + inset, r.top + inset, r.right - r.left - 2 * inset, r.bottom - r.top - 2 * inset);
    const float d = static_cast<float>(std::min<double>(radius * 2, std::min(rf.Width, rf.Height)));
    p.AddArc(rf.X, rf.Y, d, d, 180, 90);
    p.AddArc(rf.X + rf.Width - d, rf.Y, d, d, 270, 90);
    p.AddArc(rf.X + rf.Width - d, rf.Y + rf.Height - d, d, d, 0, 90);
    p.AddArc(rf.X, rf.Y + rf.Height - d, d, d, 90, 90);
    p.CloseFigure();
    Gdiplus::Pen pen(Argb(color, alpha), static_cast<float>(width));
    g.DrawPath(&pen, &p);
}

void FillCircle(HDC hdc, double cx, double cy, double radius, COLORREF color, int alpha)
{
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::SolidBrush b(Argb(color, alpha));
    g.FillEllipse(&b, static_cast<float>(cx - radius), static_cast<float>(cy - radius), static_cast<float>(radius * 2), static_cast<float>(radius * 2));
}
void StrokeArc(HDC hdc, double cx, double cy, double radius, double startDeg, double sweepDeg, COLORREF color, double width, int alpha)
{
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    // GDI+ arcs start at 3 o'clock and grow clockwise; the ring starts at 12 o'clock, so rotate by -90
    Gdiplus::Pen pen(Argb(color, alpha), static_cast<float>(width));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    const float d = static_cast<float>(radius * 2);
    g.DrawArc(&pen, static_cast<float>(cx - radius), static_cast<float>(cy - radius), d, d, static_cast<float>(startDeg - 90), static_cast<float>(sweepDeg));
}

void StrokePath(HDC hdc, const double* xy, int points, double amount, COLORREF color, double width)
{
    if (points < 2 || amount <= 0)
        return;
    double total = 0;
    for (int i = 1; i < points; ++i)
        total += std::hypot(xy[2 * i] - xy[2 * i - 2], xy[2 * i + 1] - xy[2 * i - 1]);
    double left = total * std::min(amount, 1.0);
    Gdiplus::GraphicsPath path;
    for (int i = 1; i < points && left > 0; ++i)
    {
        const double x0 = xy[2 * i - 2], y0 = xy[2 * i - 1], x1 = xy[2 * i], y1 = xy[2 * i + 1];
        const double len = std::hypot(x1 - x0, y1 - y0), t = len > 0 ? std::min(1.0, left / len) : 1.0;
        path.AddLine(static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x0 + (x1 - x0) * t), static_cast<float>(y0 + (y1 - y0) * t));
        left -= len;
    }
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(Argb(color, 255), static_cast<float>(width));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    g.DrawPath(&pen, &path);
}

void FillRect(HDC hdc, const RECT& r, COLORREF color)
{
    SetDCBrushColor(hdc, color);
    ::FillRect(hdc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void Text(HDC hdc, HFONT font, const std::wstring& text, RECT r, COLORREF color, UINT format)
{
    // DT_PATH_ELLIPSIS (the install path read-only box): GDI-only. It's one call site, the truncation style
    // DirectWrite has no equivalent for (ellipsis in the middle, keeping the path's last segment visible), and
    // getting it pixel-identical to Explorer's own path shortening isn't worth a bespoke DirectWrite trimming
    // callback for a box that's rarely more than one short path long.
    if (DWriteText::Available() && !(format & DT_PATH_ELLIPSIS))
    {
        if (format & DT_WORDBREAK)
            DWriteText::DrawWrapped(hdc, font, text, r, color, format);
        else
            DWriteText::DrawLabel(hdc, font, text, r, color, format);
        return;
    }
    HGDIOBJ old = SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &r, format | DT_NOPREFIX);
    SelectObject(hdc, old);
}

int TextWidth(HDC hdc, HFONT font, const std::wstring& text)
{
    if (DWriteText::Available())
        return DWriteText::TextWidth(hdc, font, text);
    HGDIOBJ old = SelectObject(hdc, font);
    SIZE s{};
    GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &s);
    SelectObject(hdc, old);
    return s.cx;
}

void DrawAppIcon(HDC hdc, int x, int y, int size)
{
    // LoadImage picks the icon's closest frame; cached per size (a few sizes per DPI)
    static std::map<int, HICON> cache;
    HICON& icon = cache[size];
    if (!icon)
        icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kIconId), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
    if (icon)
        DrawIconEx(hdc, x, y, icon, size, size, 0, nullptr, DI_NORMAL);
}

double EaseOut(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return 1 - std::pow(1 - t, 3);
}

double EaseOutBack(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    const double c1 = 1.70158, c3 = c1 + 1;
    return 1 + c3 * std::pow(t - 1, 3) + c1 * std::pow(t - 1, 2);
}
