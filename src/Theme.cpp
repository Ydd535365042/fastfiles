#include "Theme.h"
#include "Backdrop.h"
#include "DWriteText.h"
#include "resource.h"
#include <cmath>

namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

void StartGdiplus()
{
    // Started once for the process lifetime; shutting down at exit buys nothing
    static const bool started = []
    {
        ULONG_PTR token = 0;
        Gdiplus::GdiplusStartupInput input;
        return Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    }();
    (void)started;
}

namespace
{
void EnsureGdiplus() { StartGdiplus(); }

Gdiplus::Color ToColor(COLORREF c) { return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); }

// WCAG contrast ratio of two colors (1..21)
double Contrast(COLORREF a, COLORREF b)
{
    const auto lum = [](COLORREF c)
    {
        const auto ch = [](int v)
        {
            const double s = v / 255.0;
            return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * ch(GetRValue(c)) + 0.7152 * ch(GetGValue(c)) + 0.0722 * ch(GetBValue(c));
    };
    const double x = lum(a), y = lum(b);
    return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}

// Text a colored background (Files' AppThemeBackgroundColor) still has to carry: primary 7:1, secondary 4.5:1
constexpr double kInkContrast = 7.0, kMutedContrast = 4.5;

void AddRound(Gdiplus::GraphicsPath& path, Gdiplus::RectF r, float radius, bool bottom)
{
    const float d = std::min(radius * 2, std::min(r.Width, r.Height));
    if (d <= 0)
    {
        path.AddRectangle(r);
        return;
    }
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    if (bottom)
    {
        path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
        path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    }
    else
    {
        path.AddLine(r.X + r.Width, r.Y + r.Height, r.X, r.Y + r.Height);
    }
    path.CloseFigure();
}

void FillPath(HDC hdc, const RECT& rc, int radius, COLORREF color, bool bottom)
{
    EnsureGdiplus();
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::GraphicsPath path;
    AddRound(path, Gdiplus::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top),
        static_cast<float>(rc.right - rc.left), static_cast<float>(rc.bottom - rc.top)), static_cast<float>(radius), bottom);
    Gdiplus::SolidBrush brush(ToColor(color));
    g.FillPath(&brush, &path);
}

HFONT MakeFont(const wchar_t* face, int px, int weight = FW_NORMAL)
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

void Theme::Build(UINT newDpi)
{
    Release();
    dpi = newDpi;
    // Windows 11 ships Segoe Fluent Icons (same code points, rounder shapes); Windows 10 has MDL2
    static const wchar_t* iconFace = FontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    static const wchar_t* textFace = FontExists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Microsoft YaHei UI";
    const wchar_t* body = !face.empty() && FontExists(face.c_str()) ? face.c_str() : textFace;
    ui = MakeFont(body, S(13));
    caption = MakeFont(body, S(12));
    icons = MakeFont(iconFace, S(16));
    iconsMid = MakeFont(iconFace, S(12));
    iconsSmall = MakeFont(iconFace, S(10));
}

