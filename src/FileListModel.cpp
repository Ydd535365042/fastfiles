#include "FileList.h"
#include "Tab.h"
#include "Lang.h"
#include "Sandbox.h"
#include <thread>

// Listing, Files' sort order and filtering, the selection as shell items, and the folder's change notifications.

namespace
{
std::wstring Lower(std::wstring s)
{
    if (!s.empty())
        CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

bool HasExt(const std::wstring& ext, std::initializer_list<const wchar_t*> list)
{
    for (const wchar_t* e : list)
        if (_wcsicmp(ext.c_str(), e) == 0)
            return true;
    return false;
}

// Files whose icon is their own, not their type's (the shell reads it from the file)
bool OwnIcon(const std::wstring& ext)
{
    return HasExt(ext, { L".exe", L".ico", L".lnk", L".url", L".cur", L".ani", L".msc", L".scr", L".cpl", L".appref-ms", L".website" });
}

// Shortcuts never show their extension (Explorer's NeverShowExt; Files hides .lnk / .url the same way)
bool LinkExt(const std::wstring& ext) { return HasExt(ext, { L".lnk", L".url", L".appref-ms", L".website", L".pif" }); }

struct TypeInfo
{
    std::wstring type;
    int icon = -1;
    bool hideExt = false; // the shell's display name drops the extension (system setting, known type)
};
}

const FileList::Column FileList::kColumns[FileList::kColumnCount] = {
    { L"", 44, -1 },                                                    // Files' icon column (24 + list/item padding)
    { L"名称", 252, static_cast<int>(SortKey::Name) },                  // NameColumnWidth 240
    { L"文件标签", 152, static_cast<int>(SortKey::Tag) },               // TagColumnWidth 140
    { L"修改日期", 212, static_cast<int>(SortKey::Modified) },          // DateModifiedColumnWidth 200
    { L"创建日期", 212, static_cast<int>(SortKey::Created) },           // DateCreatedColumnWidth 200 (collapsed by default)
    { L"类型", 152, static_cast<int>(SortKey::Type) },                  // TypeColumnWidth 140
    { L"大小", 112, static_cast<int>(SortKey::Size) },                  // SizeColumnWidth 100
};

void FileList::ListWorker(HWND to, std::wstring path, unsigned generation, std::shared_ptr<std::atomic<unsigned>> current,
    std::wstring folderType)
{
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    auto stale = [&] { return current->load() != generation; };
    auto listing = std::make_unique<Listing>();
    listing->generation = generation;
    std::wstring dir = path;
    if (!dir.empty() && dir.back() != L'\\')
        dir += L'\\';
    std::unordered_map<std::wstring, TypeInfo> types; // by lowercased extension
    std::vector<int> own;                             // items whose icon comes from the file itself
    WIN32_FIND_DATAW fd{};
    DWORD oldMode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS, &oldMode); // no "insert a disk" prompts
    HANDLE find = FindFirstFileExW((dir + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    const DWORD error = find == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    SetThreadErrorMode(oldMode, nullptr);
    listing->ok = find != INVALID_HANDLE_VALUE || error == ERROR_FILE_NOT_FOUND;
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2])))
                continue;
            if (listing->items.size() % 512 == 0 && stale())
                break;
            Item item;
            item.name = fd.cFileName;
            item.attrs = fd.dwFileAttributes;
            item.modified = fd.ftLastWriteTime;
            item.created = fd.ftCreationTime;
            const bool folder = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            item.size = folder ? 0 : (static_cast<ULONGLONG>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            const wchar_t* dot = folder ? nullptr : wcsrchr(fd.cFileName, L'.');
            const std::wstring ext = dot ? Lower(dot) : std::wstring(); // (".dotfile": its extension, as Explorer and Files see it)
            auto [it, fresh] = types.try_emplace(folder ? std::wstring(L"\\") : ext);
            if (fresh)
            {
                // The type's name and icon from its registration (no disk access); the folder kind once
                SHFILEINFOW sfi{};
                const std::wstring probe = folder ? std::wstring(L"folder") : L"file" + ext;
                if (SHGetFileInfoW(probe.c_str(), folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                        SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME | SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_DISPLAYNAME))
                {
                    it->second.type = folder ? folderType : sfi.szTypeName;
                    it->second.icon = sfi.iIcon;
                    it->second.hideExt = !folder && !ext.empty() && _wcsicmp(sfi.szDisplayName, L"file") == 0;
                }
                else if (folder)
                    it->second.type = folderType;
            }
            item.type = it->second.type;
            item.icon = it->second.icon;
            item.link = HasExt(ext, { L".lnk", L".url" });
            // Explorer's display name: no extension for shortcuts, and for known types when extensions are hidden
            const size_t base = dot ? static_cast<size_t>(dot - fd.cFileName) : item.name.size();
            item.display = !ext.empty() && base > 0 && (LinkExt(ext) || it->second.hideExt) ? item.name.substr(0, base) : item.name;
            if (OwnIcon(ext) || (folder && (fd.dwFileAttributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM))))
                own.push_back(static_cast<int>(listing->items.size())); // (read-only / system folders may have a desktop.ini icon)
            listing->items.push_back(std::move(item));
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    // Files' file tags of the items directly in this folder (tags.txt; one lookup per tag)
    listing->tags = Tags::List();
    if (!listing->tags.empty() && !stale())
    {
        std::unordered_map<std::wstring, size_t> byName;
        for (const FileTag& tag : listing->tags)
            for (const std::wstring& tagged : Tags::Items(tag.id, path))
            {
                const size_t slash = tagged.find_last_of(L'\\');
                if (slash == std::wstring::npos || CompareStringOrdinal(tagged.c_str(), static_cast<int>(slash + 1), dir.c_str(),
                        static_cast<int>(dir.size()), TRUE) != CSTR_EQUAL)
                    continue; // deeper down
                if (byName.empty())
                    for (size_t i = 0; i < listing->items.size(); ++i)
                        byName.emplace(Lower(listing->items[i].name), i);
                if (auto found = byName.find(Lower(tagged.substr(slash + 1))); found != byName.end())
                    listing->items[found->second].tags.push_back(tag.id);
            }
    }
    std::vector<std::wstring> ownNames;
    for (int i : own)
        ownNames.push_back(listing->items[static_cast<size_t>(i)].name);
    if (stale() || !PostMessageW(to, kMsgListed, generation, reinterpret_cast<LPARAM>(listing.get())))
    {
        if (SUCCEEDED(co))
            CoUninitialize();
        return;
    }
    listing.release(); // the container owns it now
    // Second pass: the real icons of executables, shortcuts, icon files and customized folders, in batches
    auto batch = std::make_unique<std::vector<std::pair<int, int>>>();
    ULONGLONG last = GetTickCount64();
    for (size_t k = 0; k < own.size() && !stale(); ++k)
    {
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW((dir + ownNames[k]).c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
            batch->emplace_back(own[k], sfi.iIcon);
        if (!batch->empty() && (k + 1 == own.size() || GetTickCount64() - last > 150))
        {
            if (PostMessageW(to, kMsgIcons, generation, reinterpret_cast<LPARAM>(batch.get())))
                batch.release();
            batch = std::make_unique<std::vector<std::pair<int, int>>>();
            last = GetTickCount64();
        }
    }
    if (SUCCEEDED(co))
        CoUninitialize();
}

