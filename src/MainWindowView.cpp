#include "MainWindow.h"
#include "Format.h"
#include "Lang.h"
#include "Sandbox.h"
#include <sddl.h>

// View toggles, context buttons and the extra menus (system New menu, history, "more").
namespace
{
enum : int { MenuHistoryBase = 100 };
enum : int { MoreCopyPath = 1, MoreCopyQuoted, MoreTerminal, MoreTerminalAdmin, MoreNewWindow, MorePasteInto, MorePin, MoreOtherPane, MoreSettings, MorePalette };

std::wstring CurrentDir(Tab* tab)
{
    std::wstring dir = tab ? PidlName(tab->Location(), SIGDN_FILESYSPATH) : std::wstring();
    if (dir.empty())
    {
        wchar_t profile[MAX_PATH]{};
        GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
        dir = profile;
    }
    return dir;
}

bool OnPath(const wchar_t* exe)
{
    wchar_t found[MAX_PATH]{};
    return SearchPathW(nullptr, exe, nullptr, MAX_PATH, found, nullptr) > 0;
}
}

void MainWindow::ApplyPrefsToTabs()
{
    ForEachPane([](Tab* tab) { tab->ApplyPrefs(); });
}

void MainWindow::ToggleHidden()
{
    // Files' own ShowHiddenItems: the system setting (and Explorer) stay as they are
    m_settings.showHidden = !m_settings.showHidden;
    SaveAndBroadcast();
    ApplySettings();
}

void MainWindow::ToggleDotFiles()
{
    // Files' ToggleDotFilesSetting
    m_settings.showDotFiles = !m_settings.showDotFiles;
    SaveAndBroadcast();
    ApplySettings();
}

void MainWindow::ToggleCheckboxes()
{
    m_prefs.checkboxes = !m_prefs.checkboxes;
    ApplyPrefsToTabs();
    UpdateToggleButtons();
}

void MainWindow::ToggleExtensions()
{
    // Extensions are a system-wide shell setting; Explorer has no per-window switch either
    SHELLSTATEW state{};
    SHGetSetSettings(&state, SSF_SHOWEXTENSIONS, FALSE);
    SetShellFlag(SSF_SHOWEXTENSIONS, !state.fShowExtensions);
}

void MainWindow::ToggleToolbar()
{
    m_toolbarVisible = !m_toolbarVisible;
    ShowWindow(m_cmd.Hwnd(), m_toolbarVisible ? SW_SHOW : SW_HIDE);
    Layout();
}

void MainWindow::ToggleFilter()
{
    m_filterVisible = !m_filterVisible;
    ShowWindow(m_filter, m_filterVisible ? SW_SHOW : SW_HIDE);
    UpdateToggleButtons();
    Tab* tab = Active();
    if (m_filterVisible)
    {
        SetWindowTextW(m_filter, tab ? tab->Filter().c_str() : L"");
        Layout();
        SetFocus(m_filter);
        SendMessageW(m_filter, EM_SETSEL, 0, -1);
    }
    else
    {
        KillTimer(m_hwnd, kFilterTimer);
        SetWindowTextW(m_filter, L"");
        if (tab)
        {
            tab->SetFilter(L"");
            tab->Focus();
        }
        Layout();
    }
}

