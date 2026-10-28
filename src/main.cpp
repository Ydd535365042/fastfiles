#include "MainWindow.h"
#include "Drives.h"
#include "Update.h"
#include "Sandbox.h"

namespace
{
// Relative paths are resolved here: the primary instance has a different working directory
std::wstring Absolute(const std::wstring& arg)
{
    if (arg.empty() || arg.find(L':') != std::wstring::npos || arg.rfind(L"\\\\", 0) == 0)
        return arg;
    wchar_t full[MAX_PATH * 2]{};
    const DWORD n = GetFullPathNameW(arg.c_str(), ARRAYSIZE(full), full, nullptr);
    return n > 0 && n < ARRAYSIZE(full) ? std::wstring(full) : arg;
}

bool Elevated()
{
    HANDLE token = nullptr;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated;
    if (token)
        CloseHandle(token);
    return ok;
}

// One command-line argument as CommandLineToArgvW reads it back (backslashes before a quote doubled)
std::wstring Quote(const std::wstring& arg)
{
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg)
    {
        if (c == L'\\')
        {
            ++slashes;
            continue;
        }
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}

// FastFiles runs as administrator by default: a plain launch without an elevated token starts itself again through
// "runas" (one UAC prompt where UAC is on) and quits. False when that didn't happen (prompt declined, no admin
// account entered): the launch goes on without admin rights. Test mode never elevates; FASTFILES_TEST_ELEVATE=1
// there writes the command it would run to <data>\elevate-log.txt instead.
bool RelaunchElevated(const std::wstring& params)
{
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (Sandbox::Active())
    {
        wchar_t v[4]{};
        if (!GetEnvironmentVariableW(L"FASTFILES_TEST_ELEVATE", v, 4) || v[0] != L'1')
            return false;
        if (FILE* f = nullptr; _wfopen_s(&f, (Sandbox::Dir() + L"elevate-log.txt").c_str(), L"a, ccs=UTF-8") == 0 && f)
        {
            fwprintf(f, L"runas \"%s\" %s\n", exe, params.c_str());
            fclose(f);
        }
        return true;
    }
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = params.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    // --window <path>: extra window (Ctrl+N), never forwarded. --background: started with Windows, stay in the tray.
    // --tab <hex> [--pos x,y]: a tab moved out of another window (both panes), placed where it was dropped.
    bool restoreSession = true, background = false, select = false, hasPos = false, restarted = false;
    POINT pos{};
    SessionData::TabState startTab;
    bool hasTab = false;
    std::wstring target;
    int argc = 0;
    if (PWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc))
    {
        for (int i = 1; i < argc; ++i)
        {
            if (wcscmp(argv[i], L"--default-file-manager") == 0 && i + 1 < argc)
            {
                // The uninstaller runs "--default-file-manager off" so folders don't point at a deleted exe
                const bool ok = SetDefaultFileManager(wcscmp(argv[i + 1], L"on") == 0);
                LocalFree(argv);
                return ok ? 0 : 2;
            }
            if (wcscmp(argv[i], L"--format-drive") == 0 && i + 1 < argc)
            {
                // Started elevated by 格式化 (Drives::Format): only the system's Format dialog, no window
                const int code = Drives::RunFormatDialog(argv[i + 1]);
                LocalFree(argv);
                return code;
            }
            if (wcscmp(argv[i], L"--wait-pid") == 0 && i + 1 < argc)
            {
                restarted = true;
                // RestartApp: let the old process finish quitting first
                if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, wcstoul(argv[++i], nullptr, 10)))
                {
                    WaitForSingleObject(old, 15000);
                    CloseHandle(old);
                }
            }
            else if (wcscmp(argv[i], L"--window") == 0)
                restoreSession = false;
            else if (wcscmp(argv[i], L"--background") == 0)
                background = true;
            else if (wcscmp(argv[i], L"--select") == 0 || _wcsicmp(argv[i], L"/select") == 0)
                select = true;
            else if (wcscmp(argv[i], L"--pos") == 0 && i + 1 < argc)
                hasPos = swscanf_s(argv[++i], L"%ld,%ld", &pos.x, &pos.y) == 2;
            else if (wcscmp(argv[i], L"--tab") == 0 && i + 1 < argc)
            {
                const std::vector<BYTE> bytes = HexToBytes(argv[++i]);
                hasTab = !bytes.empty() && DecodeTab(bytes.data(), bytes.size(), startTab);
            }
            else if (target.empty())
                target = argv[i];
        }
        LocalFree(argv);
    }
    target = Absolute(target);
    // Forwarded launches carry the flag in the target itself
    const std::wstring forwarded = select && !target.empty() ? L"|select|" + target : target;

    // Single instance: a plain launch goes to the running primary window (as a new tab) when the setting is on.
    // The named object only marks "a primary exists"; the window is found by class + property + same exe.
    HANDLE primaryMutex = nullptr;
    bool primary = false;
    if (restoreSession)
    {
        const std::wstring name = PrimaryMutexName();
        const bool single = LoadSettings().singleInstance;
        for (int attempt = 0; attempt < 30 && !primary; ++attempt)
        {
            primaryMutex = CreateMutexW(nullptr, FALSE, name.c_str());
            if (primaryMutex && GetLastError() != ERROR_ALREADY_EXISTS)
            {
                primary = true;
                break;
            }
            if (!single)
                break; // a second full instance, as before (it saves the session too)
            if (HWND existing = FindPrimaryWindow(); existing && ForwardToPrimary(existing, background ? std::wstring() : forwarded))
                return 0;
            // The primary is still starting (no window yet) or just quitting: wait a little and retry
            if (primaryMutex)
                CloseHandle(primaryMutex);
            primaryMutex = nullptr;
            Sleep(100);
        }
    }
    else if (background)
    {
        background = false;
    }

    // Administrator by default. Only a plain launch asks, once: a later one was handed to the running window above,
    // and the windows / tabs / restarts an elevated FastFiles starts inherit its token. The primary mark is released
    // first, so the elevated copy becomes the primary instance.
    if (restoreSession && !restarted && !Elevated())
    {
        std::wstring params;
        if (background)
            params = L"--background";
        if (!target.empty())
            params += (params.empty() ? L"" : L" ") + std::wstring(select ? L"--select " : L"") + Quote(target);
        if (primaryMutex)
            CloseHandle(primaryMutex);
        primaryMutex = nullptr;
        if (RelaunchElevated(params))
            return 0;
        // Declined: go on without admin rights, as the primary instance again unless one started meanwhile
        if (primary)
        {
            primaryMutex = CreateMutexW(nullptr, FALSE, PrimaryMutexName().c_str());
            primary = primaryMutex && GetLastError() != ERROR_ALREADY_EXISTS;
            if (!primary && primaryMutex)
            {
                CloseHandle(primaryMutex);
                primaryMutex = nullptr;
            }
        }
    }

    Log::Start(); // keep the last 100 lines, record crashes
    Update::CleanUp(); // FastFiles.exe.old and the setup download left by an update

    // OLE (not just COM) so the shell view gets drag-and-drop and clipboard
    if (FAILED(OleInitialize(nullptr)))
    {
        Log::Error(L"OleInitialize failed");
        return 1;
    }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    UniquePidl startup, selectItem;
    if (!target.empty())
        ParseLaunchTarget(target, startup, selectItem, select);

    int exitCode = 1;
    {
        MainWindow window;
        window.SetLaunchMode(primary, background && primary);
        if (hasPos)
            window.SetStartPosition(pos);
        if (hasTab)
            window.SetStartTab(std::move(startTab));
        if (window.Create(startup.get(), restoreSession, reinterpret_cast<PCUITEMID_CHILD>(selectItem.get())))
        {
            NewItem::StartScan(); // the 新建 types (Files reads them once), on a low-priority worker

            MSG msg{};
            while (GetMessageW(&msg, nullptr, 0, 0) > 0)
            {
                if (window.PreTranslate(&msg))
                    continue;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            exitCode = static_cast<int>(msg.wParam);
        }
    }
    OleUninitialize();
    if (primaryMutex)
        CloseHandle(primaryMutex);
    return exitCode;
}
