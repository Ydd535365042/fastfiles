#include "MainWindow.h"
#include "Lang.h"
#include "Sandbox.h"

// Background process and single instance (Files: "continue running in the background", "open folders in a new tab").
// The first plain launch is the primary instance: it owns the session, the tray icon and receives later launches
// (WM_COPYDATA with the folder/file to open). Ctrl+N windows (--window) stay independent processes, as before.

namespace
{
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"FastFiles";
constexpr ULONG_PTR kCopyOpen = 0x46464F31; // 'FFO1': payload is the UTF-16 target, may be empty
constexpr wchar_t kPrimaryProp[] = L"FastFiles.Primary";
constexpr UINT kTrayId = 1;

std::wstring ExePath()
{
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return exe;
}

bool SameExe(HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return false;
    wchar_t image[MAX_PATH]{};
    DWORD size = MAX_PATH;
    const bool same = QueryFullProcessImageNameW(process, 0, image, &size) && _wcsicmp(image, ExePath().c_str()) == 0;
    CloseHandle(process);
    return same;
}

UINT TaskbarCreated()
{
    static const UINT message = RegisterWindowMessageW(L"TaskbarCreated");
    return message;
}
}

std::wstring PrimaryMutexName()
{
    // Per exe path: a dev build and the installed copy don't hand launches to each other
    std::wstring exe = ExePath();
    CharLowerBuffW(exe.data(), static_cast<DWORD>(exe.size()));
    unsigned long long hash = 1469598103934665603ull;
    for (wchar_t c : exe)
        hash = (hash ^ c) * 1099511628211ull;
    wchar_t name[64];
    swprintf_s(name, L"Local\\FastFiles.Primary.%016llx", hash);
    return name;
}

HWND FindPrimaryWindow()
{
    for (HWND hwnd = FindWindowExW(nullptr, nullptr, L"FastFiles.MainWindow", nullptr); hwnd;
         hwnd = FindWindowExW(nullptr, hwnd, L"FastFiles.MainWindow", nullptr))
        if (GetPropW(hwnd, kPrimaryProp) && SameExe(hwnd))
            return hwnd;
    return nullptr;
}

bool ForwardToPrimary(HWND primary, const std::wstring& target)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(primary, &pid);
    AllowSetForegroundWindow(pid); // we were just launched by the user, so we may pass on the right to come forward
    COPYDATASTRUCT cds{ kCopyOpen, static_cast<DWORD>(target.size() * sizeof(wchar_t)), const_cast<wchar_t*>(target.c_str()) };
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(primary, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 5000, &result) && result == 1;
}

bool ParseLaunchTarget(const std::wstring& raw, UniquePidl& folder, UniquePidl& select, bool forceSelect)
{
    std::wstring arg = raw;
    // "|select|<path>": select the item in its folder even when it's a folder (explorer /select, Files' --select)
    if (arg.rfind(L"|select|", 0) == 0)
    {
        arg = arg.substr(8);
        forceSelect = true;
    }
    // CommandLineToArgvW turns "C:\" into C:" (backslash escapes the quote), so strip stray quotes
    while (!arg.empty() && (arg.back() == L'"' || arg.back() == L' '))
        arg.pop_back();
    if (arg.size() == 2 && arg[1] == L':')
        arg += L'\\';
    PIDLIST_ABSOLUTE pidl = nullptr;
    SFGAOF attrs = 0;
    if (arg.empty() || FAILED(SHParseDisplayName(arg.c_str(), nullptr, &pidl, SFGAO_FOLDER | SFGAO_STREAM, &attrs)))
        return false;
    folder.reset(pidl);
    select.reset();
    // A file: open its folder with the file selected (like explorer /select)
    if ((forceSelect || !(attrs & SFGAO_FOLDER) || (attrs & SFGAO_STREAM)) && !ILIsEmpty(pidl))
    {
        select.reset(reinterpret_cast<PIDLIST_ABSOLUTE>(ILClone(ILFindLastID(pidl))));
        ILRemoveLastID(folder.get());
    }
    return true;
}

