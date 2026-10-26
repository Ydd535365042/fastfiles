#include "Setup.h"
#include <objidl.h>
#include <gdiplus.h>

namespace
{
// Inno-compatible switches: /SILENT, /VERYSILENT (/S), /DIR="x", /MERGETASKS="!desktopicon", /uninstall, /DELETEDATA
void Parse(Options& o)
{
    int argc = 0;
    PWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return;
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring a = argv[i];
        auto is = [&](const wchar_t* s) { return CompareStringOrdinal(a.c_str(), -1, s, -1, TRUE) == CSTR_EQUAL; };
        auto value = [&](const wchar_t* prefix)
        {
            const size_t n = wcslen(prefix);
            return a.size() > n && CompareStringOrdinal(a.c_str(), static_cast<int>(n), prefix, static_cast<int>(n), TRUE) == CSTR_EQUAL
                ? a.substr(n) : std::wstring();
        };
        if (is(L"/SILENT"))
            o.silent = true;
        else if (is(L"/VERYSILENT") || is(L"/S"))
            o.silent = o.verySilent = true;
        else if (is(L"/uninstall"))
            o.uninstall = true;
        else if (is(L"/DELETEDATA"))
            o.deleteData = true;
        else if (std::wstring d = value(L"/DIR="); !d.empty())
            o.dir = d;
        else if (std::wstring f = value(L"/uninstall-from="); !f.empty())
        {
            o.uninstall = true;
            o.from = f;
        }
        else if (std::wstring u = value(L"/TESTUNINSTALL="); !u.empty())
        {
            // Test-only hook (tests/test-ff-h30.ps1): runs RunUninstall directly against a fake install folder, with
            // none of the temp-copy relaunch or self-delete-on-reboot scheduling real uninstalls go through below.
            o.uninstall = true;
            o.from = u;
            o.testUninstall = true;
        }
        else if (std::wstring t = value(L"/MERGETASKS="); !t.empty())
            o.desktopIcon = t.find(L"!desktopicon") == std::wstring::npos;
        else if (std::wstring t2 = value(L"/TASKS="); !t2.empty() || is(L"/TASKS="))
            o.desktopIcon = t2.find(L"desktopicon") != std::wstring::npos && t2.find(L"!desktopicon") == std::wstring::npos;
    }
    LocalFree(argv);
    for (std::wstring* s : { &o.dir, &o.from })
        while (s->size() > 3 && (s->back() == L'\\' || s->back() == L'"'))
            s->pop_back();
}

// Uninstall.exe can't delete itself while it runs: continue from a temp copy, which deletes the original
int RelaunchFromTemp(const Options& o, const std::wstring& dir)
{
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring copy = std::wstring(temp) + L"FastFiles-uninstall-" + std::to_wstring(GetCurrentProcessId()) + L".exe";
    if (!CopyFileW(ExePath().c_str(), copy.c_str(), FALSE))
        return -1;
    std::wstring cmd = L"\"" + copy + L"\" \"/uninstall-from=" + dir + L"\"";
    if (o.verySilent)
        cmd += L" /VERYSILENT";
    else if (o.silent)
        cmd += L" /SILENT";
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(copy.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        DeleteFileW(copy.c_str());
        return -1;
    }
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ExitOk;
}

std::wstring FolderOf(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? path : path.substr(0, slash);
}

