#include "DWriteText.h"
#include <dwrite.h>
#include <dwrite_1.h> // IDWriteBitmapRenderTarget1::SetTextAntialiasMode only, through QueryInterface (optional: see EnsureTarget)
#include <unordered_map>
#include <cmath>

#pragma comment(lib, "dwrite.lib")

namespace DWriteText
{
namespace
{
// --- Factory / GDI interop / rendering params: created once, kept for the process' lifetime ---

struct Globals
{
    ComPtr<IDWriteFactory> factory;
    ComPtr<IDWriteGdiInterop> interop;
    ComPtr<IDWriteRenderingParams> renderingParams; // grayscale, GDI-classic hinting (see G())
    bool ok = false;
};

Globals& G()
{
    static Globals g = []
    {
        Globals out;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(out.factory.GetAddressOf()))))
            return out;
        if (FAILED(out.factory->GetGdiInterop(&out.interop)))
        {
            out.factory.Reset();
            return out;
        }
        // Start from the system's defaults (gamma / contrast match whatever this machine is calibrated to) and
        // override the two knobs that make GDI's hinted ClearType look different from WinUI's XAML text:
        // clearTypeLevel 0 (IDWriteRenderingParams v1's documented way to force grayscale antialiasing - no color
        // fringing to fight with FastFiles' own alpha compositing; IDWriteBitmapRenderTarget1::SetTextAntialiasMode
        // would be the Windows 8.1+ way but this keeps working down to the Windows 8 / 7 minimum) and rendering
        // mode GDI_CLASSIC: the font's hinting snaps every stem to the pixel grid, in grayscale. Both unhinted modes
        // blur dense CJK: NATURAL_SYMMETRIC softened the 12px header (55% of ink pixels partial vs 43%), and NATURAL
        // ran the stacked horizontal strokes of the 20px SemiBold settings titles (置, 规) into solid blocks.
        FLOAT gamma = 1.8f, contrast = 0.5f;
        ComPtr<IDWriteRenderingParams> def;
        if (SUCCEEDED(out.factory->CreateRenderingParams(&def)))
        {
            gamma = def->GetGamma();
            contrast = def->GetEnhancedContrast();
        }
        if (FAILED(out.factory->CreateCustomRenderingParams(gamma, contrast, 0.0f, DWRITE_PIXEL_GEOMETRY_FLAT,
            DWRITE_RENDERING_MODE_GDI_CLASSIC, &out.renderingParams)))
            out.renderingParams.Reset(); // DrawGlyphRun(nullptr) falls back to the target's own default params
        out.ok = true;
        return out;
    }();
    return g;
}

// --- IDWriteTextFormat cache, keyed on the LOGFONT values (not the HFONT handle: handles get reused) ---

struct FormatKey
{
    std::wstring face;
    LONG height, weight;
    BYTE italic;
    bool operator==(const FormatKey& o) const { return height == o.height && weight == o.weight && italic == o.italic && face == o.face; }
};
struct FormatKeyHash
{
    size_t operator()(const FormatKey& k) const
    {
        size_t h = std::hash<std::wstring>()(k.face);
        h = h * 31 + std::hash<LONG>()(k.height);
        h = h * 31 + std::hash<LONG>()(k.weight);
        h = h * 31 + k.italic;
        return h;
    }
};

// A format plus the line metrics GDI's DrawTextW/GetTextExtentPoint32W would report for it (ascent-based vertical
// centering: DirectWrite centers a layout's ink box by its own rules, which read a hair off from GDI's tmHeight
// centering at a handful of sizes). Caching these together means TextWidth and DrawLabel always agree.
struct CachedFormat
{
    ComPtr<IDWriteTextFormat> format;
    float lineHeight = 0, baseline = 0; // ascent, in DIPs == pixels at this font's size (already device pixels)
    ComPtr<IDWriteInlineObject> ellipsis; // lazily created: CreateEllipsisTrimmingSign per draw call was the hot
                                          // path's biggest cost (profiled: SettingsView's many DT_END_ELLIPSIS rows)
};

std::unordered_map<FormatKey, CachedFormat, FormatKeyHash>& FormatCache()
{
    static std::unordered_map<FormatKey, CachedFormat, FormatKeyHash> cache;
    return cache;
}

