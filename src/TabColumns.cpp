#include "Tab.h"
#include "Session.h"
#include "fastfiles_core.h"

// How a folder opens (Files' LayoutPreferencesManager.GetLayoutType + AdaptiveLayoutHelpers) and the column view.
// Per-folder choices live in layouts.txt (Rust core): "columns", "adaptive" or "manual". A folder without one uses
// the default layout. The adaptive layout shows large icons when more than 60% of the folder is pictures, music or
// video (or desktop.ini says Pictures/Videos), details otherwise; picking a layout by hand turns it off for the folder.

namespace
{
std::wstring LayoutsFile() { return AppDataFile(L"layouts.txt"); }

std::wstring Normalize(std::wstring path)
{
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/'))
        path.pop_back();
    return path;
}

bool SamePath(const std::wstring& a, const std::wstring& b)
{
    return CompareStringOrdinal(Normalize(a).c_str(), -1, Normalize(b).c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool IsRootPath(const std::wstring& path)
{
    return path.size() < MAX_PATH && PathIsRootW(Normalize(path).c_str());
}

std::wstring ParentPath(std::wstring path)
{
    path = Normalize(path);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return {};
    path.resize(slash);
    if (path.size() == 2 && path[1] == L':')
        path += L'\\';
    return path;
}

// Files' TryPredictLayout skips network folders (no sampling before they show)
bool IsNetwork(const std::wstring& path)
{
    if (path.rfind(L"\\\\", 0) == 0)
        return true;
    if (path.size() < 2 || path[1] != L':')
        return false;
    const wchar_t root[] = { path[0], L':', L'\\', 0 };
    const UINT type = GetDriveTypeW(root);
    return type == DRIVE_REMOTE || type == DRIVE_NO_ROOT_DIR;
}

bool CanColumnsAt(PCIDLIST_ABSOLUTE pidl)
{
    return pidl && !IsPagePidl(pidl) && !PidlName(pidl, SIGDN_FILESYSPATH).empty();
}
}

int Tab::LoadMode(const std::wstring& key, bool fresh) const
{
    if (key.empty())
        return 0;
    if (fresh || key != m_modeKey)
    {
        const std::wstring file = LayoutsFile();
        m_modeValue = static_cast<int>(ff_layout_get(reinterpret_cast<const uint16_t*>(file.c_str()), file.size(),
            reinterpret_cast<const uint16_t*>(key.c_str()), key.size()));
        m_modeKey = key;
    }
    return m_modeValue;
}

void Tab::StoreMode(const std::wstring& key, int mode)
{
    if (key.empty())
        return;
    const std::wstring file = LayoutsFile();
    ff_layout_set(reinterpret_cast<const uint16_t*>(file.c_str()), file.size(), reinterpret_cast<const uint16_t*>(key.c_str()), key.size(),
        static_cast<uint32_t>(mode));
    m_modeKey = key;
    m_modeValue = mode;
}

// Files' GetLayoutPreferencesForPath for sort / grouping: synced, everyone's defaults; else the folder's stored choice
// (each part it has) over the defaults, which for Downloads group by date created, newest first (GetDefaultLayoutPreferences)
ListSort Tab::SortFor(PCIDLIST_ABSOLUTE folder) const
{
    ListSort s = m_prefs ? m_prefs->defaultSort : ListSort{};
    if (!m_prefs || m_prefs->syncLayout || !folder)
        return s;
    const std::wstring key = PidlName(folder, SIGDN_DESKTOPABSOLUTEPARSING);
    const std::wstring file = LayoutsFile();
    const uint32_t code = key.empty() ? 0 : ff_layout_get_sort(reinterpret_cast<const uint16_t*>(file.c_str()), file.size(),
        reinterpret_cast<const uint16_t*>(key.c_str()), key.size());
    if (!(code & (1u << 31)))
    {
        PWSTR downloads = nullptr;
        const std::wstring path = PidlName(folder, SIGDN_FILESYSPATH);
        if (!path.empty() && SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DONT_VERIFY, nullptr, &downloads)) &&
            SamePath(path, downloads))
        {
            s.group = FileList::kGroupCreated;
            s.groupDescending = true;
            s.unit = 0;
        }
        CoTaskMemFree(downloads);
        return s;
    }
    if ((code & 0xF) != 0xF)
    {
        s.key = static_cast<int>(code & 0xF);
        s.descending = (code & (1u << 4)) != 0;
    }
    if (((code >> 8) & 0xF) != 0xF)
    {
        s.group = static_cast<int>((code >> 8) & 0xF);
        s.unit = static_cast<int>((code >> 12) & 0x3);
        s.groupDescending = (code & (1u << 14)) != 0;
    }
    if (((code >> 16) & 0x3) != 0x3)
        s.priority = static_cast<int>((code >> 16) & 0x3);
    return s;
}