bool FileList::Open(PCIDLIST_ABSOLUTE folder, const std::wstring& select)
{
    const std::wstring path = PidlName(folder, SIGDN_FILESYSPATH);
    ComPtr<IShellFolder> shellFolder;
    if (!m_hwnd || path.empty() || FAILED(SHBindToObject(nullptr, folder, nullptr, IID_PPV_ARGS(&shellFolder))))
        return false;
    if (m_edit)
        CommitRename(true);
    const bool same = m_folder && ILIsEqual(m_folder.get(), folder);
    if (!same)
    {
        // Another folder: nothing of the last one stays up while this one is listed
        m_generation->fetch_add(1);
        Unwatch();
        FreeItems();
        m_listed = false;
        m_hot = m_hotCheck = -1;
        m_pendingRename.clear();
        m_groups.clear();
        m_groupInfo.clear();
        m_sel.Reset(0);
        m_scroll = { 0, 0 };
        Relayout();
        UpdateHeaderCheck();
        m_folder = ClonePidl(folder);
        m_path = path;
        m_shellFolder = shellFolder;
        Watch();
        m_blank = false; // (a refresh of the last folder is over)
        m_held.reset();
        KillTimer(m_hwnd, kRevealTimer);
        RemovePropW(m_list, L"FastFiles.Refreshing");
    }
    m_pendingSelect = select;
    m_pendingTries = 0;
    StartListing(true);
    return true;
}

