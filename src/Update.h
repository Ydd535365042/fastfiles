#pragma once
#include "Common.h"

// Online update from the GitHub repository's releases (Files' IUpdateService: CheckForUpdatesAsync, then
// DownloadUpdatesAsync with progress, then the app restarts). Files reads an .appinstaller; FastFiles reads the
// latest release of kRepo through the GitHub API and takes the asset that matches how it runs:
//  - installed (Uninstall.exe next to the exe): FastFiles_<ver>_x64_Setup.exe, run silently over the same folder
//  - portable (no Uninstall.exe): FastFiles.exe, swapped in place of the running exe
// Test mode (Sandbox::Active): the check and the download are real (read-only GitHub requests), applying never
// starts the setup or opens a browser (both are logged to <data>\update-log.txt), and a portable swap only ever
// happens to an exe under %TEMP%. FASTFILES_TEST_VERSION pretends to be another version there, so a test can see
// an update even when the build is the latest release.
namespace Update
{
inline constexpr wchar_t kRepo[] = L"Ydd535365042/fastfiles";
inline constexpr wchar_t kRepoUrl[] = L"https://github.com/Ydd535365042/fastfiles";

struct Release
{
    std::wstring version; // "1.0.0.1" (tag without the leading v)
    std::wstring url;     // the asset for this kind of install
};

std::wstring CurrentVersion();
bool Installed(); // Uninstall.exe next to the exe
// 1 when a is newer than b, -1 when older, 0 when the same (missing parts count as 0)
int Compare(const std::wstring& a, const std::wstring& b);

// Worker thread only (blocking). Messages are posted to `notify`; each lParam is an owned Result*.
inline constexpr UINT kMsgUpdate = WM_APP + 95;
struct Result
{
    enum class Kind { Checked, Progress, Downloaded, Failed } kind = Kind::Failed;
    Release release;       // Checked: version empty = up to date
    int percent = 0;       // Progress
    std::wstring file;     // Downloaded: the new file on disk
    std::wstring error;    // Failed
};
void CheckAsync(HWND notify);
void DownloadAsync(HWND notify, Release release);

// UI thread, after Downloaded: start the setup / swap the exe. True when this process should now quit.
bool Apply(HWND owner, const std::wstring& file, std::wstring& error);
// Startup: removes FastFiles.exe.old left by a portable update
void CleanUp();
// Opens kRepoUrl in the browser (logged instead in test mode)
void OpenRepo();
void TestLog(const std::wstring& line); // <data>\update-log.txt in test mode, nothing otherwise
}
