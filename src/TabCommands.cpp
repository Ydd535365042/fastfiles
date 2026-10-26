#include "Tab.h"
#include "Lang.h"
#include "Sandbox.h"

namespace
{
// FMTID_Storage keys (System.ItemNameDisplay / DateModified / ItemTypeText / Size); defined locally to avoid INITGUID games
constexpr GUID kStorage = { 0xB725F130, 0x47EF, 0x101A, { 0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC } };
constexpr PROPERTYKEY kKeyName{ kStorage, 10 };
constexpr PROPERTYKEY kKeyModified{ kStorage, 14 };
constexpr PROPERTYKEY kKeyType{ kStorage, 4 };
constexpr PROPERTYKEY kKeySize{ kStorage, 12 };
constexpr PROPERTYKEY kKeyCreated{ kStorage, 15 }; // System.DateCreated
constexpr PROPERTYKEY kKeyNull{};

const PROPERTYKEY& KeyFor(SortKey key)
{
    switch (key)
    {
    case SortKey::Modified: return kKeyModified;
    case SortKey::Created: return kKeyCreated;
    case SortKey::Type: return kKeyType;
    case SortKey::Size: return kKeySize;
    default: return kKeyName;
    }
}

bool SameKey(const PROPERTYKEY& a, const PROPERTYKEY& b) { return a.pid == b.pid && IsEqualGUID(a.fmtid, b.fmtid); }

std::wstring UniqueName(const std::wstring& dir, const std::wstring& base, const std::wstring& ext)
{
    for (int i = 1; i < 1000; ++i)
    {
        std::wstring name = i == 1 ? base + ext : base + L" (" + std::to_wstring(i) + L")" + ext;
        if (GetFileAttributesW((dir + L"\\" + name).c_str()) == INVALID_FILE_ATTRIBUTES)
            return name;
    }
    return {};
}
}

ComPtr<IFolderView2> Tab::View() const
{
    // Nothing while the home page is up: the hidden view still holds the previous folder's selection.
    // (The column view's columns are own lists: List() is the active one, Files' ActivePaneOrColumn.)
    ComPtr<IFolderView2> view;
    if (IExplorerBrowser* browser = ActiveBrowser(); browser && !m_onHome && !InList())
        browser->GetCurrentView(IID_PPV_ARGS(&view));
    return view;
}

int Tab::SelectedCount() const
{
    if (FileList* list = List())
        return list->SelectedCount();
    int n = 0;
    if (auto view = View())
        view->ItemCount(SVGIO_SELECTION, &n);
    return n;
}

unsigned Tab::SelectionStamp() const
{
    FileList* list = List();
    return list ? list->SelectionVersion() : 0;
}

int Tab::FocusedIndex() const
{
    if (FileList* list = List())
        return list->FocusedIndex();
    int i = -1;
    if (auto view = View())
        view->GetFocusedItem(&i);
    return i;
}

// The selection as shell items: the own list's, or the shell view's (null / empty when nothing is selected)
ComPtr<IShellItemArray> Tab::Selection() const
{
    ComPtr<IShellItemArray> selection;
    if (FileList* list = List())
        return list->Selection();
    if (auto view = View())
        view->GetSelection(FALSE, &selection);
    return selection;
}