void FileList::Reload()
{
    if (m_folder)
        StartListing();
}

void FileList::Refresh()
{
    // Files' RefreshItems: the items go (filesAndFolders.Clear) and come back as the folder is listed again, the
    // tab's icon a ProgressRing meanwhile. FastFiles keeps them in memory (selection, scroll and pictures survive)
    // and only stops drawing them, at least kBlankMs so the refresh is seen even when the listing is instant.
    if (!m_folder)
        return;
    if (m_edit)
        CommitRename(true);
    m_blank = true;
    m_blankSince = GetTickCount64();
    SetPropW(m_list, L"FastFiles.Refreshing", reinterpret_cast<HANDLE>(1)); // (tests)
    KillTimer(m_hwnd, kRevealTimer);
    m_held.reset();
    InvalidateRect(m_list, nullptr, FALSE);
    UpdateWindow(m_list);
    StartListing(true);
}

void FileList::StartListing(bool shown)
{
    const unsigned generation = m_generation->fetch_add(1) + 1;
    m_loading = true;
    if (shown)
        m_events->OnListLoading(true);
    KillTimer(m_hwnd, 1); // a pending change notification reload: this is it
    std::thread(&FileList::ListWorker, m_hwnd, m_path, generation, m_generation, std::wstring(Tr(L"文件夹"))).detach();
}

void FileList::TakeListing(Listing* raw)
{
    std::unique_ptr<Listing> listing(raw);
    if (listing->generation != m_generation->load())
        return; // a later listing is under way
    if (m_blank)
    {
        // 刷新: the items stay away for kBlankMs at least
        const ULONGLONG shown = GetTickCount64() - m_blankSince;
        if (shown < kBlankMs)
        {
            m_held = std::move(listing);
            SetTimer(m_hwnd, kRevealTimer, static_cast<UINT>(kBlankMs - shown), nullptr);
            return;
        }
        m_blank = false;
    }
    RemovePropW(m_list, L"FastFiles.Refreshing");
    m_events->OnListLoading(false);
    const bool fresh = !m_listed;
    const Kept kept = fresh ? Kept{} : Keep();
    // The same folder again: pictures and card details of unchanged items stay (a new folder starts without)
    std::unordered_map<std::wstring, const Item*> before;
    if (!fresh)
        for (const Item& item : m_items)
            if (item.thumb || !item.context.empty())
                before.emplace(item.name, &item);
    for (Item& item : listing->items)
        if (auto it = before.find(item.name); it != before.end() && it->second->size == item.size &&
            CompareFileTime(&it->second->modified, &item.modified) == 0)
        {
            item.thumb = it->second->thumb;
            item.thumbW = it->second->thumbW;
            item.thumbH = it->second->thumbH;
            item.thumbAsked = item.thumb != nullptr; // (one still being made comes back for the old position: asked again)
            item.context = it->second->context;
            item.contextAsked = !item.context.empty();
        }
    if (fresh)
        ResetThumbs();
    FreeItems();
    m_items = std::move(listing->items);
    m_tags = std::move(listing->tags);
    m_loading = false;
    m_listed = true;
    m_hot = -1;
    Arrange();
    m_sel.Reset(Count());
    if (fresh)
        m_scroll = { 0, 0 };
    Relayout();
    Restore(kept);
    ApplyPending();
    m_selectionDirty = true;
    m_hotCheck = -1;
    InvalidateRect(m_list, nullptr, FALSE);
    UpdateHeaderCheck();
    m_events->OnListLoaded();
}

