#pragma once
#include "HomeView.h"
#include "SettingsView.h"
#include "FileSearch.h"
#include "ColumnView.h"
#include "FileList.h"

class Tab;

// The folder an item opens as: a folder, or a shortcut's stored target when that's a folder (zip files are files)
bool FolderTarget(IShellItem* item, UniquePidl& target);

// The file view's right-click menu while it's being built: the shell's own items (ids kShellFirst..kShellLast) plus
// whatever the window adds (ids from kAppFirst)
struct TabViewMenu
{
    static constexpr UINT kShellFirst = 1, kShellLast = 0x6FFF, kAppFirst = 0x7000, kAppLast = 0x7FFF;
    HMENU menu = nullptr;
    bool background = false; // empty area of the view (the folder itself), not the selection
    IContextMenu* shell = nullptr;
    bool ownList = false;    // FastFiles' file list: no DefView items (Files' 布局 / 排序方式 / 刷新 come from the window)
    int VerbIndex(const wchar_t* verb) const; // position of the shell item with this canonical verb, -1 if none
};

enum class ViewLayout; // (below)

struct TabEvents
{
    virtual void OnTabNavigated(Tab* tab) = 0;
    virtual void OnTabNavigationFailed(Tab* tab) = 0;
    // From the tab's home page: open somewhere other than this tab (new tab/window, other pane). activate: a new tab
    // becomes the current one whatever 总是切换到新创建的标签页 says (Files' OpenFoldersInNewTab)
    virtual void OnTabOpen(Tab* tab, PCIDLIST_ABSOLUTE pidl, OpenMode mode, bool activate = false) = 0;
    virtual void OnTabHomeAction(Tab* tab, HomeAction action, const std::wstring& key, PCIDLIST_ABSOLUTE pidl, int section) = 0;
    // Right-click menu of the file view: add items (ids from TabViewMenu::kAppFirst), then run the one picked
    // (0 = the menu closed without picking one of ours; also the place to free anything the menu held)
    virtual void OnTabViewMenu(Tab*, TabViewMenu&) {}
    virtual void OnTabViewCommand(Tab*, UINT) {}
    // A shell verb picked in that menu ("paste", "delete"): true when the window ran it itself (status center)
    virtual bool OnTabVerb(Tab*, const wchar_t*, bool /*background*/) { return false; }
    // The own list's 设为默认 (Files' SetDetailsColumnsAsDefault): these columns collapsed in every folder from now on
    virtual void OnTabColumnsDefault(Tab*, unsigned /*hidden*/) {}
    // 增加大小 / 减小大小 changed a layout's size (Files' LayoutSettingsService.*ViewSize: every folder's, saved)
    virtual void OnTabLayoutSize(Tab*, int /*mode: FileList::Mode*/, int /*size*/) {}
    // LayoutCycler moved on to another layout (picked like the menu's: the folder's choice, or everyone's when synced)
    virtual void OnTabCycleLayout(Tab*, ViewLayout) {}
    // Sort / grouping changed while every folder shares one layout (Files writes LayoutSettingsService's defaults then)
    virtual void OnTabSortDefault(Tab*, const ListSort&) {}
    // The own list started / finished listing (opening a folder, 刷新): Files' tab LoadingStates (a ProgressRing icon)
    virtual void OnTabLoading(Tab*, bool /*loading*/) {}
};

// Tag and Created: FastFiles' own file list only (Files' SortOption.FileTag / DateCreated)
enum class SortKey { Name, Modified, Type, Size, Tag, Created };
// The first eight are the shell view's own; Columns is FastFiles' column view; Adaptive (only a default or a
// per-folder choice) picks details or large icons from what the folder holds (Files' Adaptive layout).
// Files' layouts are Details, List, Tiles (卡片) and LargeIcons (网格) with Columns; the other shell ones (stored by
// older versions) show as the closest of those: Content as cards, the other icon sizes as the grid.
enum class ViewLayout { Details, List, Tiles, Content, SmallIcons, MediumIcons, LargeIcons, ExtraLargeIcons, Columns, Adaptive };
// A layout index for the shell view: Columns and Adaptive start out as details
inline int ShellLayoutIndex(int layout) { return layout >= 0 && layout <= 7 ? layout : 0; }
// Files' layout list in its order (详细信息, 列表, 卡片, 网格, 列视图: Ctrl+Shift+1..5)
inline constexpr ViewLayout kFilesLayouts[] = { ViewLayout::Details, ViewLayout::List, ViewLayout::Tiles, ViewLayout::LargeIcons,
    ViewLayout::Columns };
