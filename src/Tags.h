#pragma once
#include "Common.h"

// File tags (Files' "File tags"): thin wrapper over the Rust core (core/src/tags.rs), which owns tags.txt.
struct FileTag
{
    std::wstring id, name;
    COLORREF color = 0;
};

namespace Tags
{
std::vector<FileTag> List();
const FileTag* Find(const std::vector<FileTag>& tags, const std::wstring& id);
std::wstring Add(const std::wstring& name, COLORREF color); // new id, empty on failure
bool Rename(const std::wstring& id, const std::wstring& name);
bool Recolor(const std::wstring& id, COLORREF color);
bool Remove(const std::wstring& id);
bool Move(size_t from, size_t to);
std::vector<std::wstring> Of(const std::wstring& path);                          // tag ids on one item
bool Set(const std::vector<std::wstring>& paths, const std::wstring& id, bool on); // add / remove on many
std::vector<std::wstring> Items(const std::wstring& id, const std::wstring& root); // worker thread: may touch disks

std::wstring ColorText(COLORREF color); // "#RRGGBB"
// Anti-aliased colored dot for menu items (32bpp premultiplied DIB); checked adds a white tick. Caller deletes it.
HBITMAP MenuDot(COLORREF color, int size, bool checked);


// Registered message posted to every FastFiles window after a change (Ctrl+N windows are other processes)
UINT ChangedMessage();
void Broadcast();
}