void FileList::TakeIcons(std::vector<std::pair<int, int>>* raw, unsigned generation)
{
    std::unique_ptr<std::vector<std::pair<int, int>>> icons(raw);
    if (generation != m_generation->load())
        return;
    for (const auto& [item, icon] : *icons)
        if (item >= 0 && item < static_cast<int>(m_items.size()))
            m_items[static_cast<size_t>(item)].icon = icon;
    InvalidateRect(m_list, nullptr, FALSE);
}

FileList::Kept FileList::Keep() const
{
    Kept kept;
    for (int row : SelectedRows())
        kept.selected.push_back(Lower(m_items[m_order[row]].name));
    if (const int focused = FocusedIndex(); focused >= 0 && focused < Count())
        kept.focused = m_items[m_order[focused]].name;
    return kept;
}

void FileList::Restore(const Kept& kept)
{
    if (!kept.selected.empty())
    {
        std::vector<std::wstring> wanted = kept.selected;
        std::sort(wanted.begin(), wanted.end());
        std::vector<int> rows;
        for (int row = 0; row < Count(); ++row)
            if (std::binary_search(wanted.begin(), wanted.end(), Lower(m_items[m_order[row]].name)))
                rows.push_back(row);
        for (int row : rows)
            m_sel.Set(row, true);
    }
    if (const int row = kept.focused.empty() ? -1 : RowOf(kept.focused); row >= 0)
        m_sel.focus = m_sel.anchor = row;
    m_userSel = false; // (a re-listing keeps the selection: not the user's)
    SelectionChanged();
}

void FileList::ApplyPending()
{
    // A new item's notification may lag behind the listing that follows it: three listings before giving up
    if (!m_pendingRename.empty())
    {
        if (Select(m_pendingRename))
        {
            m_pendingRename.clear();
            BeginRename();
        }
        else if (++m_pendingTries > 3)
            m_pendingRename.clear();
        return;
    }
    if (!m_pendingSelect.empty())
    {
        if (Select(m_pendingSelect) || ++m_pendingTries > 3)
            m_pendingSelect.clear();
    }
}

// The user picked something: an item still waiting to be listed (a finished copy / extraction) doesn't take over later
void FileList::UserSelected()
{
    m_pendingSelect.clear();
    m_userSel = true;
}

void FileList::SelectFirst()
{
    if (Count() == 0)
        return;
    m_sel.All(false);
    m_userSel = true; // (Files: SelectedIndex = 0 raises SelectionChanged like a click)
    FocusRow(0, true, false, false);
}

void FileList::Unselect()
{
    m_userSel = false;
    m_sel.All(false);
    SelectionChanged();
}

void FileList::SelectWhenListed(const std::wstring& name)
{
    m_pendingRename.clear();
    m_pendingSelect = name;
    m_pendingTries = 0;
    if (m_listed && Select(name))
        m_pendingSelect.clear();
}

void FileList::RenameWhenListed(const std::wstring& name)
{
    m_pendingSelect.clear();
    m_pendingRename = name;
    m_pendingTries = 0;
    if (m_listed && Select(name))
    {
        m_pendingRename.clear();
        BeginRename();
    }
}

bool FileList::Visible(const Item& item) const
{
    const ViewPrefs* prefs = m_events->ListPrefs();
    if (prefs && (item.attrs & FILE_ATTRIBUTE_HIDDEN))
    {
        // Files' ShowHiddenItems / ShowProtectedSystemFiles (hidden + system: a protected operating system file)
        if (!prefs->showHidden || ((item.attrs & FILE_ATTRIBUTE_SYSTEM) && !prefs->showProtected))
            return false;
    }
    if (prefs && !prefs->showDotFiles && item.name[0] == L'.')
        return false; // Files' ShowDotFiles
    return m_filter.empty() || Lower(item.display).find(m_filter) != std::wstring::npos;
}