// Files' layout for a shell layout index (Details, List, Tiles = cards, LargeIcons = grid)
inline ViewLayout FilesLayout(int layout)
{
    switch (ShellLayoutIndex(layout))
    {
    case 1: return ViewLayout::List;
    case 2: case 3: return ViewLayout::Tiles;
    case 4: case 5: case 6: case 7: return ViewLayout::LargeIcons;
    default: return ViewLayout::Details;
    }
}

// Window-wide view preferences every tab reads (owned by MainWindow)
struct ViewPrefs
{
    bool checkboxes = false;
    const Theme* theme = nullptr;
    SettingsHost* settings = nullptr;     // builds the settings pages a Settings tab shows
    std::shared_ptr<const HomeData> home; // what every home page shows
    unsigned homeCollapsed = 0;           // HomeView::Section bits
    unsigned homeHidden = 0;              // HomeView::Section bits turned off in settings
    int defaultLayout = 9;                // ViewLayout for folders that have no view of their own yet (Files: Adaptive)
    bool syncLayout = false;              // every folder uses defaultLayout
    // Files' own hidden-item settings (FoldersSettingsService), applied without touching the system's
    bool showHidden = false, showProtected = false;
    bool ownHiddenFilter = false;         // they differ from the system's: views list everything and we filter
    bool columnLines = false;             // vertical lines between details columns
    // Files' FoldersSettingsService: ShowDotFiles, OpenFoldersInNewTab, DoubleClickToGoUp, ScrollToPreviousFolderWhenNavigatingUp
    bool showDotFiles = true, openFoldersNewTab = false, doubleClickUp = true, selectPrevious = true, hoverSelect = false;
    // Files' 排序优先级 (DefaultSortDirectoriesAlongsideFiles / DefaultSortFilesFirst): 0 folders first, 1 files first,
    // 2 together. FastFiles' own file list only (the shell's view always puts folders first).
    int sortPriority = 0;
    // Files' DefaultSortOption / DefaultDirectorySortDirection / DefaultGroupOption / DefaultDirectoryGroupDirection /
    // DefaultGroupByDateUnit: a folder without its own choice (every folder when syncLayout), own lists only
    ListSort defaultSort;
    // Files' Show*Column (LayoutSettingsService): the own list's collapsed columns, FileList column bits (创建日期 off)
    unsigned hiddenColumns = 1u << 4;
    // Files' DetailsViewSize / ListViewSize / CardsViewSize / GridViewSize / ColumnsViewSize (1-based), by FileList::Mode
    int layoutSize[5] = { 2, 2, 1, 8, 2 };
    // Where the column view's first column starts when a folder is inside one: pinned and cloud folders (Files'
    // ColumnsLayoutPage: pinned folders, cloud drives, libraries, else the drive root); file system paths
    std::vector<std::wstring> columnRoots;
};

// What the current selection contains, for context buttons in the command bar
// (Files' FileExtensionHelpers: wallpaper-compatible images, executables, fonts, batch / ahk, .ps1, media, .inf, certificates)
enum SelectionKind : unsigned { SelImage = 1, SelRunnable = 2, SelFont = 4, SelFolder = 8, SelArchive = 16, SelZip = 32,
    SelBatch = 64, SelPowerShell = 128, SelMedia = 256, SelInf = 512, SelCertificate = 1024, SelOther = 2048,
    SelShareable = 4096 }; // Files' ShareItemHelpers.IsItemShareable: not hidden, not a .lnk, not a folder