void Theme::SetPalette(bool darkPalette)
{
    // Files on Windows 10 (no Mica): SolidBackgroundFillColorBase under the tab row, LayerOnMicaBaseAlt for the
    // nav row and sidebar, cards for the toolbar and file area. Light values measured from Files 4.3, dark ones are
    // the same WinUI tokens flattened over #202020.
    dark = darkPalette;
    if (dark)
    {
        chrome = RGB(0x20, 0x20, 0x20);      // SolidBackgroundFillColorBase
        chromeHover = RGB(0x2D, 0x2D, 0x2D);
        layer = RGB(0x2C, 0x2C, 0x2C);       // LayerOnMicaBaseAltFillColorDefault #733A3A3A
        content = RGB(0x37, 0x37, 0x37);     // App.Theme.FileArea: CardBackgroundFillColorDefault #0DFFFFFF over the layer
        toolbar = RGB(0x33, 0x33, 0x33);     // CardBackgroundFillColorSecondary #08FFFFFF over the layer
        field = RGB(0x37, 0x37, 0x37);       // omnibar / search box
        fieldStroke = RGB(0x3D, 0x3D, 0x3D);
        cardEdge = RGB(0x1C, 0x1C, 0x1C);    // CardStrokeColorDefault #19000000
        line = RGB(0x3D, 0x3D, 0x3D);        // DividerStrokeColorDefault #15FFFFFF
        columnGrip = RGB(0x1C, 0x1C, 0x1C);     // CardStrokeColorDefaultSolid (BladeResizer's ResizerPointerOver)
        columnGripDrag = RGB(0x1C, 0x1C, 0x1C); // SolidBackgroundFillColorSecondary (ResizerPressed): same value in dark
        selected = RGB(0x24, 0x3A, 0x55);
        hover = RGB(0x42, 0x42, 0x42);       // SubtleFillColorSecondary #0FFFFFFF over the file area
        pressed = RGB(0x3E, 0x3E, 0x3E);     // SubtleFillColorTertiary #0AFFFFFF
        ink = RGB(0xFF, 0xFF, 0xFF);
        muted = RGB(0xCE, 0xCE, 0xCE);       // TextFillColorSecondary #C5FFFFFF over the layer
        accent = RGB(0x60, 0xCD, 0xFF);
    }
    else
    {
        chrome = RGB(0xF3, 0xF3, 0xF3);      // tab row (measured)
        chromeHover = RGB(0xEA, 0xEA, 0xEA);
        layer = RGB(0xFB, 0xFB, 0xFB);       // nav row, sidebar, active tab (measured)
        content = RGB(0xFC, 0xFC, 0xFC);     // file area card #C0FCFCFC (measured)
        toolbar = RGB(0xF9, 0xF9, 0xF9);     // CardBackgroundFillColorSecondary #80F6F6F6
        field = RGB(0xFE, 0xFE, 0xFE);       // omnibar (measured)
        fieldStroke = RGB(0xE5, 0xE5, 0xE5);
        cardEdge = RGB(0xE5, 0xE5, 0xE5);    // file area card stroke (measured)
        line = RGB(0xEB, 0xEB, 0xEB);
        columnGrip = RGB(0xEB, 0xEB, 0xEB);     // CardStrokeColorDefaultSolid (BladeResizer's ResizerPointerOver)
        columnGripDrag = RGB(0xEE, 0xEE, 0xEE); // SolidBackgroundFillColorSecondary (ResizerPressed)
        selected = RGB(0xD7, 0xE6, 0xF4);
        hover = RGB(0xF2, 0xF2, 0xF2);       // SubtleFillColorSecondary #09000000
        pressed = RGB(0xF5, 0xF5, 0xF5);
        ink = RGB(0x1B, 0x1B, 0x1B);         // TextFillColorPrimary #E4000000
        muted = RGB(0x60, 0x60, 0x60);       // TextFillColorSecondary #9E000000
        accent = RGB(0x00, 0x5A, 0x9E);      // measured (SystemAccentColorDark1 of the default blue)
    }
    // LayerOnMicaBaseAltFillColorDefault (#733A3A3A / #B3FFFFFF) and the toolbar's CardBackgroundFillColorSecondary
    // (#08FFFFFF / #80F6F6F6); the file area: CardBackgroundFillColorDefault / App.Theme.FileArea #C0FCFCFC
    layerFill = dark ? RGB(0x3A, 0x3A, 0x3A) : RGB(0xFF, 0xFF, 0xFF);
    layerPct = dark ? 45 : 70;
    toolbarFill = dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0xF6, 0xF6, 0xF6);
    toolbarPct = dark ? 3 : 50;
    contentFill = dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0xFC, 0xFC, 0xFC);
    contentPct = dark ? 5 : 75;
    if (backdrop != CLR_INVALID)
    {
        // A live material (Backdrop.cpp, Files' background color included): the tab row is the material, the
        // layers over it are WinUI's translucent fills. The averages here; BufferedPaint draws the real gradient.
        chrome = backdrop;
        chromeHover = Mix(chrome, dark ? RGB(255, 255, 255) : RGB(0, 0, 0), dark ? 5 : 4);
        // A strong background color or image (black / white / red / green) shows through the nav row, sidebar and
        // cards until the theme's text can't be read on them: each gets more opaque until it can. The cards stack
        // on the nav layer: when it gets more opaque they get less, so as much as before shows through them
        // (Files' look) unless their own text needs more
        const auto score = [&](COLORREF s) { return std::min(Contrast(ink, s) / kInkContrast, Contrast(muted, s) / kMutedContrast); };
        const int baseLayer = layerPct, baseToolbar = toolbarPct, baseContent = contentPct;
        const auto keep = [&](int pct)
        { return layerPct >= 100 ? pct : std::clamp(100 - (100 - baseLayer) * (100 - pct) / (100 - layerPct), 0, 100); };
        // A card's own fill only while it helps (dark cards are white fills: more of them is less contrast)
        const auto raise = [&](COLORREF fill, int& pct)
        {
            COLORREF s = Mix(layer, fill, pct);
            while (score(s) < 1 && pct < 100)
            {
                const int next = std::min(100, pct + 5);
                const COLORREF t = Mix(layer, fill, next);
                if (score(t) <= score(s))
                    break;
                pct = next;
                s = t;
            }
            return s;
        };
        for (;;)
        {
            layer = Mix(chrome, layerFill, layerPct);
            toolbarPct = keep(baseToolbar);
            contentPct = keep(baseContent);
            toolbar = raise(toolbarFill, toolbarPct);
            content = raise(contentFill, contentPct);
            if ((score(layer) >= 1 && score(toolbar) >= 1 && score(content) >= 1) || layerPct >= 100)
                break;
            layerPct = std::min(100, layerPct + 5);
        }
        // chrome / layer / toolbar are keys for BufferedPaint: they must differ from each other and from the
        // flat file area
        auto nudge = [](COLORREF& c, std::initializer_list<COLORREF> others)
        {
            for (int i = 0; i < 4 && std::find(others.begin(), others.end(), c) != others.end(); ++i)
                c = NudgeKey(c, i);
        };
        nudge(layer, { chrome, content });
        nudge(toolbar, { chrome, layer, content });
        nudge(chrome, { content });
    }
    // Files' AppThemeBackgroundColor sits under the translucent layers: the tab row shows it through the backdrop
    // (the whole alpha), the nav row / sidebar and the cards let less through (their own alphas: ~45%, ~25%)
    else if (const int a = static_cast<int>(tint >> 24); a > 0)
    {
        const COLORREF c = RGB((tint >> 16) & 0xFF, (tint >> 8) & 0xFF, tint & 0xFF);
        const int p = a * 100 / 255;
        chrome = Mix(chrome, c, p);
        chromeHover = Mix(chromeHover, c, p);
        // As with the material: less of the color through each layer while the theme's text can't be read on it
        const auto readable = [&](COLORREF s) { return Contrast(ink, s) >= kInkContrast && Contrast(muted, s) >= kMutedContrast; };
        const auto through = [&](COLORREF base, int share)
        {
            COLORREF s = Mix(base, c, p * share / 100);
            for (int q = p; !readable(s) && q > 0; s = Mix(base, c, q * share / 100))
                q = std::max(0, q - 5);
            return s;
        };
        layer = through(layer, 45);
        toolbar = through(toolbar, 25);
        content = through(content, 25);
    }
    // The tab row is the background color itself: its text and glyphs take whichever theme's ink reads on it
    // (Windows' caption buttons over a colored title bar do the same), the secondary one pulled toward it until
    // it reads too
    {
        const COLORREF lightInk = RGB(0x1B, 0x1B, 0x1B), darkInk = RGB(0xFF, 0xFF, 0xFF);
        const COLORREF lightMuted = RGB(0x60, 0x60, 0x60), darkMuted = RGB(0xCE, 0xCE, 0xCE);
        const bool flip = Contrast(ink, chrome) < kInkContrast && Contrast(dark ? lightInk : darkInk, chrome) > Contrast(ink, chrome);
        chromeInk = flip ? (dark ? lightInk : darkInk) : ink;
        chromeMuted = flip ? (dark ? lightMuted : darkMuted) : muted;
        for (int i = 0; i < 10 && Contrast(chromeMuted, chrome) < kMutedContrast; ++i)
            chromeMuted = Mix(chromeMuted, chromeInk, 20);
        chromePressed = pressed;
        if (flip)
        {
            chromeHover = Mix(chrome, chromeInk, 8); // the hover / press tints follow the ink, not the theme
            chromePressed = Mix(chrome, chromeInk, 5);
        }
    }
    // Alpha tokens from WinUI's theme resources flattened onto the sidebar/nav layer
    const COLORREF black = RGB(0, 0, 0), white = RGB(255, 255, 255);
    if (dark)
    {
        subtleHover = Mix(layer, white, 6);        // #0FFFFFFF
        subtlePressed = Mix(layer, white, 4);      // #0AFFFFFF
        controlFill = Mix(layer, white, 6);        // #0FFFFFFF
        controlFillHover = Mix(layer, white, 8);   // #15FFFFFF
        controlFillPressed = Mix(layer, white, 3); // #08FFFFFF
        controlStroke = Mix(layer, white, 8);      // #12FFFFFF..#18FFFFFF
        buttonHoverPct = 9;
        buttonPressedPct = 15;
        card = Mix(content, white, 5);             // SettingsCard: CardBackgroundFillColorDefault over the file area
        cardHover = Mix(content, white, 8);
        cardPressed = Mix(content, white, 3);
        cardStroke = Mix(content, black, 10);      // CardStrokeColorDefault #19000000
    }
    else
    {
        subtleHover = Mix(layer, black, 4);        // #09000000
        subtlePressed = Mix(layer, black, 2);      // #06000000
        controlFill = Mix(layer, white, 70);       // #B3FFFFFF
        controlFillHover = Mix(layer, white, 50);  // #80F9F9F9
        controlFillPressed = Mix(layer, white, 30); // #4DF9F9F9
        controlStroke = Mix(layer, black, 9);      // #0F000000..#29000000
        buttonHoverPct = 6;
        buttonPressedPct = 11;
        card = RGB(0xFF, 0xFF, 0xFF);              // Files' settings cards (measured)
        cardHover = RGB(0xFA, 0xFA, 0xFA);
        cardPressed = RGB(0xF7, 0xF7, 0xF7);
        cardStroke = RGB(0xED, 0xED, 0xED);        // measured
    }
    fillWhite = white;
    fillBlack = black;
    overlay = dark ? white : black;
    // WinUI theme resources (Common_themeresources_any.xaml), light / dark
    textOnAccent = dark ? black : white;                                // TextOnAccentFillColorPrimary
    critical = dark ? RGB(0xFF, 0x99, 0xA4) : RGB(0xC4, 0x2B, 0x1C);    // SystemFillColorCritical
    criticalBackground = dark ? RGB(0x44, 0x27, 0x26) : RGB(0xFD, 0xE7, 0xE9);
    criticalText = dark ? RGB(0xF0, 0x8B, 0x81) : RGB(0xA8, 0x32, 0x2B); // InfoBar error text (Files' settings)
    success = dark ? RGB(0x6C, 0xCB, 0x5F) : RGB(0x0F, 0x7B, 0x0F);     // SystemFillColorSuccess
    successBackground = dark ? RGB(0x39, 0x3D, 0x1B) : RGB(0xDF, 0xF6, 0xDD);
    flyout = dark ? RGB(0x2C, 0x2C, 0x2C) : RGB(0xF9, 0xF9, 0xF9);       // AcrylicInAppFillColorDefault without the blur
    flyoutCard = dark ? RGB(0x34, 0x34, 0x34) : white;                  // CardBackgroundFillColorDefault over it
    flyoutStroke = dark ? RGB(0x1C, 0x1C, 0x1C) : RGB(0xE0, 0xE0, 0xE0); // SurfaceStrokeColorFlyout
    altFill = dark ? white : RGB(0xF9, 0xF9, 0xF9);                     // ControlFillColor bases (#xxF9F9F9 in light)
    dialogFooter = Mix(layer, black, dark ? 18 : 4);                    // ContentDialog's CommandSpace
    sliderThumb = dark ? RGB(0x45, 0x45, 0x45) : white;                 // ControlSolidFillColorDefault
    scrubTrack = Mix(content, overlay, dark ? 6 : 4);
    smoke = black;           // SmokeFillColorDefault #4D000000 (the alpha is the smoke window's layering)
    videoBackground = black; // MediaPlayerElement draws the picture on black in both themes
    checkerLight = RGB(0xD3, 0xD3, 0xD3);                               // ColorPicker's checkerboard
    checkerDark = RGB(0x80, 0x80, 0x80);
    // ColorPicker's channel sliders: the filled part in the channel's own color (the same in both themes)
    channelRed = RGB(0xFF, 0x3C, 0x3C);
    channelGreen = RGB(0x3C, 0xC8, 0x5A);
    channelBlue = RGB(0x50, 0x8C, 0xFF);
    glyphMask = white;
    closeHover = RGB(0xC4, 0x2B, 0x1C);                                 // the caption close button (Windows' red)
    errorStroke = closeHover;                                           // a text box in error (same in both themes)
    closeInk = white;                                                   // the caption close button on red
    closePressed = Mix(closeHover, black, 15);
    // The own file list over the file area: SubtleFillColorSecondary / Tertiary rows, ControlAltFillColorTertiary on the
    // header, TextFillColorTertiary header text
    listHoverPct = dark ? 6 : 4;
    listPressedPct = dark ? 4 : 2;
    listHover = Mix(content, overlay, listHoverPct);
    listPressed = Mix(content, overlay, listPressedPct);
    headerHover = Mix(content, overlay, dark ? 4 : 6);
    headerText = Mix(content, overlay, dark ? 54 : 45); // #8BFFFFFF / #72000000
    // The header's column dividers: HeaderGridSplitterStyle (Opacity 0.8) over DefaultGridSplitterStyle's 1px Border in
    // SplitterBorderBrush = ControlStrokeColorDefault (#12FFFFFF / #0F000000)
    headerDivider = Mix(content, overlay, dark ? 6 : 5);
    // WinUI ProgressBar (Files' drive and network cards): a 1px track in ProgressBarBackground =
    // ControlStrongStrokeColorDefault (#8BFFFFFF / #72000000) under the 3px indicator
    progressTrack = Mix(content, overlay, dark ? 54 : 45);
    // WinUI CheckBox (Common_themeresources_any.xaml): dark #19000000 / #0BFFFFFF / #12FFFFFF, stroke #8BFFFFFF / #28FFFFFF;
    // light #06000000 / #0F000000 / #18000000, stroke #72000000 / #37000000; AccentFillColorSecondary / Tertiary 0.9 / 0.8
    checkFill[0] = black;
    checkFill[1] = checkFill[2] = overlay;
    const int fillPct[2][3] = { { 2, 6, 9 }, { 10, 4, 7 } };
    for (int i = 0; i < 3; ++i)
        checkFillPct[i] = fillPct[dark ? 1 : 0][i];
    checkStroke[0] = checkStroke[1] = overlay;
    checkStrokePct[0] = dark ? 55 : 45;
    checkStrokePct[1] = dark ? 16 : 22;
    checkAccentPct[0] = 100;
    checkAccentPct[1] = 90;
    checkAccentPct[2] = 80;
    checkGlyphPressedPct = dark ? 50 : 70; // #80000000 / #B3FFFFFF
    // Group headers' divider (SystemControlForegroundBaseLowBrush #33FFFFFF / #33000000) and the rubber band's stroke
    // (SystemAccentColorLight1: the accent toward white)
    groupDivider = Mix(content, overlay, 20);
    marqueeStroke = Mix(accent, white, 20);
    focusStroke = ink; // WinUI's focus visual (FocusStrokeColorOuter: the text color)
    // WinUI ScrollBar_themeresources.xaml: ScrollBarThumbBackground = ControlStrongFillColorDefault (#8BFFFFFF / #72000000),
    // ScrollBarButtonArrowForegroundPointerOver = TextFillColorSecondary (#C5FFFFFF / #9E000000), ScrollBarTrackFill =
    // AcrylicInAppFillColorDefault (its solid fallback)
    scrollInk = overlay;
    scrollThumbAlpha = dark ? 0x8B : 0x72;
    scrollArrowHotAlpha = dark ? 0xC5 : 0x9E;
    scrollTrack = flyout;
    // Cards and grid cells: light #B3FFFFFF / #FFFFFFFF / #0F000000 / #06000000 / #9E000000; dark #0DFFFFFF / #12FFFFFF /
    // #19000000 / #08FFFFFF / #C5FFFFFF
    cardFillBase = cardHoverBase = white;
    cardFillPct = dark ? 5 : 70;
    cardHoverPct = dark ? 7 : 100;
    cardStrokeBase = black;
    cardStrokePct = dark ? 10 : 6;
    neutralBase = overlay;
    neutralPct = dark ? 3 : 2;
    secondaryText = overlay;
    secondaryTextPct = dark ? 77 : 62;
    // Windows 11's window edge: a thin gray line (lighter when inactive) and a soft shadow under it
    windowBorder = dark ? RGB(0x3C, 0x3C, 0x3C) : RGB(0x80, 0x80, 0x80);
    windowBorderPct = dark ? 100 : 55;
    windowBorderInactivePct = dark ? 80 : 35;
    windowShadowPct = dark ? 40 : 22;
    windowShadowInactivePct = dark ? 28 : 14;
}
void DrawEjectIcon(HDC hdc, const RECT& rc, COLORREF color)
{
    // App.ThemedIcons.Actions.Eject.12 outline: a rounded triangle over a bar, 12px design grid
    EnsureGdiplus();
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float size = static_cast<float>(std::min(rc.right - rc.left, rc.bottom - rc.top)) / 2;
    const float k = size / 12, x = (rc.left + rc.right) / 2.0f - 6 * k, y = (rc.top + rc.bottom) / 2.0f - 6 * k;
    Gdiplus::Pen pen(ToColor(color), std::max(1.0f, k));
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    const Gdiplus::PointF tri[] = { { x + 6 * k, y + 1.2f * k }, { x + 10.8f * k, y + 7.5f * k }, { x + 1.2f * k, y + 7.5f * k } };
    g.DrawPolygon(&pen, tri, 3);
    g.DrawLine(&pen, x + 1 * k, y + 10.5f * k, x + 11 * k, y + 10.5f * k);
}
void DrawPaneIcon(HDC hdc, const RECT& rc, COLORREF color)
{
    // App.ThemedIcons.Panes.Single outline (16px grid, path data from Icons.TabPane.xaml): a rounded square with a
    // 1px frame, a header bar and a left column, filled even-odd so the two panes are holes
    EnsureGdiplus();
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const float k = static_cast<float>(std::min(rc.right - rc.left, rc.bottom - rc.top)) / 16;
    g.TranslateTransform(static_cast<float>(rc.left), static_cast<float>(rc.top));
    g.ScaleTransform(k, k);
    Gdiplus::GraphicsPath path(Gdiplus::FillModeAlternate);
    path.AddArc(0.0f, 0.0f, 6.0f, 6.0f, 180, 90);
    path.AddArc(9.0f, 0.0f, 6.0f, 6.0f, 270, 90);
    path.AddArc(9.0f, 9.0f, 6.0f, 6.0f, 0, 90);
    path.AddArc(0.0f, 9.0f, 6.0f, 6.0f, 90, 90);
    path.CloseFigure();
    // Top pane: the header and the left column
    path.StartFigure();
    path.AddArc(1.0f, 1.0f, 4.0f, 4.0f, 180, 90);
    path.AddArc(10.0f, 1.0f, 4.0f, 4.0f, 270, 90);
    path.AddLine(14.0f, 3.0f, 14.0f, 4.0f);
    path.AddLine(14.0f, 4.0f, 5.5f, 4.0f);
    path.AddArc(4.0f, 4.0f, 3.0f, 3.0f, 270, -90);
    path.AddLine(4.0f, 5.5f, 4.0f, 14.0f);
    path.AddLine(4.0f, 14.0f, 3.0f, 14.0f);
    path.AddArc(1.0f, 10.0f, 4.0f, 4.0f, 90, 90);
    path.CloseFigure();
    // Content pane
    path.StartFigure();
    path.AddLine(5.0f, 14.0f, 12.0f, 14.0f);
    path.AddArc(10.0f, 10.0f, 4.0f, 4.0f, 90, -90);
    path.AddLine(14.0f, 12.0f, 14.0f, 5.0f);
    path.AddLine(14.0f, 5.0f, 5.5f, 5.0f);
    path.AddArc(5.0f, 5.0f, 1.0f, 1.0f, 270, -90);
    path.CloseFigure();
    Gdiplus::SolidBrush brush(ToColor(color));
    g.FillPath(&brush, &path);
}