// Files' SortingHelper.OrderFileList: folders first (or files first, or together; shortcuts and archives count as
// files), then the sort key in its direction with the name (natural order) after it
void FileList::Arrange()
{
    m_order.clear();
    for (int i = 0; i < static_cast<int>(m_items.size()); ++i)
        if (Visible(m_items[static_cast<size_t>(i)]))
            m_order.push_back(i);
    const int priority = Priority();
    const SortKey key = m_sortKey;
    const bool desc = m_sortDesc;
    auto isFile = [](const Item& it) { return !(it.attrs & FILE_ATTRIBUTE_DIRECTORY); };
    auto firstTag = [this](const Item& it) -> std::wstring
    {
        // Files sorts by the first tag (its id; here its name, which is what shows)
        for (const FileTag& t : m_tags)
            if (!it.tags.empty() && t.id == it.tags[0])
                return t.name;
        return {};
    };
    std::stable_sort(m_order.begin(), m_order.end(), [&](int ia, int ib)
        {
            const Item& a = m_items[static_cast<size_t>(ia)];
            const Item& b = m_items[static_cast<size_t>(ib)];
            if (priority != 2)
            {
                // OrderBy(PrioritizeFilesOrFolders): false (0) before true; sortFilesFirst flips it
                const bool pa = isFile(a) ^ (priority == 1), pb = isFile(b) ^ (priority == 1);
                if (pa != pb)
                    return !pa;
            }
            int c = 0;
            switch (key)
            {
            case SortKey::Modified: c = CompareFileTime(&a.modified, &b.modified); break;
            case SortKey::Created: c = CompareFileTime(&a.created, &b.created); break;
            case SortKey::Size: c = a.size < b.size ? -1 : a.size > b.size ? 1 : 0; break;
            case SortKey::Type:
                c = CompareStringEx(LOCALE_NAME_USER_DEFAULT, 0, a.type.c_str(), -1, b.type.c_str(), -1, nullptr, nullptr, 0) - CSTR_EQUAL;
                break;
            case SortKey::Tag:
            {
                // Untagged items last in both directions, then the tag
                const std::wstring ta = firstTag(a), tb = firstTag(b);
                if (ta.empty() != tb.empty())
                    return tb.empty();
                c = CompareStringEx(LOCALE_NAME_USER_DEFAULT, 0, ta.c_str(), -1, tb.c_str(), -1, nullptr, nullptr, 0) - CSTR_EQUAL;
                break;
            }
            default: break;
            }
            if (c == 0)
                c = StrCmpLogicalW(a.display.c_str(), b.display.c_str());
            return desc ? c > 0 : c < 0;
        });
    Group();
}

void FileList::Rearrange()
{
    const Kept kept = Keep();
    Arrange();
    m_sel.Reset(Count());
    Relayout();
    Restore(kept);
    m_hot = m_hotCheck = -1;
    m_selectionDirty = true;
    InvalidateRect(m_list, nullptr, FALSE);
    if (m_header)
        InvalidateRect(m_header, nullptr, FALSE);
}

void FileList::SetSort(SortKey key, bool descending)
{
    if (key == m_sortKey && descending == m_sortDesc)
        return;
    // Files' SortStates: the same column's icon turns 0 <-> -180 in 0.2 s (DoubleAnimation, linear); another
    // column's appears already turned
    if (key == m_sortKey && m_header && AnimationsEnabled())
    {
        m_sortTurnFrom = SortTurn();
        m_sortTurnStart = GetTickCount64();
        SetTimer(m_header, kSortTurnTimer, 15, nullptr);
    }
    else
        m_sortTurnStart = 0;
    m_sortKey = key;
    m_sortDesc = descending;
    Rearrange();
}

void FileList::SetSortState(const ListSort& state)
{
    const ListSort now = SortState();
    ListSort s = state;
    s.key = std::clamp(s.key, 0, static_cast<int>(SortKey::Created));
    s.group = std::clamp(s.group, kGroupNone, kGroupTag);
    s.unit = std::clamp(s.unit, 0, 2);
    s.priority = std::clamp(s.priority, -1, 2);
    if (s == now)
        return;
    m_sortKey = static_cast<SortKey>(s.key);
    m_sortDesc = s.descending;
    m_groupKey = s.group;
    m_groupUnit = s.unit;
    m_groupDesc = s.groupDescending;
    m_priority = s.priority;
    Rearrange();
    if (m_header)
        InvalidateRect(m_header, nullptr, FALSE); // the sort arrow
}