// Files' SetLayoutPreferencesForPath: synced, the defaults change for every folder; else the folder's own choice (not
// written while it still equals what the folder would get anyway: SetLayoutPreferencesToDatabase)
void Tab::StoreSort(FileList* list)
{
    if (!list || !m_prefs || !list->Folder())
        return;
    const ListSort now = list->SortState();
    if (m_prefs->syncLayout)
    {
        if (now != m_prefs->defaultSort)
            m_events->OnTabSortDefault(this, now);
        return;
    }
    if (now == SortFor(list->Folder()))
        return;
    const std::wstring key = PidlName(list->Folder(), SIGDN_DESKTOPABSOLUTEPARSING);
    if (key.empty())
        return;
    const uint32_t code = (1u << 31) | (static_cast<uint32_t>(now.key) & 0xF) | (now.descending ? 1u << 4 : 0) |
        ((static_cast<uint32_t>(now.group) & 0xF) << 8) | ((static_cast<uint32_t>(now.unit) & 0x3) << 12) |
        (now.groupDescending ? 1u << 14 : 0) | ((now.priority < 0 ? 0x3u : static_cast<uint32_t>(now.priority) & 0x3) << 16);
    const std::wstring file = LayoutsFile();
    ff_layout_set_sort(reinterpret_cast<const uint16_t*>(file.c_str()), file.size(), reinterpret_cast<const uint16_t*>(key.c_str()),
        key.size(), code);
}

void Tab::ApplySortDefault()
{
    if (!m_prefs || !m_prefs->syncLayout)
        return;
    const ListSort s = m_prefs->defaultSort;
    ForEachList([&s](FileList* list) { list->SetSortState(s); });
}

int Tab::SortPriority() const
{
    if (FileList* list = List())
        return list->Priority();
    return m_prefs ? m_prefs->sortPriority : 0;
}

// Files' SortFoldersFirst / SortFilesFirst / SortFilesAndFoldersTogether: the folder's (or, synced, everyone's)
void Tab::SetSortPriority(int priority)
{
    FileList* list = List();
    if (!list || !m_prefs)
        return;
    ListSort s = list->SortState();
    s.priority = std::clamp(priority, 0, 2);
    if (m_prefs->syncLayout)
    {
        // Everyone's: the lists keep following ViewPrefs::sortPriority (-1), which the window changes
        m_events->OnTabSortDefault(this, s);
        return;
    }
    list->SetSortState(s);
    StoreSort(list);
}

