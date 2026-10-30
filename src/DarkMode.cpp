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

// --- The system dialogs (DialogScope) ---

namespace
{
constexpr UINT_PTR kDialogSubclassId = 0xFFD1;
struct DialogColors
{
    COLORREF back = 0, ink = 0;
    HBRUSH brush = nullptr;
    int depth = 0; // nested scopes on this thread
};
thread_local DialogColors t_dialog;

bool ClassIs(HWND hwnd, const wchar_t* name)
{
    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    return _wcsicmp(cls, name) == 0;
}

LRESULT CALLBACK ComboProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    switch (msg)
    {
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        if (t_dialog.brush)
        {
            SetTextColor(reinterpret_cast<HDC>(wp), t_dialog.ink);
            SetBkColor(reinterpret_cast<HDC>(wp), t_dialog.back);
            return reinterpret_cast<LRESULT>(t_dialog.brush);
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, ComboProc, kDialogSubclassId);
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// The dialog's own Win32 children: DarkMode_Explorer buttons / scroll bars, DarkMode_CFD (the common file dialog's
// dark class: the boxes and lists Explorer's own dialogs use). The shell's views (DirectUIHWND, SHELLDLL_DefView,
// the navigation tree) are left as they are: SetApp already themes them.
BOOL CALLBACK ThemeChild(HWND child, LPARAM)
{
    wchar_t cls[64]{};
    GetClassNameW(child, cls, ARRAYSIZE(cls));
    const Api& api = Get();
    auto apply = [&](const wchar_t* dark)
    {
        if (api.allowForWindow)
            api.allowForWindow(child, true);
        SetWindowTheme(child, dark, nullptr);
        SendMessageW(child, WM_THEMECHANGED, 0, 0);
    };
    // The combo box inside a ComboBoxEx32 (the 文件名 box) stays unthemed: DarkMode_CFD on it drew a light frame around
    // its edit; the outer ComboBoxEx32 carries the dark border
    const bool insideEx = !_wcsicmp(cls, WC_COMBOBOXW) && ClassIs(GetParent(child), WC_COMBOBOXEXW);
    if (!_wcsicmp(cls, WC_BUTTONW) || !_wcsicmp(cls, WC_SCROLLBARW))
        apply(L"DarkMode_Explorer");
    else if (!insideEx && (!_wcsicmp(cls, WC_COMBOBOXW) || !_wcsicmp(cls, WC_COMBOBOXEXW) || !_wcsicmp(cls, WC_EDITW)))
        apply(L"DarkMode_CFD");
    // A combo box colors its own edit (the 文件名 box) and list: it gets their WM_CTLCOLOR* and doesn't pass them up.
    // The 文件名 box is a ComboBoxEx32 around one: the light ring around the text was the outer one's fill
    if (!_wcsicmp(cls, WC_COMBOBOXW) || !_wcsicmp(cls, WC_COMBOBOXEXW))
        SetWindowSubclass(child, ComboProc, kDialogSubclassId, 0);
    return TRUE;
}

LRESULT CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        if (t_dialog.brush)
        {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, t_dialog.brush);
            return 1;
        }
        break;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:    // the 文件名 box (the editable combo box's edit; the combo boxes pass it up)
    case WM_CTLCOLORLISTBOX: // their drop-down lists
        if (t_dialog.brush)
        {
            SetTextColor(reinterpret_cast<HDC>(wp), t_dialog.ink);
            SetBkColor(reinterpret_cast<HDC>(wp), t_dialog.back);
            return reinterpret_cast<LRESULT>(t_dialog.brush);
        }
        break;
    case WM_SHOWWINDOW:
    case WM_INITDIALOG:
        // The controls exist (and more come with the dialog's own layout): theme them all
        EnumChildWindows(hwnd, ThemeChild, 0);
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, DialogProc, kDialogSubclassId);
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK DialogHook(int code, WPARAM wp, LPARAM lp)
{
    // A top-level dialog window (#32770: the file dialog, the folder browser), once it is built (activation: before
    // it is shown; at creation it isn't initialized yet, and touching it there broke the dialog)
    if (code == HCBT_ACTIVATE)
    {
        const HWND hwnd = reinterpret_cast<HWND>(wp);
        if (ClassIs(hwnd, L"#32770") && !(GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD) &&
            SetWindowSubclass(hwnd, DialogProc, kDialogSubclassId, 0))
        {
            const Api& api = Get();
            if (api.allowForWindow)
                api.allowForWindow(hwnd, true);
            const BOOL value = TRUE;
            if (api.build >= 17763)
                DwmSetWindowAttribute(hwnd, api.build >= 18985 ? 20 : 19, &value, sizeof(value));
            EnumChildWindows(hwnd, ThemeChild, 0);
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
}

DialogScope::DialogScope(bool dark, COLORREF back, COLORREF ink)
{
    if (!dark || !Get().allowForWindow)
        return; // light: the system dialogs are light already
    if (t_dialog.depth++ == 0)
    {
        t_dialog.back = back;
        t_dialog.ink = ink;
        t_dialog.brush = CreateSolidBrush(back);
    }
    m_hook = SetWindowsHookExW(WH_CBT, DialogHook, nullptr, GetCurrentThreadId());
}

DialogScope::~DialogScope()
{
    if (!m_hook && t_dialog.depth == 0)
        return;
    if (m_hook)
        UnhookWindowsHookEx(m_hook);
    if (m_hook && --t_dialog.depth == 0 && t_dialog.brush)
    {
        DeleteObject(t_dialog.brush);
        t_dialog.brush = nullptr;
    }
}
}