bool Tab::InvokeVerb(const char* verb, bool background)
{
    if (FileList* list = List())
        return list->Invoke(verb, background);
    if (!Sandbox::VerbAllowed(verb))
        return false; // test mode: shell verbs that would change the system (open, install, wallpaper, ...) don't run
    ComPtr<IShellView> view = ShellView();
    if (!view)
        return false;
    ComPtr<IContextMenu> menu;
    if (FAILED(view->GetItemObject(background ? SVGIO_BACKGROUND : SVGIO_SELECTION, IID_PPV_ARGS(&menu))))
        return false;
    HMENU popup = CreatePopupMenu();
    // Many handlers only honour string verbs after QueryContextMenu has run
    menu->QueryContextMenu(popup, 0, 1, 0x7FFF, CMF_NORMAL | (GetKeyState(VK_SHIFT) < 0 ? CMF_EXTENDEDVERBS : 0));
    CMINVOKECOMMANDINFOEX info{ sizeof(info) };
    info.fMask = CMIC_MASK_UNICODE | (GetKeyState(VK_SHIFT) < 0 ? CMIC_MASK_SHIFT_DOWN : 0) | (GetKeyState(VK_CONTROL) < 0 ? CMIC_MASK_CONTROL_DOWN : 0);
    info.hwnd = m_parent;
    info.lpVerb = verb;
    wchar_t verbW[64]{};
    MultiByteToWideChar(CP_ACP, 0, verb, -1, verbW, ARRAYSIZE(verbW));
    info.lpVerbW = verbW;
    info.nShow = SW_SHOWNORMAL;
    const bool ok = SUCCEEDED(menu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info)));
    DestroyMenu(popup);
    return ok;
}

bool Tab::CreateNewItem(bool folder)
{
    const std::wstring dir = PidlName(m_current.get(), SIGDN_FILESYSPATH);
    if (dir.empty())
        return false;
    const std::wstring name = folder ? UniqueName(dir, Tr(L"新建文件夹"), L"") : UniqueName(dir, Tr(L"新建文本文档"), L".txt");
    if (name.empty())
        return false;
    const std::wstring full = dir + L"\\" + name;
    if (folder ? !CreateDirectoryW(full.c_str(), nullptr) : !CloseHandle(CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)))
        return false;
    SHChangeNotify(folder ? SHCNE_MKDIR : SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSH, full.c_str(), nullptr);
    if (FileList* list = List())
    {
        list->RenameWhenListed(name);
        list->Reload();
        return true;
    }

    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(SHParseDisplayName(full.c_str(), nullptr, &pidl, 0, nullptr)))
    {
        m_pendingRename.reset(reinterpret_cast<PIDLIST_ABSOLUTE>(ILClone(ILFindLastID(pidl))));
        CoTaskMemFree(pidl);
        m_pendingName = name;
        m_renameTries = 0;
        SetTimer(m_host, kRenameTimer, 50, nullptr);
        TryPendingRename();
    }
    return true;
}

void Tab::TryPendingRename()
{
    ComPtr<IShellView> shellView = ShellView();
    auto view = View();
    if (!m_pendingRename || !view || ++m_renameTries > 40 || !shellView)
    {
        if (m_renameTries > 40 || !m_pendingRename)
        {
            KillTimer(m_host, kRenameTimer);
            m_pendingRename.reset();
        }
        return;
    }
    // The new item shows up after the shell's change notification lands; until then SelectItem selects nothing
    shellView->SelectItem(reinterpret_cast<PCUITEMID_CHILD>(m_pendingRename.get()), SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_ENSUREVISIBLE | SVSI_FOCUSED);
    ComPtr<IShellItemArray> selection;
    ComPtr<IShellItem> item;
    PWSTR name = nullptr;
    if (SUCCEEDED(view->GetSelection(FALSE, &selection)) && SUCCEEDED(selection->GetItemAt(0, &item)) &&
        SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &name)))
    {
        const bool match = _wcsicmp(name, m_pendingName.c_str()) == 0;
        CoTaskMemFree(name);
        if (match)
        {
            KillTimer(m_host, kRenameTimer);
            m_pendingRename.reset();
            view->DoRename();
        }
    }
}

bool Tab::Rename()
{
    if (FileList* list = List())
        return list->BeginRename();
    auto view = View();
    return view && SelectedCount() > 0 && SUCCEEDED(view->DoRename());
}

