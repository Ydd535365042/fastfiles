// Windows 8+ (the minimum): the Windows 10 DPI functions (GetDpiForWindow...) are looked up at run time (Common.h
// redirects every call), with the classic system DPI as the fallback on Windows 8 / 8.1.
#include "Common.h"
#undef GetDpiForWindow
#undef GetDpiForSystem
#undef GetSystemMetricsForDpi
#undef AdjustWindowRectExForDpi

namespace
{
template <class F> F Lookup(const char* name)
{
    static const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    return reinterpret_cast<F>(GetProcAddress(user32, name));
}

UINT SystemDpi()
{
    HDC screen = GetDC(nullptr);
    const int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSX) : 96;
    if (screen)
        ReleaseDC(nullptr, screen);
    return dpi > 0 ? static_cast<UINT>(dpi) : 96u;
}
}

namespace Compat
{
UINT DpiForWindow(HWND hwnd)
{
    static const auto f = Lookup<UINT(WINAPI*)(HWND)>("GetDpiForWindow");
    return f ? f(hwnd) : SystemDpi();
}
UINT DpiForSystem()
{
    static const auto f = Lookup<UINT(WINAPI*)()>("GetDpiForSystem");
    return f ? f() : SystemDpi();
}
int SystemMetricsForDpi(int index, UINT dpi)
{
    static const auto f = Lookup<int(WINAPI*)(int, UINT)>("GetSystemMetricsForDpi");
    return f ? f(index, dpi) : GetSystemMetrics(index); // Windows 7: one DPI for everything, the system's
}
BOOL AdjustWindowRectExForDpi(RECT* rc, DWORD style, BOOL menu, DWORD exStyle, UINT dpi)
{
    static const auto f = Lookup<BOOL(WINAPI*)(RECT*, DWORD, BOOL, DWORD, UINT)>("AdjustWindowRectExForDpi");
    return f ? f(rc, style, menu, exStyle, dpi) : AdjustWindowRectEx(rc, style, menu, exStyle);
}
}
