#pragma once
#include "Common.h"

// DirectWrite text drawing behind Theme.cpp's DrawLabel / DrawGlyph / TextWidth, so CJK (and every other) text gets
// DirectWrite's grayscale-antialiased, unhinted shapes instead of GDI's ClearType pixel-snapped ones (Files/WinUI
// always draws through DirectWrite). dwrite.dll and DWriteCreateFactory exist from Windows 7 on; everything here is
// IDWriteFactory (v1) only, so it loads on Windows 8 same as GDI. If the factory can't be created (very old or
// locked-down system, or a sandboxed test), Available() returns false and every call site keeps using GDI.
namespace DWriteText
{
// Call once at process start (main.cpp); safe to call more than once. False: DirectWrite unavailable, GDI only.
bool Available();

// Same contract as Theme.cpp's DrawLabel: single line, DT_VCENTER, DT_NOPREFIX (no accelerator underline), ellipsis
// at the end when it doesn't fit, DT_LEFT / DT_RIGHT / DT_CENTER horizontal alignment (bit flags from `format`).
// `hdc` supplies the HFONT's current DPI context (unused directly: the font's own LOGFONT already carries the
// scaled pixel height) and is where the glyphs land.
void DrawLabel(HDC hdc, HFONT font, const std::wstring& text, const RECT& rc, COLORREF color, UINT format);

// Same metric contract as GetTextExtentPoint32W through this HFONT: the ideal (no wrapping) width of `text` in
// pixels, using the same text format DrawLabel lays out with (so layout and measurement never disagree).
int TextWidth(HDC hdc, HFONT font, const std::wstring& text);

// Multi-line measuring / drawing for the few DT_WORDBREAK sites (InfoPane's property grid, StatusCenter's header,
// SettingsView's notes, the dialogs' body text). `rc` width is the wrap column; DT_EDITCONTROL's extra line leading
// is folded into the font metrics the same way GDI's is (negligible at these sizes) so callers see matching heights.
int WrappedHeight(HDC hdc, HFONT font, const std::wstring& text, int width);
void DrawWrapped(HDC hdc, HFONT font, const std::wstring& text, const RECT& rc, COLORREF color, UINT format);
// This font's single line height (ascent + descent + line gap, device pixels) - for callers that need to turn a
// WrappedHeight() back into a line count the same way GDI's tmHeight used to (StatusCenter's 1-or-2-line header).
int LineHeight(HFONT font);

// Drops every cached IDWriteTextFormat (DarkMode / DPI change rebuilds every HFONT in Theme::Build, which can reuse
// a freed handle's address; the format cache keys on LOGFONT values already, but a full rebuild is a good point to
// let go of formats nothing references anymore).
void ClearCache();
}
