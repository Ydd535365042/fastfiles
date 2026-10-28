#include "Setup.h"
#include <restartmanager.h>
#include <exdisp.h>
#include <shldisp.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <vector>

using Microsoft::WRL::ComPtr;

std::wstring KnownFolder(REFKNOWNFOLDERID id)
{
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path)))
        out = path;
    CoTaskMemFree(path);
    return out;
}

std::wstring ExePath()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n < path.size())
        {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring Join(const std::wstring& dir, const wchar_t* name)
{
    return dir.empty() || dir.back() == L'\\' ? dir + name : dir + L"\\" + name;
}

bool SamePath(const std::wstring& a, const std::wstring& b)
{
    auto trim = [](std::wstring s) { while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back(); return s; };
    return CompareStringOrdinal(trim(a).c_str(), -1, trim(b).c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool DeleteRetry(const std::wstring& path, int tries)
{
    for (int i = 0; i < tries; ++i)
    {
        if (DeleteFileW(path.c_str()))
            return true;
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
            return true;
        Sleep(150); // a process that just exited may still hold its image for a moment
    }
    return false;
}

std::wstring LastErrorText(DWORD code)
{
    PWSTR text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
        reinterpret_cast<PWSTR>(&text), 0, nullptr);
    std::wstring out = text ? text : L"";
    LocalFree(text);
    while (!out.empty() && (out.back() == L'\n' || out.back() == L'\r' || out.back() == L' '))
        out.pop_back();
    return out.empty() ? L"错误 " + std::to_wstring(code) : out;
}

// Restart Manager sends the app WM_QUERYENDSESSION/WM_ENDSESSION, which FastFiles handles by saving its session and
// really exiting (even when it waits in the tray). Nothing is force-killed.
bool CloseRunning(const std::wstring& exe, const Progress& progress, std::wstring& error)
{
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
        return true;
    DWORD session = 0;
    WCHAR key[CCH_RM_SESSION_KEY + 1]{};
    if (RmStartSession(&session, 0, key) != ERROR_SUCCESS)
        return true; // no Restart Manager: the file replace below reports a lock
    bool ok = true;
    LPCWSTR files[] = { exe.c_str() };
    UINT need = 0, count = 0;
    DWORD reasons = 0;
    if (RmRegisterResources(session, 1, files, 0, nullptr, 0, nullptr) == ERROR_SUCCESS &&
        RmGetList(session, &need, &count, nullptr, &reasons) == ERROR_MORE_DATA && need > 0)
    {
        progress(-1, L"正在关闭 FastFiles…");
        if (RmShutdown(session, 0, nullptr) != ERROR_SUCCESS)
        {
            ok = false;
            error = L"FastFiles 正在运行，无法自动关闭。请退出 FastFiles 后重试。";
        }
    }
    RmEndSession(session);
    return ok;
}

bool MakeShortcut(const std::wstring& link, const std::wstring& target, const std::wstring& dir)
{
    ComPtr<IShellLinkW> shell;
    ComPtr<IPersistFile> file;
    return SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell))) &&
        SUCCEEDED(shell->SetPath(target.c_str())) && SUCCEEDED(shell->SetWorkingDirectory(dir.c_str())) &&
        SUCCEEDED(shell->SetIconLocation(target.c_str(), 0)) && SUCCEEDED(shell.As(&file)) && SUCCEEDED(file->Save(link.c_str(), TRUE));
}

namespace
{
// The account a process runs as (its token's user SID)
std::vector<BYTE> TokenUser(HANDLE process)
{
    std::vector<BYTE> sid;
    HANDLE token = nullptr;
    if (!process || !OpenProcessToken(process, TOKEN_QUERY, &token))
        return sid;
    DWORD size = 0;
    GetTokenInformation(token, ::TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    if (size && GetTokenInformation(token, ::TokenUser, buffer.data(), size, &size))
    {
        PSID user = reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid;
        sid.assign(static_cast<BYTE*>(user), static_cast<BYTE*>(user) + GetLengthSid(user));
    }
    CloseHandle(token);
    return sid;
}

// The setup (elevated) runs as the signed-in user: the desktop's Explorer has the same account
bool SameUserAsShell()
{
    DWORD pid = 0;
    if (HWND shell = GetShellWindow())
        GetWindowThreadProcessId(shell, &pid);
    HANDLE explorer = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    const std::vector<BYTE> theirs = TokenUser(explorer), ours = TokenUser(GetCurrentProcess());
    if (explorer)
        CloseHandle(explorer);
    return !ours.empty() && theirs == ours;
}
}

// FastFiles runs as administrator (it elevates itself at start). The setup is elevated already: when it runs as the
// signed-in user, start the app straight from here, elevated, without a second UAC prompt. When it runs as another
// account (a standard user entered an administrator's password), ask the desktop's Explorer to start it, so it runs
// as the signed-in user with that user's data, and asks for administrator rights itself.
bool LaunchAsUser(const std::wstring& exe)
{
    if (SameUserAsShell())
    {
        std::wstring cmd = L"\"" + exe + L"\"";
        const std::wstring dir = exe.substr(0, exe.find_last_of(L'\\'));
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi))
        {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return true;
        }
    }
    ComPtr<IShellWindows> windows;
    ComPtr<IDispatch> dispatch;
    ComPtr<IServiceProvider> provider;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IDispatch> background;
    ComPtr<IShellFolderViewDual> folderView;
    ComPtr<IDispatch> app;
    ComPtr<IShellDispatch2> shell;
    VARIANT empty{};
    long hwnd = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows))) &&
        windows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &dispatch) == S_OK && dispatch &&
        SUCCEEDED(dispatch.As(&provider)) && SUCCEEDED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) &&
        SUCCEEDED(browser->QueryActiveShellView(&view)) && SUCCEEDED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background))) &&
        SUCCEEDED(background.As(&folderView)) && SUCCEEDED(folderView->get_Application(&app)) && SUCCEEDED(app.As(&shell)))
    {
        BSTR file = SysAllocString(exe.c_str());
        VARIANT args{}, dir{}, verb{}, show{};
        show.vt = VT_I4;
        show.lVal = SW_SHOWNORMAL;
        const HRESULT hr = shell->ShellExecute(file, args, dir, verb, show);
        SysFreeString(file);
        if (SUCCEEDED(hr))
            return true;
    }
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, nullptr, exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

DWORD RunWait(const std::wstring& exe, const std::wstring& args, DWORD timeout)
{
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return static_cast<DWORD>(-1);
    DWORD code = static_cast<DWORD>(-1);
    if (WaitForSingleObject(pi.hProcess, timeout) == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

bool IsWindows11()
{
    using Fn = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
    static const DWORD build = []
    {
        RTL_OSVERSIONINFOW v{ sizeof(v) };
        if (auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
            fn(&v);
        return v.dwBuildNumber;
    }();
    return build >= 22000; // rounded corners and DWM's own border
}