Tab::LayoutDecision Tab::DecideLayout(PCIDLIST_ABSOLUTE pidl, ModeHint hint)
{
    LayoutDecision d;
    if (!m_prefs || !pidl || IsPagePidl(pidl))
        return d;
    const std::wstring path = PidlName(pidl, SIGDN_FILESYSPATH);
    const bool canColumns = CanColumnsAt(pidl);
    const int def = m_prefs->defaultLayout;
    // History remembers the page: back to a column view opens one again (or the other way round)
    if (hint == ModeHint::Columns && canColumns)
    {
        d.columns = true;
        return d;
    }
    if (m_prefs->syncLayout)
    {
        // Files: one layout for every folder; the adaptive layout isn't available then
        d.columns = def == static_cast<int>(ViewLayout::Columns) && canColumns && hint != ModeHint::Normal;
        d.layout = ShellLayoutIndex(def);
        return d;
    }
    d.key = PidlName(pidl, SIGDN_DESKTOPABSOLUTEPARSING);
    const int code = LoadMode(d.key, true);
    const int stored = code & 0xFF;
    const bool columns = stored == 1 || (stored == 0 && def == static_cast<int>(ViewLayout::Columns));
    if (columns && canColumns && hint != ModeHint::Normal)
    {
        d.columns = true;
        return d;
    }
    if (stored == 3)
    {
        // Picked by hand; the layout itself when it was recorded (otherwise the shell's view memory has it)
        if (const int layout = (code >> 8) & 0xFF; layout >= 1 && layout <= 8)
            d.layout = layout - 1;
        return d;
    }
    if (stored == 0 && def != static_cast<int>(ViewLayout::Adaptive))
        return d; // the default layout (new folders get it from the browser's folder settings)
    if (path.empty())
        return d; // This PC, Network...: the shell's own choice
    if (IsNetwork(path))
    {
        d.adaptiveLater = true;
        return d;
    }
    switch (ff_layout_predict(reinterpret_cast<const uint16_t*>(path.c_str()), path.size()))
    {
    case 1: d.layout = static_cast<int>(ViewLayout::Details); break;
    case 2: d.layout = static_cast<int>(ViewLayout::LargeIcons); break; // Files' grid at its default (large) size
    default: break; // empty or unreadable: keeps the layout it has
    }
    return d;
}

void Tab::OpenLocation(PCIDLIST_ABSOLUTE pidlIn, ModeHint hint)
{
    const UniquePidl pidl = ClonePidl(pidlIn); // may point at m_current
    const LayoutDecision d = DecideLayout(pidl.get(), hint);
    if (d.columns && EnterColumns(pidl.get()))
        return;
    if (WantsList(pidl.get(), d) && EnterList(pidl.get(), static_cast<ViewLayout>(ListLayout(d))))
    {
        // Tracked like the shell's view: a layout picked some other way before leaving becomes the folder's
        m_layoutKey = d.key;
        m_layoutShown = static_cast<int>(GetLayout());
        return;
    }
    m_pendingTrack = !d.key.empty();
    m_pendingAdaptiveLater = d.adaptiveLater;
    m_pendingKey = d.key;
    m_applyLayout = d.layout;
    if (m_columns)
        m_leavingColumns = true; // the columns stay up until the folder browser has loaded
    if (CreateBrowser())
        m_browser->BrowseToIDList(pidl.get(), SBSP_ABSOLUTE);
}

// Files' ColumnsLayoutPage.OnNavigatedTo: from the target up to a pinned/cloud folder or the drive root, one column each
std::vector<UniquePidl> Tab::ColumnChain(const std::wstring& target) const
{
    std::vector<std::wstring> paths;
    std::wstring path = Normalize(target);
    for (int guard = 0; !path.empty() && guard < 128; ++guard)
    {
        paths.push_back(path);
        const bool root = IsRootPath(path) || (m_prefs && std::any_of(m_prefs->columnRoots.begin(), m_prefs->columnRoots.end(),
            [&](const std::wstring& r) { return SamePath(r, path); }));
        if (root)
            break;
        path = ParentPath(path);
    }
    std::vector<UniquePidl> chain;
    for (auto it = paths.rbegin(); it != paths.rend(); ++it)
    {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(SHParseDisplayName(it->c_str(), nullptr, &pidl, 0, nullptr)) || !pidl)
            return {};
        chain.emplace_back(pidl);
    }
    return chain;
}