void Theme::Release()
{
    for (HFONT* f : { &ui, &caption, &icons, &iconsMid, &iconsSmall })
        if (*f)
        {
            DeleteObject(*f);
            *f = nullptr;
        }
    // Every HFONT derived from these (ContentDialog's title, InfoPane's bold name, ...) gets freed and rebuilt
    // around the same Build() call; DWriteText's format cache keys on LOGFONT values so it stays correct either
    // way, but dropping it here keeps it from growing across many DPI / dark-mode / custom-font changes in one run.
    DWriteText::ClearCache();
}

COLORREF Mix(COLORREF a, COLORREF b, int p)
{
    auto ch = [p](int x, int y) { return static_cast<BYTE>((x * (100 - p) + y * p) / 100); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}

MaterialRecipe Theme::Material(bool alt, bool dark)
{
    // MicaController: Base dark #202020 tint 0.8, light #F3F3F3 0.5; BaseAlt dark #0A0A0A 0, light #DADADA 0.5; luminosity 1
    if (alt)
        return dark ? MaterialRecipe{ RGB(0x0A, 0x0A, 0x0A), 0.0f, 1.0f } : MaterialRecipe{ RGB(0xDA, 0xDA, 0xDA), 0.5f, 1.0f };
    return dark ? MaterialRecipe{ RGB(0x20, 0x20, 0x20), 0.8f, 1.0f } : MaterialRecipe{ RGB(0xF3, 0xF3, 0xF3), 0.5f, 1.0f };
}

COLORREF TagFallbackColor() { return RGB(0x80, 0x80, 0x80); }

COLORREF NudgeKey(COLORREF c, int step)
{
    return RGB(GetRValue(c), GetGValue(c), GetBValue(c) ^ (1 << step));
}

bool AnimationsEnabled()
{
    // Test-only escape hatch (FASTFILES_FORCE_NO_ANIM=1): lets a test exercise the "animations off" immediate-jump
    // branch deterministically on an unmodified binary, without ever calling SPI_SETCLIENTAREAANIMATION on the
    // real machine (DWriteText::Available's FASTFILES_FORCE_GDI_TEXT is the same pattern). Not read anywhere else;
    // never set by the app itself.
    static const bool forceOff = [] { wchar_t v[4]{}; return GetEnvironmentVariableW(L"FASTFILES_FORCE_NO_ANIM", v, 4) && v[0] == L'1'; }();
    if (forceOff)
        return false;
    BOOL on = TRUE;
    return !SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0) || on;
}