std::wstring Tab::SelectionPaths() const
{
    std::wstring out;
    ComPtr<IShellItemArray> selection = Selection();
    DWORD count = 0;
    if (selection && SUCCEEDED(selection->GetCount(&count)) && count > 0)
    {
        for (DWORD i = 0; i < count; ++i)
        {
            ComPtr<IShellItem> item;
            PWSTR path = nullptr;
            if (SUCCEEDED(selection->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &path)))
            {
                out += (out.empty() ? L"" : L"\r\n") + std::wstring(path);
                CoTaskMemFree(path);
            }
        }
        return out;
    }
    out = PidlName(m_current.get(), SIGDN_FILESYSPATH);
    return out.empty() ? PidlName(m_current.get(), SIGDN_DESKTOPABSOLUTEPARSING) : out;
}

bool Tab::SelectionSize(ULONGLONG& bytes) const
{
    bytes = 0;
    ComPtr<IShellItemArray> selection = Selection();
    DWORD count = 0;
    if (!selection || FAILED(selection->GetCount(&count)) || count == 0 || count > 2000)
        return false;
    bool any = false;
    for (DWORD i = 0; i < count; ++i)
    {
        ComPtr<IShellItem> item;
        ComPtr<IShellItem2> item2;
        SFGAOF attrs = 0;
        ULONGLONG size = 0;
        if (SUCCEEDED(selection->GetItemAt(i, &item)) && SUCCEEDED(item.As(&item2)) &&
            SUCCEEDED(item->GetAttributes(SFGAO_FOLDER | SFGAO_STREAM, &attrs)) &&
            (!(attrs & SFGAO_FOLDER) || (attrs & SFGAO_STREAM)) && SUCCEEDED(item2->GetUInt64(kKeySize, &size)))
        {
            bytes += size;
            any = true;
        }
    }
    return any;
}

void Tab::SelectAll()
{
    if (FileList* list = List())
        return list->SelectAll();
    auto view = View();
    int n = 0;
    if (!view || FAILED(view->ItemCount(SVGIO_ALLVIEW, &n)))
        return;
    for (int i = 0; i < n; ++i)
        view->SelectItem(i, SVSI_SELECT);
}

void Tab::InvertSelection()
{
    if (FileList* list = List())
        return list->InvertSelection();
    auto view = View();
    int n = 0;
    if (!view || FAILED(view->ItemCount(SVGIO_ALLVIEW, &n)))
        return;
    for (int i = 0; i < n; ++i)
    {
        PITEMID_CHILD child = nullptr;
        DWORD state = 0;
        if (SUCCEEDED(view->Item(i, &child)))
        {
            view->GetSelectionState(child, &state);
            CoTaskMemFree(child);
        }
        view->SelectItem(i, (state & SVSI_SELECT) ? SVSI_DESELECT : SVSI_SELECT);
    }
}

void Tab::ClearSelection()
{
    if (FileList* list = List())
        return list->ClearSelection();
    if (ComPtr<IShellView> view = ShellView())
        view->SelectItem(nullptr, SVSI_DESELECTOTHERS);
}

void Tab::SetLayout(ViewLayout layout)
{
    if (InList())
    {
        // The own list shows every one of Files' layouts
        m_list->SetMode(ListMode(layout));
        return;
    }
    auto view = View();
    if (!view)
        return;
    static const struct { FOLDERVIEWMODE mode; int size; } map[] = {
        { FVM_DETAILS, 16 }, { FVM_LIST, 16 }, { FVM_TILE, 48 }, { FVM_CONTENT, 32 },
        { FVM_SMALLICON, 16 }, { FVM_ICON, 48 }, { FVM_ICON, 96 }, { FVM_ICON, 256 } };
    const auto& m = map[ShellLayoutIndex(static_cast<int>(layout))];
    view->SetViewModeAndIconSize(m.mode, m.size);
}

