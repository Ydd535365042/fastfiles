#pragma once
#include "Theme.h"
#include "Backdrop.h"
#include "Tags.h"
#include "FileItems.h"
#include "SmoothScroll.h"
#include <atomic>
#include <unordered_map>

// Files' details layout (Views/Layouts/DetailsLayoutPage) drawn by FastFiles itself for file system folders: the
// columns Files shows by default (名称, 文件标签, 修改日期, 类型, 大小), Files' sort order (SortingHelper.OrderFileList, with
// its 排序优先级), Files' row and header metrics, and the file area card composed over the backdrop material / background
// image like the rest of the window. The items are a window of FastFiles' own ("FastFiles.FileItems": layout and
// selection in FileItems.h, Files' groups under headers) with a SysHeader32 over it for the details columns; it
// answers the list view messages the tests read (LVM_GETITEMCOUNT / ITEMTEXT / ITEMSTATE / ITEMRECT / HEADER) and
// IAccessible for screen readers. The folder is listed on a worker thread (FindFirstFileEx); the shell still
// supplies icons, type names, context menus, drag data and invoking.

struct ViewPrefs;
enum class SortKey;
class FileList;

// A folder's sort and grouping (Files' LayoutPreferencesItem: DirectorySortOption / Direction, DirectoryGroupOption /
// Direction / GroupByDateUnit, SortDirectoriesAlongsideFiles / SortFilesFirst)
struct ListSort
{
    int key = 0;          // SortKey
    bool descending = false;
    int group = 0;        // FileList::kGroup*
    int unit = 0;         // 0 year, 1 month, 2 day
    bool groupDescending = false;
    int priority = -1;    // 0 folders first, 1 files first, 2 together; -1: ViewPrefs::sortPriority
    bool operator==(const ListSort& o) const
    {
        return key == o.key && descending == o.descending && group == o.group && unit == o.unit && groupDescending == o.groupDescending &&
            priority == o.priority;
    }
    bool operator!=(const ListSort& o) const { return !(*this == o); }
};

struct FileListEvents
{
    virtual const ViewPrefs* ListPrefs() const = 0;
    virtual void OnListOpen(PCIDLIST_ABSOLUTE folder) = 0;        // a folder (or a shortcut to one) was opened
    virtual void OnListUp() = 0;                                  // double click on empty space (Files' DoubleClickToGoUp)
    virtual void OnListMenu(HWND list, POINT screen, bool background) = 0; // right click / Apps key
    virtual void OnListLoaded() = 0;                              // the folder's items are in (also after a refresh)
    virtual void OnListColumnsDefault(unsigned hidden) = 0;       // Files' 设为默认 in the header menu
    virtual void OnListZoom(int direction) = 0;                   // Ctrl+wheel (Files' LayoutPage_PointerWheelChanged)
    // The column view's (ColumnLayoutPage): the selection changed (the user's doing, or SelectFirst), a key before the
    // list handles it (true: handled), the items window got the keyboard
    virtual void OnListUserSelection() {}
    virtual bool OnListKey(WPARAM /*key*/) { return false; }
    virtual void OnListFocus() {}
    // The user sorted by a column header (Files keeps it as the folder's choice); also called by the owner itself
    virtual void OnListSorted(FileList* /*list*/) {}
};

class FileList
{
public:
    FileList(FileListEvents* events, const Theme* theme);
    ~FileList();
    bool Create(HWND parent);
    HWND Hwnd() const { return m_hwnd; }     // the container (child of the tab host)
    HWND List() const { return m_list; }     // the items window (focus, menus)
    void Move(const RECT& rc);

    // Lists `folder` (a file system folder); `select` is a name in it to select and focus once listed
    bool Open(PCIDLIST_ABSOLUTE folder, const std::wstring& select);
    void Reload(); // list again, keeping the selection, focus and scroll position
    PCIDLIST_ABSOLUTE Folder() const { return m_folder.get(); }
    const std::wstring& Path() const { return m_path; }
    bool Loading() const { return m_loading; }

