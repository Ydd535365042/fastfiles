#include "Setup.h"
#include "Payload.h"
#include <compressapi.h>
#include <aclapi.h>
#include <shlwapi.h>
#include <algorithm>
#include <vector>

namespace
{
constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FastFiles";
// Inno Setup builds (0.9.4 and earlier) registered "{AppId}_is1"; an upgrade takes that install over
constexpr wchar_t kLegacyKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{6E3B1F0A-5C2D-4F7E-9B8A-FA57F11E0001}_is1";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
// 0.9.29 and earlier shipped 7z.dll and its license next to the exe (built-in compress / extract through 7z.dll);
// removed at the user's request (the shell's own menu items - WinRAR, 7-Zip... - cover it now). Cleaned up here
// regardless of kCompanionFiles so an upgrade or an uninstall of one of those older installs leaves nothing behind.
constexpr const wchar_t* kLegacyCompanionFiles[] = { L"7z.dll", L"7z.License.txt" };

std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* name)
{
    // RRF_SUBKEY_WOW6464KEY (Windows 10+) is left out: this is an x64-only build, so a 64-bit process already sees
    // the 64-bit registry view with no WOW64 redirection to force past.
    wchar_t buf[1024]{};
    DWORD size = sizeof(buf);
    return RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS ? buf : L"";
}

std::wstring StartMenuLink() { return Join(KnownFolder(FOLDERID_CommonPrograms), L"FastFiles.lnk"); }
std::wstring DesktopLink() { return Join(KnownFolder(FOLDERID_PublicDesktop), L"FastFiles.lnk"); }

bool Unpack(std::vector<BYTE>& out, std::vector<PayloadFile>& files, std::wstring& error)
{
    error = L"安装程序已损坏，请重新下载。";
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(kPayloadId), RT_RCDATA);
    HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
    const auto* data = static_cast<const BYTE*>(mem ? LockResource(mem) : nullptr);
    const DWORD size = res ? SizeofResource(nullptr, res) : 0;
    if (!data || size < sizeof(PayloadHeader))
        return false;
    PayloadHeader h;
    memcpy(&h, data, sizeof(h));
    if (h.magic != kPayloadMagic || h.version != kPayloadVersion || h.compressed > size - sizeof(h) || h.size > 0x40000000)
        return false;
    DECOMPRESSOR_HANDLE d = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &d))
        return false;
    out.resize(static_cast<size_t>(h.size));
    SIZE_T written = 0;
    const bool ok = Decompress(d, data + sizeof(h), static_cast<SIZE_T>(h.compressed), out.data(), out.size(), &written) &&
        written == h.size && PayloadHash(out.data(), out.size()) == h.hash && PayloadFiles(out, files) &&
        std::any_of(files.begin(), files.end(), [](const PayloadFile& f) { return f.name == kAppExe; });
    CloseDecompressor(d);
    if (ok)
        error.clear();
    return ok;
}

// Written next to the target, flushed, then swapped in: an interrupted setup never leaves a half-written exe
bool WriteFileAtomic(const std::wstring& path, const void* data, size_t size, std::wstring& error)
{
    const std::wstring temp = path + L".new";
    HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        error = L"无法写入 " + temp + L"\n" + LastErrorText(GetLastError());
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(f, data, static_cast<DWORD>(size), &written, nullptr) && written == size && FlushFileBuffers(f);
    const DWORD code = GetLastError();
    CloseHandle(f);
    if (ok)
        for (int i = 0; i < 20; ++i)
        {
            if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                return true;
            Sleep(150);
        }
    error = L"无法替换 " + path + L"\n" + LastErrorText(ok ? GetLastError() : code);
    DeleteFileW(temp.c_str());
    return false;
}

