#pragma once
#include <windows.h>
#include <shlobj.h>
#include <functional>
#include <string>
#include "version.h"

// FastFiles' own installer and uninstaller (no third-party setup engine). One exe: Setup.exe carries the app as RCDATA;
// the copy installed as Uninstall.exe (payload removed) uninstalls.
inline constexpr wchar_t kAppName[] = L"FastFiles";
inline constexpr wchar_t kAppExe[] = L"FastFiles.exe";
inline constexpr wchar_t kUninstallExe[] = L"Uninstall.exe";
// Installed next to the exe (FastFiles' MIT license and the third-party notices); the uninstaller removes exactly these.
// 7z.dll / 7z.License.txt are no longer shipped (no built-in compress / extract); kLegacyCompanionFiles in Install.cpp
// still cleans up a leftover pair from an older install.
inline constexpr const wchar_t* kCompanionFiles[] = { L"LICENSE.txt", L"THIRD-PARTY-NOTICES.txt" };
inline constexpr wchar_t kVersion[] = L"" FF_VER_STR;
inline constexpr int kPayloadId = 100, kIconId = 1;

enum ExitCode : int { ExitOk = 0, ExitInitFailed = 1, ExitCancelled = 2, ExitFailed = 5 };

struct Options
{
    bool uninstall = false;
    bool silent = false;     // /SILENT: progress only
    bool verySilent = false; // /VERYSILENT, /S: no window
    bool desktopIcon = true;
    bool launch = true;      // finish page: start FastFiles
    bool launchAfter = false; // /LAUNCH: start FastFiles after a /SILENT or /VERYSILENT install too (online update)
    bool deleteData = false; // uninstall: also remove data\ and the legacy %APPDATA%\FastFiles (/DELETEDATA). Off keeps them for a reinstall.
    bool testUninstall = false; // /TESTUNINSTALL: tests/test-ff-h30.ps1 only, see Main.cpp
    std::wstring dir;        // install folder
    std::wstring from;       // uninstall: the installed folder (we run from a temp copy)
    // Test hook only: FASTFILES_SETUP_PREVIEW=install|uninstall, honoured only together with FASTFILES_DATA (like
    // FastFiles' other test hooks). Runs the full wizard UI with a simulated worker (fake steps, no real work):
    // never calls RunInstall/RunUninstall, WriteUninstaller, any registry write, shortcut creation, or self-relaunch,
    // and never launches FastFiles on finish. Lets the UI (splash / working / done animations) be screenshotted
    // without ever touching the real machine. Registry reads (InstalledVersion) still happen normally.
    bool preview = false;
};

// Worker progress: 0..100 and a status line (called on the worker thread)
using Progress = std::function<void(int percent, const std::wstring& status)>;

// Install.cpp
std::wstring DefaultInstallDir();
std::wstring InstalledVersion(); // empty when not installed
bool HasDesktopIcon();
bool HasPayload();
bool RunInstall(const Options& o, const Progress& progress, std::wstring& error);
bool RunUninstall(const Options& o, const Progress& progress, std::wstring& error);

// System.cpp
std::wstring KnownFolder(REFKNOWNFOLDERID id);
std::wstring ExePath();
std::wstring Join(const std::wstring& dir, const wchar_t* name);
bool SamePath(const std::wstring& a, const std::wstring& b);
bool DeleteRetry(const std::wstring& path, int tries = 20);
bool CloseRunning(const std::wstring& exe, const Progress& progress, std::wstring& error); // Restart Manager
bool MakeShortcut(const std::wstring& link, const std::wstring& target, const std::wstring& dir);
bool LaunchAsUser(const std::wstring& exe); // unelevated, through the shell's own process
DWORD RunWait(const std::wstring& exe, const std::wstring& args, DWORD timeout);
std::wstring LastErrorText(DWORD code);
bool IsWindows11();

// Ui.cpp: the self-drawn wizard. Returns the exit code.
int RunUi(Options& o);