    int Count() const { return static_cast<int>(m_order.size()); }
    int SelectedCount() const;
    int FocusedIndex() const;
    unsigned SelectionVersion() const { return m_sel.version; }
    // The selection as shell items / child IDs of Folder() (cached until it changes); nullptr when nothing is selected
    ComPtr<IShellItemArray> Selection();
    std::vector<std::wstring> SelectedPaths() const;
    IShellFolder* ShellFolder() const { return m_shellFolder.Get(); }
    // The shell's own menu object for the selection, or the folder background (null when that fails)
    ComPtr<IContextMenu> MenuFor(bool background);
    bool OpenSelection(); // Enter / double click: folders through the events, files by their default verb
    bool Invoke(const char* verb, bool background); // a shell verb on the selection / folder (test mode: safe ones only)

    void SelectAll();
    void InvertSelection();
    void ClearSelection();
    bool Select(const std::wstring& name); // selected, focused and scrolled to; false while it isn't listed (yet)
    // A just-created item (the change notification may not have come in yet): select it, or rename it, once listed
    void SelectWhenListed(const std::wstring& name);
    void RenameWhenListed(const std::wstring& name);
    bool BeginRename(); // the focused (or first selected) item

    void SetSort(SortKey key, bool descending);
    SortKey GetSortKey() const { return m_sortKey; }
    bool SortDescending() const { return m_sortDesc; }
    // Sort, grouping and 排序优先级 at once (a folder's stored choice, before or after it's listed)
    void SetSortState(const ListSort& state);
    ListSort SortState() const;
    int Priority() const; // the one in effect (its own, else ViewPrefs::sortPriority)
    void SetFilter(const std::wstring& lowerText); // the tab's name filter (already lowercased)
    // Files' 分组依据 (GroupOption None / Name / DateModified / DateCreated / Size / FileType / FileTag as GroupKey,
    // GroupByDateUnit Year / Month / Day, its own 升序 / 递减)
    // Files' layouts drawn here (FileListLayout.cpp): DetailsLayoutPage, and GridLayoutPage's list / cards / grid
    // templates; each at its size from ViewPrefs::layoutSize (Files' *ViewSizeKind)
    // Column: one column of the column view (ColumnLayoutPage: icon, name, the first tag's dot, a chevron on folders)
    enum class Mode { Details, List, Cards, Grid, Column };
    static constexpr int kModeCount = 5;
    void SetMode(Mode mode);
    Mode GetMode() const { return m_mode; }
    static int MaxSize(Mode mode); // Files' largest size kind of the layout (details / list / columns 5, cards 4, grid 12)
    // The column view's
    int NameWidth() const;               // the widest shown name (Files' BladeItem.SetWidth)
    void SelectFirst();                  // Files' MoveFocusToNextBlade: SelectedIndex = 0
    void Unselect();                     // nothing selected, not the user's doing (no OnListUserSelection)
    bool Renaming() const { return m_edit != nullptr; }
    // Files' PathTracedRowStyle: the row leading to the next column keeps the selected background (empty: none)
    void SetTraced(const std::wstring& name);
    void ApplyLayout();            // the size (or the mode) changed: metrics, thumbnails, repaint
    static constexpr int kGroupNone = 0, kGroupName = 1, kGroupModified = 2, kGroupCreated = 3, kGroupSize = 4, kGroupType = 5,
        kGroupTag = 6; // Files' GroupOption order (SyncStatus left out: no cloud sync status here)
    void SetGroup(int key, int unit, bool descending);
    int GroupKeyShown() const { return m_groupKey; }
    int GroupUnit() const { return m_groupUnit; }
    bool GroupDescending() const { return m_groupDesc; }
    // Files' column toggles (ColumnsViewModel.*.UserCollapsed): bit per column index; widths come back when shown again
    void SetHiddenColumns(unsigned hidden);
    unsigned HiddenColumns() const { return m_hiddenColumns; }
    // Settings names of the columns that can be collapsed ("tag", "modified", "created", "type", "size") <-> bits
    static unsigned ColumnBits(const std::vector<std::wstring>& names);
    static std::vector<std::wstring> ColumnNames(unsigned bits);
    void ApplyPrefs();  // hidden / dot-file / sort priority settings changed: re-filter and re-sort
    void ApplyTheme();  // palette, fonts or DPI changed

private:
    struct Item
    {
        std::wstring name;     // file system name
        std::wstring display;  // shown (shortcuts without .lnk / .url, extensions per the shell setting)
        std::wstring type;
        DWORD attrs = 0;
        ULONGLONG size = 0;
        FILETIME modified{}, created{};
        int icon = -1;
        bool link = false;     // shortcut overlay
        std::vector<std::wstring> tags;
        PITEMID_CHILD child = nullptr; // parsed on demand
        // Made by the thumbnail worker: a premultiplied 32bpp picture (its size), and the cards' contextual property
        std::shared_ptr<void> thumb;   // HBITMAP owner
        int thumbW = 0, thumbH = 0;
        bool thumbAsked = false, contextAsked = false;
        std::wstring context;
    };
    // A worker's result (posted to the container, which owns it then)
    struct Listing
    {
        unsigned generation = 0;
        bool ok = false; // the folder could be read
        std::vector<Item> items;
        std::vector<FileTag> tags;
    };
    // The selection and focus by name, kept across a re-list / re-sort (the scroll position stays: LVSICF_NOSCROLL)
    struct Kept
    {
        std::vector<std::wstring> selected; // lowercased
        std::wstring focused;
    };
    static constexpr UINT kMsgListed = WM_APP + 76; // to the container: lParam an owned Listing*
    static constexpr UINT kMsgIcons = WM_APP + 77;  // lParam an owned std::vector<std::pair<int, int>>* (item, icon), wParam generation
    static constexpr UINT kMsgChanged = WM_APP + 78; // SHChangeNotifyRegister
    static constexpr UINT kMsgRename = WM_APP + 79;  // the rename box lost focus: commit (posted, not inside the focus change)
    // Worker thread: lists `path` (FindFirstFileEx), type names and icons by extension, then the files with their own icons
    static void ListWorker(HWND to, std::wstring path, unsigned generation, std::shared_ptr<std::atomic<unsigned>> current,
        std::wstring folderType);
    // Files' DetailsLayoutPage columns: header text, width (96 DPI, the 12px grid splitter included), SortKey (-1: none)
    struct Column { const wchar_t* title; int width; int key; };
    enum : int { ColIcon, ColName, ColTag, ColModified, ColCreated, ColType, ColSize, kColumnCount };
    static const Column kColumns[kColumnCount];