ULONGLONG AnimNow()
{
    static const double freq = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart); }();
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return 1 + static_cast<ULONGLONG>(static_cast<double>(q.QuadPart) * 1000.0 / freq);
}

double AnimProgress(ULONGLONG start, int ms)
{
    if (!start || ms <= 0)
        return 1.0;
    const ULONGLONG now = AnimNow();
    const double x = std::min(1.0, static_cast<double>(now > start ? now - start : 0) / ms);
    const double inv = 1.0 - x;
    return 1.0 - inv * inv * inv; // cubic ease-out (WinUI's decelerate curve)
}

int StateFade::Level() const
{
    if (!start || from == to)
        return to;
    const ULONGLONG now = AnimNow(), t = now > start ? now - start : 0;
    if (t >= kMs)
        return to;
    return from + static_cast<int>((to - from) * static_cast<long long>(t) / kMs);
}

void StateFade::Set(int level, bool snap)
{
    if (level == to)
        return;
    from = snap || !AnimationsEnabled() ? level : Level();
    to = level;
    start = from == to ? 0 : AnimNow();
}

COLORREF FadeFill(COLORREF base, COLORREF hover, COLORREF pressed, int level)
{
    if (level <= 0)
        return base;
    if (level <= 100)
        return level == 100 ? hover : Mix(base, hover, level);
    return level >= 200 ? pressed : Mix(hover, pressed, level - 100);
}