ListSort FileList::SortState() const
{
    ListSort s;
    s.key = static_cast<int>(m_sortKey);
    s.descending = m_sortDesc;
    s.group = m_groupKey;
    s.unit = m_groupUnit;
    s.groupDescending = m_groupDesc;
    s.priority = m_priority;
    return s;
}

int FileList::Priority() const
{
    if (m_priority >= 0)
        return m_priority;
    const ViewPrefs* prefs = m_events->ListPrefs();
    return prefs ? prefs->sortPriority : 0;
}

void FileList::SetFilter(const std::wstring& lowerText)
{
    if (lowerText == m_filter)
        return;
    m_filter = lowerText;
    Rearrange();
}

void FileList::ApplyPrefs()
{
    Rearrange();
}

int FileList::RowOf(const std::wstring& name) const
{
    for (int row = 0; row < Count(); ++row)
        if (CompareStringOrdinal(m_items[m_order[row]].name.c_str(), -1, name.c_str(), -1, TRUE) == CSTR_EQUAL)
            return row;
    return -1;
}

bool FileList::Select(const std::wstring& name)
{
    const int row = RowOf(name);
    if (row < 0)
        return false;
    m_userSel = false; // (FastFiles' own choice: not the user's)
    FocusRow(row, true, false, false);
    return true;
}

void FileList::SelectAll()
{
    UserSelected();
    m_sel.All(true);
    SelectionChanged();
}

void FileList::ClearSelection()
{
    UserSelected();
    m_sel.All(false);
    SelectionChanged();
}

void FileList::InvertSelection()
{
    UserSelected();
    m_sel.Invert();
    SelectionChanged();
}

// The item's child ID in the folder, parsed the first time something needs it (menus, drag, the selection)
PCUITEMID_CHILD FileList::ChildOf(int index)
{
    if (index < 0 || index >= static_cast<int>(m_items.size()) || !m_shellFolder)
        return nullptr;
    Item& item = m_items[static_cast<size_t>(index)];
    if (!item.child)
    {
        std::wstring name = item.name; // (ParseDisplayName takes a writable string)
        PIDLIST_RELATIVE pidl = nullptr;
        if (SUCCEEDED(m_shellFolder->ParseDisplayName(nullptr, nullptr, name.data(), nullptr, &pidl, nullptr)) && pidl)
        {
            if (!ILIsEmpty(pidl) && ILIsEmpty(ILNext(pidl)))
                item.child = reinterpret_cast<PITEMID_CHILD>(pidl);
            else
                CoTaskMemFree(pidl);
        }
    }
    return item.child;
}

ComPtr<IShellItemArray> FileList::Selection()
{
    if (!m_selectionDirty)
        return m_selection;
    m_selectionDirty = false;
    m_selection.Reset();
    std::vector<PCUITEMID_CHILD> children;
    for (int row : SelectedRows())
        if (PCUITEMID_CHILD child = ChildOf(m_order[row]))
            children.push_back(child);
    if (!children.empty())
        SHCreateShellItemArray(m_folder.get(), m_shellFolder.Get(), static_cast<UINT>(children.size()), children.data(), &m_selection);
    return m_selection;
}

std::vector<std::wstring> FileList::SelectedPaths() const
{
    std::vector<std::wstring> paths;
    const std::wstring dir = m_path.empty() || m_path.back() == L'\\' ? m_path : m_path + L'\\';
    for (int row : SelectedRows())
        paths.push_back(dir + m_items[m_order[row]].name);
    return paths;
}

ComPtr<IContextMenu> FileList::MenuFor(bool background)
{
    ComPtr<IContextMenu> menu;
    if (!m_shellFolder)
        return menu;
    if (background)
    {
        m_shellFolder->CreateViewObject(m_hwnd, IID_PPV_ARGS(&menu)); // the folder's background menu (新建, 粘贴, 属性, ...)
        return menu;
    }
    std::vector<PCUITEMID_CHILD> children;
    for (int row : SelectedRows())
        if (PCUITEMID_CHILD child = ChildOf(m_order[row]))
            children.push_back(child);
    if (!children.empty())
        m_shellFolder->GetUIObjectOf(m_hwnd, static_cast<UINT>(children.size()), children.data(), IID_IContextMenu, nullptr,
            reinterpret_cast<void**>(menu.GetAddressOf()));
    return menu;
}