// Builds (or reuses) the text format for this HFONT's LOGFONT. Returns nullptr if DirectWrite can't represent it
// (e.g. a face GDI resolved through font linking that has no matching family DirectWrite can enumerate); the caller
// falls back to GDI for that one draw.
CachedFormat* GetFormat(HFONT hfont)
{
    if (!hfont)
        return nullptr;
    Globals& g = G();
    if (!g.ok)
        return nullptr;
    LOGFONTW lf{};
    if (!GetObjectW(hfont, sizeof(lf), &lf))
        return nullptr;
    FormatKey key{ lf.lfFaceName, lf.lfHeight, lf.lfWeight, lf.lfItalic };
    auto& cache = FormatCache();
    if (auto it = cache.find(key); it != cache.end())
        return &it->second;

    ComPtr<IDWriteFont> font;
    // A fake HDC-less lookup: CreateFontFromLOGFONT only reads the LOGFONT's face/weight/style/stretch fields, not
    // a device context, but the interop API still asks for one of its matching overloads by face name.
    if (FAILED(g.interop->CreateFontFromLOGFONT(&lf, &font)))
        return nullptr;
    ComPtr<IDWriteFontFamily> family;
    if (FAILED(font->GetFontFamily(&family)))
        return nullptr;
    ComPtr<IDWriteLocalizedStrings> names;
    if (FAILED(family->GetFamilyNames(&names)))
        return nullptr;
    wchar_t localeName[LOCALE_NAME_MAX_LENGTH]{};
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH) == 0 ||
        FAILED(names->FindLocaleName(localeName, &index, &exists)) || !exists)
        index = 0;
    UINT32 len = 0;
    if (FAILED(names->GetStringLength(index, &len)))
        return nullptr;
    std::wstring familyName(len + 1, L'\0');
    if (FAILED(names->GetString(index, familyName.data(), len + 1)))
        return nullptr;
    familyName.resize(len);

    const float px = static_cast<float>(std::abs(lf.lfHeight));
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(g.factory->CreateTextFormat(familyName.c_str(), nullptr, font->GetWeight(), font->GetStyle(), font->GetStretch(),
        px, localeName, &format)))
        return nullptr;
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // Ascent / descent / lineGap for this family at this size, scaled like GDI's TEXTMETRIC (DirectWrite's metrics
    // are in font design units per em; multiply by size / designUnitsPerEm for pixels at this font size).
    CachedFormat entry;
    entry.format = format;
    entry.lineHeight = px * 1.2f;
    entry.baseline = px;
    ComPtr<IDWriteFontFace> face;
    if (SUCCEEDED(font->CreateFontFace(&face)))
    {
        DWRITE_FONT_METRICS fm{};
        face->GetMetrics(&fm);
        if (fm.designUnitsPerEm)
        {
            const float scale = px / fm.designUnitsPerEm;
            entry.baseline = fm.ascent * scale;
            entry.lineHeight = (fm.ascent + fm.descent + fm.lineGap) * scale;
        }
    }
    return &cache.emplace(key, std::move(entry)).first->second;
}

// --- IDWriteTextRenderer: feeds each glyph run DrawGlyphRun hands us straight to the GDI bitmap render target ---

struct BitmapRenderer : IDWriteTextRenderer
{
    IDWriteBitmapRenderTarget* target = nullptr; // not owned
    IDWriteRenderingParams* params = nullptr;     // not owned, may be null (use the target's default)
    COLORREF color = 0;

    // IUnknown: a static object (one per thread), never actually ref-counted away
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** obj) override
    {
        if (riid == __uuidof(IDWriteTextRenderer) || riid == __uuidof(IDWritePixelSnapping) || riid == __uuidof(IUnknown))
        {
            *obj = this;
            return S_OK;
        }
        *obj = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    // IDWritePixelSnapping
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* isDisabled) override { *isDisabled = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) override
    {
        *transform = { 1, 0, 0, 1, 0, 0 };
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixelsPerDip) override { *pixelsPerDip = 1.0f; return S_OK; }

    // IDWriteTextRenderer
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT baselineX, FLOAT baselineY, DWRITE_MEASURING_MODE measuringMode,
        DWRITE_GLYPH_RUN const* glyphRun, DWRITE_GLYPH_RUN_DESCRIPTION const*, IUnknown*) override
    {
        return target->DrawGlyphRun(baselineX, baselineY, measuringMode, glyphRun, params, color, nullptr);
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, DWRITE_UNDERLINE const*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, DWRITE_STRIKETHROUGH const*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return S_OK; }
};

// --- The GDI bitmap render target: one per thread, grown (never shrunk) to the largest rect drawn so far. A fresh
// DIB per DrawLabel call would dominate the 2000-repaint budget; reusing one is the difference between this being
// viable and not (measured: see the perf notes in the task report).

struct TargetCache
{
    ComPtr<IDWriteBitmapRenderTarget> target;
    int w = 0, h = 0;
    BitmapRenderer renderer;
};

