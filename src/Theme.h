#pragma once
#include "Common.h"

// Files' surfaces (SetPalette: light measured from Files, dark from the same WinUI tokens) plus the fonts every custom control draws with.
struct MaterialRecipe
{
    COLORREF tint = 0;
    float tintOpacity = 0, luminosityOpacity = 0;
};

// Every color FastFiles draws comes from here: Theme.cpp (SetPalette, Material) is the only place color values are written.
struct Theme
{
    COLORREF chrome = 0;      // title / tab row
    COLORREF chromeHover = 0; // inactive tab under the mouse
    COLORREF chromeInk = 0, chromeMuted = 0; // text / glyphs on the tab row (the other theme's when the background color needs it)
    COLORREF chromePressed = 0;              // a tab row button pressed
    COLORREF layer = 0;       // nav row, sidebar, active tab
    COLORREF content = 0;     // file area card
    COLORREF toolbar = 0;     // command bar card
    COLORREF field = 0;       // omnibar, search and filter boxes
    COLORREF fieldStroke = 0;
    COLORREF cardEdge = 0;    // stroke of the toolbar and file area cards
    COLORREF line = 0;
    COLORREF columnGrip = 0, columnGripDrag = 0; // the column view's resize grip under the mouse / while dragging
    COLORREF selected = 0;
    COLORREF hover = 0;
    COLORREF pressed = 0;
    COLORREF ink = 0;
    COLORREF muted = 0;
    COLORREF accent = 0;
    COLORREF closeHover = 0;
    COLORREF closePressed = 0, closeInk = 0;  // the caption close button pressed / its glyph on the red
    COLORREF errorStroke = 0;                 // a text box with invalid input
    // WinUI's alpha fills (Subtle / Control / Card fills, shadows, strokes) are white or black at some alpha: the two
    // bases, and `overlay`, the one this theme's subtle fills use (white in dark, black in light)
    COLORREF fillWhite = 0, fillBlack = 0, overlay = 0;
    COLORREF textOnAccent = 0;                // TextOnAccentFillColorPrimary
    COLORREF critical = 0, criticalBackground = 0, criticalText = 0; // SystemFillColorCritical (+Background), error text
    COLORREF success = 0, successBackground = 0;                     // SystemFillColorSuccess (+Background)
    COLORREF flyout = 0, flyoutCard = 0, flyoutStroke = 0; // AcrylicInAppFillColorDefault fallback, a card on it, its stroke
    COLORREF altFill = 0;                     // ControlAltFillColor base (#F9F9F9 in light) the settings buttons use
    COLORREF dialogFooter = 0;                // ContentDialog's button row
    COLORREF sliderThumb = 0;                 // SliderThumbBackground over the media bar
    COLORREF scrubTrack = 0;                  // media bar track
    COLORREF smoke = 0;                       // SmokeFillColorDefault's color (the dimmed window under a dialog)
    COLORREF videoBackground = 0;             // MediaPlayerElement's letterbox before / around the picture
    COLORREF checkerLight = 0, checkerDark = 0; // transparency checkerboard under color swatches
    COLORREF channelRed = 0, channelGreen = 0, channelBlue = 0; // the color picker's R / G / B slider fills
    COLORREF glyphMask = 0;                   // white glyph drawn into a mask bitmap (recolored afterwards)
    // The own file list (FileList): rows under the mouse / selected (SubtleFillColorSecondary), pressed (Tertiary), the
    // header button under the mouse (ControlAltFillColorTertiary), all over the file area
    COLORREF listHover = 0, listPressed = 0, headerHover = 0;
    int listHoverPct = 0, listPressedPct = 0; // their alphas (the backdrop composes them over the material)
    COLORREF headerText = 0;                  // TextFillColorTertiary over the file area
    COLORREF headerDivider = 0;               // the 1px line in each header column splitter (ControlStrokeColorDefault, 0.8)
    COLORREF progressTrack = 0;               // ProgressBarBackground (ControlStrongStrokeColorDefault): the 1px track
    // WinUI CheckBox (the list's selection boxes): unchecked fill at rest / under the mouse / pressed
    // (ControlAltFillColorSecondary / Tertiary / Quarternary) and its stroke (ControlStrongStrokeColorDefault / Disabled
    // pressed) as a base and alpha mixed over what's under the box; checked: the accent, Secondary / Tertiary mixed toward
    // what's under it by checkAccentPct
    COLORREF checkFill[3]{}, checkStroke[2]{};
    int checkFillPct[3]{}, checkStrokePct[2]{}, checkAccentPct[3]{};
    int checkGlyphPressedPct = 0;             // the check mark pressed: TextOnAccentFillColorSecondary's alpha
    COLORREF groupDivider = 0, marqueeStroke = 0, focusStroke = 0; // the list's group header line, rubber band, focus frame
    // WinUI ScrollBar (ScrollOverlay): thumb and arrows in ControlStrongFillColorDefault (scrollInk at scrollThumbAlpha), a
    // hovered arrow in TextFillColorSecondary (scrollArrowHotAlpha), the expanded track in AcrylicInAppFillColorDefault
    COLORREF scrollInk = 0, scrollTrack = 0;
    int scrollThumbAlpha = 0, scrollArrowHotAlpha = 0;
    // The own list's cards / grid (GridLayoutPage): CardBackgroundFillColorDefault, App.Theme.CardBackgroundFillColorTertiary
    // (hover), CardStrokeColorDefault, SystemFillColorNeutralBackground (the cards' picture box), TextFillColorSecondary;
    // each a base and an alpha over what's under it
    COLORREF cardFillBase = 0, cardHoverBase = 0, cardStrokeBase = 0, neutralBase = 0, secondaryText = 0;
    int cardFillPct = 0, cardHoverPct = 0, cardStrokePct = 0, neutralPct = 0, secondaryTextPct = 0;
    // The rounded window's 1px border and shadow where DWM can't draw them (Windows 8 / 10: RoundFrame), alphas
    // active / inactive
    COLORREF windowBorder = 0;
    int windowBorderPct = 0, windowBorderInactivePct = 0, windowShadowPct = 0, windowShadowInactivePct = 0;
    // WinUI fill tokens Files' controls use, composited over `layer` (SetPalette derives them)
    COLORREF subtleHover = 0, subtlePressed = 0;                          // SubtleFillColorSecondary / Tertiary
    COLORREF controlFill = 0, controlFillHover = 0, controlFillPressed = 0; // ControlFillColorDefault / Secondary / Tertiary
    COLORREF controlStroke = 0;                                          // ButtonBorderBrush (ControlElevationBorder)
    // Buttons on the toolbar / nav row / address bar: the ink mixed into what's under them on hover / press. Files'
    // subtle fills (#09000000 / #06000000) were hardly visible here, so these are stronger, the press darker than the hover
    int buttonHoverPct = 0, buttonPressedPct = 0;
    COLORREF card = 0, cardHover = 0, cardPressed = 0, cardStroke = 0;   // SettingsCard over `content`
    bool dark = false;