bool Tab::EnterColumns(PCIDLIST_ABSOLUTE pidlIn)
{
    const UniquePidl target = ClonePidl(pidlIn); // may point at m_current, which Complete replaces
    const std::wstring path = PidlName(target.get(), SIGDN_FILESYSPATH);
    if (!m_host || !m_prefs || !m_prefs->theme || path.empty())
        return false;
    std::vector<UniquePidl> chain = ColumnChain(path);
    if (chain.empty())
        return false;
    if (!m_columns)
    {
        m_columns = std::make_unique<ColumnView>(this, m_prefs->theme);
        if (!m_columns->Create(m_host))
        {
            m_columns.reset();
            return false;
        }
    }
    m_leavingColumns = false;
    m_applyLayout = -1;
    // What to select in the last column: the --select item, or the folder we came up from (as the folder view does)
    UniquePidl child;
    const std::wstring before = m_current ? PidlName(m_current.get(), SIGDN_FILESYSPATH) : std::wstring();
    if (m_requestedSelect)
        child = std::move(m_requestedSelect);
    else if ((!m_prefs || m_prefs->selectPrevious) && !before.empty() && before.size() > path.size() && SamePath(ParentPath(before), path))
    {
        PIDLIST_ABSOLUTE from = nullptr;
        if (SUCCEEDED(SHParseDisplayName(before.c_str(), nullptr, &from, 0, nullptr)) && from)
        {
            child.reset(reinterpret_cast<PIDLIST_ABSOLUTE>(ILClone(ILFindLastID(from))));
            CoTaskMemFree(from);
        }
    }
    const bool hadFocus = HasFocus();
    const bool sameFolder = !before.empty() && SamePath(before, path);
    if (!m_columns->Open(chain, reinterpret_cast<PCUITEMID_CHILD>(child.get()), hadFocus))
    {
        DestroyColumns(); // the root can't be listed: the folder opens without the column view
        return false;
    }
    ShowColumnsSurface();
    if (!sameFolder || m_pending != Pending::None)
        Complete(chain.back().get());
    else
        m_current = ClonePidl(chain.back().get()); // a layout switch: no history entry
    m_pendingSelect.reset();
    m_currentColumns = true;
    if (!m_filter.empty())
        SetFilter(L"");
    m_events->OnTabNavigated(this);
    return true;
}

void Tab::ShowColumnsSurface()
{
    if (!m_columns)
        return;
    if (m_onHome)
    {
        m_onHome = false;
        if (HWND page = PageWindow())
            ShowWindow(page, SW_HIDE);
    }
    SetBrowserShown(false);
    HideList();
    RECT rc{};
    GetClientRect(m_host, &rc);
    SetWindowPos(m_columns->Hwnd(), HWND_TOP, 0, 0, rc.right, rc.bottom, SWP_SHOWWINDOW | SWP_NOACTIVATE);
}

void Tab::DestroyColumns()
{
    if (!m_columns)
        return;
    const bool hadFocus = HasFocus();
    m_columns.reset();
    m_leavingColumns = false;
    m_resultsOverColumns = false;
    m_columnsDark = false;
    if (hadFocus && !m_onHome)
        Focus();
}

