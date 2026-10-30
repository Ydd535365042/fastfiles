#include "Backdrop.h"
#include "Sandbox.h"
#include <shobjidl.h>
#include <thread>
#include <cmath>
#include <unordered_map>

namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

void StartGdiplus(); // Theme.cpp

namespace
{
// The wallpaper image is kept at 1/8 of the virtual screen; the blur makes it smooth enough to sample bilinearly
constexpr int kScale = 8;

struct Image
{
    RECT screen{};          // virtual screen (pixels)
    int w = 0, h = 0;
    std::vector<float> mica; // RGB 0..1, blurred
};

struct State
{
    HWND window = nullptr;
    int material = Backdrop::Solid;
    bool dark = false;
    uint32_t tint = 0;
    bool active = true;
    unsigned generation = 0;
    bool loading = false;
    std::unique_ptr<Image> image;
    std::vector<uint32_t> material32; // the recipe applied: 0x00RRGGBB per image pixel
    unsigned version = 0;             // bumped whenever material32 / the image change (the Sampled caches go stale)
    std::vector<Backdrop::Layer> layers;
    COLORREF base = 0;                // the solid page color (no material)
};
static State g;

// The material sampled for one window's client area at one screen position: Compose reads these pixels instead of
// sampling the 1/8 image bilinearly for every pixel of every frame (scrolling repaints a whole list ~60 times a
// second while the material under it doesn't change). Same arithmetic as before, so the same pixels.
struct Sampled
{
    POINT origin{ LONG_MIN, LONG_MIN }; // the client area's top-left on the screen these rows are for
    POINT seen{ LONG_MIN, LONG_MIN };   // the origin of the last paint (a window being moved never caches)
    unsigned version = ~0u;
    int stride = 0;                     // pixels per row: a little wider than the window, so a resize keeps the rows
    std::vector<uint32_t> px;
    std::vector<int> filled;            // per row: columns 0..filled[y] are sampled
};
std::unordered_map<HWND, Sampled>& SampledCache()
{
    static std::unordered_map<HWND, Sampled> cache;
    return cache;
}

// The background image, decoded (0x00RRGGBB, at most 2560 px on its long side)
struct Picture
{
    std::wstring path;
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    COLORREF mean = 0;
};
struct ImageState
{
    std::wstring path;
    int fit = 3, vertical = 1, horizontal = 1, opacity = 100;
    unsigned generation = 0;
    std::unique_ptr<Picture> picture;
};
static ImageState gi;

// --- Loading (worker thread) ---

void BoxBlur(std::vector<float>& px, int w, int h, int radius)
{
    if (radius <= 0 || w <= 0 || h <= 0)
        return;
    std::vector<float> tmp(px.size());
    auto pass = [&](const std::vector<float>& src, std::vector<float>& dst, bool horizontal)
    {
        const int n = horizontal ? w : h, lines = horizontal ? h : w;
        const float inv = 1.0f / (2 * radius + 1);
        for (int line = 0; line < lines; ++line)
            for (int c = 0; c < 3; ++c)
            {
                auto at = [&](int i) { i = std::clamp(i, 0, n - 1); return horizontal ? (line * w + i) * 3 + c : (i * w + line) * 3 + c; };
                float sum = 0;
                for (int i = -radius; i <= radius; ++i)
                    sum += src[at(i)];
                for (int i = 0; i < n; ++i)
                {
                    dst[at(i)] = sum * inv;
                    sum += src[at(i + radius + 1)] - src[at(i - radius)];
                }
            }
    };
    // Three box passes ~ a gaussian
    for (int k = 0; k < 3; ++k)
    {
        pass(px, tmp, true);
        pass(tmp, px, false);
    }
}

// Where an image of size iw x ih goes in a monitor rect for a DESKTOP_WALLPAPER_POSITION
std::vector<RECT> Placements(DESKTOP_WALLPAPER_POSITION pos, const RECT& m, const RECT& span, int iw, int ih)
{
    const int mw = m.right - m.left, mh = m.bottom - m.top;
    std::vector<RECT> out;
    switch (pos)
    {
    case DWPOS_CENTER:
        out.push_back({ m.left + (mw - iw) / 2, m.top + (mh - ih) / 2, m.left + (mw - iw) / 2 + iw, m.top + (mh - ih) / 2 + ih });
        break;
    case DWPOS_TILE:
        for (int y = m.top; y < m.bottom; y += std::max(1, ih))
            for (int x = m.left; x < m.right; x += std::max(1, iw))
                out.push_back({ x, y, x + iw, y + ih });
        break;
    case DWPOS_STRETCH:
        out.push_back(m);
        break;
    case DWPOS_FIT:
    case DWPOS_FILL:
    case DWPOS_SPAN:
    {
        const RECT& box = pos == DWPOS_SPAN ? span : m;
        const int bw = box.right - box.left, bh = box.bottom - box.top;
        const double sx = static_cast<double>(bw) / std::max(1, iw), sy = static_cast<double>(bh) / std::max(1, ih);
        const double s = pos == DWPOS_FIT ? std::min(sx, sy) : std::max(sx, sy);
        const int w = static_cast<int>(iw * s + 0.5), h = static_cast<int>(ih * s + 0.5);
        out.push_back({ box.left + (bw - w) / 2, box.top + (bh - h) / 2, box.left + (bw - w) / 2 + w, box.top + (bh - h) / 2 + h });
        break;
    }
    }
    return out;
}

struct Monitor
{
    RECT rc{};
    std::wstring path;
};

std::unique_ptr<Image> LoadWallpaper()
{
    StartGdiplus();
    auto img = std::make_unique<Image>();
    img->screen = { GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0 };
    img->screen.right = img->screen.left + std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    img->screen.bottom = img->screen.top + std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    img->w = std::max(1L, (img->screen.right - img->screen.left + kScale - 1) / kScale);
    img->h = std::max(1L, (img->screen.bottom - img->screen.top + kScale - 1) / kScale);

    // Monitors, their wallpapers, the fit and the background color: IDesktopWallpaper (Windows 8+), else the
    // classic desktop settings (Windows 7). Read only.
    std::vector<Monitor> monitors;
    DESKTOP_WALLPAPER_POSITION pos = DWPOS_FILL;
    COLORREF background = GetSysColor(COLOR_DESKTOP);
    const std::wstring testImage = Sandbox::Active() ? [] { wchar_t b[MAX_PATH]{}; return GetEnvironmentVariableW(L"FASTFILES_WALLPAPER", b, MAX_PATH) ? std::wstring(b) : std::wstring(); }() : std::wstring();
    ComPtr<IDesktopWallpaper> wallpaper;
    if (testImage.empty() && SUCCEEDED(CoCreateInstance(__uuidof(DesktopWallpaper), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wallpaper))))
    {
        wallpaper->GetPosition(&pos);
        wallpaper->GetBackgroundColor(&background);
        UINT count = 0;
        wallpaper->GetMonitorDevicePathCount(&count);
        for (UINT i = 0; i < count; ++i)
        {
            PWSTR id = nullptr, path = nullptr;
            Monitor m;
            if (SUCCEEDED(wallpaper->GetMonitorDevicePathAt(i, &id)) && SUCCEEDED(wallpaper->GetMonitorRECT(id, &m.rc)))
            {
                if (SUCCEEDED(wallpaper->GetWallpaper(id, &path)) && path)
                    m.path = path;
                monitors.push_back(m);
            }
            CoTaskMemFree(path);
            CoTaskMemFree(id);
        }
    }
    if (monitors.empty())
    {
        wchar_t path[MAX_PATH]{};
        if (testImage.empty())
        {
            SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0);
            wchar_t style[8]{}, tile[8]{};
            DWORD a = sizeof(style), b = sizeof(tile);
            RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WallpaperStyle", RRF_RT_REG_SZ, nullptr, style, &a);
            RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"TileWallpaper", RRF_RT_REG_SZ, nullptr, tile, &b);
            const int s = _wtoi(style);
            pos = _wtoi(tile) ? DWPOS_TILE : s == 2 ? DWPOS_STRETCH : s == 6 ? DWPOS_FIT : s == 10 ? DWPOS_FILL : s == 22 ? DWPOS_SPAN : DWPOS_CENTER;
        }
        else
            wcscpy_s(path, testImage.c_str());
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR mon, HDC, LPRECT, LPARAM p)
            {
                MONITORINFO mi{ sizeof(mi) };
                GetMonitorInfoW(mon, &mi);
                reinterpret_cast<std::vector<Monitor>*>(p)->push_back({ mi.rcMonitor, L"" });
                return TRUE;
            }, reinterpret_cast<LPARAM>(&monitors));
        for (auto& m : monitors)
            m.path = path;
    }

    // Draw at 1/8 scale
    Gdiplus::Bitmap canvas(img->w, img->h, PixelFormat32bppRGB);
    {
        Gdiplus::Graphics gfx(&canvas);
        gfx.Clear(Gdiplus::Color(255, GetRValue(background), GetGValue(background), GetBValue(background)));
        gfx.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
        gfx.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        auto toCanvas = [&](const RECT& r)
        {
            return Gdiplus::RectF(static_cast<float>(r.left - img->screen.left) / kScale, static_cast<float>(r.top - img->screen.top) / kScale,
                static_cast<float>(r.right - r.left) / kScale, static_cast<float>(r.bottom - r.top) / kScale);
        };
        std::wstring loaded;
        std::unique_ptr<Gdiplus::Bitmap> picture;
        for (const Monitor& m : monitors)
        {
            if (m.path.empty())
                continue;
            if (m.path != loaded)
            {
                loaded = m.path;
                picture.reset(Gdiplus::Bitmap::FromFile(m.path.c_str()));
                if (picture && picture->GetLastStatus() != Gdiplus::Ok)
                    picture.reset();
            }
            if (!picture)
                continue;
            const int iw = static_cast<int>(picture->GetWidth()), ih = static_cast<int>(picture->GetHeight());
            const auto mrect = toCanvas(m.rc);
            gfx.SetClip(mrect);
            for (const RECT& r : Placements(pos, m.rc, img->screen, iw, ih))
                gfx.DrawImage(picture.get(), toCanvas(r));
            gfx.ResetClip();
        }
    }
    std::vector<float> px(static_cast<size_t>(img->w) * img->h * 3);
    Gdiplus::BitmapData data{};
    Gdiplus::Rect all(0, 0, img->w, img->h);
    if (canvas.LockBits(&all, Gdiplus::ImageLockModeRead, PixelFormat32bppRGB, &data) == Gdiplus::Ok)
    {
        for (int y = 0; y < img->h; ++y)
        {
            const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const BYTE*>(data.Scan0) + static_cast<ptrdiff_t>(y) * data.Stride);
            for (int x = 0; x < img->w; ++x)
            {
                const uint32_t c = row[x];
                float* p = &px[(static_cast<size_t>(y) * img->w + x) * 3];
                p[0] = ((c >> 16) & 0xFF) / 255.0f;
                p[1] = ((c >> 8) & 0xFF) / 255.0f;
                p[2] = (c & 0xFF) / 255.0f;
            }
        }
        canvas.UnlockBits(&data);
    }
    // Mica samples the wallpaper very wide: a large radius at 1/8 scale
    img->mica = std::move(px);
    BoxBlur(img->mica, img->w, img->h, 10);
    return img;
}