bool AutoStartEnabled()
{
    wchar_t value[MAX_PATH * 2]{};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS)
        return false;
    // Only counts when it starts this copy (a stale entry from another install folder doesn't)
    const std::wstring expected = L"\"" + ExePath() + L"\"";
    return _wcsnicmp(value, expected.c_str(), expected.size()) == 0;
}

bool SetAutoStart(bool on)
{
    if (Sandbox::Active())
        return false; // test mode: the Run key isn't touched
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    LSTATUS status;
    if (on)
    {
        const std::wstring command = L"\"" + ExePath() + L"\" --background";
        status = RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    }
    else
    {
        status = RegDeleteValueW(key, kRunValue);
        if (status == ERROR_FILE_NOT_FOUND)
            status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

// --- Default file manager (Files: Settings > 高级 > 将 Files 设为默认文件管理器, off by default) ---
// The same keys Files' SetFilesAsDefault.reg writes, all under HKCU: folders open (and explore) with FastFiles, and
// Win+E (the "This PC" opennewwindow verb) starts it. Unset removes only what this wrote, so Explorer's own
// HKLM defaults show through again.

namespace
{
constexpr wchar_t kFolderOpen[] = L"Software\\Classes\\Folder\\shell\\open\\command";
constexpr wchar_t kFolderExplore[] = L"Software\\Classes\\Folder\\shell\\explore\\command";
constexpr wchar_t kWinE[] = L"Software\\Classes\\CLSID\\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}";
constexpr wchar_t kWinECommand[] = L"Software\\Classes\\CLSID\\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}\\shell\\opennewwindow\\command";

bool WriteCommand(const wchar_t* subkey, const std::wstring& command)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    // REG_EXPAND_SZ like Files'; DelegateExecute empty so the command line is used instead of Explorer's handler
    LSTATUS s = RegSetValueExW(key, nullptr, 0, REG_EXPAND_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    if (s == ERROR_SUCCESS)
        s = RegSetValueExW(key, L"DelegateExecute", 0, REG_SZ, reinterpret_cast<const BYTE*>(L""), sizeof(wchar_t));
    RegCloseKey(key);
    return s == ERROR_SUCCESS;
}

bool IsOurs(const wchar_t* subkey)
{
    wchar_t value[MAX_PATH * 2]{};
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, subkey, nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, value, &size) == ERROR_SUCCESS &&
        StrStrIW(value, L"FastFiles.exe") != nullptr;
}

// Deletes subkey and then each parent up to (not including) stop, while they're empty
void DeleteEmptyChain(std::wstring subkey, const std::wstring& stop)
{
    while (!subkey.empty() && _wcsicmp(subkey.c_str(), stop.c_str()) != 0)
    {
        HKEY key = nullptr;
        DWORD subkeys = 0, values = 0;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_READ, &key) == ERROR_SUCCESS)
        {
            RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
            RegCloseKey(key);
            if (subkeys || values || RegDeleteKeyW(HKEY_CURRENT_USER, subkey.c_str()) != ERROR_SUCCESS)
                return;
        }
        const size_t slash = subkey.find_last_of(L'\\');
        subkey = slash == std::wstring::npos ? std::wstring() : subkey.substr(0, slash);
    }
}

void UnsetCommand(const wchar_t* subkey)
{
    if (!IsOurs(subkey))
        return; // someone else's handler: leave it
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS)
    {
        RegDeleteValueW(key, nullptr);
        RegDeleteValueW(key, L"DelegateExecute");
        RegCloseKey(key);
    }
    DeleteEmptyChain(subkey, L"Software\\Classes");
}
}