    UINT dpi = 96;
    HFONT ui = nullptr;       // body text
    HFONT caption = nullptr;  // status bar, secondary text ("small" is a macro in rpcndr.h)
    HFONT icons = nullptr;    // 16px glyphs (nav, command bar)
    HFONT iconsMid = nullptr;   // 12px glyphs (new-tab plus)
    HFONT iconsSmall = nullptr; // 10px glyphs (chevrons, tab close, caption buttons)

    // Files' AppThemeFontFamily (empty: the standard face) and AppThemeBackgroundColor (0xAARRGGBB, alpha 0 = none),
    // set before Build / SetPalette
    std::wstring face;
    uint32_t tint = 0;
    COLORREF backdrop = CLR_INVALID; // the live backdrop material's average (Backdrop::Mean), CLR_INVALID = solid
    // WinUI layers over the material (SetPalette): LayerOnMicaBaseAlt for the nav row / sidebar, the toolbar card on it
    COLORREF layerFill = 0, toolbarFill = 0;
    int layerPct = 0, toolbarPct = 0;
    // Files' App.Theme.FileArea.BackgroundBrush over the layer (#C0FCFCFC / CardBackgroundFillColorDefault #0DFFFFFF)
    COLORREF contentFill = 0;
    int contentPct = 0;

    void Build(UINT newDpi);
    void Release();
    // WinUI MicaController's recipe for Mica (Base) or Mica Alt (BaseAlt); read before SetPalette (the palette follows the material)
    static MaterialRecipe Material(bool alt, bool dark);
    void SetPalette(bool darkPalette); // blueprint light or dark (--win-* under prefers-color-scheme: dark)
    int S(int v) const { return Scale(v, dpi); }
};