    static LRESULT CALLBACK HostProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK ItemsProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK HeaderProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT OnHeaderNotify(NMHEADERW* nm);
    LRESULT ListMessage(UINT msg, WPARAM wp, LPARAM lp, bool& handled); // the LVM_ messages answered for tests

    // The items window (FileList.cpp): layout, scrolling, mouse and keyboard
    void Relayout(bool keepFocusVisible = false); // metrics, groups, scroll ranges; repaint
    void PlaceHeader();
    int ScrollX() const { return m_scroll.x; }
    int ScrollY() const { return m_scroll.y; }
    void ScrollTo(int x, int y);
    void EnsureVisible(int row);
    RECT RowRect(int row) const;           // client coordinates of the items window (empty if none)
    int RowAt(POINT client) const;         // -1 none
    void OnButtonDown(UINT msg, POINT pt, WPARAM keys);
    void OnButtonUp(POINT pt);
    void OnMouseMove(POINT pt, WPARAM keys);
    bool OnKey(WPARAM key);
    void OnChar(wchar_t c);
    void FocusRow(int row, bool select, bool extend, bool toggleCtrl);
    void SelectionChanged();               // repaint what changed, the header box, accessibility
    void UserSelected();                   // the user chose a selection: a pending SelectWhenListed is dropped
    ItemsMetrics Metrics(int clientWidth) const; // the layout's cell / row / header sizes
    std::wstring CellText(int row, int column) const;
    bool m_inLayout = false;
    // Screen readers (FileListAccessible.cpp): IAccessible for the items window, disconnected when the list goes
    class Accessible;
    Accessible* m_accessible = nullptr;
    LRESULT OnGetObject(WPARAM wp, LPARAM lp);
    void ReleaseAccessible();
    void UpdateMarquee(POINT client);
    ItemsLayout m_layout;
    ItemsSelection m_sel;
    POINT m_scroll{ 0, 0 };
    SmoothScroll m_smoothX, m_smoothY; // the wheel's animation (ScrollTo itself stays immediate: keys, drag, EnsureVisible)
    int m_downRow = -1;                    // row the left button went down on (selection on release / drag)
    POINT m_downAt{};
    WPARAM m_downKeys = 0;
    bool m_downSelected = false;           // the row was selected already when pressed (Files keeps the selection to drag it)
    bool m_marquee = false;                // a rubber band from empty space
    POINT m_marqueeFrom{};                 // content coordinates
    RECT m_marqueeRect{};                  // content coordinates
    std::vector<unsigned char> m_marqueeBase; // the selection before the band (Ctrl adds to it)
    std::wstring m_typed;                  // type-ahead
    ULONGLONG m_typedAt = 0;
    bool m_keyboardFocus = false;          // the focus frame shows after keyboard navigation (WinUI focus visuals)
    unsigned m_paintedSel = 0;             // m_sel.version last painted