void MainWindow::ToggleFullScreen()
{
    if (!m_fullScreen)
    {
        // Files' presenters are exclusive: full screen replaces the compact overlay
        SetCompactOverlay(false);
        GetWindowPlacement(m_hwnd, &m_restorePlacement);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        m_fullScreen = true;
        m_round.Suppress(true);
        SetWindowLongPtrW(m_hwnd, GWL_STYLE, (GetWindowLongPtrW(m_hwnd, GWL_STYLE) & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        const RECT& r = mi.rcMonitor;
        SetWindowPos(m_hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    }
    else
    {
        m_fullScreen = false;
        SetWindowLongPtrW(m_hwnd, GWL_STYLE, (GetWindowLongPtrW(m_hwnd, GWL_STYLE) & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(m_hwnd, &m_restorePlacement);
        SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        m_round.Suppress(false);
    }
    m_strip.SetMaximized(m_fullScreen || IsZoomed(m_hwnd));
}

void MainWindow::OpenTerminal(bool admin, const std::wstring& folder)
{
    std::wstring dir = folder.empty() ? CurrentDir(Active()) : folder;
    std::wstring quoted = dir;
    for (size_t p = quoted.find(L'\''); p != std::wstring::npos; p = quoted.find(L'\'', p + 2))
        quoted.insert(p, 1, L'\'');
    // Windows Terminal when installed, otherwise Windows PowerShell; runas resets the working
    // directory to System32, so the folder is passed explicitly in both cases
    std::wstring exe, args;
    if (OnPath(L"wt.exe"))
    {
        std::wstring d = dir;
        if (!d.empty() && d.back() == L'\\')
            d += L'.';
        exe = L"wt.exe";
        args = L"-d \"" + d + L"\"";
    }
    else
    {
        exe = L"powershell.exe";
        args = L"-NoExit -Command \"Set-Location -LiteralPath '" + quoted + L"'\"";
    }
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = m_hwnd;
    sei.lpVerb = admin ? L"runas" : nullptr;
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.lpDirectory = dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) && GetLastError() != ERROR_CANCELLED)
        SetStatusText(Tr(L"无法打开终端"), true);
}

void MainWindow::CopyPathQuoted()
{
    Tab* tab = Active();
    if (!tab)
        return;
    std::wstring text, line;
    const std::wstring paths = tab->SelectionPaths() + L"\r\n";
    for (size_t start = 0, end; (end = paths.find(L"\r\n", start)) != std::wstring::npos; start = end + 2)
        if (end > start)
            text += (text.empty() ? L"" : L"\r\n") + (L"\"" + paths.substr(start, end - start) + L"\"");
    if (text.empty() || Sandbox::CopyText(text) || !OpenClipboard(m_hwnd))
        return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes))
    {
        memcpy(GlobalLock(mem), text.c_str(), bytes);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem))
            GlobalFree(mem);
    }
    CloseClipboard();
}

void MainWindow::OpenSelectionInNewWindow()
{
    Tab* tab = Active();
    if (!tab)
        return;
    UniquePidl folder;
    OpenNewWindow(tab->SingleSelectedFolder(folder) ? folder.get() : tab->Location());
}

void MainWindow::OpenSelectionInOtherPane()
{
    Tab* tab = Active();
    if (!tab)
        return;
    UniquePidl folder;
    const UniquePidl target = ClonePidl(tab->SingleSelectedFolder(folder) ? folder.get() : tab->Location());
    OpenInOtherPane(target.get());
}