ViewLayout Tab::GetLayout() const
{
    if (InList())
    {
        switch (m_list->GetMode())
        {
        case FileList::Mode::List: return ViewLayout::List;
        case FileList::Mode::Cards: return ViewLayout::Tiles;
        case FileList::Mode::Grid: return ViewLayout::LargeIcons;
        default: return ViewLayout::Details;
        }
    }
    FOLDERVIEWMODE mode = FVM_AUTO;
    int size = 0;
    auto view = View();
    if (!view || FAILED(view->GetViewModeAndIconSize(&mode, &size)))
        return ViewLayout::Details;
    switch (mode)
    {
    case FVM_LIST: return ViewLayout::List;
    case FVM_TILE: return ViewLayout::Tiles;
    case FVM_CONTENT: return ViewLayout::Content;
    case FVM_SMALLICON: return ViewLayout::SmallIcons;
    case FVM_ICON: return size >= 256 ? ViewLayout::ExtraLargeIcons : size >= 96 ? ViewLayout::LargeIcons : ViewLayout::MediumIcons;
    default: return ViewLayout::Details;
    }
}

void Tab::SetSort(SortKey key, bool descending)
{
    if (FileList* list = List())
    {
        list->SetSort(key, descending);
        StoreSort(list); // the folder's choice (Files' LayoutPreferencesManager)
        return;
    }
    if (key == SortKey::Tag)
        return; // (tags are FastFiles' own: the shell can't sort by them)
    if (auto view = View())
    {
        SORTCOLUMN column{ KeyFor(key), descending ? SORT_DESCENDING : SORT_ASCENDING };
        view->SetSortColumns(&column, 1);
    }
}

bool Tab::GetSort(SortKey& key, bool& descending) const
{
    if (FileList* list = List())
    {
        key = list->GetSortKey();
        descending = list->SortDescending();
        return true;
    }
    SORTCOLUMN column{};
    auto view = View();
    if (!view || FAILED(view->GetSortColumns(&column, 1)))
        return false;
    descending = column.direction == SORT_DESCENDING;
    for (SortKey k : { SortKey::Name, SortKey::Modified, SortKey::Type, SortKey::Size, SortKey::Created })
        if (SameKey(column.propkey, KeyFor(k)))
        {
            key = k;
            return true;
        }
    return false;
}

namespace
{
// The shell's key for a Files GroupOption (kKeyNull: none / not one it has)
const PROPERTYKEY& ShellGroupKey(int key)
{
    switch (key)
    {
    case FileList::kGroupName: return kKeyName;
    case FileList::kGroupModified: return kKeyModified;
    case FileList::kGroupCreated: return kKeyCreated;
    case FileList::kGroupSize: return kKeySize;
    case FileList::kGroupType: return kKeyType;
    default: return kKeyNull;
    }
}
}

bool Tab::CanGroup(int key) const
{
    return List() || key != FileList::kGroupTag;
}

bool Tab::CanSortBy(SortKey key) const
{
    return List() || key != SortKey::Tag;
}

void Tab::SetGroup(const GroupChoice& choice)
{
    if (FileList* list = List())
    {
        list->SetGroup(choice.key, choice.unit, choice.descending);
        StoreSort(list);
        return;
    }
    if (!CanGroup(choice.key))
        return;
    if (auto view = View())
        view->SetGroupBy(ShellGroupKey(choice.key), choice.descending ? FALSE : TRUE);
}

Tab::GroupChoice Tab::GetGroup() const
{
    GroupChoice choice;
    if (FileList* list = List())
    {
        choice.key = list->GroupKeyShown();
        choice.unit = list->GroupUnit();
        choice.descending = list->GroupDescending();
        return choice;
    }
    PROPERTYKEY key{};
    BOOL ascending = TRUE;
    auto view = View();
    if (!view || FAILED(view->GetGroupBy(&key, &ascending)))
        return choice;
    choice.descending = !ascending;
    for (int k = FileList::kGroupName; k < FileList::kGroupTag; ++k)
        if (SameKey(key, ShellGroupKey(k)))
            choice.key = k;
    return choice;
}