// --- Recipes (UI thread) ---

// WinUI's MicaController defaults (Base / BaseAlt): the values are the theme's (Theme::Material)
struct Recipe
{
    float tint[3];
    float tintOpacity;
    float luminosityOpacity;
};

Recipe RecipeFor(int material, bool dark)
{
    const MaterialRecipe m = Theme::Material(material == Backdrop::MicaAlt, dark);
    return Recipe{ { GetRValue(m.tint) / 255.0f, GetGValue(m.tint) / 255.0f, GetBValue(m.tint) / 255.0f }, m.tintOpacity, m.luminosityOpacity };
}

// W3C blend helpers (non-separable modes)
float Lum(const float* c) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }
void SetLum(float* c, float l)
{
    const float d = l - Lum(c);
    for (int i = 0; i < 3; ++i)
        c[i] += d;
    const float lum = Lum(c), lo = std::min({ c[0], c[1], c[2] }), hi = std::max({ c[0], c[1], c[2] });
    for (int i = 0; i < 3; ++i)
    {
        if (lo < 0)
            c[i] = lum + (c[i] - lum) * lum / std::max(1e-6f, lum - lo);
        if (hi > 1)
            c[i] = lum + (c[i] - lum) * (1 - lum) / std::max(1e-6f, hi - lum);
    }
}