    // FileListModel.cpp
    void StartListing();
    void TakeListing(Listing* listing);
    void TakeIcons(std::vector<std::pair<int, int>>* icons, unsigned generation);
    void Arrange();   // filter + sort m_items into m_order (Files' OrderFileList)
    Kept Keep() const;
    void Restore(const Kept& kept);
    void Rearrange(); // Arrange with the selection kept, then repaint
    void ApplyPending(); // select / rename what was waiting for this listing
    bool Visible(const Item& item) const;
    // FileListGroup.cpp: Files' GroupingHelper / OrderGroups over m_order (sorted already)
    struct GroupInfo
    {
        std::wstring title, subtitle;
        bool countBelow = false; // ShowCountTextBelow (tags): the count under the title, the subtitle beside it
    };
    void Group();
    int m_groupKey = 0, m_groupUnit = 0;
    bool m_groupDesc = false;
    int m_priority = -1; // ListSort::priority
    std::vector<ItemsGroup> m_groups;      // empty: not grouped
    std::vector<GroupInfo> m_groupInfo;
    PCUITEMID_CHILD ChildOf(int item);
    int RowOf(const std::wstring& name) const; // shown row, -1 if not shown
    void Watch();
    void Unwatch();
    void FreeItems();

    // FileListLayout.cpp: the layouts' sizes and cells (Files' LayoutSizeKindHelper, GridLayoutPage templates)
    Mode m_mode = Mode::Details;
    int Size() const;              // the current layout's size kind
    int IconSize() const;          // Files' LayoutSizeKindHelper.GetIconSize (96 DPI)
    int RowHeight() const;         // details / list rows (96 DPI)
    void LayoutMetrics(ItemsMetrics& m) const; // the mode's cells, spacing and padding (Metrics: the rest)
    void DrawListCell(HDC hdc, int row, const RECT& rc);
    void DrawCard(HDC hdc, int row, const RECT& rc);
    void DrawGridCell(HDC hdc, int row, const RECT& rc);
    void DrawColumnRow(HDC hdc, int row, const RECT& rc);
    std::wstring m_traced;         // SetTraced (lowercased)
    bool m_userSel = false;        // UserSelected came first: the next selection change is the user's
    void DrawCellHeader(HDC hdc, int group, const RECT& rc); // GridView's group header (the divider beside the text)
    void DrawFocusFrame(HDC hdc);  // list / cards / grid: the focus visual outside the focused cell
    // The item's picture centered in `box`: its thumbnail / icon at the layout's size, else the system's icon
    void DrawPicture(HDC hdc, int row, const RECT& box, bool dim);
    void DrawCheckFor(HDC hdc, int row, const RECT& hit, COLORREF under); // the item's SelectionCheckbox if it shows
    bool CheckShown(int row) const;
    RECT NameRect(int row) const;  // where the rename box goes (client coordinates)
    std::wstring ContextText(const Item& item) const; // Files' ContextualProperty (cards)
    // GridViewItem's backgrounds (LocalRegularItemContainerStyle: none, App.Theme.CardBackgroundFillColorTertiary hovered /
    // pressed, CardBackgroundFillColorDefault selected, SubtleFillColorSecondary selected and pressed)
    enum Back { BackNone, BackHover, BackSelected, BackSelectedPressed, kBackCount };
    Back CellBack(int row) const;
    // Keys composed over the material: per background the card face, its picture box and its stroke; the cell fill alone
    COLORREF m_cardFace[kBackCount]{}, m_cardBox[kBackCount]{}, m_cardStroke[kBackCount]{}, m_cellFill[kBackCount]{};
    void BuildCellLayers();        // (BuildLayers, after the rows' keys)
    HIMAGELIST m_iconLists[4]{};   // the system's 16 / 32 / 48 / 256 icons (SHGetImageList; not ours to free)
    HFONT m_strongFont = nullptr;  // BodyStrongTextBlockStyle: 14px SemiBold (the cards' names)
    bool WantsThumbs() const;      // pictures from the worker (larger than the system's small icons, or cards' details)
    std::vector<int> m_wantThumbs; // rows painted without theirs yet (RequestThumbs after the paint)