TargetCache& ThreadTarget()
{
    thread_local TargetCache t;
    return t;
}

// Ensures the cached target is at least w x h, bound to `hdc`'s pixel format (DPI/compatible-DC pairing: a target
// created against one HDC draws correctly into any compatible one, but CreateBitmapRenderTarget wants a reference
// HDC to match the display's pixel format on creation).
IDWriteBitmapRenderTarget* EnsureTarget(HDC hdc, int w, int h)
{
    Globals& g = G();
    TargetCache& t = ThreadTarget();
    if (t.target && t.w >= w && t.h >= h)
        return t.target.Get();
    const int nw = std::max(w, t.w), nh = std::max(h, t.h);
    ComPtr<IDWriteBitmapRenderTarget> fresh;
    if (FAILED(g.interop->CreateBitmapRenderTarget(hdc, nw, nh, &fresh)))
        return nullptr;
    // IDWriteBitmapRenderTarget1::SetTextAntialiasMode(GRAYSCALE) would make this explicit, but that interface is
    // Windows 8.1+; grayscale is already guaranteed on this (Windows 8+) target by renderingParams' clearTypeLevel
    // 0 above, queried for whenever it's there (no behavior change, just avoids depending on the fallback path).
    if (ComPtr<IDWriteBitmapRenderTarget1> rt1; SUCCEEDED(fresh.As(&rt1)))
        rt1->SetTextAntialiasMode(DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    t.target = fresh;
    t.w = nw;
    t.h = nh;
    return t.target.Get();
}

// --- Layout construction shared by DrawLabel / TextWidth / the wrapped variants ---

ComPtr<IDWriteTextLayout> MakeLayout(CachedFormat& cf, const std::wstring& text, float maxWidth, float maxHeight,
    bool wrap, UINT dtFormat, bool measureOnly = false)
{
    Globals& g = G();
    // DT_WORDBREAK sites need wrapping; everything else (DrawLabel) is single-line, so the cached format's own
    // NO_WRAP stands. IDWriteTextFormat allows flipping word-wrap in place; flip it for this call and restore it
    // right after the layout is built, so the many more single-line callers keep seeing NO_WRAP on the shared format.
    IDWriteTextFormat* fmt = cf.format.Get();
    if (wrap)
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    ComPtr<IDWriteTextLayout> layout;
    const HRESULT hr = g.factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt, maxWidth, maxHeight, &layout);
    if (wrap)
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); // put the cached format back for the next (single-line) caller
    if (FAILED(hr))
        return nullptr;
    // TextWidth() only reads GetMetrics() afterwards: alignment and trimming never affect the measured width
    // (maxWidth is always huge there), so skipping both saves two calls on what's often a tight loop (e.g.
    // SettingsView::OpenDropdown measuring every option's width - the whole system font list for the font picker).
    if (measureOnly)
        return layout;
    layout->SetTextAlignment(dtFormat & DT_RIGHT ? DWRITE_TEXT_ALIGNMENT_TRAILING : dtFormat & DT_CENTER ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
    // DT_END_ELLIPSIS: trim the overflowing line (single-line: the only line; wrapped: the last one that fits
    // maxHeight) and append DirectWrite's ellipsis inline object, same place GDI's DT_END_ELLIPSIS puts it. The
    // inline object only depends on the format (face/size/weight), so it's created once per cached format instead
    // of once per draw call - CreateEllipsisTrimmingSign per call was the dominant cost on pages with many labels
    // (SettingsView: ~400% slower before this cache; see the task's perf notes).
    if (!wrap || (dtFormat & DT_END_ELLIPSIS))
    {
        if (!cf.ellipsis)
            g.factory->CreateEllipsisTrimmingSign(fmt, &cf.ellipsis);
        if (cf.ellipsis)
        {
            DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            layout->SetTrimming(&trimming, cf.ellipsis.Get());
        }
    }
    return layout;
}
}

bool Available()
{
    // Perf-comparison escape hatch only (FASTFILES_FORCE_GDI_TEXT=1): lets a test measure the old GDI DrawTextW
    // path on an unmodified binary, instead of needing two separate builds. Not read anywhere else; never set by
    // the app itself.
    static const bool forceGdi = [] { wchar_t v[4]{}; return GetEnvironmentVariableW(L"FASTFILES_FORCE_GDI_TEXT", v, 4) && v[0] == L'1'; }();
    return !forceGdi && G().ok;
}

void ClearCache()
{
    FormatCache().clear();
    // The bitmap render target doesn't depend on any HFONT/LOGFONT; keep it (it'll just get reused or grown).
}