// Uninstall.exe: this exe without the payload
bool WriteUninstaller(const std::wstring& path, std::wstring& error)
{
    const std::wstring temp = path + L".tmp";
    if (!CopyFileW(ExePath().c_str(), temp.c_str(), FALSE))
    {
        error = L"无法写入 " + path + L"\n" + LastErrorText(GetLastError());
        return false;
    }
    if (HANDLE update = BeginUpdateResourceW(temp.c_str(), FALSE))
    {
        const BOOL removed = UpdateResourceW(update, RT_RCDATA, MAKEINTRESOURCEW(kPayloadId), MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), nullptr, 0);
        EndUpdateResourceW(update, !removed); // keeps the full copy if removing failed: it still works as the uninstaller
    }
    for (int i = 0; i < 20; ++i)
    {
        if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        Sleep(150);
    }
    error = L"无法替换 " + path + L"\n" + LastErrorText(GetLastError());
    DeleteFileW(temp.c_str());
    return false;
}

// A previous install in another folder (or Inno's uninstaller files): remove only the files setups put there
void RemoveOldFiles(const std::wstring& dir, const std::wstring& keep)
{
    if (dir.empty())
        return;
    const bool other = !SamePath(dir, keep);
    for (const wchar_t* name : { L"unins000.exe", L"unins000.dat", L"unins000.msg" })
        DeleteRetry(Join(dir, name), 3);
    if (other)
    {
        DeleteRetry(Join(dir, kAppExe), 3);
        DeleteRetry(Join(dir, kUninstallExe), 3);
        for (const wchar_t* name : kCompanionFiles)
            DeleteRetry(Join(dir, name), 3);
        RemoveDirectoryW(dir.c_str()); // only when empty
    }
}

// <install folder>\data: FastFiles keeps its settings, session, tags and logs there. Program Files is read-only for
// users, so this one folder (only it) gives the Users group modify rights, inherited by the files in it.
void MakeDataFolder(const std::wstring& dir)
{
    const std::wstring data = Join(dir, L"data");
    CreateDirectoryW(data.c_str(), nullptr);
    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD sidSize = sizeof(sid);
    if (!CreateWellKnownSid(WinBuiltinUsersSid, nullptr, sid, &sidSize))
        return;
    PACL current = nullptr, updated = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(data.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &current, nullptr, &sd) != ERROR_SUCCESS)
        return;
    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
    if (SetEntriesInAclW(1, &ea, current, &updated) == ERROR_SUCCESS)
    {
        SetNamedSecurityInfoW(const_cast<LPWSTR>(data.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, updated, nullptr);
        LocalFree(updated);
    }
    LocalFree(sd);
}

void SetString(HKEY key, const wchar_t* name, const std::wstring& value)
{
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

void SetDword(HKEY key, const wchar_t* name, DWORD value)
{
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

// --- Uninstall cleanup helpers (System leftovers + optional user data) ---
// Test mode: set only by tests/test-ff-h30.ps1, never by a real install/uninstall. FASTFILES_DATA names an isolated
// folder; while it is set, every registry change below is a dry run (logged, never applied) and nothing outside the
// folders the caller passes in is touched. Without the variable this is all a no-op and behaves exactly as before.
bool TestMode()
{
    wchar_t value[4]{};
    return GetEnvironmentVariableW(L"FASTFILES_DATA", value, 4) != 0;
}

// Appends one line to <FASTFILES_DATA>\uninstall-dryrun.txt so an isolated test can see what would have run
void LogDryRun(const std::wstring& line)
{
    wchar_t dir[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"FASTFILES_DATA", dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return;
    std::wstring path = dir;
    if (!path.empty() && path.back() != L'\\')
        path += L'\\';
    path += L"uninstall-dryrun.txt";
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    const std::string utf8 = [&]
    {
        const std::wstring text = line + L"\r\n";
        const int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), len, nullptr, nullptr);
        return out;
    }();
    DWORD written = 0;
    WriteFile(f, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(f);
}

// Deletes a value under root\subkey; in test mode only logs it (the real key is never opened)
void RemoveRegValue(HKEY root, const wchar_t* subkey, const wchar_t* name)
{
    const wchar_t* rootName = root == HKEY_CURRENT_USER ? L"HKCU" : L"HKLM";
    if (TestMode())
    {
        LogDryRun(std::wstring(L"[dry-run] delete value ") + rootName + L"\\" + subkey + L"\\" + (name ? name : L"(default)"));
        return;
    }
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS)
    {
        RegDeleteValueW(key, name);
        RegCloseKey(key);
    }
}

// Deletes root\subkey and everything under it; in test mode only logs it
void RemoveRegTree(HKEY root, const wchar_t* subkey, REGSAM extra = 0)
{
    const wchar_t* rootName = root == HKEY_CURRENT_USER ? L"HKCU" : L"HKLM";
    if (TestMode())
    {
        LogDryRun(std::wstring(L"[dry-run] delete key ") + rootName + L"\\" + subkey);
        return;
    }
    RegDeleteTreeW(root, subkey);
    RegDeleteKeyExW(root, subkey, extra, 0);
}

// --- Default file manager: the exact keys MainWindowTray.cpp's SetDefaultFileManager(true) writes under HKCU.
// Reimplemented here (not called through FastFiles.exe) so it still undoes them when the exe is already gone, e.g. a
// previous uninstall deleted the exe but was interrupted before this step, or the user removed it by hand.
constexpr wchar_t kFolderOpen[] = L"Software\\Classes\\Folder\\shell\\open\\command";
constexpr wchar_t kFolderExplore[] = L"Software\\Classes\\Folder\\shell\\explore\\command";
constexpr wchar_t kWinE[] = L"Software\\Classes\\CLSID\\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}";
constexpr wchar_t kWinECommand[] = L"Software\\Classes\\CLSID\\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}\\shell\\opennewwindow\\command";

// True when subkey's default value names FastFiles.exe (so unsetting never takes someone else's handler)
bool IsOurs(const wchar_t* subkey)
{
    if (TestMode()) // dry run: never reads the real HKCU\Software\Classes
        return true;
    wchar_t value[MAX_PATH * 2]{};
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, subkey, nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, value, &size) == ERROR_SUCCESS &&
        StrStrIW(value, L"FastFiles.exe") != nullptr;
}