std::wstring Env(const wchar_t* name)
{
    wchar_t value[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(name, value, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? value : std::wstring();
}

// Test hook only: FASTFILES_SETUP_PREVIEW=install|uninstall drives the whole wizard with a simulated worker, never
// a real install/uninstall. Honoured only together with FASTFILES_DATA being set too (same gating as FastFiles'
// other test hooks: Sandbox::Active()), so a plain launch on a real machine is never affected by a stray env var.
bool ReadPreview(Options& o)
{
    const std::wstring data = Env(L"FASTFILES_DATA");
    if (data.empty())
        return false;
    const std::wstring mode = Env(L"FASTFILES_SETUP_PREVIEW");
    if (CompareStringOrdinal(mode.c_str(), -1, L"install", -1, TRUE) == CSTR_EQUAL)
        o.uninstall = false;
    else if (CompareStringOrdinal(mode.c_str(), -1, L"uninstall", -1, TRUE) == CSTR_EQUAL)
        o.uninstall = true;
    else
        return false;
    o.preview = true;
    // Never a real folder: harmless even though preview never writes to o.dir (RunInstall/RunUninstall are never
    // called), kept inside the test's own data directory so nothing can ever point at Program Files.
    o.dir = data + L"\\setup-preview";
    o.from = o.dir;
    return true;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    Options o;
    Parse(o);
    if (o.testUninstall)
    {
        // Test-only path: no relaunch from a temp copy, no scheduling this exe's own deletion, no wizard window,
        // no mutex. Refuses to run at all unless FASTFILES_DATA is set, so it can never do anything for real even if
        // this switch were passed outside of tests/test-ff-h30.ps1 (RunUninstall's registry edits become dry runs,
        // logged under that folder, and its only filesystem target is o.from, the fake folder the test built).
        wchar_t sandbox[4]{};
        if (!GetEnvironmentVariableW(L"FASTFILES_DATA", sandbox, 4))
            return ExitInitFailed;
        o.dir = o.from;
        std::wstring error;
        const Progress none = [](int, const std::wstring&) {};
        return RunUninstall(o, none, error) ? ExitOk : ExitFailed;
    }
    // Test hook only: a preview run (FASTFILES_SETUP_PREVIEW + FASTFILES_DATA) skips the payload check, the
    // uninstall relaunch-to-temp-copy, and (below) the real install/uninstall paths entirely.
    const bool preview = ReadPreview(o);
    if (!preview)
    {
        // An installed copy (Uninstall.exe, no payload) only uninstalls
        const bool payload = HasPayload();
        if (!payload)
            o.uninstall = true;
        if (o.uninstall && o.from.empty())
        {
            // Uninstall.exe: its own folder. Setup.exe /uninstall: the registered install.
            const std::wstring dir = payload ? DefaultInstallDir() : FolderOf(ExePath());
            if (const int code = RelaunchFromTemp(o, dir); code >= 0)
                return code;
            o.from = dir; // no temp copy: uninstall in place (Uninstall.exe itself stays behind)
        }
        if (o.uninstall)
            o.dir = o.from;
        else if (o.dir.empty())
            o.dir = DefaultInstallDir();
        o.desktopIcon = o.desktopIcon && (o.uninstall || HasDesktopIcon());
    }

    HANDLE once = CreateMutexW(nullptr, TRUE, L"Local\\FastFiles.Setup");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (HWND other = FindWindowW(L"FastFiles.Setup", nullptr))
            SetForegroundWindow(other);
        return ExitInitFailed;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
        return ExitInitFailed;
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdiplus = 0;
    Gdiplus::GdiplusStartup(&gdiplus, &gsi, nullptr);

    int code = ExitOk;
    if (preview)
        code = RunUi(o); // simulated worker only (Ui::Start): never verySilent, never the real RunInstall/RunUninstall
    else if (o.verySilent)
    {
        std::wstring error;
        const Progress none = [](int, const std::wstring&) {};
        code = (o.uninstall ? RunUninstall(o, none, error) : RunInstall(o, none, error)) ? ExitOk : ExitFailed;
    }
    else
        code = RunUi(o);

    // The temp copy we run as goes away on the next restart (it can't delete itself either); never for a preview
    if (!preview && o.uninstall && !o.from.empty() && !SamePath(FolderOf(ExePath()), o.from))
        MoveFileExW(ExePath().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    Gdiplus::GdiplusShutdown(gdiplus);
    CoUninitialize();
    if (once)
        CloseHandle(once);
    return code;
}