void FillRound(HDC hdc, const RECT& rc, int radius, COLORREF color) { FillPath(hdc, rc, radius, color, true); }
void FillTopRound(HDC hdc, const RECT& rc, int radius, COLORREF color) { FillPath(hdc, rc, radius, color, false); }

void FrameRound(HDC hdc, const RECT& rc, int radius, COLORREF color)
{
    EnsureGdiplus();
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    // Strokes centered on half pixels stay one device pixel wide
    AddRound(path, Gdiplus::RectF(rc.left + 0.5f, rc.top + 0.5f, rc.right - rc.left - 1.0f, rc.bottom - rc.top - 1.0f), static_cast<float>(radius), true);
    Gdiplus::Pen pen(ToColor(color), 1.0f);
    g.DrawPath(&pen, &path);
}

void DrawFocusVisual(HDC hdc, const Theme& t, const RECT& rc, int radius, COLORREF under, const RECT* bounds)
{
    // WinUI's system focus visual: FocusVisualMargin -3 (the ring sits 3px outside the control), the 2px outer stroke in
    // FocusStrokeColorOuter and the 1px inner stroke in what is under it. Kept inside `bounds` (the window) when given.
    const int out = t.S(3), outer = t.S(2), inner = std::max(1, t.S(1));
    RECT f{ rc.left - out, rc.top - out, rc.right + out, rc.bottom + out };
    if (bounds)
        IntersectRect(&f, &f, bounds);
    for (int k = 0; k < outer; ++k)
        FrameRound(hdc, { f.left + k, f.top + k, f.right - k, f.bottom - k }, std::max(0, radius + out - k), t.focusStroke);
    for (int k = 0; k < inner; ++k)
        FrameRound(hdc, { f.left + outer + k, f.top + outer + k, f.right - outer - k, f.bottom - outer - k }, std::max(0, radius + out - outer - k), under);
}