    // FileListThumbs.cpp: Files' thumbnails (IShellItemImageFactory, ReturnIconOnly below 48 or without 显示缩略图),
    // made on a worker for the rows on screen, newest request first
    struct ThumbQueue;
    struct ThumbResult;
    std::shared_ptr<ThumbQueue> m_thumbs;
    unsigned m_thumbGen = 0;       // bumped by ResetThumbs: results made before are dropped
    static constexpr UINT kMsgThumb = WM_APP + 80; // to the container: wParam generation, lParam an owned ThumbResult*
    static void ThumbWorker(std::shared_ptr<ThumbQueue> queue);
    void RequestThumbs();          // the rows painted without a picture (m_wantThumbs), newest first
    void ResetThumbs();            // a new size / layout / setting: drop the made ones
    void StopThumbs();             // the list goes: the worker ends after the picture it's making, posting nothing more
    bool m_iconsOnly = false;      // 显示缩略图 off (SSF_ICONSONLY) when the pictures were asked for
    void TakeThumb(LPARAM result, unsigned generation);
    static void DropThumb(LPARAM result); // one that came in after the list went

    // FileListPaint.cpp
    void Paint(HWND hwnd, bool header);
    void DrawItems(HDC hdc, const RECT& clip);
    void DrawGroupHeader(HDC hdc, int group, const RECT& rc);
    void DrawRow(HDC hdc, int row, const RECT& rc);
    void DrawHeader(HDC hdc, const RECT& rc);
    void DrawEmpty(HDC hdc, const RECT& client);
    void BuildLayers();
    void UpdateFonts();
    void CacheColumns(); // header item edges (header coordinates = row coordinates from the row's left), once per paint
    int m_colLeft[kColumnCount]{}, m_colRight[kColumnCount]{};
    int m_columnDpi = 96; // the DPI the column widths are at

    // FileListInput.cpp
    void SetupColumns();
    void OnColumnClick(int column);
    void SetHot(int row);
    void InvalidateRow(int row);
    void CommitRename(bool apply);
    std::vector<int> SelectedRows() const;
    void DragOut();
    // Files' header flyout: 自动调整所有列宽, a toggle per column, 设为默认
    void ShowHeaderMenu(POINT screen);
    void SizeColumnToFit(int column); // Files' ResizeColumnToFit (also the divider's double click)
    void ApplyColumnWidths();
    unsigned m_hiddenColumns = 1u << ColCreated;
    bool m_settingWidths = false;     // ApplyColumnWidths is setting them (not the user's drag: no limits)
    int m_width96[kColumnCount]{};    // each column's width at 96 DPI while shown (kept while hidden)

    FileListEvents* m_events;
    const Theme* m_theme;
    HWND m_hwnd = nullptr, m_list = nullptr, m_header = nullptr, m_edit = nullptr;
    HIMAGELIST m_rowImages = nullptr;   // 1px wide: sets the row height
    HIMAGELIST m_icons = nullptr;       // the system's small icons (not ours to free)
    int m_linkOverlay = -1;
    HFONT m_headerFont = nullptr, m_glyphFont = nullptr, m_groupFont = nullptr;
    HBRUSH m_editBrush = nullptr;
    UINT m_fontDpi = 0;
    // The card (content), hovered / selected rows and pressed rows: keys swapped for the material when it's composed
    std::vector<Backdrop::Layer> m_layers;
    COLORREF m_hoverKey = 0, m_pressedKey = 0;
    POINT m_scrolled{ -1, -1 }; // scroll position of the last paint (a scroll moved composed pixels)