// One tab (or pane) = one IExplorerBrowser (the same view Explorer itself uses), created lazily on first show.
// The Home location shows FastFiles' own HomeView instead; history treats it like any other folder.
class Tab final : public IExplorerBrowserEvents, public IFolderFilter, public HomeEvents, public IServiceProvider, public ICommDlgBrowser2,
                  public ColumnViewEvents, public FileListEvents
{
public:
    Tab(HWND parent, TabEvents* events, PCIDLIST_ABSOLUTE location, const ViewPrefs* prefs);

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // The browser's site: the view asks it (SID_SExplorerBrowserFrame) for ICommDlgBrowser
    IFACEMETHODIMP QueryService(REFGUID service, REFIID riid, void** ppv) override;
    // Opening a folder (Enter, double click, the "open" verb) navigates this tab itself, so it stays in place even
    // when FastFiles is the default file manager (the folder's open verb would start FastFiles.exe again)
    IFACEMETHODIMP OnDefaultCommand(IShellView* view) override;
    IFACEMETHODIMP OnStateChange(IShellView*, ULONG) override { return E_NOTIMPL; }
    IFACEMETHODIMP IncludeObject(IShellView* view, PCUITEMID_CHILD child) override;
    // ICommDlgBrowser2: GetViewFlags lets the view list hidden items whatever the system setting says
    IFACEMETHODIMP Notify(IShellView*, DWORD) override { return S_OK; }
    IFACEMETHODIMP GetDefaultMenuText(IShellView*, LPWSTR, int) override { return S_FALSE; }
    IFACEMETHODIMP GetViewFlags(DWORD* flags) override;

    IFACEMETHODIMP OnNavigationPending(PCIDLIST_ABSOLUTE) override { return S_OK; }
    IFACEMETHODIMP OnViewCreated(IShellView* view) override; // the adaptive layout, before the view first paints
    IFACEMETHODIMP OnNavigationComplete(PCIDLIST_ABSOLUTE pidl) override;
    IFACEMETHODIMP OnNavigationFailed(PCIDLIST_ABSOLUTE) override;

    // IFolderFilter: name filter, applied while the view enumerates
    IFACEMETHODIMP ShouldShow(IShellFolder* folder, PCIDLIST_ABSOLUTE parent, PCUITEMID_CHILD child) override;
    IFACEMETHODIMP GetEnumFlags(IShellFolder*, PCIDLIST_ABSOLUTE, HWND*, DWORD* flags) override;

    // HomeEvents (this tab's home page)
    void OnHomeOpen(PCIDLIST_ABSOLUTE pidl, OpenMode mode) override;
    void OnHomeReveal(PCIDLIST_ABSOLUTE item) override;
    void OnHomeAction(HomeAction action, const std::wstring& key, PCIDLIST_ABSOLUTE pidl, int section) override;

    // FastFiles' own page is up instead of a folder view: Home, or Settings (Files' SettingsPage in a tab)
    bool OnHome() const { return m_onHome; }
    bool OnSettings() const { return m_onHome && IsSettingsPidl(m_current.get()); }
    SettingsView* Settings() const { return OnSettings() ? m_settings.get() : nullptr; }
    void ShowSettingsPage(int page); // navigate this tab to Settings, on that page
    void RebuildSettings();          // setting values changed
    void ApplyHome();   // re-read ViewPrefs::home / homeCollapsed
    void RelayoutHome(); // DPI / font / theme change (the settings page too)
    HWND Host() const { return m_host; }
    bool Contains(HWND hwnd) const { return m_host && hwnd && (hwnd == m_host || IsChild(m_host, hwnd)); }

    void Show(const RECT& rc);
    void Hide();
    void Move(const RECT& rc);
    void Close();
    void Focus();
    bool HasFocus() const;
    bool TranslateAccelerator(MSG* msg);

    void Navigate(PCIDLIST_ABSOLUTE pidl);
    void Back();
    void Forward();
    void Up();
    void Refresh(bool user = false); // user: the 刷新 command (F5, menus): the items are seen to go and come back
    void Reload(); // a new view of the same folder (view flags such as the hidden-item filter changed)

    bool CanBack() const { return !m_back.empty() || m_results; } // (column moves aren't history, as in Files)
    bool CanForward() const { return !m_forward.empty(); }
    // Newest first; GoHistory(back, 1) equals Back()
    std::vector<PCIDLIST_ABSOLUTE> History(bool back) const;
    void GoHistory(bool back, int steps); // a column view entry opens as the column view again
    PCIDLIST_ABSOLUTE Location() const { return m_current.get(); }
    bool GetCounts(int& items, int& selected) const;

    // Commands on the current view (TabCommands.cpp)
    int SelectedCount() const;
    int FocusedIndex() const;
    unsigned SelectionStamp() const; // changes with every selection change of the own list (0 for the shell's view)
    ComPtr<IShellItemArray> Selection() const; // the own list's or the shell view's; null when nothing is selected
    bool InvokeVerb(const char* verb, bool background);
    bool CreateNewItem(bool folder);
    bool Rename();
    std::wstring SelectionPaths() const;
    bool SelectionSize(ULONGLONG& bytes) const;
    void SelectAll();
    void InvertSelection();
    void ClearSelection();
    void SetLayout(ViewLayout layout);  // Files' layouts (0-7: see FilesLayout); the own list's mode, or the shell view's
    ViewLayout GetLayout() const;       // ... as shown (Files' layout: Details, List, Tiles, LargeIcons)
    // A layout picked by the user (menu, Ctrl+Shift+1-6), remembered for this folder like Files' layout preferences:
    // Columns opens the column view, Adaptive turns the adaptive layout on, the rest turn it off
    void ChooseLayout(ViewLayout layout);
    ViewLayout DisplayedLayout() const; // GetLayout, or Columns in the column view
    bool AdaptiveOn() const;            // Files' LayoutAdaptive.IsOn for this folder
    bool CanColumns() const;            // a file system folder (Files: not the recycle bin)
    bool InColumns() const { return m_columns && !m_results; }
    // ColumnViewEvents
    void OnColumnActive(PCIDLIST_ABSOLUTE folder) override;
    void OnColumnCantShow(PCIDLIST_ABSOLUTE folder) override;
    const ViewPrefs* ColumnPrefs() const override { return m_prefs; }
    FileListEvents* ColumnListEvents() override { return this; }
    // FastFiles' own file list (TabList.cpp): file system folders in the details layout
    static bool OwnList(); // in use at all (FASTFILES_OWNLIST=1 while it's being finished)
    bool InList() const { return m_list && m_listShown && !m_results && !m_onHome; }
    void SetListColumns(unsigned hidden) { if (m_list) m_list->SetHiddenColumns(hidden); }
    // The list the tab's commands go to: the own list, or the active column's in the column view
    FileList* List() const { return InList() ? m_list.get() : InColumns() ? m_columns->ActiveList() : nullptr; }
    // Every own list of the tab (the folder's and each column's), shown or not
    template <class F> void ForEachList(F f)
    {
        if (m_list)
            f(m_list.get());
        if (m_columns)
            for (int i = 0; i < m_columns->Count(); ++i)
                f(m_columns->ListAt(i));
    }
    void ColumnsResized() { if (m_columns) m_columns->Resized(); } // ColumnsViewSize changed
    const ViewPrefs* ListPrefs() const override { return m_prefs; }
    void OnListOpen(PCIDLIST_ABSOLUTE folder) override;
    void OnListUp() override;
    void OnListMenu(HWND list, POINT screen, bool background) override;
    void OnListLoaded() override {}
    void OnListLoading(bool loading) override { m_events->OnTabLoading(this, loading); }
    void OnListColumnsDefault(unsigned hidden) override { m_events->OnTabColumnsDefault(this, hidden); }
    void OnListZoom(int direction) override { StepIconSize(direction); }
    void OnListSorted(FileList* list) override { StoreSort(list); }
    // Files' LayoutPreferencesManager for sort / grouping (TabColumns.cpp): the folder's stored choice (layouts.txt)
    // or the defaults; a choice made in a list is stored for its folder (or as everyone's default when synced)
    ListSort SortFor(PCIDLIST_ABSOLUTE folder) const;
    void StoreSort(FileList* list);
    ListSort ColumnSortFor(PCIDLIST_ABSOLUTE folder) const override { return SortFor(folder); }
    void ApplySortDefault(); // ViewPrefs::defaultSort changed while synced: every list here follows it
    int SortPriority() const; // 排序优先级 of the list the commands go to (ViewPrefs::sortPriority otherwise)
    void SetSortPriority(int priority);
    void SetSort(SortKey key, bool descending);
    bool GetSort(SortKey& key, bool& descending) const;
    // Files' 分组依据: GroupOption (FileList::kGroup*: none, name, modified, created, size, type, tag), GroupByDateUnit
    // (0 year, 1 month, 2 day) and its direction. The shell's view groups by name / dates / size / type (no units, no tags)
    struct GroupChoice { int key = 0, unit = 0; bool descending = false; };
    void SetGroup(const GroupChoice& choice);
    GroupChoice GetGroup() const;
    bool CanGroup(int key) const; // the view can group by it (the shell's: not by tag)
    bool CanSortBy(SortKey key) const;
    void SetFilter(const std::wstring& text);
    const std::wstring& Filter() const { return m_filter; }
    void ApplyPrefs(); // re-read ViewPrefs (checkboxes, same-layout-everywhere)
    void ApplyTheme(); // palette change: the shell view's colors and dark scroll bars
    unsigned SelectionKinds(unsigned* every = nullptr) const; // kinds any selected item has; every: kinds all of them have
    bool IsRecycleBin() const;
    PCIDLIST_ABSOLUTE SingleSelectedFolder(UniquePidl& holder) const;
    PCIDLIST_ABSOLUTE SingleSelectedItem(UniquePidl& holder) const;
    void SelectAfterNavigate(PCUITEMID_CHILD child);
    bool SelectPath(const std::wstring& path); // item in the current folder, retried until the view lists it
    std::vector<std::wstring> SelectedFileSystemPaths() const;
    // Every selected item with its path, pidl and display name; false (nothing filled) if one isn't a file system item
    bool SelectedFileSystemItems(std::vector<std::wstring>& paths, std::vector<UniquePidl>& pidls, std::vector<std::wstring>& names) const;
    std::vector<UniquePidl> SelectedFolders() const; // empty unless every selected item is a folder (zip files aren't)
    bool StepIconSize(int direction);
    ComPtr<IShellView> ShellView() const;

    // File-name search results in this tab (TabSearch.cpp)
    bool StartSearch(const std::wstring& query); // false: not a file system folder (use Windows Search)
    // Files carrying a tag; inside this folder when `here`, otherwise anywhere (works on Home too)
    bool StartTagSearch(const std::wstring& tagId, const std::wstring& tagName, bool here);
    bool InResults() const { return m_results; }
    const std::wstring& Query() const { return m_query; }
    const std::wstring& TagId() const { return m_tagId; } // non-empty while showing a tag's files
    void SetTagName(const std::wstring& name) { if (!m_tagId.empty()) m_query = name; }
    std::wstring SearchStatus() const;

private:
    enum class Pending { None, Back, Forward };
    // How the next navigation shows the folder: as its layout says, or as a history entry was (Files' back stack
    // remembers the page type, so Back returns to the column view)
    enum class ModeHint { None, Columns, Normal };
    struct HistoryEntry
    {
        UniquePidl pidl;
        bool columns = false;
    };
    // What a folder opens as (TabColumns.cpp, Files' LayoutPreferencesManager.GetLayoutType)
    struct LayoutDecision
    {
        bool columns = false;
        int layout = -1;            // shell layout set when its view is created (-1: the folder's remembered view)
        bool adaptiveLater = false; // network folder: decided once listed (Files doesn't sample those first)
        std::wstring key;           // layouts.txt key (empty when every folder shares one layout)
    };
    static constexpr UINT_PTR kRenameTimer = 1;
    static constexpr UINT_PTR kSelectTimer = 2;
    static constexpr UINT_PTR kThemeTimer = 3;
    static constexpr UINT_PTR kSearchTimer = 4;
    static constexpr UINT_PTR kAdaptiveTimer = 5;
    static constexpr UINT_PTR kSubclassId = 0xFF01; // our subclass of SHELLDLL_DefView (TabMenu.cpp)

    // Layout and column view (TabColumns.cpp)
    LayoutDecision DecideLayout(PCIDLIST_ABSOLUTE pidl, ModeHint hint);
    // layouts.txt: low byte 0 none, 1 columns, 2 adaptive, 3 manual (bits 8-15: its shell layout + 1)
    int LoadMode(const std::wstring& key, bool fresh) const;
    void StoreMode(const std::wstring& key, int mode);
    void OpenLocation(PCIDLIST_ABSOLUTE pidl, ModeHint hint); // the folder browser or the column view
    bool EnterColumns(PCIDLIST_ABSOLUTE pidl);
    bool AdjustColumns(PCIDLIST_ABSOLUTE pidl); // Files' SetSelectedPathOrNavigate: inside the columns already shown
    std::vector<UniquePidl> ColumnChain(const std::wstring& path) const;
    void ShowColumnsSurface();
    void DestroyColumns();
    // The own file list (TabList.cpp): for this folder and layout decision? Then shown in place of the shell's view
    bool WantsList(PCIDLIST_ABSOLUTE pidl, const LayoutDecision& d) const;
    bool EnterList(PCIDLIST_ABSOLUTE pidl, ViewLayout layout);
    bool SwitchToList(ViewLayout layout); // a layout picked by the user while the shell's view shows the folder
    static FileList::Mode ListMode(ViewLayout layout); // Files' layout -> the list's mode
    int ListLayout(const LayoutDecision& d) const;     // the decision's layout (or the default) as Files' layout
    void HideList(); // another surface took over (the shell's view, Home, the columns)
    void DropBrowser(); // the shell's browser goes (it would come back showing its old folder)
    void CheckLeave(); // the adaptive layout changed by hand (view menu, Ctrl+wheel): it's this folder's choice now
    void CheckNetworkAdaptive();
    IExplorerBrowser* ActiveBrowser() const; // the active column's browser in the column view
    ComPtr<IShellView> ViewForWindow(HWND viewWindow) const;
    void HookViewWindow(IShellView* view);
    void ApplyThemeTo(IShellView* view, bool& darkApplied);
    bool EnterResults();
    void AttachResultsFolder();
    void PumpSearch();
    void EndResults();

    // Right-click menu of the file view (TabMenu.cpp)
    void HookView();
    static LRESULT CALLBACK ViewProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    bool ShowViewMenu(HWND view, HWND from, POINT screen);

    // lParam: owned PIDLIST_ABSOLUTE to open here (posted by the home page); wParam 1: in a new tab instead (Files' OpenFoldersInNewTab)
    static constexpr UINT kMsgOpen = WM_APP + 50;
    static constexpr UINT kMsgLeaveResults = WM_APP + 51; // lParam: owned PIDLIST_ABSOLUTE opened from search results
    static constexpr UINT kMsgUp = WM_APP + 52; // a double click on the view's empty space (Files' DoubleClickToGoUp)

    static LRESULT CALLBACK HostProc(HWND, UINT, WPARAM, LPARAM);
    bool CreateHost(const RECT& rc);
    bool CreateBrowser();
    bool NewBrowser(ComPtr<IExplorerBrowser>& browser, DWORD& cookie);
    HWND BrowserWindow(IExplorerBrowser* browser) const;
    void Go(PCIDLIST_ABSOLUTE pidl, ModeHint hint = ModeHint::None); // every navigation: Home, the shell browser or the columns
    void EnterHome(PCIDLIST_ABSOLUTE pidl);
    void ShowHome(); // the page m_current is: the home page or the settings page
    void HideHome();
    HWND PageWindow() const; // the page shown (or last shown) by ShowHome
    void SetBrowserShown(bool shown);
    void Complete(PCIDLIST_ABSOLUTE pidl);
    ComPtr<IFolderView2> View() const;
    void TryPendingRename();
    void TryPendingSelect();
    void InstallFilter();
    void ApplyCheckboxes();
    void ApplyLayoutPref(bool afterNavigation);

    LONG m_ref = 1;
    HWND m_parent;
    HWND m_host = nullptr;
    TabEvents* m_events;
    ComPtr<IExplorerBrowser> m_browser;
    DWORD m_cookie = 0;
    Pending m_pending = Pending::None;
    UniquePidl m_current;     // in the column view: the active column's folder
    bool m_currentColumns = false; // m_current is shown in the column view (recorded with it in the history)
    std::vector<HistoryEntry> m_back;
    std::vector<HistoryEntry> m_forward;
    std::unique_ptr<ColumnView> m_columns;
    std::unique_ptr<FileList> m_list;
    bool m_listShown = false;        // the own list is this tab's folder view (search results may be over it)
    bool m_leavingColumns = false;   // the folder browser is loading; the columns stay up until it has
    bool m_resultsOverColumns = false;
    bool m_columnsDark = false;
    int m_applyLayout = -1;          // shell layout for the view being created (from DecideLayout)
    bool m_pendingTrack = false, m_pendingAdaptiveLater = false; // for the folder being opened
    std::wstring m_pendingKey;
    // The folder shown and the layout it opened with: a different layout when it's left (view menu, Ctrl+wheel)
    // is stored as the folder's own choice, like Files writes its layout preferences
    std::wstring m_layoutKey;
    int m_layoutShown = -1;
    int m_adaptiveTicks = 0, m_adaptiveCount = -1;
    mutable std::wstring m_modeKey;  // last layouts.txt lookup
    mutable int m_modeValue = 0;
    UniquePidl m_pendingRename; // child IDList of a just-created item waiting to appear in the view
    std::wstring m_pendingName;
    int m_renameTries = 0;
    int m_pendingSteps = 1;
    UniquePidl m_pendingSelect; // child of the new folder we came up from
    UniquePidl m_requestedSelect; // child to select once the next navigation completes
    int m_selectTries = 0;
    bool m_pendingUnselect = false; // m_pendingSelect is to be deselected instead (向上导航时滚动至上一目录 off)
    const ViewPrefs* m_prefs;
    std::wstring m_filter;
    std::wstring m_filterLower;
    bool m_filterInstalled = false;
    std::unique_ptr<HomeView> m_home;
    std::unique_ptr<SettingsView> m_settings;
    int m_settingsPage = 0; // page a Settings tab opens on
    bool m_onHome = false;  // m_home or m_settings is up (whichever m_current is)
    bool m_darkApplied = false; // the list currently has the dark theme classes
    FileSearch m_search;
    FileSearch::Progress m_searchFinal;
    ComPtr<IResultsFolder> m_resultsFolder;
    bool m_results = false; // m_browser is the search results browser (m_current is still the searched folder)
    ComPtr<IExplorerBrowser> m_mainBrowser; // the folder's own browser, hidden while results are shown
    DWORD m_mainCookie = 0;
    bool m_mainFilterInstalled = false;
    bool m_mainDarkApplied = false;
    std::wstring m_query;
    std::wstring m_tagId;           // tag results: the tag shown
    std::wstring m_tagRoot;         // ... and the folder they're limited to (empty: everywhere)
    bool m_resultsOverHome = false; // results were opened from the home page (no folder view underneath)
    size_t m_resultsAdded = 0;
    ComPtr<IContextMenu> m_viewMenu; // shell menu being tracked (owner-drawn submenus need its HandleMenuMsg)
};
