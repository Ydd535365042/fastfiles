#include "DarkMode.h"
#include <dwmapi.h>
#include <uxtheme.h>

namespace
{
enum class AppMode { Default, AllowDark, ForceDark, ForceLight };
using SetPreferredAppModeFn = AppMode(WINAPI*)(AppMode);         // uxtheme #135 (1903+)
using AllowDarkModeForWindowFn = bool(WINAPI*)(HWND, bool);      // uxtheme #133
using RefreshImmersiveColorPolicyStateFn = void(WINAPI*)();      // uxtheme #104
using FlushMenuThemesFn = void(WINAPI*)();                       // uxtheme #136

struct Api
{
    DWORD build = 0;
    SetPreferredAppModeFn setAppMode = nullptr;
    AllowDarkModeForWindowFn allowForWindow = nullptr;
    RefreshImmersiveColorPolicyStateFn refreshPolicy = nullptr;
    FlushMenuThemesFn flushMenus = nullptr;
};

DWORD OsBuild()
{
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW info{ sizeof(info) };
    if (auto get = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
        get(&info);
    return info.dwMajorVersion >= 10 ? info.dwBuildNumber : 0;
}

const Api& Get()
{
    static const Api api = []
    {
        Api a;
        a.build = OsBuild();
        HMODULE ux = a.build >= 18362 ? LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) : nullptr;
        if (ux)
        {
            a.setAppMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
            a.allowForWindow = reinterpret_cast<AllowDarkModeForWindowFn>(GetProcAddress(ux, MAKEINTRESOURCEA(133)));
            a.refreshPolicy = reinterpret_cast<RefreshImmersiveColorPolicyStateFn>(GetProcAddress(ux, MAKEINTRESOURCEA(104)));
            a.flushMenus = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
        }
        if (!a.setAppMode || !a.allowForWindow)
            a = Api{ a.build };
        return a;
    }();
    return api;
}

constexpr UINT_PTR kHeaderSubclassId = 0xFF10;

LRESULT CALLBACK HeaderTextProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref)
{
    if (msg == WM_NOTIFY)
    {
        auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(lp);
        if (cd->hdr.code == NM_CUSTOMDRAW && cd->hdr.hwndFrom == ListView_GetHeader(hwnd))
        {
            if (cd->dwDrawStage == CDDS_PREPAINT)
                return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT)
            {
                SetTextColor(cd->hdc, *reinterpret_cast<const COLORREF*>(ref));
                return CDRF_DODEFAULT;
            }
        }
    }
    else if (msg == WM_NCDESTROY)
        RemoveWindowSubclass(hwnd, HeaderTextProc, kHeaderSubclassId);
    return DefSubclassProc(hwnd, msg, wp, lp);
}
}

namespace DarkMode
{
bool Supported()
{
    return Get().setAppMode != nullptr;
}

bool SystemPrefersDark()
{
    DWORD value = 1, size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
               RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS && value == 0;
}

void SetApp(bool dark)
{
    const Api& api = Get();
    if (!api.setAppMode)
        return;
    // Forced either way: the app's theme setting can differ from the system's
    api.setAppMode(dark ? AppMode::ForceDark : AppMode::ForceLight);
    if (api.refreshPolicy)
        api.refreshPolicy();
    if (api.flushMenus)
        api.flushMenus();
}

void ApplyWindow(HWND hwnd, bool dark)
{
    const Api& api = Get();
    if (api.allowForWindow)
        api.allowForWindow(hwnd, dark);
    // DWMWA_USE_IMMERSIVE_DARK_MODE: 20 since 20H1, 19 before that
    const BOOL value = dark;
    if (api.build >= 17763)
        DwmSetWindowAttribute(hwnd, api.build >= 18985 ? 20 : 19, &value, sizeof(value));
    // Windows 10 keeps the old caption until the frame repaints: flip the active state twice to force it
    if (IsWindowVisible(hwnd))
    {
        const bool active = GetActiveWindow() == hwnd;
        SendMessageW(hwnd, WM_NCACTIVATE, !active, 0);
        SendMessageW(hwnd, WM_NCACTIVATE, active, 0);
    }
}

void ApplyControl(HWND hwnd, bool dark, const wchar_t* lightClass, const wchar_t* darkClass)
{
    if (!hwnd)
        return;
    const Api& api = Get();
    if (api.allowForWindow)
        api.allowForWindow(hwnd, dark);
    SetWindowTheme(hwnd, dark && api.allowForWindow ? darkClass : lightClass, nullptr);
    SendMessageW(hwnd, WM_THEMECHANGED, 0, 0);
}

void ColorHeaderText(HWND list, const COLORREF* color)
{
    if (!list || !color)
        return;
    SetWindowSubclass(list, HeaderTextProc, kHeaderSubclassId, reinterpret_cast<DWORD_PTR>(color));
    if (HWND header = ListView_GetHeader(list))
        InvalidateRect(header, nullptr, TRUE);
}
}