    UniquePidl m_folder;
    std::wstring m_path;
    ComPtr<IShellFolder> m_shellFolder;
    std::vector<Item> m_items;
    std::vector<int> m_order;           // shown rows -> m_items
    std::vector<FileTag> m_tags;        // tags.txt when listed (names and colors of the pills)
    std::shared_ptr<std::atomic<unsigned>> m_generation = std::make_shared<std::atomic<unsigned>>(0);
    bool m_loading = false;
    bool m_listed = false;              // a listing has come in (empty text only then)
    std::wstring m_pendingSelect, m_pendingRename;
    int m_pendingTries = 0; // listings a just-created item may still be missing from
    std::wstring m_filter;
    SortKey m_sortKey;
    bool m_sortDesc = false;
    ULONG m_notify = 0;
    ComPtr<IShellItemArray> m_selection; // cached Selection()
    bool m_selectionDirty = true;
    int m_hot = -1, m_pressed = -1, m_headerHot = -1;
    std::wstring m_renameFrom;          // file system name being renamed

    // Files' SelectionCheckbox over the icon (shown on selected rows with 项目复选框 on, and always under the mouse; a click
    // adds / removes just that row) and the header's SelectAllCheckbox (checked: all, mixed: some; a click selects all
    // from none, else clears). FileListInput.cpp / FileListPaint.cpp
    enum class Check { Off, On, Mixed };
    RECT CheckRect(const RECT& row) const;  // the row's box hit area (20 wide, the row's height less its margins)
    int CheckAt(POINT client) const;        // row whose box is at the point, -1 if none
    RECT HeaderCheckRect() const;           // header coordinates
    Check HeaderCheckState() const;
    void DrawCheck(HDC hdc, const RECT& box, Check state, bool hot, bool pressed, COLORREF under);
    void SetHotCheck(int row);
    void ToggleRow(int row);
    void UpdateHeaderCheck();               // repaint the header box when the selection changed what it shows
    int m_hotCheck = -1, m_pressedCheck = -1; // row whose box is under the mouse / held down
    bool m_headerCheckHot = false, m_headerCheckPressed = false;
    Check m_headerCheck = Check::Off;       // as last painted

    // Dropping into the list (FileListDrop.cpp, Files' BaseLayoutPage Item_DragOver / ItemsLayout_DragOver): on a folder
    // row into that folder (hover 1.3 s opens it), anywhere else into the folder shown; the shell's own drop targets
    // decide the effect, copies / moves go through the window's status center (RouteFileDrop)
    struct DropTarget final : IDropTarget
    {
        FileList* owner = nullptr;
        IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
        IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }  // lives as long as the list
        IFACEMETHODIMP_(ULONG) Release() override { return 1; }
        IFACEMETHODIMP DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override;
        IFACEMETHODIMP DragOver(DWORD keys, POINTL pt, DWORD* effect) override;
        IFACEMETHODIMP DragLeave() override;
        IFACEMETHODIMP Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override;
    };
    void UpdateDrop(POINTL screen, DWORD keys, DWORD* effect);
    void ClearDrop();
    void SetDropRow(int row);
    void OpenDropRow();
    static constexpr UINT_PTR kDragOpenTimer = 3; // on the container (1 reload, 2 hover select)
    DropTarget m_dropTarget;
    ComPtr<IDropTargetHelper> m_dropHelper;
    ComPtr<IDataObject> m_dragData;
    std::vector<std::wstring> m_dragPaths; // what's dragged (a folder can't go into itself)
    ComPtr<IDropTarget> m_target;          // the folder's own drop target (a row's, or the folder shown)
    int m_targetRow = -2;                  // row m_target belongs to (-1: the folder shown, -2: none)
    UniquePidl m_targetPidl;
    DWORD m_dragKeys = 0;                  // DragEnter / DragOver keys (Drop's have lost the mouse button)
    int m_dropRow = -1;                    // folder row drawn as the drop target
};