void FillSolid(HDC hdc, const RECT& rc, COLORREF color)
{
    const COLORREF old = SetBkColor(hdc, color);
    ExtTextOutW(hdc, 0, 0, ETO_OPAQUE, &rc, nullptr, 0, nullptr);
    SetBkColor(hdc, old);
}

// A GDI pattern (the color picker's checkerboard swatch) clipped to a rounded rect with an anti-aliased edge:
// `paint` fills (0,0)-(w,h) with plain GDI calls (SelectClipRgn would clip correctly but leaves the rounded corner
// jagged); this draws that content into an offscreen bitmap first, then composites it through GDI+ so only the
// rounded boundary is AA'd, same as every other rounded shape here
void FillRoundPattern(HDC hdc, const RECT& rc, int radius, void (*paint)(HDC, const RECT&, void*), void* context)
{
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0)
        return;
    HDC mem = CreateCompatibleDC(hdc);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp)
    {
        DeleteDC(mem);
        return;
    }
    HGDIOBJ old = SelectObject(mem, bmp);
    paint(mem, { 0, 0, w, h }, context); // the callback draws at (0,0)-relative coordinates
    SelectObject(mem, old);
    DeleteDC(mem);

    EnsureGdiplus();
    // Opaque pixels only (no alpha channel written above): mark every pixel 0xFF so GDI+ composites it normally
    for (uint32_t* p = static_cast<uint32_t*>(bits), *end = p + static_cast<size_t>(w) * h; p < end; ++p)
        *p |= 0xFF000000u;
    Gdiplus::Bitmap image(w, h, w * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    AddRound(path, Gdiplus::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top), static_cast<float>(w), static_cast<float>(h)),
        static_cast<float>(radius), true);
    g.SetClip(&path);
    g.DrawImage(&image, rc.left, rc.top, 0, 0, w, h, Gdiplus::UnitPixel);
    DeleteObject(bmp);
}