// AcrylicBrush's luminosity color: the tint with its value clamped (TintOpacity modifies the curve)
void LuminosityColor(const Recipe& r, float* out)
{
    const float hi = std::max({ r.tint[0], r.tint[1], r.tint[2] });
    const float v = std::clamp(hi, 0.125f, 0.965f);
    for (int i = 0; i < 3; ++i)
        out[i] = hi > 0 ? r.tint[i] * v / hi : v;
}

// The recipe over the blurred image: Luminosity blend (the backdrop's hue at the luminosity color's lightness), the
// tint on top, then Files' background color (App.Theme.BackgroundBrush) at its alpha.
void ApplyRecipe()
{
    ++g.version;
    g.material32.clear();
    if (!g.image || g.material == Backdrop::Solid)
        return;
    const Image& img = *g.image;
    const std::vector<float>& src = img.mica;
    const Recipe r = RecipeFor(g.material, g.dark);
    float lumColor[3];
    LuminosityColor(r, lumColor);
    const float tintA = static_cast<float>(g.tint >> 24) / 255.0f;
    const float tint[3] = { ((g.tint >> 16) & 0xFF) / 255.0f, ((g.tint >> 8) & 0xFF) / 255.0f, (g.tint & 0xFF) / 255.0f };
    g.material32.resize(static_cast<size_t>(img.w) * img.h);
    for (size_t i = 0; i < g.material32.size(); ++i)
    {
        float c[3] = { src[i * 3], src[i * 3 + 1], src[i * 3 + 2] };
        float lum[3] = { c[0], c[1], c[2] };
        SetLum(lum, Lum(lumColor));
        for (int k = 0; k < 3; ++k)
        {
            c[k] = c[k] + (lum[k] - c[k]) * r.luminosityOpacity;
            c[k] = c[k] + (r.tint[k] - c[k]) * r.tintOpacity;
            c[k] = c[k] + (tint[k] - c[k]) * tintA;
        }
        uint32_t out = 0;
        for (int k = 0; k < 3; ++k)
            out = out << 8 | static_cast<uint32_t>(std::clamp(c[k], 0.0f, 1.0f) * 255.0f + 0.5f);
        g.material32[i] = out;
    }
}

