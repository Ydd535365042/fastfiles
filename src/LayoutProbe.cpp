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

struct State
{
    int depth = 0;
    ULONGLONG since = 0; // the outermost Start (a step never finished is dropped after a while)
    HWND hwnd = nullptr;
    const wchar_t* tag = L"";
    Frame before;
} g;

bool Enabled()
{
    static const bool on = Sandbox::Active() && GetEnvironmentVariableW(L"FF_LAYOUT_TRACE", nullptr, 0) > 0;
    return on;
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
        g.depth = 0; // a step whose finish never came: don't let it swallow every later one
    if (g.depth++ > 0)
        return true;
    g.since = GetTickCount64();
    g.hwnd = nullptr;
    if (hwnd && IsWindowVisible(hwnd) && !IsIconic(hwnd) && Grab(hwnd, g.before))
    {
        g.hwnd = hwnd;
        g.tag = tag;
    }
    return true;
}

void Finish()
{
    if (g.depth == 0 || --g.depth > 0 || !g.hwnd)
        return;
    const HWND hwnd = g.hwnd;
    g.hwnd = nullptr;
    Frame moved, painted;
    if (!Grab(hwnd, moved))
        return;
    RedrawWindow(hwnd, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
    if (!Grab(hwnd, painted) || painted.w != moved.w || painted.h != moved.h)
        return;
    int ghosts = 0;
    RECT box{ moved.w, moved.h, 0, 0 };
    for (int y = 0; y < moved.h; ++y)
        for (int x = 0; x < moved.w; ++x)
        {
            const uint32_t b = moved.px[static_cast<size_t>(y) * moved.w + x];
            const uint32_t c = painted.px[static_cast<size_t>(y) * painted.w + x];
            uint32_t a = 0;
            // Newly uncovered (no "before" there): only the repaint counts
            const bool hadBefore = g.before.At(moved.origin.x + x, moved.origin.y + y, a);
            if (Same(b, c) || (hadBefore && Same(b, a)) || !hadBefore)
                continue;
            ++ghosts;
            box = { std::min<LONG>(box.left, x), std::min<LONG>(box.top, y), std::max<LONG>(box.right, x + 1), std::max<LONG>(box.bottom, y + 1) };
        }
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
    fclose(fp);
}
}