// Deletes subkey's default value and DelegateExecute, then subkey and each empty parent up to (not including) stop
void UnsetCommand(const wchar_t* subkey, const wchar_t* stopAt)
{
    if (!IsOurs(subkey))
        return; // someone else's handler (or nothing): leave it
    RemoveRegValue(HKEY_CURRENT_USER, subkey, nullptr);
    RemoveRegValue(HKEY_CURRENT_USER, subkey, L"DelegateExecute");
    if (TestMode())
    {
        LogDryRun(std::wstring(L"[dry-run] delete empty key chain HKCU\\") + subkey + L" up to " + stopAt);
        return;
    }
    std::wstring key = subkey;
    const std::wstring stop = stopAt;
    while (!key.empty() && _wcsicmp(key.c_str(), stop.c_str()) != 0)
    {
        HKEY h = nullptr;
        DWORD subkeys = 0, values = 0;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &h) == ERROR_SUCCESS)
        {
            RegQueryInfoKeyW(h, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
            RegCloseKey(h);
            if (subkeys || values || RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str()) != ERROR_SUCCESS)
                return;
        }
        const size_t slash = key.find_last_of(L'\\');
        key = slash == std::wstring::npos ? std::wstring() : key.substr(0, slash);
    }
}

// Undo exactly what "--default-file-manager on" writes: Folder\shell\open & \explore, and Win+E's CLSID (only if ours)
void UndoDefaultFileManager()
{
    UnsetCommand(kFolderOpen, L"Software\\Classes");
    UnsetCommand(kFolderExplore, L"Software\\Classes");
    if (IsOurs(kWinECommand))
        RemoveRegTree(HKEY_CURRENT_USER, kWinE);
}

// Start menu / desktop .lnk files; in test mode the caller passes paths inside its own sandbox, so this is safe either way
void RemoveShortcuts(const std::wstring& startMenu, const std::wstring& desktop)
{
    if (TestMode()) // the real Start menu / desktop links: never touched by a test
    {
        LogDryRun(L"[dry-run] delete " + startMenu);
        LogDryRun(L"[dry-run] delete " + desktop);
        return;
    }
    DeleteFileW(startMenu.c_str());
    DeleteFileW(desktop.c_str());
}