// Bilinear sample at a screen point
uint32_t Sample(int sx, int sy)
{
    const Image& img = *g.image;
    const float fx = std::clamp((sx - img.screen.left) / static_cast<float>(kScale) - 0.5f, 0.0f, static_cast<float>(img.w - 1));
    const float fy = std::clamp((sy - img.screen.top) / static_cast<float>(kScale) - 0.5f, 0.0f, static_cast<float>(img.h - 1));
    const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
    const int x1 = std::min(x0 + 1, img.w - 1), y1 = std::min(y0 + 1, img.h - 1);
    const int ax = static_cast<int>((fx - x0) * 256), ay = static_cast<int>((fy - y0) * 256);
    const uint32_t* m = g.material32.data();
    const uint32_t c00 = m[y0 * img.w + x0], c10 = m[y0 * img.w + x1], c01 = m[y1 * img.w + x0], c11 = m[y1 * img.w + x1];
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8)
    {
        const int a = (c00 >> shift) & 0xFF, b = (c10 >> shift) & 0xFF, c = (c01 >> shift) & 0xFF, d = (c11 >> shift) & 0xFF;
        const int top = a * (256 - ax) + b * ax, bottom = c * (256 - ax) + d * ax;
        out |= static_cast<uint32_t>(((top * (256 - ay) + bottom * ay) >> 16) & 0xFF) << shift;
    }
    return out;
}

void StartLoad()
{
    if (!g.window || g.loading)
        return;
    g.loading = true;
    const HWND window = g.window;
    const unsigned generation = ++g.generation;
    std::thread([window, generation]
        {
            const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            Image* img = LoadWallpaper().release();
            if (SUCCEEDED(co))
                CoUninitialize();
            if (!PostMessageW(window, Backdrop::kMsgReady, generation, reinterpret_cast<LPARAM>(img)))
                delete img;
        }).detach();
}

std::unique_ptr<Picture> LoadPicture(const std::wstring& path)
{
    StartGdiplus();
    std::unique_ptr<Gdiplus::Bitmap> source(Gdiplus::Bitmap::FromFile(path.c_str()));
    if (!source || source->GetLastStatus() != Gdiplus::Ok || !source->GetWidth() || !source->GetHeight())
        return nullptr;
    auto pic = std::make_unique<Picture>();
    pic->path = path;
    const double scale = std::min(1.0, 2560.0 / std::max(source->GetWidth(), source->GetHeight()));
    pic->w = std::max(1, static_cast<int>(source->GetWidth() * scale + 0.5));
    pic->h = std::max(1, static_cast<int>(source->GetHeight() * scale + 0.5));
    Gdiplus::Bitmap canvas(pic->w, pic->h, PixelFormat32bppRGB);
    {
        Gdiplus::Graphics gfx(&canvas);
        gfx.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        gfx.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        gfx.Clear(Gdiplus::Color(Gdiplus::Color::Transparent)); // (an RGB canvas: transparent parts of the picture come out empty)
        gfx.DrawImage(source.get(), Gdiplus::Rect(0, 0, pic->w, pic->h));
    }
    Gdiplus::BitmapData data{};
    Gdiplus::Rect all(0, 0, pic->w, pic->h);
    if (canvas.LockBits(&all, Gdiplus::ImageLockModeRead, PixelFormat32bppRGB, &data) != Gdiplus::Ok)
        return nullptr;
    pic->px.resize(static_cast<size_t>(pic->w) * pic->h);
    uint64_t r = 0, gr = 0, b = 0;
    for (int y = 0; y < pic->h; ++y)
    {
        const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const BYTE*>(data.Scan0) + static_cast<ptrdiff_t>(y) * data.Stride);
        for (int x = 0; x < pic->w; ++x)
        {
            const uint32_t c = row[x] & 0xFFFFFF;
            pic->px[static_cast<size_t>(y) * pic->w + x] = c;
            r += (c >> 16) & 0xFF;
            gr += (c >> 8) & 0xFF;
            b += c & 0xFF;
        }
    }
    canvas.UnlockBits(&data);
    const uint64_t n = static_cast<uint64_t>(pic->w) * pic->h;
    pic->mean = RGB(r / n, gr / n, b / n);
    return pic;
}

