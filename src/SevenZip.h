#pragma once
#include "Common.h"

// 7z.dll from 7-Zip, shipped next to FastFiles.exe the way Files ships it for SevenZipSharp: lists and extracts what the
// Rust zip code doesn't (7z, rar, tar, gz, lzh..., and zips that are encrypted or use methods other than store/deflate).
// Extraction follows the Rust rules: nothing is overwritten (" (2)"), unsafe names and reparse points are skipped.
// Blocking; call from a worker thread.
namespace SevenZip
{
bool Available(); // 7z.dll found and loaded

enum class Status { Ok, Cancelled, NeedPassword, WrongPassword, Unreadable, NoLibrary, Failed };

struct Entry
{
    std::wstring path; // backslashes
    bool folder = false;
    ULONGLONG size = 0;
};

struct Listing
{
    Status status = Status::Ok;
    std::vector<Entry> entries; // at most `max`
    ULONGLONG items = 0, files = 0, folders = 0, bytes = 0; // over the whole archive
    bool encrypted = false; // Files' IsEncryptedAsync: an entry is encrypted (or the headers are)
};

// password empty: none (an encrypted header then gives NeedPassword). codepage: names in zips without the UTF-8 flag.
Listing List(const std::wstring& archive, const std::wstring& password, size_t max, UINT codepage = 0);

// Files' smart extract: distinct top-level items (0, 1, or 2 for more than one); -1 when it can't be read
int TopLevel(const std::wstring& archive);

// Into `target` (created). progress: FfZipProgress layout (total bytes, done bytes, total files, done files), updated
// atomically; *cancel is polled. *skipped: entries left out for unsafe names. error: message when not Ok / Cancelled.
Status Extract(const std::wstring& archive, const std::wstring& target, const std::wstring& password, UINT codepage,
    uint64_t* progress, const bool* cancel, uint64_t* skipped, std::wstring& error);

std::wstring Message(Status status); // user-facing text for a failed status
}