COLORREF Mix(COLORREF a, COLORREF b, int percentB);
// A backdrop key moved off a color it collides with (bit `step` of blue flipped: invisible, still unique)
COLORREF NudgeKey(COLORREF c, int step);
COLORREF TagFallbackColor(); // a tag whose stored color can't be read (tags.txt edited by hand)
void StartGdiplus(); // once per process, before any GDI+ drawing outside Theme.cpp
// Windows' "Show animations in Windows" (Settings > Accessibility > Visual effects)
bool AnimationsEnabled();
// The animation clock: milliseconds, never 0, from QueryPerformanceCounter (GetTickCount64 moves in 15.6 ms steps, so
// an animation sampled once per 16.7 ms frame advanced unevenly: some frames by 0, the next by two steps)
ULONGLONG AnimNow();
// Eased 0..1 for an animation started at AnimNow() value `start` (0 = finished)
double AnimProgress(ULONGLONG start, int ms);

// A control's hover / pressed fill moving between states the way WinUI's BrushTransition does (Duration 0:0:0.083
// on Files' toolbar buttons, the breadcrumb chevron and DefaultButtonStyle): linear, from wherever it is now.
// Levels: 0 none, 100 hover, 200 pressed. A press snaps (it shows at once, before what the click runs).
struct StateFade
{
    static constexpr int kMs = 83;
    int from = 0, to = 0;
    ULONGLONG start = 0;
    int Level() const;
    bool Running() const { return from != to && Level() != to; }
    void Set(int level, bool snap = false);
};
// The fill for a fade level: none (`base`), hover, pressed, and the mixes between
COLORREF FadeFill(COLORREF base, COLORREF hover, COLORREF pressed, int level);

// Anti-aliased shapes through GDI+; text stays on GDI so ClearType and font linking keep working
void FillRound(HDC hdc, const RECT& rc, int radius, COLORREF color);
void FrameRound(HDC hdc, const RECT& rc, int radius, COLORREF color);
// WinUI's keyboard focus visual around a control's rect (2px focus stroke 3px outside, 1px inner stroke in `under`)
void DrawFocusVisual(HDC hdc, const Theme& t, const RECT& rc, int radius, COLORREF under, const RECT* bounds = nullptr);
void FillTopRound(HDC hdc, const RECT& rc, int radius, COLORREF color);
void FillSolid(HDC hdc, const RECT& rc, COLORREF color);
// A GDI pattern (plain fills only; `paint` draws at (0,0)-relative coordinates) clipped to a rounded rect, its
// corner anti-aliased (unlike SelectClipRgn's hard edge) — the color picker's checkerboard swatch
void FillRoundPattern(HDC hdc, const RECT& rc, int radius, void (*paint)(HDC, const RECT&, void*), void* context);
void DrawGlyph(HDC hdc, HFONT font, const wchar_t* glyph, const RECT& rc, COLORREF color);
// DrawGlyph turned `degrees` clockwise about rc's center (a RotateTransform in an animation); 0 is DrawGlyph itself
void DrawGlyphTurned(HDC hdc, HFONT font, const wchar_t* glyph, const RECT& rc, COLORREF color, float degrees);
void DrawLabel(HDC hdc, HFONT font, const std::wstring& text, const RECT& rc, COLORREF color, UINT format = DT_LEFT);
int TextWidth(HDC hdc, HFONT font, const std::wstring& text);
// Files' sidebar section icon (res/sidebar, Sidebar::Section order) at the closest scale, drawn into rc
void DrawSectionIcon(HDC hdc, int section, const RECT& rc, UINT dpi);
void DrawEjectIcon(HDC hdc, const RECT& rc, COLORREF color); // Files' 12px eject glyph centered in rc
void DrawPaneIcon(HDC hdc, const RECT& rc, COLORREF color);  // Files' 16px Panes.Single icon filling rc (16 units)

