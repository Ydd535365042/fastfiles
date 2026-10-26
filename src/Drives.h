#pragma once
#include "Common.h"

// Files' drive actions: 弹出 (DriveHelpers.EjectDeviceAsync: the shell's "eject" verb) and 格式化 (FormatDriveAction:
// the system's Format dialog, SHFormatDrive, run elevated). Both do nothing in test mode.
namespace Drives
{
// "X:\" for a drive root path, else empty
std::wstring Root(const std::wstring& path);
// Files' ShowEjectDevice (IsRemovable): removable media and CD/DVD drives
bool CanEject(const std::wstring& path);
// Files' FormatDriveAction.IsExecutable: a drive that is neither a network drive nor the system drive
bool CanFormat(const std::wstring& path);
void Eject(HWND owner, const std::wstring& path);
void Format(HWND owner, const std::wstring& path);
// "--format-drive X": the elevated copy Format starts (Files runs SHFormatDrive through an elevated PowerShell)
int RunFormatDialog(const wchar_t* drive);
}