bool FileList::Invoke(const char* verb, bool background)
{
    if (!Sandbox::VerbAllowed(verb))
        return false; // test mode: shell verbs that would change the system (open, clipboard, ...) don't run
    ComPtr<IContextMenu> menu = MenuFor(background);
    if (!menu)
        return false;
    HMENU popup = CreatePopupMenu();
    // Many handlers only honour string verbs after QueryContextMenu has run
    menu->QueryContextMenu(popup, 0, 1, 0x7FFF, CMF_NORMAL | (GetKeyState(VK_SHIFT) < 0 ? CMF_EXTENDEDVERBS : 0));
    CMINVOKECOMMANDINFOEX info{ sizeof(info) };
    info.fMask = CMIC_MASK_UNICODE | (GetKeyState(VK_SHIFT) < 0 ? CMIC_MASK_SHIFT_DOWN : 0) | (GetKeyState(VK_CONTROL) < 0 ? CMIC_MASK_CONTROL_DOWN : 0);
    info.hwnd = GetAncestor(m_hwnd, GA_ROOT);
    info.lpVerb = verb;
    wchar_t verbW[64]{};
    MultiByteToWideChar(CP_ACP, 0, verb, -1, verbW, ARRAYSIZE(verbW));
    info.lpVerbW = verbW;
    info.nShow = SW_SHOWNORMAL;
    const bool ok = SUCCEEDED(menu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info)));
    DestroyMenu(popup);
    return ok;
}

// Files' OpenSelectedItems: one folder (or a shortcut to one) opens here; anything else runs its default verb
bool FileList::OpenSelection()
{
    const std::vector<int> rows = SelectedRows();
    if (rows.empty())
        return false;
    if (rows.size() == 1)
    {
        ComPtr<IShellItem> item;
        UniquePidl target;
        if (PCUITEMID_CHILD child = ChildOf(m_order[rows[0]]);
            child && SUCCEEDED(SHCreateItemWithParent(m_folder.get(), m_shellFolder.Get(), child, IID_PPV_ARGS(&item))) &&
            FolderTarget(item.Get(), target))
        {
            m_events->OnListOpen(target.get());
            return true;
        }
    }
    if (Sandbox::Active())
        return true; // test mode: no app starts
    ComPtr<IContextMenu> menu = MenuFor(false);
    HMENU popup = CreatePopupMenu();
    if (!menu || !popup || FAILED(menu->QueryContextMenu(popup, 0, 1, 0x7FFF, CMF_DEFAULTONLY)))
    {
        if (popup)
            DestroyMenu(popup);
        return false;
    }
    const UINT id = GetMenuDefaultItem(popup, FALSE, 0);
    bool ok = false;
    if (id != static_cast<UINT>(-1) && id >= 1)
    {
        CMINVOKECOMMANDINFOEX info{ sizeof(info) };
        info.fMask = CMIC_MASK_UNICODE;
        info.hwnd = GetAncestor(m_hwnd, GA_ROOT);
        info.lpVerb = MAKEINTRESOURCEA(id - 1);
        info.lpVerbW = MAKEINTRESOURCEW(id - 1);
        info.nShow = SW_SHOWNORMAL;
        ok = SUCCEEDED(menu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info)));
    }
    DestroyMenu(popup);
    return ok;
}

void FileList::Watch()
{
    Unwatch();
    if (!m_folder || !m_hwnd)
        return;
    SHChangeNotifyEntry entry{ m_folder.get(), FALSE };
    m_notify = SHChangeNotifyRegister(m_hwnd, SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
        SHCNE_CREATE | SHCNE_DELETE | SHCNE_MKDIR | SHCNE_RMDIR | SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER | SHCNE_UPDATEITEM |
        SHCNE_UPDATEDIR | SHCNE_ATTRIBUTES, kMsgChanged, 1, &entry);
}

void FileList::Unwatch()
{
    if (m_notify)
        SHChangeNotifyDeregister(m_notify);
    m_notify = 0;
}
