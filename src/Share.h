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
// Prepares the items on a worker, then shows the sheet from the UI thread (kMsgReady to `owner`, see OnReady)
void Show(HWND owner, std::vector<std::wstring> paths);
inline constexpr UINT kMsgReady = WM_APP + 96; // lParam: owned prepared data, for OnReady
void OnReady(HWND owner, const Theme* theme, LPARAM data);
}