// Files' SetSelectedPathOrNavigate: a folder already shown, or one below a shown column, stays in the column view
bool Tab::AdjustColumns(PCIDLIST_ABSOLUTE pidl)
{
    if (!InColumns() || !pidl)
        return false;
    const std::wstring path = PidlName(pidl, SIGDN_FILESYSPATH);
    if (path.empty())
        return false;
    const bool hadFocus = HasFocus();
    for (int i = m_columns->Count() - 1; i >= 0; --i)
    {
        const std::wstring folder = PidlName(m_columns->Folder(i), SIGDN_FILESYSPATH);
        if (SamePath(folder, path))
        {
            m_columns->Dismiss(i);
            m_columns->Activate(i, hadFocus);
        }
        else if (SamePath(folder, ParentPath(path)))
        {
            PIDLIST_ABSOLUTE parsed = nullptr;
            if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &parsed, 0, nullptr)) || !parsed)
                return false;
            const UniquePidl target(parsed);
            m_columns->OpenAfter(i, target.get());
            m_columns->Activate(i + 1, hadFocus);
        }
        else
            continue;
        if (m_requestedSelect)
        {
            m_columns->Select(reinterpret_cast<PCUITEMID_CHILD>(m_requestedSelect.get()));
            m_requestedSelect.reset();
        }
        OnColumnActive(m_columns->Folder(m_columns->Active()));
        return true;
    }
    return false;
}

// Moving between columns isn't history (Files keeps one page for the whole column view); the tab shows the active one
void Tab::OnColumnActive(PCIDLIST_ABSOLUTE folder)
{
    if (!folder || (m_current && ILIsEqual(m_current.get(), folder)))
        return;
    m_current = ClonePidl(folder);
    if (!m_filter.empty())
        SetFilter(L""); // another folder: the filter starts empty, as it does after navigating
    m_events->OnTabNavigated(this);
}

// A folder a column can't list (FileList: file system folders only): the tab opens it as a folder of its own
void Tab::OnColumnCantShow(PCIDLIST_ABSOLUTE folder)
{
    if (!folder || !m_host)
        return;
    // Posted: the column view is still inside its own call
    if (PIDLIST_ABSOLUTE copy = ILCloneFull(folder); copy && !PostMessageW(m_host, kMsgOpen, 2, reinterpret_cast<LPARAM>(copy)))
        CoTaskMemFree(copy);
}

// The shell's browser, while it's what shows the folder (the column view's columns are own lists)
IExplorerBrowser* Tab::ActiveBrowser() const
{
    if (InColumns())
        return nullptr; // (also while the browser loads behind them: they show until it has)
    return m_browser.Get();
}

ComPtr<IShellView> Tab::ViewForWindow(HWND viewWindow) const
{
    ComPtr<IShellView> view;
    HWND w = nullptr;
    if (m_browser && SUCCEEDED(m_browser->GetCurrentView(IID_PPV_ARGS(&view))) && SUCCEEDED(view->GetWindow(&w)) && w == viewWindow)
        return view;
    return nullptr;
}

bool Tab::CanColumns() const
{
    return !m_onHome && !m_results && CanColumnsAt(m_current.get());
}

ViewLayout Tab::DisplayedLayout() const
{
    return InColumns() ? ViewLayout::Columns : FilesLayout(static_cast<int>(GetLayout())); // (Files' five layouts)
}

// Files' LayoutAdaptiveAction.IsOn: the folder follows the adaptive layout (not in the column view, not when synced)
bool Tab::AdaptiveOn() const
{
    if (!m_prefs || m_prefs->syncLayout || m_onHome || m_results || m_columns || !m_current)
        return false;
    const int stored = LoadMode(PidlName(m_current.get(), SIGDN_DESKTOPABSOLUTEPARSING), false) & 0xFF;
    return stored == 2 || (stored == 0 && m_prefs->defaultLayout == static_cast<int>(ViewLayout::Adaptive));
}

