#include "LayoutProbe.h"
#include "Sandbox.h"
#include <cstdio>

namespace
{
struct Frame
{
    int w = 0, h = 0;
    POINT origin{};               // the client area's top left on the screen
    std::vector<uint32_t> px;
    // The pixel at a screen point (false: outside this frame)
    bool At(int sx, int sy, uint32_t& out) const
    {
        const int x = sx - origin.x, y = sy - origin.y;
        if (x < 0 || y < 0 || x >= w || y >= h)
            return false;
        out = px[static_cast<size_t>(y) * w + x];
        return true;
    }
};

// What one erase / paint of a step drew: the pixels of its window it changed (every kStep-th one each way, on the
// screen) and what they showed right after it
struct Sample
{
    int x, y;
    uint32_t v;
};
struct Mid
{
    std::vector<Sample> drawn;
    UINT msg = 0;
    wchar_t cls[64]{};
};
// An erase / paint under way: its window's pixels just before it
struct Pending
{
    RECT area{};
    POINT origin{};
    std::vector<uint64_t> px;
};
constexpr int kStep = 3;
constexpr size_t kMaxMids = 400;

struct State
{
    int depth = 0;
    ULONGLONG since = 0; // the outermost Start (a step never finished is dropped after a while)
    HWND hwnd = nullptr;
    const wchar_t* tag = L"";
    Frame before;
    HRGN beforeShape = nullptr; // the window's region at the start (rounded corners), window coordinates; null: none
    POINT beforeWindow{};       // the window's top left on the screen at the start
    Frame early; // (Uncovered: the new size, not laid out yet)
    bool hasEarly = false;
    HRGN shape = nullptr; // the window's region then (Windows 8 / 10's rounded corners), client coordinates; null: none
    POINT shapeAt{};      // the client area's top left in the window
    HHOOK pre = nullptr, post = nullptr;
    std::vector<Pending> pending; // (a paint can erase inside it: nested)
    std::vector<Mid> mids;
} g;

bool Enabled()
{
    static const bool on = Sandbox::Active() && GetEnvironmentVariableW(L"FF_LAYOUT_TRACE", nullptr, 0) > 0;
    return on;
}

bool Grab(HWND hwnd, Frame& f);
bool Same(uint32_t a, uint32_t b);

bool Watched(HWND w, UINT msg)
{
    // (the top-level window's own WM_NCPAINT draws outside its client area)
    return g.hwnd && (msg == WM_ERASEBKGND || msg == WM_PAINT || (msg == WM_NCPAINT && w != g.hwnd));
}

// The samples of `area` (screen) in the window's frame, in a fixed order (kOutside: not in the frame)
constexpr uint64_t kOutside = 1ull << 32;
void SampleArea(const Frame& f, const RECT& area, std::vector<uint64_t>& out)
{
    out.clear();
    for (int y = area.top; y < area.bottom; y += kStep)
        for (int x = area.left; x < area.right; x += kStep)
        {
            uint32_t v = 0;
            out.push_back(f.At(x, y, v) ? (v & 0xFFFFFFu) : kOutside);
        }
}

// Before / after each erase / paint of any window of the thread during a step. A window cleared before it's drawn
// shows the clear right after its erase, which neither the frame before nor the one after has: a flash
LRESULT CALLBACK BeforePaint(int code, WPARAM wp, LPARAM lp)
{
    const auto* m = reinterpret_cast<const CWPSTRUCT*>(lp);
    if (code == HC_ACTION && Watched(m->hwnd, m->message))
    {
        Pending p;
        Frame f;
        GetWindowRect(m->hwnd, &p.area);
        if (Grab(g.hwnd, f))
        {
            p.origin = f.origin;
            SampleArea(f, p.area, p.px);
        }
        g.pending.push_back(std::move(p));
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK AfterPaint(int code, WPARAM wp, LPARAM lp)
{
    const auto* m = reinterpret_cast<const CWPRETSTRUCT*>(lp);
    if (code == HC_ACTION && Watched(m->hwnd, m->message) && !g.pending.empty())
    {
        Pending p = std::move(g.pending.back());
        g.pending.pop_back();
        Frame f;
        std::vector<uint64_t> now;
        if (!p.px.empty() && g.mids.size() < kMaxMids && Grab(g.hwnd, f) && f.origin.x == p.origin.x && f.origin.y == p.origin.y)
        {
            SampleArea(f, p.area, now);
            Mid mid;
            size_t i = 0;
            for (int y = p.area.top; y < p.area.bottom; y += kStep)
                for (int x = p.area.left; x < p.area.right; x += kStep, ++i)
                    if (i < now.size() && now[i] != kOutside && p.px[i] != kOutside && !Same(static_cast<uint32_t>(now[i]), static_cast<uint32_t>(p.px[i])))
                        mid.drawn.push_back({ x, y, static_cast<uint32_t>(now[i]) });
            if (!mid.drawn.empty())
            {
                mid.msg = m->message;
                GetClassNameW(m->hwnd, mid.cls, ARRAYSIZE(mid.cls));
                g.mids.push_back(std::move(mid));
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void Unhook()
{
    if (g.pre)
        UnhookWindowsHookEx(g.pre);
    if (g.post)
        UnhookWindowsHookEx(g.post);
    g.pre = g.post = nullptr;
    g.pending.clear();
}

// The window's client pixels as they are now (its surface, children included: the DC isn't clipped to them)
bool Grab(HWND hwnd, Frame& f)
{
    RECT rc{};
    GetClientRect(hwnd, &rc);
    f.w = rc.right;
    f.h = rc.bottom;
    f.origin = {};
    ClientToScreen(hwnd, &f.origin);
    if (f.w <= 0 || f.h <= 0)
        return false;
    HDC src = GetDCEx(hwnd, nullptr, DCX_CACHE);
    if (!src)
        return false;
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), f.w, -f.h, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(src, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (dib && bits)
    {
        HDC mem = CreateCompatibleDC(src);
        HGDIOBJ old = SelectObject(mem, dib);
        ok = BitBlt(mem, 0, 0, f.w, f.h, src, 0, 0, SRCCOPY) != FALSE;
        GdiFlush();
        SelectObject(mem, old);
        DeleteDC(mem);
        if (ok)
            f.px.assign(static_cast<const uint32_t*>(bits), static_cast<const uint32_t*>(bits) + static_cast<size_t>(f.w) * f.h);
    }
    if (dib)
        DeleteObject(dib);
    ReleaseDC(hwnd, src);
    return ok;
}

bool Same(uint32_t a, uint32_t b)
{
    for (int s = 0; s < 24; s += 8)
        if (std::abs(static_cast<int>((a >> s) & 0xFF) - static_cast<int>((b >> s) & 0xFF)) > 6)
            return false;
    return true;
}

// The classes of the windows under a client point, outermost first ("FastFiles.MainWindow>FastFiles.ButtonBar")
std::wstring ClassesAt(HWND top, POINT pt)
{
    std::wstring path;
    for (HWND w = top; w;)
    {
        wchar_t cls[64]{};
        GetClassNameW(w, cls, ARRAYSIZE(cls));
        if (!path.empty())
            path += L'>';
        path += cls;
        POINT local = pt;
        MapWindowPoints(top, w, &local, 1);
        HWND child = ChildWindowFromPointEx(w, local, CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
        if (!child || child == w)
            break;
        w = child;
    }
    return path;
}
}

namespace LayoutProbe
{
bool Start(HWND hwnd, const wchar_t* tag)
{
    if (!Enabled())
        return false;
    if (g.depth > 0 && GetTickCount64() - g.since > 3000)
    {
        g.depth = 0; // a step whose finish never came: don't let it swallow every later one
        Unhook();
    }
    if (g.depth++ > 0)
        return true;
    g.since = GetTickCount64();
    g.hwnd = nullptr;
    g.mids.clear();
    g.hasEarly = false;
    if (g.beforeShape)
        DeleteObject(g.beforeShape);
    g.beforeShape = nullptr;
    if (hwnd && IsWindowVisible(hwnd) && !IsIconic(hwnd) && Grab(hwnd, g.before))
    {
        g.beforeShape = CreateRectRgn(0, 0, 0, 0);
        if (g.beforeShape && GetWindowRgn(hwnd, g.beforeShape) == ERROR)
        {
            DeleteObject(g.beforeShape);
            g.beforeShape = nullptr;
        }
        RECT wr{};
        GetWindowRect(hwnd, &wr);
        g.beforeWindow = { wr.left, wr.top };
        g.hwnd = hwnd;
        g.tag = tag;
        g.pre = SetWindowsHookExW(WH_CALLWNDPROC, BeforePaint, nullptr, GetCurrentThreadId());
        g.post = SetWindowsHookExW(WH_CALLWNDPROCRET, AfterPaint, nullptr, GetCurrentThreadId());
    }
    return true;
}

void Uncovered(HWND hwnd)
{
    if (!Enabled() || !g.hwnd || hwnd != g.hwnd || g.hasEarly)
        return;
    g.hasEarly = Grab(hwnd, g.early);
    if (g.shape)
        DeleteObject(g.shape);
    g.shape = CreateRectRgn(0, 0, 0, 0);
    if (g.shape && GetWindowRgn(hwnd, g.shape) == ERROR)
    {
        DeleteObject(g.shape);
        g.shape = nullptr;
    }
    RECT wr{};
    GetWindowRect(hwnd, &wr);
    g.shapeAt = { g.early.origin.x - wr.left, g.early.origin.y - wr.top };
}

void Finish()
{
    if (g.depth == 0 || --g.depth > 0 || !g.hwnd)
        return;
    const HWND hwnd = g.hwnd;
    Frame moved, painted;
    if (!Grab(hwnd, moved))
    {
        g.hwnd = nullptr;
        Unhook();
        return;
    }
    RedrawWindow(hwnd, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
    g.hwnd = nullptr;
    Unhook();
    if (!Grab(hwnd, painted) || painted.w != moved.w || painted.h != moved.h)
        return;
    // Flash: what one erase / paint of the step drew that neither the frame before, the one right after the moves nor
    // the full repaint shows at that spot (a window cleared to its background, then drawn)
    int flash = 0;
    const Mid* worst = nullptr;
    RECT flashBox{};
    for (const Mid& mid : g.mids)
    {
        int n = 0;
        RECT box{ LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN };
        for (const Sample& s : mid.drawn)
        {
            uint32_t v = 0;
            // (and what the new size showed before its layout: the background a frame drag fills a grown strip with)
            if ((g.before.At(s.x, s.y, v) && Same(s.v, v)) || (moved.At(s.x, s.y, v) && Same(s.v, v)) || (painted.At(s.x, s.y, v) && Same(s.v, v)) ||
                (g.hasEarly && g.early.At(s.x, s.y, v) && Same(s.v, v)))
                continue;
            ++n;
            const LONG cx = s.x - painted.origin.x, cy = s.y - painted.origin.y;
            box = { std::min(box.left, cx), std::min(box.top, cy), std::max(box.right, cx + 1), std::max(box.bottom, cy + 1) };
        }
        if (n > flash)
        {
            flash = n;
            worst = &mid;
            flashBox = box;
        }
    }
    int ghosts = 0;
    RECT box{ moved.w, moved.h, 0, 0 };
    // (pixels the rounded corners cut off, Windows 8 / 10: never on screen)
    HRGN shape = CreateRectRgn(0, 0, 0, 0);
    if (shape && GetWindowRgn(hwnd, shape) == ERROR)
    {
        DeleteObject(shape);
        shape = nullptr;
    }
    RECT wr{};
    GetWindowRect(hwnd, &wr);
    const POINT at{ moved.origin.x - wr.left, moved.origin.y - wr.top };
    for (int y = 0; y < moved.h; ++y)
        for (int x = 0; x < moved.w; ++x)
        {
            if (shape && !PtInRegion(shape, x + at.x, y + at.y))
                continue;
            const uint32_t b = moved.px[static_cast<size_t>(y) * moved.w + x];
            const uint32_t c = painted.px[static_cast<size_t>(y) * painted.w + x];
            uint32_t a = 0;
            // Newly uncovered (no "before" there, or there only outside the rounded corners then): only the repaint
            // counts
            const int sx = moved.origin.x + x, sy = moved.origin.y + y;
            const bool hadBefore = g.before.At(sx, sy, a) &&
                (!g.beforeShape || PtInRegion(g.beforeShape, sx - g.beforeWindow.x, sy - g.beforeWindow.y));
            if (Same(b, c) || (hadBefore && Same(b, a)) || !hadBefore)
                continue;
            ++ghosts;
            box = { std::min<LONG>(box.left, x), std::min<LONG>(box.top, y), std::max<LONG>(box.right, x + 1), std::max<LONG>(box.bottom, y + 1) };
        }
    if (shape)
        DeleteObject(shape);
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, (Sandbox::Dir() + L"layout-trace.txt").c_str(), L"a, ccs=UTF-8") != 0 || !fp)
        return;
    if (ghosts)
    {
        const POINT mid{ (box.left + box.right) / 2, (box.top + box.bottom) / 2 };
        fwprintf(fp, L"%s ghost %d %ld,%ld,%ld,%ld %s\n", g.tag, ghosts, box.left, box.top, box.right, box.bottom, ClassesAt(hwnd, mid).c_str());
    }
    else
        fwprintf(fp, L"%s ghost 0\n", g.tag);
    // (in samples: one every kStep pixels each way; after which message of which window)
    if (worst)
    {
        const POINT mid{ (flashBox.left + flashBox.right) / 2, (flashBox.top + flashBox.bottom) / 2 };
        fwprintf(fp, L"%s flash %d %ld,%ld,%ld,%ld after %04X %s at %s (%zu moments)\n", g.tag, flash, flashBox.left, flashBox.top,
            flashBox.right, flashBox.bottom, worst->msg, worst->cls, ClassesAt(hwnd, mid).c_str(), g.mids.size());
    }
    else
        fwprintf(fp, L"%s flash 0 (%zu moments)\n", g.tag, g.mids.size());
    // Blank: what the size uncovered (no "before" there), still unpainted (black) before the layout, where the full
    // repaint draws something
    if (g.hasEarly)
    {
        int blank = 0;
        RECT bb{ LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN };
        for (int y = 0; y < g.early.h; ++y)
            for (int x = 0; x < g.early.w; ++x)
            {
                const int sx = g.early.origin.x + x, sy = g.early.origin.y + y;
                uint32_t v = 0, c = 0;
                // (uncovered: no "before" at that spot of the screen, or past the old client size, where the left /
                // top edge's drag leaves the old picture at the top left and the rest of the window unpainted)
                const bool fresh = !g.before.At(sx, sy, v) || x >= g.before.w || y >= g.before.h;
                if (!fresh || !painted.At(sx, sy, c))
                    continue;
                // (cut off by the rounded corners: not the window's)
                if (g.shape && !PtInRegion(g.shape, x + g.shapeAt.x, y + g.shapeAt.y))
                    continue;
                if ((g.early.px[static_cast<size_t>(y) * g.early.w + x] & 0xFFFFFFu) != 0 || (c & 0xFFFFFFu) == 0)
                    continue;
                ++blank;
                bb = { std::min<LONG>(bb.left, x), std::min<LONG>(bb.top, y), std::max<LONG>(bb.right, x + 1), std::max<LONG>(bb.bottom, y + 1) };
            }
        if (blank)
            fwprintf(fp, L"%s blank %d %ld,%ld,%ld,%ld\n", g.tag, blank, bb.left, bb.top, bb.right, bb.bottom);
        else
            fwprintf(fp, L"%s blank 0\n", g.tag);
        g.hasEarly = false;
        if (g.shape)
            DeleteObject(g.shape);
        g.shape = nullptr;
    }
    fclose(fp);
}
}