// Where the image goes in a client area of cw x ch (XAML Image: Stretch, then the alignments)
RECT Placement(int cw, int ch)
{
    const Picture& p = *gi.picture;
    double w = p.w, h = p.h;
    switch (gi.fit)
    {
    case 1: w = cw; h = ch; break;                        // Fill
    case 2: { const double s = std::min(cw / w, ch / h); w *= s; h *= s; break; } // Uniform
    case 3: { const double s = std::max(cw / w, ch / h); w *= s; h *= s; break; } // UniformToFill
    default: break;                                       // None
    }
    auto place = [](int align, int room, int size) { return align == 0 ? 0 : align == 2 ? room - size : (room - size) / 2; };
    const int iw = std::max(1, static_cast<int>(w + 0.5)), ih = std::max(1, static_cast<int>(h + 0.5));
    const int x = place(gi.horizontal, cw, iw), y = place(gi.vertical, ch, ih);
    return { x, y, x + iw, y + ih };
}

// The image pixel at a point of the placed rectangle (bilinear), false outside it
bool PictureAt(const RECT& placed, int x, int y, uint32_t& out)
{
    if (x < placed.left || y < placed.top || x >= placed.right || y >= placed.bottom)
        return false;
    const Picture& p = *gi.picture;
    const float fx = std::clamp((x - placed.left + 0.5f) * p.w / (placed.right - placed.left) - 0.5f, 0.0f, static_cast<float>(p.w - 1));
    const float fy = std::clamp((y - placed.top + 0.5f) * p.h / (placed.bottom - placed.top) - 0.5f, 0.0f, static_cast<float>(p.h - 1));
    const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy), x1 = std::min(x0 + 1, p.w - 1), y1 = std::min(y0 + 1, p.h - 1);
    const int ax = static_cast<int>((fx - x0) * 256), ay = static_cast<int>((fy - y0) * 256);
    const uint32_t c00 = p.px[y0 * p.w + x0], c10 = p.px[y0 * p.w + x1], c01 = p.px[y1 * p.w + x0], c11 = p.px[y1 * p.w + x1];
    out = 0;
    for (int shift = 16; shift >= 0; shift -= 8)
    {
        const int a = (c00 >> shift) & 0xFF, b = (c10 >> shift) & 0xFF, c = (c01 >> shift) & 0xFF, d = (c11 >> shift) & 0xFF;
        const int top = a * (256 - ax) + b * ax, bottom = c * (256 - ax) + d * ax;
        out |= static_cast<uint32_t>(((top * (256 - ay) + bottom * ay) >> 16) & 0xFF) << shift;
    }
    return true;
}
}