// Everything under dir, then dir itself if it is now empty; never recurses outside dir. Used for data\ and the legacy
// %APPDATA%\FastFiles folder, both opt-in (deleteData) and both confined to the one folder passed in. Unlike the
// registry helpers above, this always runs for real: callers (real uninstall or the test hook) only ever pass a
// folder they own (the install dir's own data\, or the test's fake stand-in), never a shared or real user path by
// accident, so there is nothing here that needs a dry-run mode.
void RemoveFolderTree(const std::wstring& dir)
{
    if (dir.empty())
        return;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;
            const std::wstring child = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveFolderTree(child);
            else
            {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    RemoveDirectoryW(dir.c_str());
}
}

std::wstring DefaultInstallDir()
{
    for (const wchar_t* key : { kUninstallKey, kLegacyKey })
    {
        std::wstring dir = RegString(HKEY_LOCAL_MACHINE, key, L"InstallLocation");
        while (dir.size() > 3 && dir.back() == L'\\')
            dir.pop_back();
        if (!dir.empty())
            return dir;
    }
    return Join(KnownFolder(FOLDERID_ProgramFiles), kAppName);
}

std::wstring InstalledVersion()
{
    std::wstring v = RegString(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayVersion");
    return v.empty() ? RegString(HKEY_LOCAL_MACHINE, kLegacyKey, L"DisplayVersion") : v;
}

bool HasDesktopIcon()
{
    // Fresh install: on (Inno's desktopicon task was checked). Upgrade: keep what the user had.
    return InstalledVersion().empty() || GetFileAttributesW(DesktopLink().c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool HasPayload()
{
    return FindResourceW(nullptr, MAKEINTRESOURCEW(kPayloadId), RT_RCDATA) != nullptr;
}
bool RunInstall(const Options& o, const Progress& progress, std::wstring& error)
{
    const std::wstring app = Join(o.dir, kAppExe);
    progress(5, L"正在准备…");
    std::vector<BYTE> payload;
    std::vector<PayloadFile> files;
    if (!Unpack(payload, files, error))
        return false;
    progress(20, L"正在准备…");
    if (!CloseRunning(app, progress, error))
        return false;
    const int made = SHCreateDirectoryExW(nullptr, o.dir.c_str(), nullptr);
    if (made != ERROR_SUCCESS && made != ERROR_ALREADY_EXISTS && made != ERROR_FILE_EXISTS)
    {
        error = L"无法创建文件夹 " + o.dir + L"\n" + LastErrorText(made);
        return false;
    }
    progress(35, L"正在复制文件…");
    // The exe last: until it is in place the old one keeps working with whatever it had
    std::stable_partition(files.begin(), files.end(), [](const PayloadFile& f) { return f.name != kAppExe; });
    for (const PayloadFile& f : files)
        if (!WriteFileAtomic(Join(o.dir, f.name.c_str()), f.data, f.size, error))
            return false;
    progress(60, L"正在复制文件…");
    if (!WriteUninstaller(Join(o.dir, kUninstallExe), error))
        return false;
    MakeDataFolder(o.dir);

    progress(75, L"正在创建快捷方式…");
    // Take over an earlier install: its folder (if different) and Inno's uninstaller entry and files
    const std::wstring previous = RegString(HKEY_LOCAL_MACHINE, kUninstallKey, L"InstallLocation");
    const std::wstring legacy = RegString(HKEY_LOCAL_MACHINE, kLegacyKey, L"InstallLocation");
    RemoveOldFiles(previous, o.dir);
    RemoveOldFiles(legacy, o.dir);
    RemoveOldFiles(o.dir, o.dir);
    // RemoveOldFiles(o.dir, o.dir) above only touches the three Inno leftovers when upgrading in place (dir == keep
    // skips its companion-file loop); a leftover 7z.dll / 7z.License.txt from an older install needs its own pass
    for (const wchar_t* name : kLegacyCompanionFiles)
        DeleteRetry(Join(o.dir, name), 3);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kLegacyKey);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kLegacyKey, KEY_WOW64_64KEY, 0);
    MakeShortcut(StartMenuLink(), app, o.dir);
    if (o.desktopIcon)
        MakeShortcut(DesktopLink(), app, o.dir);
    else
        DeleteFileW(DesktopLink().c_str());

    progress(90, L"正在注册…");
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr) == ERROR_SUCCESS)
    {
        const std::wstring uninstaller = L"\"" + Join(o.dir, kUninstallExe) + L"\"";
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t date[16];
        swprintf_s(date, L"%04u%02u%02u", now.wYear, now.wMonth, now.wDay);
        SetString(key, L"DisplayName", kAppName);
        SetString(key, L"DisplayVersion", kVersion);
        SetString(key, L"Publisher", kAppName);
        SetString(key, L"DisplayIcon", app);
        SetString(key, L"InstallLocation", o.dir);
        SetString(key, L"UninstallString", uninstaller + L" /uninstall");
        SetString(key, L"QuietUninstallString", uninstaller + L" /uninstall /VERYSILENT");
        SetString(key, L"InstallDate", date);
        SetDword(key, L"EstimatedSize", static_cast<DWORD>(payload.size() / 1024 * 2));
        SetDword(key, L"NoModify", 1);
        SetDword(key, L"NoRepair", 1);
        RegCloseKey(key);
    }
    progress(100, L"");
    return true;
}

