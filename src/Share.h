#pragma once
#include "Common.h"

struct Theme;

// Files' ShareItemHelpers / ShareItemAction: the Windows share sheet for the selected files, through
// IDataTransferManagerInterop on the main window (the shell's own "Windows.ModernShare" verb needs Explorer's view
// as its site and says "不支持此项目" anywhere else). Files can be shared; folders, hidden items and .lnk shortcuts
// can't (IsItemShareable); a .url shares its address as a web link.
// Test mode (Sandbox::Active): the items are prepared for real, the sheet isn't shown (logged to <data>\share-log.txt).
namespace Share
{
bool Shareable(const std::wstring& path);
// Folders: the system's network sharing, 授予访问权限 > 特定用户... (ntshrui's "Shell extensions for sharing", the
// 网络访问 window). FolderShareable: a folder on a disk (not a drive root, not a virtual folder)
bool FolderShareable(const std::wstring& path);
bool GiveAccess(HWND owner, const std::vector<std::wstring>& folders);
// The system's 授予访问权限 item's text (for placing 共享 next to it in the right-click menu), empty if unknown
std::wstring GiveAccessText(const std::vector<std::wstring>& folders);
// Prepares the items on a worker, then shows the sheet from the UI thread (kMsgReady to `owner`, see OnReady)
void Show(HWND owner, std::vector<std::wstring> paths);
inline constexpr UINT kMsgReady = WM_APP + 96; // wParam: stage (0 checked, 1 files made), lParam: owned job, for OnReady
void OnReady(HWND owner, const Theme* theme, WPARAM stage, LPARAM data);}
