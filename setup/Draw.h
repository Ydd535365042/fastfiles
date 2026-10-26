#pragma once
#include <windows.h>
#include <string>

// WinUI 3 dialog colors flattened to solid values (light / dark), so the setup matches the app and Windows 11
struct Palette
{
    bool dark = false;
    COLORREF content, footer, title, ink, muted, disabled, accent, accentHover, accentPressed, onAccent;
    COLORREF control, controlHover, controlPressed, stroke, strokeStrong, divider, track, closeHover, error;
    COLORREF frame;      // the window's 1px border (Windows 8 / 10: drawn around the rounded edge by RoundFrame)
    int frameAlpha, frameInactiveAlpha, shadowAlpha, shadowInactiveAlpha; // that border's and the shadow's alphas
    COLORREF highlight;  // white: glyph hover tint, button sheen, ControlElevationBorder highlight (used at an alpha)
    COLORREF success;    // SystemFillColorSuccess (the finished page's check)
};

Palette MakePalette(bool dark);
bool SystemDark();
COLORREF Mix(COLORREF a, COLORREF b, double t); // t = 0 -> a, 1 -> b

struct Fonts
{
    HFONT title = nullptr, body = nullptr, caption = nullptr, icons = nullptr, button = nullptr;
    void Build(UINT dpi);
    void Release();
};

// GDI+ for anti-aliased shapes; text stays on GDI (ClearType)
void FillRound(HDC hdc, const RECT& r, double radius, COLORREF color, int alpha = 255);
void StrokeRound(HDC hdc, const RECT& r, double radius, COLORREF color, double width = 1.0, int alpha = 255);
void FillCircle(HDC hdc, double cx, double cy, double radius, COLORREF color, int alpha = 255);
// Arc, degrees clockwise from the top (12 o'clock), round caps (the Working page's indeterminate ring)
void StrokeArc(HDC hdc, double cx, double cy, double radius, double startDeg, double sweepDeg, COLORREF color, double width, int alpha = 255);
// Polyline revealed up to `amount` (0..1) of its length, round caps
void StrokePath(HDC hdc, const double* xy, int points, double amount, COLORREF color, double width);
void FillRect(HDC hdc, const RECT& r, COLORREF color);
void Text(HDC hdc, HFONT font, const std::wstring& text, RECT r, COLORREF color, UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE);
int TextWidth(HDC hdc, HFONT font, const std::wstring& text);
void DrawAppIcon(HDC hdc, int x, int y, int size);
double EaseOut(double t);     // cubic
double EaseOutBack(double t); // slight overshoot
