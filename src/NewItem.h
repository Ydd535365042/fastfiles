#pragma once
#include "Common.h"

// Files' ShellNewMenuHelper / FilesystemOperations.CreateAsync / CreateShortcutDialogViewModel: the file types Windows
// can create (HKCR\.ext\...\ShellNew), how a new file, folder or shortcut is made, and the shortcut target rules.
namespace NewItem
{
struct Entry
{
    std::wstring extension; // ".docx"
    std::wstring name;      // display type ("Microsoft Word 文档")
    std::wstring templ;     // FileName: copied when set
    std::wstring command;   // Command: Files skips the name prompt for these
    std::vector<BYTE> data; // Data: written into the new file
    HICON icon = nullptr;   // 32px shell icon of the type (kept for the life of the process)
};

void StartScan(); // worker; Entries() is empty until it finishes
bool Ready();
std::vector<Entry> Entries(); // sorted by name; .txt always present (Files adds it when the registry has none)
const Entry* EntryFor(const std::vector<Entry>& entries, const std::wstring& extension);

// FilesystemHelpers.IsValidForFilename / FilterRestrictedCharacters
bool ValidName(const std::wstring& name);
std::wstring FilterName(const std::wstring& name);

// The created path ("" on failure; *denied when access was refused). Names that exist get " (2)" like Files'
// CreationCollisionOption.GenerateUniqueName.
std::wstring CreateFolder(const std::wstring& dir, const std::wstring& name, bool* denied);
std::wstring CreateNewFile(const std::wstring& dir, const std::wstring& fileName, const std::vector<Entry>& entries, bool* denied);

// CreateShortcutDialogViewModel.ShortcutTarget: a path (quoted or not, with arguments), a program on PATH, or a URL
struct Target
{
    bool valid = false;  // IsLocationValid
    bool exists = false; // DestinationPathExists: .lnk; otherwise a .url
    std::wstring fullPath, arguments, autoName;
};
Target ParseTarget(const std::wstring& text);
std::wstring ShortcutName(const std::wstring& name); // Explorer's naming template ("{0} - 快捷方式")
// Writes dir\<name>.lnk|.url (unique); "" on failure
std::wstring CreateShortcut(const std::wstring& dir, const std::wstring& name, const Target& target, bool* denied);

HBITMAP IconBitmap(HICON icon, int size); // 32bpp premultiplied, for menu items (caller deletes)
}