void MainWindow::ShowMoreMenu(const RECT& button)
{
    Tab* tab = Active();
    if (!tab)
        return;
    UniquePidl folder;
    const bool oneFolder = tab->SingleSelectedFolder(folder) != nullptr;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, MoreCopyPath, Tr(L"复制路径\tCtrl+Shift+C"));
    AppendMenuW(menu, MF_STRING, MoreCopyQuoted, Tr(L"复制带引号的路径\tCtrl+Alt+C"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, MoreTerminal, Tr(L"在终端中打开\tCtrl+`"));
    AppendMenuW(menu, MF_STRING, MoreTerminalAdmin, Tr(L"以管理员身份打开终端\tCtrl+Shift+`"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, MoreNewWindow, oneFolder ? Tr(L"在新窗口中打开\tCtrl+Alt+Enter") : Tr(L"在新窗口中打开当前文件夹\tCtrl+Alt+Enter"));
    AppendMenuW(menu, MF_STRING, MoreOtherPane, oneFolder ? Tr(L"在另一个窗格中打开") : Tr(L"在另一个窗格中打开当前文件夹"));
    AppendMenuW(menu, MF_STRING | (oneFolder ? 0 : MF_GRAYED), MorePasteInto, Tr(L"粘贴到选中的文件夹\tCtrl+Shift+V"));

    // Pin the selected folder, or the current one when nothing is selected
    PCIDLIST_ABSOLUTE pinTarget = oneFolder ? folder.get() : tab->Location();
    const bool pinned = m_sidebar && pinTarget && m_sidebar->IsPinned(pinTarget);
    if (m_sidebar && pinTarget && !ILIsEmpty(pinTarget))
    {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, MorePin, pinned ? (oneFolder ? Tr(L"从侧边栏取消固定所选文件夹") : Tr(L"从侧边栏取消固定当前文件夹"))
                                                     : (oneFolder ? Tr(L"将所选文件夹固定到侧边栏") : Tr(L"将当前文件夹固定到侧边栏")));
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, MorePalette, Tr(L"命令面板\tCtrl+Shift+P"));
    AppendMenuW(menu, MF_STRING, MoreSettings, Tr(L"设置\tCtrl+,"));
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, button.right, button.bottom, 0, m_hwnd, nullptr);
    DestroyMenu(menu);

    if (cmd == MoreSettings || cmd == MorePalette)
    {
        RunAction(cmd == MoreSettings ? ActSettings : ActPalette);
        return;
    }
    if (cmd == MorePin && m_sidebar)
    {
        if (pinned)
            m_sidebar->Unpin(pinTarget);
        else
            m_sidebar->Pin(pinTarget);
        return;
    }
    switch (cmd)
    {
    case MoreCopyPath: CopyPath(); break;
    case MoreCopyQuoted: CopyPathQuoted(); break;
    case MoreTerminal: OpenTerminal(false); break;
    case MoreTerminalAdmin: OpenTerminal(true); break;
    case MoreNewWindow: OpenSelectionInNewWindow(); break;
    case MoreOtherPane: OpenSelectionInOtherPane(); break;
    case MorePasteInto: PasteIntoSelection(); break;

    }
}

// --- Details / preview pane ---

void MainWindow::ToggleInfoPane(int page)
{
    if (!m_info)
        return;
    const InfoPane::Page want = page == 1 ? InfoPane::Page::Preview : InfoPane::Page::Details;
    bool show = !m_infoVisible;
    if (page >= 0)
        show = !(m_infoVisible && m_info->CurrentPage() == want);
    if (page >= 0)
        m_info->SetPage(want);
    m_infoVisible = show;
    UpdateToggleButtons();
    m_infoKey = -1;
    if (show)
    {
        SetTimer(m_hwnd, kInfoTimer, 120, nullptr);
        UpdateInfoPane();
    }
    else
    {
        KillTimer(m_hwnd, kInfoTimer);
        m_info->ShowMany(0, L""); // drops the current item and unloads any preview handler
    }
    Layout();
}

void MainWindow::OnInfoPaneClose()
{
    if (m_infoVisible)
        ToggleInfoPane();
    if (Tab* tab = Active())
        tab->Focus();
}

// Selection is polled (cheap: count + focused index) because IExplorerBrowser doesn't report selection changes
void MainWindow::UpdateInfoPane()
{
    Tab* tab = Active();
    if (!m_info || !m_infoVisible || !tab)
        return;
    const int selected = tab->SelectedCount();
    const long long key = (static_cast<long long>(m_navSerial) << 40) ^ (static_cast<long long>(selected) << 20) ^
        static_cast<unsigned>(tab->FocusedIndex() & 0xFFFFF) ^ (static_cast<long long>(m_active) << 56);
    if (key == m_infoKey)
        return;
    m_infoKey = key;
    if (selected > 1)
    {
        ULONGLONG bytes = 0;
        const bool sized = tab->SelectionSize(bytes);
        m_info->ShowMany(selected, sized ? Format::Size(bytes) : L"");
        UpdateInfoTags(nullptr, false);
        return;
    }
    UniquePidl holder;
    PCIDLIST_ABSOLUTE item = selected == 1 ? tab->SingleSelectedItem(holder) : nullptr;
    m_info->ShowItem(item ? item : tab->Location());
    // Tags of the selected item, or of the folder itself (not of Home / search results)
    UpdateInfoTags(item ? item : (tab->OnHome() || tab->InResults() ? nullptr : tab->Location()), true);
}