void DrawGlyph(HDC hdc, HFONT font, const wchar_t* glyph, const RECT& rc, COLORREF color)
{
    DrawLabel(hdc, font, glyph, rc, color, DT_CENTER);
}

void DrawGlyphTurned(HDC hdc, HFONT font, const wchar_t* glyph, const RECT& rc, COLORREF color, float degrees)
{
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (degrees == 0.0f || w <= 0 || h <= 0)
    {
        DrawGlyph(hdc, font, glyph, rc, color);
        return;
    }
    // The glyph's coverage (white on black, the same text path as DrawGlyph), then the color through it, turned
    // about the rect's center with bilinear filtering. A square around that center: a narrow rect (the 8px sort
    // icon's) would clip the glyph before it turns
    const int side = std::max(w, h) + 4;
    const int cx2 = rc.left + rc.right, cy2 = rc.top + rc.bottom; // twice the center
    const RECT box{ (cx2 - side) / 2, (cy2 - side) / 2, (cx2 - side) / 2 + side, (cy2 - side) / 2 + side };
    const RECT inner{ rc.left - box.left, rc.top - box.top, rc.right - box.left, rc.bottom - box.top }; // rc in the square
    HDC mem = CreateCompatibleDC(hdc);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), side, -side, 1, 32, BI_RGB };
    void* p = nullptr;
    HBITMAP bmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
    if (!bmp)
    {
        DeleteDC(mem);
        DrawGlyph(hdc, font, glyph, rc, color);
        return;
    }
    HGDIOBJ old = SelectObject(mem, bmp);
    FillSolid(mem, { 0, 0, side, side }, RGB(0, 0, 0));
    DrawGlyph(mem, font, glyph, inner, RGB(255, 255, 255));
    GdiFlush();
    const uint32_t r = GetRValue(color), g = GetGValue(color), b = GetBValue(color);
    for (uint32_t* px = static_cast<uint32_t*>(p), *end = px + static_cast<size_t>(side) * side; px < end; ++px)
    {
        const uint32_t a = std::max({ (*px >> 16) & 0xFF, (*px >> 8) & 0xFF, *px & 0xFF });
        *px = a << 24 | (r * a / 255) << 16 | (g * a / 255) << 8 | (b * a / 255); // premultiplied
    }
    EnsureGdiplus();
    {
        Gdiplus::Bitmap image(side, side, side * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(p));
        Gdiplus::Graphics gr(hdc);
        gr.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
        gr.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        const float cx = cx2 / 2.0f, cy = cy2 / 2.0f;
        gr.TranslateTransform(cx, cy);
        gr.RotateTransform(degrees);
        gr.TranslateTransform(-cx, -cy);
        gr.DrawImage(&image, Gdiplus::Rect(box.left, box.top, side, side));
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void DrawLabel(HDC hdc, HFONT font, const std::wstring& text, const RECT& rc, COLORREF color, UINT format)
{
    if (DWriteText::Available())
    {
        DWriteText::DrawLabel(hdc, font, text, rc, color, format);
        return;
    }
    HGDIOBJ old = SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    RECT r = rc;
    DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &r, format | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(hdc, old);
}

void DrawSectionIcon(HDC hdc, int section, const RECT& rc, UINT dpi)
{
    constexpr int kSections = 8, kScales = 4;
    if (section < 0 || section >= kSections)
        return;
    const int scale = dpi >= 192 ? 3 : dpi >= 144 ? 2 : dpi >= 120 ? 1 : 0;
    static std::unique_ptr<Gdiplus::Bitmap> cache[kSections][kScales];
    auto& bitmap = cache[section][scale];
    if (!bitmap)
    {
        EnsureGdiplus();
        const HMODULE module = GetModuleHandleW(nullptr);
        HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(IDR_SECTION_ICON_BASE + section * kScales + scale), RT_RCDATA);
        HGLOBAL data = res ? LoadResource(module, res) : nullptr;
        const void* bytes = data ? LockResource(data) : nullptr;
        if (!bytes)
            return;
        ComPtr<IStream> stream;
        stream.Attach(SHCreateMemStream(static_cast<const BYTE*>(bytes), SizeofResource(module, res)));
        if (!stream)
            return;
        // Decode once and copy: GDI+ keeps reading the stream otherwise
        Gdiplus::Bitmap decoded(stream.Get());
        if (decoded.GetLastStatus() != Gdiplus::Ok)
            return;
        bitmap.reset(decoded.Clone(0, 0, decoded.GetWidth(), decoded.GetHeight(), PixelFormat32bppPARGB));
    }
    Gdiplus::Graphics g(hdc);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.DrawImage(bitmap.get(), Gdiplus::Rect(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top));
}