bool DefaultFileManagerEnabled()
{
    // Files: DetectIsSetAsDefaultFileManager reads Folder\shell\open\command through HKCR
    wchar_t value[MAX_PATH * 2]{};
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CLASSES_ROOT, L"Folder\\shell\\open\\command", nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
               nullptr, value, &size) == ERROR_SUCCESS &&
        StrStrIW(value, L"FastFiles.exe") != nullptr;
}

bool SetDefaultFileManager(bool on)
{
    if (Sandbox::Active())
        return false; // test mode: HKCU\Software\Classes isn't touched
    const std::wstring exe = L"\"" + ExePath() + L"\"";
    bool ok = true;
    if (on)
    {
        ok = WriteCommand(kFolderOpen, exe + L" \"%1\"") && WriteCommand(kFolderExplore, exe + L" \"%1\"") && WriteCommand(kWinECommand, exe);
        if (!ok)
            on = false; // half written: take it back out
    }
    if (!on)
    {
        UnsetCommand(kFolderOpen);
        UnsetCommand(kFolderExplore);
        if (IsOurs(kWinECommand))
            RegDeleteTreeW(HKEY_CURRENT_USER, kWinE); // Files' Unset removes the whole CLSID key too
    }
    // No SHCNE_ASSOCCHANGED: it makes Explorer refresh the desktop and every icon, and Folder\shell is read per launch
    return ok && DefaultFileManagerEnabled() == on;
}

void MainWindow::SetLaunchMode(bool primary, bool hidden)
{
    m_primary = primary;
    m_startHidden = hidden;
}

void MainWindow::UpdateTray()
{
    // Only the primary instance has one: shown while it runs (Files' ShowSystemTrayIcon), and kept while hidden even if
    // the setting was just turned off
    const bool want = m_primary && (m_settings.trayIcon || m_hiddenToTray);
    if (want == m_trayShown)
        return;
    if (Sandbox::Active())
    {
        // Test mode: no real notification icon (Windows remembers every one it saw); a window property says what would show
        m_trayShown = want;
        if (want)
            SetPropW(m_hwnd, L"FastFiles.Tray", reinterpret_cast<HANDLE>(1));
        else
            RemovePropW(m_hwnd, L"FastFiles.Tray");
        return;
    }
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = m_hwnd;
    nid.uID = kTrayId;
    if (want)
    {
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        nid.uCallbackMessage = kMsgTray;
        nid.hIcon = reinterpret_cast<HICON>(GetClassLongPtrW(m_hwnd, GCLP_HICONSM));
        wcscpy_s(nid.szTip, L"FastFiles");
        m_trayShown = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
        if (m_trayShown)
        {
            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
        }
    }
    else
    {
        Shell_NotifyIconW(NIM_DELETE, &nid);
        m_trayShown = false;
    }
}

void MainWindow::HideToTray()
{
    SaveSessionState();
    if (m_paletteOpen)
        ClosePalette(false);
    m_hiddenToTray = true;
    UpdateTray();
    m_center.Hide();
    ShowWindow(m_hwnd, SW_HIDE);
    // Files (App.OnClosing to the background): RemoveAllCompletedItems
    m_ops.ForgetFinished();
    m_center.Refresh();
    m_sizer.Cancel();
    // Hidden for possibly hours: give the memory back until the window comes back
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}

void MainWindow::ShowFromTray()
{
    const bool wasHidden = m_hiddenToTray || !IsWindowVisible(m_hwnd);
    m_hiddenToTray = false;
    if (wasHidden)
        ShowWindow(m_hwnd, m_showMaximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    m_showMaximized = false;
    if (IsIconic(m_hwnd))
        ShowWindow(m_hwnd, SW_RESTORE);
    SetForegroundWindow(m_hwnd);
    UpdateTray();
    if (wasHidden)
    {
        Layout();
        if (Tab* tab = Active())
            tab->Focus();
    }
}

void MainWindow::ExitApp()
{
    m_quitting = true;
    PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
}

void MainWindow::RestartApp()
{
    // The new process waits for this one (session saved, single-instance mutex released) before it starts
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring args = L"--wait-pid " + std::to_wstring(GetCurrentProcessId());
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, nullptr, exe, args.c_str(), nullptr, SW_SHOWNORMAL)) > 32)
        ExitApp();
}