void Tab::ChooseLayout(ViewLayout layout)
{
    if (!m_prefs || m_onHome || !m_current)
        return;
    if (m_results)
    {
        if (static_cast<int>(layout) <= static_cast<int>(ViewLayout::ExtraLargeIcons))
            SetLayout(layout); // search results: just the view (no folder to remember it for)
        return;
    }
    // Synced: MainWindow stores it as the default for every folder instead
    const std::wstring key = m_prefs->syncLayout ? std::wstring() : PidlName(m_current.get(), SIGDN_DESKTOPABSOLUTEPARSING);
    if (layout == ViewLayout::Columns)
    {
        if (InColumns() || !CanColumns())
            return;
        m_layoutKey.clear(); // the column view replaces whatever the folder showed
        StoreMode(key, 1);
        const UniquePidl here = ClonePidl(m_current.get());
        EnterColumns(here.get());
        return;
    }
    if (layout == ViewLayout::Adaptive)
    {
        if (m_prefs->syncLayout)
            return; // Files: not available while folders share one layout
        m_layoutKey.clear(); // whatever showed before doesn't count as a choice by hand
        StoreMode(key, 2);
        if (InColumns())
        {
            OpenLocation(m_current.get(), ModeHint::Normal);
            return;
        }
        const LayoutDecision d = DecideLayout(m_current.get(), ModeHint::Normal);
        const bool listed = d.layout >= 0 && SwitchToList(static_cast<ViewLayout>(d.layout));
        if (!listed && d.layout >= 0)
            SetLayout(static_cast<ViewLayout>(d.layout));
        m_layoutKey = d.key;
        m_layoutShown = static_cast<int>(GetLayout());
        return;
    }
    // Picked by hand: the adaptive layout is off for this folder, and this layout is its own (Files writes it at once,
    // so another tab opening the folder gets it too)
    StoreMode(key, 3 | ((static_cast<int>(layout) + 1) << 8));
    if (SwitchToList(layout))
    {
        m_layoutKey = key;
        m_layoutShown = static_cast<int>(GetLayout());
        return;
    }
    if (InColumns())
    {
        m_pendingTrack = !key.empty();
        m_pendingKey = key;
        m_pendingAdaptiveLater = false;
        m_applyLayout = static_cast<int>(layout);
        m_leavingColumns = true;
        if (CreateBrowser())
            m_browser->BrowseToIDList(m_current.get(), SBSP_ABSOLUTE);
        return;
    }
    SetLayout(layout);
    m_layoutKey = key;
    m_layoutShown = static_cast<int>(GetLayout());
}

// "Use the same layout for every folder" (Files' SyncFolderPreferencesAcrossDirectories): the default layout everywhere.
// After a navigation only the shell view's layout is corrected (DecideLayout already chose columns or not).
void Tab::ApplyLayoutPref(bool afterNavigation)
{
    if (!m_prefs || !m_prefs->syncLayout || m_onHome || m_results || !m_current)
        return;
    const bool wantColumns = m_prefs->defaultLayout == static_cast<int>(ViewLayout::Columns) && CanColumnsAt(m_current.get());
    if (!afterNavigation && wantColumns != InColumns())
    {
        if (wantColumns)
        {
            const UniquePidl here = ClonePidl(m_current.get());
            EnterColumns(here.get());
        }
        else if (!SwitchToList(FilesLayout(m_prefs->defaultLayout)))
        {
            m_applyLayout = ShellLayoutIndex(m_prefs->defaultLayout);
            m_pendingTrack = m_pendingAdaptiveLater = false;
            m_leavingColumns = true;
            if (CreateBrowser())
                m_browser->BrowseToIDList(m_current.get(), SBSP_ABSOLUTE);
        }
        return;
    }
    if (InList())
        SetLayout(FilesLayout(m_prefs->defaultLayout));
    else if (!InColumns() && GetLayout() != static_cast<ViewLayout>(ShellLayoutIndex(m_prefs->defaultLayout)))
        SetLayout(static_cast<ViewLayout>(ShellLayoutIndex(m_prefs->defaultLayout)));
}

void Tab::CheckLeave()
{
    // A layout changed some other way (the view's own 查看 menu, Ctrl+wheel): the folder's choice by hand now
    if (!m_layoutKey.empty() && !m_results && !m_onHome && !m_columns && m_prefs && !m_prefs->syncLayout)
        if (const int now = static_cast<int>(GetLayout()); now != m_layoutShown)
            StoreMode(m_layoutKey, 3 | ((now + 1) << 8));
    m_layoutKey.clear();
    m_layoutShown = -1;
    if (m_host)
        KillTimer(m_host, kAdaptiveTimer);
}