int TextWidth(HDC hdc, HFONT font, const std::wstring& text)
{
    if (DWriteText::Available())
        return DWriteText::TextWidth(hdc, font, text);
    HGDIOBJ old = SelectObject(hdc, font);
    SIZE size{};
    GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(hdc, old);
    return size.cx;
}

struct PaintBuffer::Shared
{
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    uint32_t* bits = nullptr;
    int width = 0, height = 0;
    bool busy = false;
    ~Shared()
    {
        if (dc)
        {
            SelectObject(dc, old);
            DeleteDC(dc);
        }
        if (bmp)
            DeleteObject(bmp);
    }
};

namespace
{
HBITMAP MakeDib(HDC target, int width, int height, uint32_t** bits)
{
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB };
    void* p = nullptr;
    HBITMAP bmp = CreateDIBSection(target, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
    *bits = bmp ? static_cast<uint32_t*>(p) : nullptr;
    return bmp;
}
}

PaintBuffer::PaintBuffer(HDC target, int w, int h, const RECT& clear)
{
    w = std::max(1, w), h = std::max(1, h);
    thread_local PaintBuffer::Shared shared;
    if (!shared.busy)
    {
        if (w > shared.width || h > shared.height)
        {
            // Grow with some room so a window being resized wider doesn't reallocate on every step
            const int nw = std::max(shared.width, (w + w / 8 + 63) & ~63), nh = std::max(shared.height, (h + h / 8 + 63) & ~63);
            uint32_t* p = nullptr;
            if (HBITMAP bmp = MakeDib(target, nw, nh, &p))
            {
                if (!shared.dc)
                    shared.dc = CreateCompatibleDC(nullptr);
                HGDIOBJ prev = SelectObject(shared.dc, bmp);
                if (shared.bmp)
                    DeleteObject(shared.bmp);
                else
                    shared.old = prev;
                shared.bmp = bmp, shared.bits = p, shared.width = nw, shared.height = nh;
            }
        }
        if (shared.bits && w <= shared.width && h <= shared.height)
        {
            m_shared = &shared;
            shared.busy = true;
            dc = shared.dc, bits = shared.bits, width = shared.width;
            m_saved = SaveDC(dc);
            // What a fresh DIB would hold: zeros where this paint draws (the blit and the backdrop read nothing else).
            // GdiFlush first: the last paint's batched blit out of these pixels may not have run yet.
            GdiFlush();
            RECT z{};
            const RECT all{ 0, 0, w, h };
            if (IntersectRect(&z, &clear, &all))
                for (int y = z.top; y < z.bottom; ++y)
                    memset(bits + static_cast<size_t>(y) * width + z.left, 0, static_cast<size_t>(z.right - z.left) * 4);
            return;
        }
    }
    dc = CreateCompatibleDC(target);
    width = w;
    m_bmp = MakeDib(target, w, h, &bits);
    if (!m_bmp)
        m_bmp = CreateCompatibleBitmap(target, w, h);
    m_old = SelectObject(dc, m_bmp);
}

PaintBuffer::~PaintBuffer()
{
    if (m_shared)
    {
        RestoreDC(dc, m_saved);
        m_shared->busy = false;
        return;
    }
    SelectObject(dc, m_old);
    DeleteObject(m_bmp);
    DeleteDC(dc);
}

BufferedPaint::BufferedPaint(HWND hwnd, bool visibleOnly) : m_hwnd(hwnd)
{
    m_target = BeginPaint(hwnd, &m_ps);
    GetClientRect(hwnd, &m_rc);
    // A top-down 32bpp DIB: the backdrop material is composed into its pixels before the blit
    m_buffer = std::make_unique<PaintBuffer>(m_target, m_rc.right, m_rc.bottom, m_ps.rcPaint);
    // (the paint DC's visible region is in screen coordinates; the buffer's are the client area's. The shared buffer's
    // RestoreDC takes the clip off again, an own one is deleted)
    if (visibleOnly)
        if (HRGN rgn = CreateRectRgn(0, 0, 0, 0))
        {
            if (GetRandomRgn(m_target, rgn, SYSRGN) == 1)
            {
                POINT origin{ 0, 0 };
                ClientToScreen(hwnd, &origin);
                OffsetRgn(rgn, -origin.x, -origin.y);
                SelectClipRgn(m_buffer->dc, rgn);
            }
            DeleteObject(rgn);
        }
}

BufferedPaint::~BufferedPaint()
{
    const RECT& p = m_ps.rcPaint;
    if (m_buffer->bits && Backdrop::Active())
    {
        GdiFlush();
        Backdrop::Compose(m_hwnd, m_buffer->bits, m_buffer->width, p, nullptr, m_target);
    }
    BitBlt(m_target, p.left, p.top, p.right - p.left, p.bottom - p.top, m_buffer->dc, p.left, p.top, SRCCOPY);
    m_buffer.reset();
    EndPaint(m_hwnd, &m_ps);
}