bool RunUninstall(const Options& o, const Progress& progress, std::wstring& error)
{
    const std::wstring dir = o.from.empty() ? o.dir : o.from;
    const std::wstring app = Join(dir, kAppExe);
    progress(10, L"正在准备…");
    if (!CloseRunning(app, progress, error))
        return false;
    // Folders must not keep opening a deleted exe: undo "default file manager" (only what FastFiles wrote), done here
    // directly against the registry so it still works when FastFiles.exe is already gone (an earlier uninstall that
    // was interrupted, or the user deleted it by hand) instead of depending on launching it first.
    UndoDefaultFileManager();
    progress(40, L"正在删除文件…");
    if (!DeleteRetry(app))
    {
        error = L"无法删除 " + app + L"\n" + LastErrorText(GetLastError());
        return false;
    }
    DeleteFileW((app + L".new").c_str());
    for (const wchar_t* name : kCompanionFiles)
    {
        DeleteRetry(Join(dir, name), 10);
        DeleteFileW((Join(dir, name) + L".new").c_str());
    }
    // A leftover 7z.dll / 7z.License.txt from an install made before compress / extract was removed
    for (const wchar_t* name : kLegacyCompanionFiles)
        DeleteRetry(Join(dir, name), 10);
    progress(65, L"正在删除快捷方式…");
    RemoveShortcuts(StartMenuLink(), DesktopLink());
    progress(80, L"正在清理…");
    // "开机时启动" is written by the app itself (per user); remove it like Inno's uninsdeletevalue did
    RemoveRegValue(HKEY_CURRENT_USER, kRunKey, kAppName);
    RemoveRegTree(HKEY_LOCAL_MACHINE, kUninstallKey, KEY_WOW64_64KEY);
    RemoveRegTree(HKEY_LOCAL_MACHINE, kLegacyKey, KEY_WOW64_64KEY);
    // The first Uninstall.exe started us from a temp copy; Main.cpp schedules that copy's own deletion
    // (MOVEFILE_DELAY_UNTIL_REBOOT) once this returns, since it can't delete itself while still running. This call
    // deletes the *installed* Uninstall.exe (a different file, inside dir), which nothing still has open.
    DeleteRetry(Join(dir, kUninstallExe), 40);
    RemoveOldFiles(dir, dir);
    if (o.deleteData)
    {
        // Opt-in ("同时删除我的设置和数据" / /DELETEDATA): data\ and the legacy %APPDATA%\FastFiles folder it was
        // first copied from (Session.cpp's RoamingDir). Default is to keep both, for a reinstall.
        RemoveFolderTree(Join(dir, L"data"));
        // The real %APPDATA%\FastFiles is the user's own data: a test (FASTFILES_DATA set) only logs it
        const std::wstring roaming = KnownFolder(FOLDERID_RoamingAppData);
        if (TestMode())
            LogDryRun(L"[dry-run] delete folder " + Join(roaming, kAppName));
        else if (!roaming.empty())
            RemoveFolderTree(Join(roaming, kAppName));
    }
    else
        RemoveDirectoryW(Join(dir, L"data").c_str()); // goes only if already empty; anything in it stays
    RemoveDirectoryW(dir.c_str()); // only when empty: anything the user put there stays
    progress(100, L"");
    return true;
}
