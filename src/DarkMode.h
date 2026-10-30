#pragma once
#include "Common.h"

// Dark mode for the parts Windows draws: menus, scroll bars, list views, the window frame, the shell view.
// Uses the uxtheme entry points Explorer itself relies on (no public API exists for Win32 apps); every call is
// skipped on builds that don't have them (before Windows 10 1903), where those parts simply stay light.
namespace DarkMode
{
bool Supported();
bool SystemPrefersDark(); // HKCU ...\Themes\Personalize AppsUseLightTheme == 0
void SetApp(bool dark);   // popup menus and newly themed controls follow; call before ApplyWindow
void ApplyWindow(HWND hwnd, bool dark);  // top-level: DWM frame + title bar
// Child control: scroll bars, list view, edit. The shell's file list uses the ItemsView classes instead.
void ApplyControl(HWND hwnd, bool dark, const wchar_t* lightClass = L"Explorer", const wchar_t* darkClass = L"DarkMode_Explorer");
// Column header text follows *color (read at paint time): DarkMode_ItemsView darkens the header but leaves its text black.
// Subclasses the list view, which is where the header sends NM_CUSTOMDRAW; safe to call repeatedly.
void ColorHeaderText(HWND list, const COLORREF* color);

// While alive, the system dialogs this thread opens (打开 / 另存为 / 浏览文件夹: #32770 windows) follow the dark theme:
// their title bar, their Win32 controls (file name box, file type list, the buttons) and their own background. The
// shell parts inside (the file list, the navigation pane) already follow SetApp. Nothing outside this thread's
// dialogs is touched (a thread-local CBT hook, gone with the scope). `back` / `ink`: the theme's colors for them.
class DialogScope
{
public:
    DialogScope(bool dark, COLORREF back, COLORREF ink);
    ~DialogScope();
    DialogScope(const DialogScope&) = delete;
    DialogScope& operator=(const DialogScope&) = delete;
private:
    HHOOK m_hook = nullptr;
};
}