namespace
{
// One row of the material under a client area whose top-left is at `origin` on the screen: Compose's bilinear step
// (the same arithmetic as before, so the same pixels), columns x0..x1
void SampleRow(POINT origin, int y, int x0, int x1, uint32_t* out)
{
    const Image& img = *g.image;
    const float fy = std::clamp((origin.y + y - img.screen.top) / static_cast<float>(kScale) - 0.5f, 0.0f, static_cast<float>(img.h - 1));
    const int r0 = static_cast<int>(fy);
    const uint32_t* top = g.material32.data() + static_cast<ptrdiff_t>(r0) * img.w;
    const uint32_t* bottom = g.material32.data() + static_cast<ptrdiff_t>(std::min(r0 + 1, img.h - 1)) * img.w;
    const int ay = static_cast<int>((fy - r0) * 256);
    for (int x = x0; x < x1; ++x)
    {
        const float fx = std::clamp((origin.x + x - img.screen.left) / static_cast<float>(kScale) - 0.5f, 0.0f, static_cast<float>(img.w - 1));
        const int a0 = static_cast<int>(fx), a1 = std::min(a0 + 1, img.w - 1), cw = static_cast<int>((fx - a0) * 256);
        const uint32_t c00 = top[a0], c10 = top[a1], c01 = bottom[a0], c11 = bottom[a1];
        uint32_t m = 0;
        for (int shift = 16; shift >= 0; shift -= 8)
        {
            const int a = (c00 >> shift) & 0xFF, b = (c10 >> shift) & 0xFF, cc = (c01 >> shift) & 0xFF, d = (c11 >> shift) & 0xFF;
            const int t = a * (256 - cw) + b * cw, u = cc * (256 - cw) + d * cw;
            m |= static_cast<uint32_t>(((t * (256 - ay) + u * ay) >> 16) & 0xFF) << shift;
        }
        out[x - x0] = m;
    }
}

// The material under a top-level window's client area: from the cache once the window has painted twice at the same
// screen position with the same material (rows filled as they're first needed, kept across resizes). Its children read
// their part of it at their offset, so a child that moves or resizes inside the window (a splitter drag: the list,
// the details pane) keeps reading the cache instead of sampling every row again. A window being moved on the screen
// has no cache and gets nullptr (Compose samples those rows itself, as it always did).
Sampled* SampledFor(HWND hwnd, POINT origin, int width, int height)
{
    auto& cache = SampledCache();
    Sampled& s = cache[hwnd];
    const bool same = s.seen.x == origin.x && s.seen.y == origin.y;
    s.seen = origin;
    if (s.origin.x == origin.x && s.origin.y == origin.y && s.version == g.version && width <= s.stride)
    {
        if (static_cast<int>(s.filled.size()) < height)
        {
            s.px.resize(static_cast<size_t>(s.stride) * height);
            s.filled.resize(static_cast<size_t>(height), 0);
        }
        return &s;
    }
    if (!same)
        return nullptr; // moving (or the first paint here): no cache to build yet
    // Windows that are gone: their entries go (the map only ever holds the few windows that compose)
    for (auto it = cache.begin(); it != cache.end();)
        it = it->first != hwnd && !IsWindow(it->first) ? cache.erase(it) : std::next(it);
    Sampled& e = cache[hwnd];
    e.origin = origin;
    e.version = g.version;
    e.stride = (width + width / 4 + 63) & ~63; // room for a resize wider (beyond it: built again, once)
    e.px.assign(static_cast<size_t>(e.stride) * height, 0);
    e.filled.assign(static_cast<size_t>(height), 0);
    return &e;
}
}

