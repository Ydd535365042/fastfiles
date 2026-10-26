#include "Drives.h"
#include "Sandbox.h"
#include "Log.h"
#include <thread>

namespace
{
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
}

namespace Drives
{
std::wstring Root(const std::wstring& path)
{
    if (path.size() < 2 || path.size() > 3 || path[1] != L':' || !iswalpha(path[0]) || (path.size() == 3 && path[2] != L'\\'))
        return {};
    return std::wstring(1, static_cast<wchar_t>(towupper(path[0]))) + L":\\";
}

bool CanEject(const std::wstring& path)
{
    const std::wstring root = Root(path);
    if (root.empty())
        return false;
    const UINT type = GetDriveTypeW(root.c_str());
    return type == DRIVE_REMOVABLE || type == DRIVE_CDROM;
}

bool CanFormat(const std::wstring& path)
{
    const std::wstring root = Root(path);
    if (root.empty())
        return false;
    const UINT type = GetDriveTypeW(root.c_str());
    if (type == DRIVE_REMOTE || type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN)
        return false;
    wchar_t system[MAX_PATH]{};
    GetEnvironmentVariableW(L"SystemDrive", system, MAX_PATH);
    return _wcsnicmp(root.c_str(), system, 2) != 0;
}

void Eject(HWND owner, const std::wstring& path)
{
    const std::wstring root = Root(path);
    if (root.empty() || Sandbox::Active()) // test mode: drives are never ejected
        return;
    // Files: ContextMenu.InvokeVerb("eject", path)
    PIDLIST_ABSOLUTE raw = nullptr;
    if (FAILED(SHParseDisplayName(root.c_str(), nullptr, &raw, 0, nullptr)))
        return;
    UniquePidl pidl(raw);
    ComPtr<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;
    ComPtr<IContextMenu> menu;
    if (FAILED(SHBindToParent(pidl.get(), IID_PPV_ARGS(&parent), &child)) ||
        FAILED(parent->GetUIObjectOf(owner, 1, &child, IID_IContextMenu, nullptr, reinterpret_cast<void**>(menu.GetAddressOf()))))
        return;
    HMENU popup = CreatePopupMenu();
    if (SUCCEEDED(menu->QueryContextMenu(popup, 0, 1, 0x7FFF, CMF_NORMAL)))
    {
        CMINVOKECOMMANDINFOEX info{ sizeof(info) };
        info.fMask = CMIC_MASK_UNICODE;
        info.hwnd = GetAncestor(owner, GA_ROOT);
        info.lpVerb = "eject";
        info.lpVerbW = L"eject";
        info.nShow = SW_SHOWNORMAL;
        if (FAILED(menu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info))))
            Log::Warning(L"Eject failed: " + root);
    }
    DestroyMenu(popup);
}

void Format(HWND owner, const std::wstring& path)
{
    const std::wstring root = Root(path);
    if (root.empty() || !CanFormat(root) || Sandbox::Active()) // test mode: never formats
        return;
    const std::wstring drive(1, root[0]);
    if (Elevated())
    {
        // Already elevated: the dialog on a worker (it is modal and would hold the window's thread)
        std::thread([drive] { RunFormatDialog(drive.c_str()); }).detach();
        return;
    }
    // Files: SHFormatDrive through an elevated PowerShell; here an elevated FastFiles that only shows the dialog
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring args = L"--format-drive " + drive;
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask = SEE_MASK_NOASYNC;
    sei.hwnd = GetAncestor(owner, GA_ROOT);
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) && GetLastError() != ERROR_CANCELLED)
        Log::Warning(L"Format dialog could not start for " + root);
}

int RunFormatDialog(const wchar_t* drive)
{
    if (!drive || !iswalpha(drive[0]) || Sandbox::Active())
        return 1;
    // SHFormatDrive(hwnd, drive index, SHFMT_ID_DEFAULT, SHFMT_OPT_FULL) like Files' call (0xFFFF, 0x0001); the
    // user picks the options and confirms in the system dialog
    const UINT index = static_cast<UINT>(towupper(drive[0]) - L'A');
    if (!CanFormat(std::wstring(1, drive[0]) + L":\\"))
        return 1;
    SHFormatDrive(nullptr, index, SHFMT_ID_DEFAULT, SHFMT_OPT_FULL);
    return 0;
}
}