// A 32bpp top-down DIB to paint one frame into. The UI thread reuses one (grown to the largest client seen) instead
// of a fresh CreateDIBSection per WM_PAINT, which cost ~0.7 ms at window size: `clear` is zeroed like a new DIB, the
// DC's state is saved and restored around each use. A nested paint (one already in use) gets its own bitmap.
class PaintBuffer
{
public:
    PaintBuffer(HDC target, int width, int height, const RECT& clear);
    ~PaintBuffer();
    HDC dc = nullptr;
    uint32_t* bits = nullptr; // nullptr if no DIB could be made (dc then holds a compatible bitmap)
    int width = 0;            // the row stride in pixels (>= the width asked for)

private:
    struct Shared;
    Shared* m_shared = nullptr; // the reused buffer, or nullptr for an own bitmap
    HBITMAP m_bmp = nullptr;
    HGDIOBJ m_old = nullptr;
    int m_saved = 0;
};

// Flicker-free painting: draw into a memory bitmap, blit on destruction
class BufferedPaint
{
public:
    // visibleOnly: the buffer is clipped to what of the window is on screen (its children cut out): a parent drawing
    // big shapes its children cover (the main window's cards) does only the part that shows; same pixels where seen
    explicit BufferedPaint(HWND hwnd, bool visibleOnly = false);
    ~BufferedPaint();
    HDC dc() const { return m_buffer->dc; }
    const RECT& rect() const { return m_rc; }

private:
    HWND m_hwnd;
    PAINTSTRUCT m_ps{};
    HDC m_target = nullptr;
    RECT m_rc{};
    std::unique_ptr<PaintBuffer> m_buffer;
};

namespace Glyph
{
inline constexpr wchar_t Back[] = L"\xE72B", Forward[] = L"\xE72A", Up[] = L"\xE74A", Refresh[] = L"\xE72C";
inline constexpr wchar_t Sidebar[] = L"\xE700", Add[] = L"\xE710", Cut[] = L"\xE8C6", Copy[] = L"\xE8C8", Paste[] = L"\xE77F";
inline constexpr wchar_t Rename[] = L"\xE8AC", Share[] = L"\xE72D", Delete[] = L"\xE74D", Properties[] = L"\xE946";
inline constexpr wchar_t Select[] = L"\xE8B3", Sort[] = L"\xE8CB", Layout[] = L"\xE8A9", Search[] = L"\xE721";
inline constexpr wchar_t Group[] = L"\xF168"; // Files' 分组依据 (LayoutPage's GroupBy HeaderIcon)
inline constexpr wchar_t ChevronLeft[] = L"\xE76B";
inline constexpr wchar_t ChevronRight[] = L"\xE76C", ChevronDown[] = L"\xE70D", More[] = L"\xE712", Close[] = L"\xE8BB";
inline constexpr wchar_t Minimize[] = L"\xE921", Maximize[] = L"\xE922", Restore[] = L"\xE923", Folder[] = L"\xE8B7";
inline constexpr wchar_t Admin[] = L"\xE7EF", RotateLeft[] = L"\xE7AD", RotateRight[] = L"\xE7AD", Wallpaper[] = L"\xE7C5", Font[] = L"\xE8D2";
inline constexpr wchar_t Filter[] = L"\xE71C", Pane[] = L"\xE8A0", DualPane[] = L"\xE89A", Settings[] = L"\xE713";
inline constexpr wchar_t ChevronUp[] = L"\xE70E", Color[] = L"\xE771", Customize[] = L"\xE8A7", Backdrop[] = L"\xEF1F", Calendar[] = L"\xEC92", Picture[] = L"\xE91B"; // Files' AppearancePage icons
inline constexpr wchar_t Palette[] = L"\xE790", Keyboard[] = L"\xE765", Tag[] = L"\xE8EC", Code[] = L"\xE943", Info[] = L"\xE946", Home[] = L"\xE80F", Repair[] = L"\xE90F";
inline constexpr wchar_t Branch[] = L"\xE943", Size[] = L"\xE9F9", Archive[] = L"\xE7B8", Extract[] = L"\xE8DE";
inline constexpr wchar_t RestoreDeleted[] = L"\xE7A7", Sync[] = L"\xE895";
inline constexpr wchar_t Globe[] = L"\xE774"; // Files' AboutPage 打开 GitHub 存储库 HeaderIcon
inline constexpr wchar_t Download[] = L"\xE896"; // the update button (Files: App.ThemedIcons.v4.AppUpdateAvailable)
}