namespace Backdrop
{
void Configure(HWND window, int material, bool dark, uint32_t tint)
{
    if (window != g.window)
    {
        g.window = window;
        SetPropW(GetAncestor(window, GA_ROOT), L"FastFiles.Backdrop", reinterpret_cast<HANDLE>(1));
    }
    const bool changed = material != g.material || dark != g.dark || tint != g.tint;
    g.material = material;
    g.dark = dark;
    g.tint = tint;
    if (material == Solid)
        return;
    if (!g.image)
        StartLoad();
    else if (changed || g.material32.empty())
        ApplyRecipe();
}

bool OnReady(WPARAM wp, LPARAM lp)
{
    std::unique_ptr<Image> img(reinterpret_cast<Image*>(lp));
    if (static_cast<unsigned>(wp) != g.generation)
        return false; // an older load
    g.loading = false;
    g.image = std::move(img);
    ApplyRecipe();
    return g.material != Solid;
}

void Reload()
{
    g.loading = false;
    if (g.material != Solid)
        StartLoad();
    else
        g.image.reset();
}

void SetActive(bool active) { g.active = active; }

bool Live() { return g.material != Solid && g.active && g.image && !g.material32.empty(); }

COLORREF Mean(const RECT& screen)
{
    if (!Live())
        return 0;
    uint64_t r = 0, gr = 0, b = 0, n = 0;
    const int stepX = std::max(1L, (screen.right - screen.left) / 24), stepY = std::max(1L, (screen.bottom - screen.top) / 24);
    for (int y = screen.top; y < screen.bottom; y += stepY)
        for (int x = screen.left; x < screen.right; x += stepX)
        {
            const uint32_t c = Sample(x, y);
            r += (c >> 16) & 0xFF;
            gr += (c >> 8) & 0xFF;
            b += c & 0xFF;
            ++n;
        }
    return n ? RGB(r / n, gr / n, b / n) : 0;
}

void SetLayers(const std::vector<Layer>& layers) { g.layers = layers; }

void SetBase(COLORREF base) { g.base = base; }

bool HasImage() { return gi.picture != nullptr; }
COLORREF ImageMean() { return gi.picture ? gi.picture->mean : 0; }
bool Active() { return Live() || gi.picture; }

void SetImage(const std::wstring& path, int fit, int vertical, int horizontal, int opacity)
{
    gi.fit = std::clamp(fit, 0, 3);
    gi.vertical = std::clamp(vertical, 0, 2);
    gi.horizontal = std::clamp(horizontal, 0, 2);
    gi.opacity = std::clamp(opacity, 10, 100);
    if (path == gi.path)
        return;
    gi.path = path;
    gi.picture.reset();
    const unsigned generation = ++gi.generation;
    if (path.empty() || !g.window)
        return;
    // Decoded on a worker: a large photo takes a moment
    const HWND window = g.window;
    std::thread([window, generation, path]
        {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            Picture* pic = LoadPicture(path).release();
            if (!PostMessageW(window, kMsgImage, generation, reinterpret_cast<LPARAM>(pic)))
                delete pic;
        }).detach();
}

bool OnImage(WPARAM wp, LPARAM lp)
{
    std::unique_ptr<Picture> pic(reinterpret_cast<Picture*>(lp));
    if (static_cast<unsigned>(wp) != gi.generation)
        return false;
    gi.picture = std::move(pic);
    return gi.picture != nullptr;
}

void Compose(HWND hwnd, uint32_t* bits, int width, const RECT& rc, const std::vector<Layer>* extra, HDC paint)
{
    if (!Active() || g.layers.empty() || !GetPropW(GetAncestor(hwnd, GA_ROOT), L"FastFiles.Backdrop"))
        return;
    POINT origin{ 0, 0 };
    ClientToScreen(hwnd, &origin);
    // The background image is placed in the main window's client area (Files: under the whole MainPage)
    const bool live = Live();
    RECT placed{};
    POINT main{ 0, 0 };
    if (gi.picture && g.window)
    {
        RECT client{};
        GetClientRect(g.window, &client);
        ClientToScreen(g.window, &main);
        placed = Placement(client.right, client.bottom);
    }
    const uint32_t base = static_cast<uint32_t>(GetRValue(g.base)) << 16 | static_cast<uint32_t>(GetGValue(g.base)) << 8 | GetBValue(g.base);
    // Keys as 0x00RRGGBB (DIB order)
    struct Key { uint32_t key; const Layer* layer; };
    std::vector<Key> keys;
    for (const Layer& l : g.layers)
        keys.push_back({ static_cast<uint32_t>(GetRValue(l.key)) << 16 | static_cast<uint32_t>(GetGValue(l.key)) << 8 | GetBValue(l.key), &l });
    if (extra)
        for (const Layer& l : *extra)
            keys.push_back({ static_cast<uint32_t>(GetRValue(l.key)) << 16 | static_cast<uint32_t>(GetGValue(l.key)) << 8 | GetBValue(l.key), &l });
    auto mix = [](uint32_t a, COLORREF b, int p)
    {
        const int br = GetRValue(b), bg = GetGValue(b), bb = GetBValue(b);
        const int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
        return static_cast<uint32_t>((ar * (100 - p) + br * p) / 100) << 16 | static_cast<uint32_t>((ag * (100 - p) + bg * p) / 100) << 8 |
            static_cast<uint32_t>((ab * (100 - p) + bb * p) / 100);
    };
    // This runs over every key pixel on every paint (and the window repaints as it moves over the material), so the
    // per-pixel work is table lookups. Each layer's fills act on each channel on its own: one 256-entry table per
    // channel gives exactly what mixing fill by fill gives.
    struct Table { uint8_t ch[3][256]; };
    std::vector<Table> tables(keys.size());
    for (size_t i = 0; i < keys.size(); ++i)
        for (int v = 0; v < 256; ++v)
        {
            uint32_t m = static_cast<uint32_t>(v) << 16 | static_cast<uint32_t>(v) << 8 | static_cast<uint32_t>(v);
            for (int k = 0; k < kLayerFills; ++k)
                if (keys[i].layer->pct[k] > 0)
                    m = mix(m, keys[i].layer->fill[k], keys[i].layer->pct[k]);
            tables[i].ch[0][v] = static_cast<uint8_t>(m >> 16);
            tables[i].ch[1][v] = static_cast<uint8_t>(m >> 8);
            tables[i].ch[2][v] = static_cast<uint8_t>(m);
        }
    // The material under this window: the top-level window's cache (SampledFor), read at this window's offset in it
    const int x0 = std::max(0L, rc.left), x1 = std::min<int>(rc.right, width);
    Sampled* sampled = nullptr;
    POINT offset{ 0, 0 }; // this window's client top-left in the top-level window's client area
    std::vector<uint32_t> direct; // rows the cache can't give (window being moved, or outside it), sampled as before
    if (live && x1 > x0)
    {
        const HWND root = GetAncestor(hwnd, GA_ROOT);
        POINT rootOrigin{ 0, 0 };
        ClientToScreen(root, &rootOrigin);
        offset = { origin.x - rootOrigin.x, origin.y - rootOrigin.y };
        RECT client{}, rootClient{};
        GetClientRect(hwnd, &client);
        GetClientRect(root, &rootClient);
        // Sized by the windows, not by `width` (the paint buffer's row stride, which can be wider: PaintBuffer)
        if (offset.x >= 0 && offset.y >= 0)
            sampled = SampledFor(root, rootOrigin, std::max<int>(rootClient.right, offset.x + std::max<int>(client.right, x1)),
                std::max<int>(rootClient.bottom, offset.y + std::max<int>(client.bottom, rc.bottom)));
        direct.resize(static_cast<size_t>(x1 - x0));
    }
    // The rectangles to compose: rc, or its parts in the paint DC's visible region (screen coordinates)
    const RECT whole{ x0, std::max(0L, rc.top), x1, rc.bottom };
    std::vector<RECT> parts;
    if (paint)
        if (HRGN rgn = CreateRectRgn(0, 0, 0, 0))
        {
            if (GetRandomRgn(paint, rgn, SYSRGN) == 1)
                if (const DWORD size = GetRegionData(rgn, 0, nullptr))
                {
                    std::vector<BYTE> buf(size);
                    auto* data = reinterpret_cast<RGNDATA*>(buf.data());
                    if (GetRegionData(rgn, size, data) == size)
                    {
                        const RECT* r = reinterpret_cast<const RECT*>(data->Buffer);
                        for (DWORD i = 0; i < data->rdh.nCount; ++i)
                        {
                            RECT part = r[i];
                            OffsetRect(&part, -origin.x, -origin.y);
                            if (IntersectRect(&part, &part, &whole))
                                parts.push_back(part);
                        }
                        if (parts.empty())
                            parts.push_back(RECT{}); // nothing of rc is visible
                    }
                }
            DeleteObject(rgn);
        }
    if (parts.empty())
        parts.push_back(whole);
    for (const RECT& part : parts)
    for (int y = part.top; y < part.bottom; ++y)
    {
        uint32_t* row = bits + static_cast<ptrdiff_t>(y) * width;
        const uint32_t* under = nullptr;
        const int xa = part.left, xb = part.right;
        const int cy = y + offset.y, cx1 = xb + offset.x;
        if (sampled && cy < static_cast<int>(sampled->filled.size()) && cx1 <= sampled->stride)
        {
            // The columns this paint needs that no earlier paint sampled (the next paints read them)
            uint32_t* cached = sampled->px.data() + static_cast<ptrdiff_t>(cy) * sampled->stride;
            int& done = sampled->filled[static_cast<size_t>(cy)];
            if (done < cx1)
            {
                const POINT rootOrigin{ origin.x - offset.x, origin.y - offset.y };
                SampleRow(rootOrigin, cy, done, cx1, cached + done);
                done = cx1;
            }
            under = cached + offset.x;
        }
        else if (!direct.empty())
        {
            SampleRow(origin, y, xa, xb, direct.data());
            under = direct.data() - xa;
        }
        uint32_t lastKey = 0xFF000000; // (no pixel has it: the masked colors are 24-bit)
        int lastHit = -1;
        for (int x = xa; x < xb; ++x)
        {
            const uint32_t c = row[x] & 0xFFFFFF;
            if (c != lastKey)
            {
                lastKey = c;
                lastHit = -1;
                for (size_t i = 0; i < keys.size(); ++i)
                    if (keys[i].key == c)
                    {
                        lastHit = static_cast<int>(i);
                        break;
                    }
            }
            if (lastHit < 0)
                continue;
            // The material (or the solid page color), the image over it at its opacity, then the layers
            uint32_t m = under ? under[x] : base;
            if (uint32_t img = 0; gi.picture && PictureAt(placed, origin.x + x - main.x, origin.y + y - main.y, img))
                m = mix(m, RGB((img >> 16) & 0xFF, (img >> 8) & 0xFF, img & 0xFF), gi.opacity);
            const Table& t = tables[static_cast<size_t>(lastHit)];
            row[x] = static_cast<uint32_t>(t.ch[0][(m >> 16) & 0xFF]) << 16 | static_cast<uint32_t>(t.ch[1][(m >> 8) & 0xFF]) << 8 |
                t.ch[2][m & 0xFF];
        }
    }
}
}