int TextWidth(HDC hdc, HFONT font, const std::wstring& text)
{
    CachedFormat* cf = GetFormat(font);
    if (!cf || text.empty())
        return 0;
    ComPtr<IDWriteTextLayout> layout = MakeLayout(*cf, text, 1e6f, cf->lineHeight, false, DT_LEFT, true);
    if (!layout)
        return 0;
    DWRITE_TEXT_METRICS tm{};
    if (FAILED(layout->GetMetrics(&tm)))
        return 0;
    (void)hdc;
    return static_cast<int>(std::ceil(tm.widthIncludingTrailingWhitespace));
}

void DrawLabel(HDC hdc, HFONT font, const std::wstring& text, const RECT& rc, COLORREF color, UINT format)
{
    CachedFormat* cf = GetFormat(font);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (!cf || w <= 0 || h <= 0 || text.empty())
        return;
    ComPtr<IDWriteTextLayout> layout = MakeLayout(*cf, text, static_cast<float>(w), cf->lineHeight, false, format);
    if (!layout)
        return;

    Globals& g = G();
    IDWriteBitmapRenderTarget* target = EnsureTarget(hdc, w, h);
    if (!target)
        return;
    HDC mem = target->GetMemoryDC();
    // Seed the render target's own DIB with whatever is already at `rc` in the destination, so grayscale glyph
    // antialiasing blends against the real background (the file list's hover/selection tint, a card, etc.) exactly
    // like GDI's TRANSPARENT-mode ClearType text already does today.
    BitBlt(mem, 0, 0, w, h, hdc, rc.left, rc.top, SRCCOPY);
    DWRITE_TEXT_METRICS tm{};
    layout->GetMetrics(&tm);
    // DT_VCENTER (the default every caller but one asks for): center the line's ink by ascent+descent (tm.height),
    // matching DrawTextW's tmHeight-based centering closely enough that labels don't shift against neighboring
    // GDI-measured chrome. DT_BOTTOM / DT_TOP (InfoPane's preview header name, next to a fixed-height icon): anchor
    // to that edge instead, same as DrawTextW without DT_VCENTER.
    const float originY = (format & DT_BOTTOM) ? h - tm.height : (format & DT_TOP) ? 0.0f : (h - tm.height) / 2.0f;
    BitmapRenderer& renderer = ThreadTarget().renderer;
    renderer.target = target;
    renderer.params = g.renderingParams.Get();
    renderer.color = color;
    layout->Draw(nullptr, &renderer, 0, originY);
    BitBlt(hdc, rc.left, rc.top, w, h, mem, 0, 0, SRCCOPY);
}

int LineHeight(HFONT font)
{
    CachedFormat* cf = GetFormat(font);
    return cf ? static_cast<int>(std::ceil(cf->lineHeight)) : 0;
}

int WrappedHeight(HDC hdc, HFONT font, const std::wstring& text, int width)
{
    CachedFormat* cf = GetFormat(font);
    if (!cf || width <= 0)
        return 0;
    if (text.empty())
        return static_cast<int>(std::ceil(cf->lineHeight));
    ComPtr<IDWriteTextLayout> layout = MakeLayout(*cf, text, static_cast<float>(width), 1e6f, true, DT_LEFT);
    if (!layout)
        return 0;
    DWRITE_TEXT_METRICS tm{};
    if (FAILED(layout->GetMetrics(&tm)))
        return 0;
    (void)hdc;
    return static_cast<int>(std::ceil(tm.height));
}

void DrawWrapped(HDC hdc, HFONT font, const std::wstring& text, const RECT& rc, COLORREF color, UINT format)
{
    CachedFormat* cf = GetFormat(font);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (!cf || w <= 0 || h <= 0 || text.empty())
        return;
    ComPtr<IDWriteTextLayout> layout = MakeLayout(*cf, text, static_cast<float>(w), static_cast<float>(h), true, format);
    if (!layout)
        return;

    Globals& g = G();
    IDWriteBitmapRenderTarget* target = EnsureTarget(hdc, w, h);
    if (!target)
        return;
    HDC mem = target->GetMemoryDC();
    BitBlt(mem, 0, 0, w, h, hdc, rc.left, rc.top, SRCCOPY);
    BitmapRenderer& renderer = ThreadTarget().renderer;
    renderer.target = target;
    renderer.params = g.renderingParams.Get();
    renderer.color = color;
    layout->Draw(nullptr, &renderer, 0, 0);
    BitBlt(hdc, rc.left, rc.top, w, h, mem, 0, 0, SRCCOPY);
}
}
