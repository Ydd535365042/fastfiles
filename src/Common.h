#pragma once

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

struct PidlDeleter
{
    // PIDLIST_ABSOLUTE is __unaligned on x64; using it as the pointer type keeps every API call cast-free
    using pointer = PIDLIST_ABSOLUTE;
    void operator()(PIDLIST_ABSOLUTE p) const noexcept { CoTaskMemFree(p); }
};
using UniquePidl = std::unique_ptr<ITEMIDLIST_ABSOLUTE, PidlDeleter>;

inline UniquePidl ClonePidl(PCIDLIST_ABSOLUTE p)
{
    return UniquePidl(p ? ILCloneFull(p) : nullptr);
}

inline std::wstring PidlName(PCIDLIST_ABSOLUTE p, SIGDN form)
{
    std::wstring result;
    PWSTR name = nullptr;
    if (p && SUCCEEDED(SHGetNameFromIDList(p, form, &name)))
    {
        result = name;
        CoTaskMemFree(name);
    }
    return result;
}

inline int PidlIcon(PCIDLIST_ABSOLUTE p)
{
    SHFILEINFOW sfi{};
    if (p && SHGetFileInfoW(reinterpret_cast<PCWSTR>(p), 0, &sfi, sizeof(sfi), SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
        return sfi.iIcon;
    return -1;
}

inline int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), 96); }

// A layout move of a child window (x, y in its parent). Resized: without its old picture (SWP_NOCOPYBITS), which
// Windows would put at the new top left, so whatever is pinned right / bottom / centered inside (the toolbar's right
// group, scroll bars, centered pages) showed one step off until the repaint. Only moved: the old picture is right
// as it is at the new place, so it's kept and the window isn't painted again (painting every window on every step
// of a frame drag made a step slower than a frame)
// (g_moveRepaints: the window's picture was put back cut for its new size (a frame drag's left / top edge): what's
// there at the old place isn't the child's any more, every one painted again)
inline bool g_moveRepaints = false;
inline void MoveChild(HWND w, int x, int y, int cx, int cy)
{
    RECT now{};
    GetWindowRect(w, &now);
    const bool sized = now.right - now.left != cx || now.bottom - now.top != cy;
    SetWindowPos(w, nullptr, x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE | (sized || g_moveRepaints ? SWP_NOCOPYBITS : 0));
}

inline HMENU MenuId(int id) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)); }

// Where a folder chosen in the sidebar / home page / a menu should open
enum class OpenMode { Here, NewTab, NewWindow, OtherPane };

// Files' 右键菜单选项 (GeneralSettingsService Show*): which of the app's own menu items show. Set from the settings
// (menu.hidden), read by every menu that has them (file view, sidebar, home page, address bar).
namespace MenuOptions
{
enum : unsigned { NewTab = 1, NewWindow = 2, NewPane = 4, CopyPath = 8, FolderWithSelection = 16, Shortcut = 32, Pin = 64,
    Compress = 128, SendTo = 256, Terminal = 512, Tags = 1024, PinStart = 2048, Count = 12 };
// Settings keys, bit order
inline constexpr const wchar_t* kKeys[Count] = { L"newtab", L"newwindow", L"newpane", L"copypath", L"folderwithselection", L"shortcut",
    L"pin", L"compress", L"sendto", L"terminal", L"tags", L"pinstart" };
inline unsigned g_hidden = 0;
inline bool Shown(unsigned option) { return (g_hidden & option) == 0; }
}

constexpr UINT WM_APP_NAVIGATE = WM_APP + 1; // wParam: OpenMode (| kNavigateActivate), lParam: owned PIDLIST_ABSOLUTE
constexpr WPARAM kNavigateActivate = 0x100;  // a new tab becomes the current one whatever 总是切换到新创建的标签页 says
constexpr UINT WM_APP_DROPFILES = WM_APP + 2; // lParam: owned DroppedFiles* (MainWindowFileOps.cpp)
// A drop of file system items on `folder` (sidebar, tab, breadcrumb): copy / move through the window's status center
// (Files' PerformOperationTypeAsync). False: leave it to the folder's own drop target (right drag, links, virtual items).
// On true *effect is what the source should see (an optimized move: the source deletes nothing).
bool RouteFileDrop(HWND from, IDataObject* data, PCIDLIST_ABSOLUTE folder, DWORD* effect, bool rightButton);

// Files' CopyItemFromSidebar / CopyItemFromHome ("复制" on a sidebar or Home card): puts the item itself (not just its
// path text) on the clipboard, the same way Explorer's own Copy does - SHCreateDataObject-backed IDataObject through
// the real shell, so pasting elsewhere (Explorer, another FastFiles window) pastes the object. Falls back to copying
// the path as plain text when the shell can't hand back a data object (e.g. a virtual item). Sandbox::CopyFiles
// redirects to <Dir>clipboard-files.txt in test mode; the real clipboard is never touched there.
void CopyPathsToClipboard(HWND owner, const std::vector<std::wstring>& paths);

// Files' Home page lives at the Quick Access location (shell:::{679f85cb-...}); FastFiles draws its own page there
PCIDLIST_ABSOLUTE HomePidl();
bool IsHomePidl(PCIDLIST_ABSOLUTE pidl);
// Files' "Settings" location: a page in a tab, like Home. A desktop item with a CLSID nothing registers, so the shell
// never resolves it and no real folder can be mistaken for it; history and sessions store it like any location.
PCIDLIST_ABSOLUTE SettingsPidl();
bool IsSettingsPidl(PCIDLIST_ABSOLUTE pidl);
inline bool IsPagePidl(PCIDLIST_ABSOLUTE pidl) { return IsHomePidl(pidl) || IsSettingsPidl(pidl); } // FastFiles draws it
// Shell menu items (ids first..last of cm) that Files' 右键菜单选项 turned off: 发送到, 创建快捷方式, 固定到"开始"屏幕; SidebarDrop.cpp
// Windows 7: the Windows 10 DPI functions are looked up at run time (Compat.cpp); every call site uses these
namespace Compat
{
UINT DpiForWindow(HWND hwnd);
UINT DpiForSystem();
int SystemMetricsForDpi(int index, UINT dpi);
BOOL AdjustWindowRectExForDpi(RECT* rc, DWORD style, BOOL menu, DWORD exStyle, UINT dpi);
}
#define GetDpiForWindow Compat::DpiForWindow
#define GetDpiForSystem Compat::DpiForSystem
#define GetSystemMetricsForDpi Compat::SystemMetricsForDpi
#define AdjustWindowRectExForDpi Compat::AdjustWindowRectExForDpi

void RemoveHiddenShellItems(HMENU menu, IContextMenu* cm, UINT first, UINT last);
// Drops the shell menu items with these verbs (ids first..last); Files' own items replace them
void RemoveShellVerbs(HMENU menu, IContextMenu* cm, UINT first, UINT last, std::initializer_list<const wchar_t*> verbs);
// The caption under an Explorer-style drag image ("移动到 %1"); SidebarDrop.cpp
void SetDropDescription(IDataObject* data, DROPIMAGETYPE type, const wchar_t* message, const std::wstring& insert);
// Display name with Home renamed to "主页" (Windows 10 calls it 快速访问) and Settings as "设置"
std::wstring LocationName(PCIDLIST_ABSOLUTE pidl);