void MainWindow::ShowTrayMenu(POINT pt)
{
    enum : UINT { TrayOpen = 1, TrayNewWindow, TraySettings, TrayExit };
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, TrayOpen, Tr(L"打开 FastFiles"));
    AppendMenuW(menu, MF_STRING, TrayNewWindow, Tr(L"新建窗口"));
    AppendMenuW(menu, MF_STRING, TraySettings, Tr(L"设置"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, TrayExit, Tr(L"退出"));
    SetMenuDefaultItem(menu, TrayOpen, FALSE);
    // Required so the menu closes when the user clicks elsewhere
    SetForegroundWindow(m_hwnd);
    const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, m_hwnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(m_hwnd, WM_NULL, 0, 0);
    switch (cmd)
    {
    case TrayOpen: ShowFromTray(); break;
    case TrayNewWindow:
    {
        UniquePidl home = DefaultLocation();
        OpenNewWindow(home.get());
        break;
    }
    case TraySettings:
        ShowFromTray();
        OpenSettings(kSettingsAdvanced);
        break;
    case TrayExit: ExitApp(); break;
    }
}

// Messages for the background / single-instance machinery; true when handled
bool MainWindow::HandleTrayMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result)
{
    if (msg == TaskbarCreated())
    {
        // Explorer restarted: the icon is gone
        m_trayShown = false;
        UpdateTray();
        result = 0;
        return true;
    }
    switch (msg)
    {
    case kMsgTray:
        // NOTIFYICON_VERSION_4: event in LOWORD(lParam), anchor point in wParam
        switch (LOWORD(lp))
        {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK:
            ShowFromTray();
            break;
        case WM_CONTEXTMENU:
            ShowTrayMenu({ GET_X_LPARAM(wp), GET_Y_LPARAM(wp) });
            break;
        }
        result = 0;
        return true;
    case WM_COPYDATA:
    {
        const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lp);
        if (!m_primary || !cds || cds->dwData != kCopyOpen)
            return false;
        // Copied out and posted: the sender is blocked in SendMessage until we return
        auto* target = new std::wstring(static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t));
        if (!PostMessageW(m_hwnd, kMsgLaunch, 0, reinterpret_cast<LPARAM>(target)))
            delete target;
        result = 1;
        return true;
    }
    case kMsgLaunch:
    {
        std::unique_ptr<std::wstring> target(reinterpret_cast<std::wstring*>(lp));
        const bool wasHidden = m_hiddenToTray || !IsWindowVisible(m_hwnd);
        ShowFromTray();
        UniquePidl folder, select;
        if (!target->empty() && ParseLaunchTarget(*target, folder, select))
        {
            // Not activated yet: activation browses there, and the selection has to be waiting when that completes
            const int index = NewTab(folder.get(), false);
            if (index >= 0 && index < static_cast<int>(m_tabList.size()))
            {
                if (select)
                    m_tabList[index].panes[0]->SelectAfterNavigate(reinterpret_cast<PCUITEMID_CHILD>(select.get()));
                ActivateTab(index);
            }
        }
        else if (!wasHidden)
        {
            // Plain second launch while the window is already up: like Files, a new tab
            NewTab(nullptr, true);
        }
        result = 0;
        return true;
    }
    case WM_QUERYENDSESSION:
        if (m_restoreSession)
            SaveSessionState();
        result = TRUE;
        return true;
    case WM_ENDSESSION:
        // Logoff, or an installer's Restart Manager closing us: really exit, even from the tray
        if (wp)
        {
            m_quitting = true;
            DestroyWindow(m_hwnd);
        }
        result = 0;
        return true;
    }
    return false;
}