// Network folders aren't sampled before they show (Files); once the list stops growing, its names decide
void Tab::CheckNetworkAdaptive()
{
    auto view = View();
    if (m_layoutKey.empty() || m_results || m_onHome || m_columns || !view || ++m_adaptiveTicks > 12)
    {
        KillTimer(m_host, kAdaptiveTimer);
        return;
    }
    int count = 0;
    view->ItemCount(SVGIO_ALLVIEW, &count);
    if (count != m_adaptiveCount)
    {
        m_adaptiveCount = count; // still listing
        return;
    }
    KillTimer(m_host, kAdaptiveTimer);
    if (static_cast<int>(GetLayout()) != m_layoutShown)
        return; // changed by hand meanwhile (CheckLeave records that when the folder is left)
    ComPtr<IShellFolder> folder;
    if (FAILED(view->GetFolder(IID_PPV_ARGS(&folder))))
        return;
    std::wstring names;
    for (int i = 0; i < std::min(count, 2000); ++i)
    {
        PITEMID_CHILD child = nullptr;
        STRRET ret{};
        wchar_t name[MAX_PATH]{};
        if (SUCCEEDED(view->Item(i, &child)) && SUCCEEDED(folder->GetDisplayNameOf(child, SHGDN_INFOLDER | SHGDN_FORPARSING, &ret)) &&
            SUCCEEDED(StrRetToBufW(&ret, child, name, MAX_PATH)))
        {
            names += name;
            names += L'\0';
        }
        CoTaskMemFree(child);
    }
    if (names.empty())
        return;
    const uint32_t guess = ff_layout_classify(reinterpret_cast<const uint16_t*>(names.c_str()), names.size());
    const int layout = guess == 2 ? static_cast<int>(ViewLayout::LargeIcons) : guess == 1 ? static_cast<int>(ViewLayout::Details) : -1;
    if (layout >= 0 && layout != static_cast<int>(GetLayout()))
        SetLayout(static_cast<ViewLayout>(layout));
    if (layout >= 0)
        m_layoutShown = layout;
}

IFACEMETHODIMP Tab::OnViewCreated(IShellView* view)
{
    // The folder's layout from DecideLayout, set before the view has shown anything
    ComPtr<IFolderView2> folderView;
    if (m_applyLayout >= 0 && !m_results && view && SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&folderView))))
    {
        static const struct { FOLDERVIEWMODE mode; int size; } map[] = {
            { FVM_DETAILS, 16 }, { FVM_LIST, 16 }, { FVM_TILE, 48 }, { FVM_CONTENT, 32 },
            { FVM_SMALLICON, 16 }, { FVM_ICON, 48 }, { FVM_ICON, 96 }, { FVM_ICON, 256 } };
        const auto& m = map[ShellLayoutIndex(m_applyLayout)];
        folderView->SetViewModeAndIconSize(m.mode, m.size);
    }
    // Palette, list theme and our subclass (background erase) before the view first paints: no white frame in dark
    if (view)
    {
        HookViewWindow(view);
        ApplyThemeTo(view, m_darkApplied);
    }
    return S_OK;
}

void Tab::HookViewWindow(IShellView* view)
{
    HWND hwnd = nullptr;
    if (!view || FAILED(view->GetWindow(&hwnd)) || !hwnd)
        return;
    wchar_t cls[32]{};
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    DWORD_PTR existing = 0;
    // A new view window comes with every folder; the subclass goes away with its window
    if (wcscmp(cls, L"SHELLDLL_DefView") == 0 && !GetWindowSubclass(hwnd, ViewProc, kSubclassId, &existing))
        SetWindowSubclass(hwnd, ViewProc, kSubclassId, reinterpret_cast<DWORD_PTR>(this));
}