namespace
{
std::wstring UserSid()
{
    std::wstring result;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return result;
    BYTE buffer[256];
    DWORD size = 0;
    PWSTR text = nullptr;
    if (GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size) &&
        ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, &text))
    {
        result = text;
        LocalFree(text);
    }
    CloseHandle(token);
    return result;
}
}

// QuickLook (github.com/QL-Win/QuickLook) listens on a per-user named pipe; Files sends it the same Toggle message
// --- Files' preview popup providers (PreviewPopupService): QuickLook, then Seer, then PowerToys Peek ---

namespace
{
bool QuickLookSend(const std::wstring& message, const std::wstring& path)
{
    static const std::wstring pipe = L"\\\\.\\pipe\\QuickLook.App.Pipe." + UserSid();
    HANDLE h = CreateFileW(pipe.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(pipe.c_str(), 150))
            return false;
        h = CreateFileW(pipe.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return false;
    }
    const std::wstring line = L"QuickLook.App.PipeMessages." + message + L"|" + path + L"\n";
    const int len = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), utf8.data(), len, nullptr, nullptr);
    DWORD written = 0;
    const bool ok = WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) && written == utf8.size();
    CloseHandle(h);
    return ok;
}

// Seer (SeerProProvider): WM_COPYDATA 5000 with the path toggles its window
HWND SeerWindow() { return FindWindowW(L"SeerWindowClass", nullptr); }
std::wstring g_seerPath; // shown in Seer (SeerProProvider.CurrentPath)

bool SeerToggle(const std::wstring& path)
{
    HWND seer = SeerWindow();
    if (!seer)
        return false;
    COPYDATASTRUCT data{ 5000, static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)), const_cast<wchar_t*>(path.c_str()) };
    DWORD_PTR result = 0;
    SendMessageTimeoutW(seer, WM_COPYDATA, 0 /* as Files */, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 1000, &result);
    g_seerPath = IsWindowVisible(seer) ? path : std::wstring();
    return true;
}

// Seer's "track the selected file" (registry, then its ini files); on unless it says false
bool SeerTracksSelection()
{
    static int cached = -1;
    if (cached >= 0)
        return cached == 1;
    wchar_t value[16]{};
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Corey\\Seer", L"tracking_file", RRF_RT_REG_SZ, nullptr, value, &bytes) == ERROR_SUCCESS)
        return (cached = _wcsicmp(value, L"false") == 0 ? 0 : 1) == 1;
    for (const wchar_t* ini : { L"%USERPROFILE%\\Documents\\Seer\\uwp.ini",
             L"%USERPROFILE%\\appdata\\Local\\Packages\\CNABA5E861-AC2A-4523-B3C1.Seer-AWindowsQuickLookTo_p7t0z30wh4868\\LocalCache\\Local\\Corey\\Seer\\uwp.ini",
             L"%USERPROFILE%\\appdata\\Local\\Corey\\Seer\\uwp.ini", L"%USERPROFILE%\\Documents\\Seer\\config.ini" })
    {
        wchar_t path[MAX_PATH]{};
        ExpandEnvironmentStringsW(ini, path, MAX_PATH);
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
            continue;
        wchar_t setting[16]{};
        // Seer's ini has no section header for this key: read it line by line
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE)
        {
            char buf[8192]{};
            DWORD read = 0;
            ReadFile(f, buf, sizeof(buf) - 1, &read, nullptr);
            CloseHandle(f);
            const std::string text(buf, read);
            const size_t at = text.find("tracking_file");
            if (at != std::string::npos)
            {
                const size_t eq = text.find('=', at), end = text.find_first_of("\r\n", at);
                if (eq != std::string::npos && (end == std::string::npos || eq < end))
                {
                    std::string v = text.substr(eq + 1, end == std::string::npos ? std::string::npos : end - eq - 1);
                    v.erase(0, v.find_first_not_of(" \t"));
                    MultiByteToWideChar(CP_UTF8, 0, v.c_str(), -1, setting, ARRAYSIZE(setting));
                }
            }
        }
        cached = _wcsnicmp(setting, L"false", 5) == 0 ? 0 : 1;
        return cached == 1;
    }
    cached = 1;
    return true;
}

