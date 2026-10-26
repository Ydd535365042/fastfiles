#pragma once
#include "Common.h"

// Test mode: on only when the environment variable FASTFILES_DATA names a folder (the test scripts set it to their own
// temp folder). Then FastFiles keeps its data there and leaves the machine alone: no clipboard, no recycle bin, no
// Explorer / registry settings, no autostart or default file manager, no Recent items, no tray icon, no Shell Bags,
// no shell verbs that change the system, no messages to other FastFiles windows. Without the variable nothing changes.
namespace Sandbox
{
bool Active();
const std::wstring& Dir(); // the data folder, ending in a backslash (empty when not active)

// Clipboard text goes to <Dir>clipboard.txt instead; true when handled (the caller skips the real clipboard)
bool CopyText(const std::wstring& text);
// Files' CopyItemFromSidebar / CopyItemFromHome ("复制" on a sidebar/Home card): the paths go to
// <Dir>clipboard-files.txt (same format PasteFiles reads, first line "copy"); true when handled.
bool CopyFiles(const std::vector<std::wstring>& paths);
// Files to paste: <Dir>clipboard-files.txt, first line "copy" or "cut", then one path per line
bool PasteFiles(std::vector<std::wstring>& paths, bool& move);
bool HasPasteFiles();
// Where 删除 (to the recycle bin) moves items instead: <Dir>recycled\ (created)
std::wstring RecycledDir();
// A shell verb that's safe to run here: it only works on the items themselves or shows a window
bool VerbAllowed(const char* verb);
bool VerbAllowed(const wchar_t* verb); // menu items without a canonical verb ("") aren't
}