// PowerToys Peek (PowerToysPeekProvider): <InstallLocation>\WinUI3Apps\PowerToys.Peek.UI.exe "<path>"
std::wstring PeekExe()
{
    for (HKEY hive : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE })
        for (const wchar_t* root : { L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall" })
        {
            HKEY key = nullptr;
            if (RegOpenKeyExW(hive, root, 0, KEY_READ, &key) != ERROR_SUCCESS)
                continue;
            wchar_t sub[256];
            for (DWORD i = 0;; ++i)
            {
                DWORD len = ARRAYSIZE(sub);
                if (RegEnumKeyExW(key, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                    break;
                wchar_t name[256]{}, location[MAX_PATH]{};
                DWORD n1 = sizeof(name), n2 = sizeof(location);
                if (RegGetValueW(key, sub, L"DisplayName", RRF_RT_REG_SZ, nullptr, name, &n1) != ERROR_SUCCESS || !StrStrIW(name, L"PowerToys"))
                    continue;
                if (RegGetValueW(key, sub, L"InstallLocation", RRF_RT_REG_SZ, nullptr, location, &n2) == ERROR_SUCCESS && location[0])
                {
                    RegCloseKey(key);
                    std::wstring exe = location;
                    if (exe.back() != L'\\')
                        exe += L'\\';
                    return exe + L"WinUI3Apps\\PowerToys.Peek.UI.exe"; // Files returns the first PowerToys entry with a location
                }
            }
            RegCloseKey(key);
        }
    return {};
}
}

std::wstring MainWindow::PreviewPopupPath() const
{
    // Files' LaunchPreviewPopup: exactly one selected item
    Tab* tab = Active();
    UniquePidl holder;
    PCIDLIST_ABSOLUTE item = tab && tab->SelectedCount() == 1 ? tab->SingleSelectedItem(holder) : nullptr;
    return PidlName(item, SIGDN_FILESYSPATH);
}

bool MainWindow::TryQuickLook()
{
    const std::wstring path = PreviewPopupPath();
    if (path.empty())
        return false;
    m_popupPath = path;
    if (QuickLookSend(L"Toggle", path))
    {
        m_popupProvider = 1;
        return true;
    }
    if (SeerToggle(path))
    {
        m_popupProvider = 2;
        return true;
    }
    const std::wstring peek = PeekExe();
    if (!peek.empty() && GetFileAttributesW(peek.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        ShellExecuteW(m_hwnd, nullptr, peek.c_str(), (L"\"" + path + L"\"").c_str(), nullptr, SW_HIDE);
        m_popupProvider = 3;
        return true;
    }
    return false;
}

void MainWindow::SwitchPreviewPopup()
{
    // Files' SwitchPopupPreviewAsync on every selection change (Peek follows by itself, so it has no switch)
    const std::wstring path = PreviewPopupPath();
    if (path.empty() || path == m_popupPath)
        return;
    m_popupPath = path;
    if (m_popupProvider == 1)
        QuickLookSend(L"Switch", path);
    else if (m_popupProvider == 2 && !g_seerPath.empty())
    {
        HWND seer = SeerWindow();
        if (!seer || !IsWindowVisible(seer))
            g_seerPath.clear();
        else if (!SeerTracksSelection())
            SeerToggle(g_seerPath); // Seer not following the selection: Files closes it
        else if (path != g_seerPath)
            SeerToggle(path);
    }
}

void MainWindow::ShowHistoryMenu(bool back, const RECT& button)
{
    Tab* tab = Active();
    if (!tab)
        return;
    const auto entries = tab->History(back);
    if (entries.empty())
        return;
    HMENU menu = CreatePopupMenu();
    for (size_t i = 0; i < entries.size() && i < 16; ++i)
    {
        std::wstring label = LocationName(entries[i]);
        for (size_t p = label.find(L'&'); p != std::wstring::npos; p = label.find(L'&', p + 2))
            label.insert(p, 1, L'&');
        AppendMenuW(menu, MF_STRING, MenuHistoryBase + i, label.c_str());
    }
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, button.left, button.bottom, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd >= MenuHistoryBase)
        tab->GoHistory(back, cmd - MenuHistoryBase + 1);
}

