#pragma once
#include "Theme.h"
#include "SmoothScroll.h"
#include <functional>
#include <unordered_map>

struct SidebarEvents
{
    virtual void OnSidebarOpen(PCIDLIST_ABSOLUTE pidl, OpenMode mode) = 0;
    virtual void OnSidebarNotice(const std::wstring& text) = 0;
    virtual void OnSidebarChanged() {} // pins or devices changed (the home page mirrors them)
    virtual void OnSidebarTag(const std::wstring&, const std::wstring&, OpenMode) {} // id, name: show the tag's files
};

// Files-style sectioned sidebar (Controls/Sidebar in Files): Home, Pinned, Libraries, Drives, Cloud, Network, WSL, Tags.
// Custom drawn; pins live in the Rust core (pins.txt); drives/cloud/WSL are scanned on a worker thread.
class Sidebar final : public IDropTarget
{
public:
    enum Section : int { SecHome, SecPinned, SecLibraries, SecDrives, SecCloud, SecNetwork, SecWsl, SecTags, SecCount };

    explicit Sidebar(SidebarEvents* events) : m_events(events) {}

    bool Create(HWND parent, int id, const Theme* theme);
    void Destroy();
    void Move(const RECT& rc);
    void Relayout(); // after a theme / DPI change
    HWND Hwnd() const { return m_hwnd; }

    void SetLocation(PCIDLIST_ABSOLUTE pidl);
    unsigned HiddenSections() const { return m_hidden; }
    unsigned CollapsedSections() const { return m_collapsed; }
    void SetSectionState(unsigned hidden, unsigned collapsed);

    bool IsPinned(PCIDLIST_ABSOLUTE pidl) const;
    bool Pin(PCIDLIST_ABSOLUTE pidl, int index = -1);
    bool Unpin(PCIDLIST_ABSOLUTE pidl);
    bool UnpinKey(const std::wstring& key);

    void LoadTags();                            // tags.txt changed (here or in another window)
    void SetTagLocation(const std::wstring& id); // the active tab shows this tag's files (highlights it)

    // IDropTarget: drop folders on the Pinned section to pin, anything else on an item to move/copy into it
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;
    IFACEMETHODIMP DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override;
    IFACEMETHODIMP DragOver(DWORD keys, POINTL pt, DWORD* effect) override;
    IFACEMETHODIMP DragLeave() override;
    IFACEMETHODIMP Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override;

    struct Entry
    {
        Section section = SecHome;
        UniquePidl pidl;       // null until resolved for lazy entries (UNC pins, WSL)
        std::wstring name;
        std::wstring parsing;  // SIGDN_DESKTOPABSOLUTEPARSING; also the pin key
        std::wstring lazyPath; // parsed on click, off the UI thread (may wake a share or the WSL VM)
        int icon = -1;
        bool missing = false;  // pinned folder that no longer resolves
        ULONGLONG freeBytes = 0, totalBytes = 0;
        std::wstring tagId;    // SecTags entries (parsing is "tag:<id>")
        COLORREF tagColor = 0;
        // Files' tree view (ApplyTreeViewExpandabilityAsync): rooted file-system items outside Pinned get a chevron
        std::wstring treePath; // folder to enumerate when expanded; empty = not expandable
        bool hasChildren = false;
        bool removable = false; // Files' eject decorator
        bool hidden = false;    // tree child with the hidden attribute (drawn dimmed)
    };
    const std::vector<Entry>& Entries() const { return m_entries; }
    void ReloadTrees(); // hidden-items settings changed: re-enumerate expanded folders
    void SetHiddenFilter(bool showHidden, bool showProtected, bool showDots); // FastFiles' settings (reloads the tree on change)
    struct ChildrenPayload; // worker -> UI: one folder's subfolders (or their real icons)

private:
    // A realized tree-view folder (Files' ExpandableSidebarItemBase): expansion state plus lazily loaded subfolders
    struct Node
    {
        std::wstring key, path; // lowercase / real folder path
        Entry entry;            // children only; roots use their m_entries item
        bool expanded = false;
        bool loaded = false, loading = false;
        std::vector<std::unique_ptr<Node>> children;
    };
    struct Row
    {
        bool header = false;
        Section section = SecHome;
        int entry = -1; // index into m_entries, -1 for headers, tree children and the empty-Pinned hint
        Node* child = nullptr; // tree-view subfolder row
        int depth = 0;  // Files' NestingLevel: 0 headers, 1 section items, 2+ subfolders
        int top = 0, height = 0; // content coordinates
    };
    enum class DropMode { None, Pin, Item, Tag };
    struct DevicePayload;  // worker -> UI: scanned device entries
    struct ResolvePayload; // worker -> UI: a lazy path parsed into a PIDL
    static constexpr UINT kMsgDevices = WM_APP + 20, kMsgResolved = WM_APP + 21, kMsgChange = WM_APP + 22, kMsgChildren = WM_APP + 23;
    static constexpr UINT_PTR kRefreshTimer = 1, kDragScrollTimer = 2;

    static const wchar_t* SectionName(int section);
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    void Paint(HDC hdc, const RECT& rc);

    void LoadPins();
    void ScanDevices();
    void OnDevices(DevicePayload* payload);
    void OnResolved(ResolvePayload* payload);
    void OnShellChange(HANDLE lock, DWORD process);
    bool PinKeys(const std::vector<std::wstring>& keys, int index);
    int PinnedOrdinal(int entry) const;
    int PinnedCount() const;
    int DragOrdinal() const; // pinned position of the entry being dragged, -1 if gone
    void MovePin(int from, int to);
    void Rebuild(bool notify = true); // notify: tell the host the entries changed (not for expand/collapse)
    void AppendTree(Node& node, Section section, int depth, int& y);
    void MatchLocation();
    int RowAt(int y) const;
    RECT RowRect(int row) const;
    RECT ItemBox(int row) const;     // Files' ElementGrid: the rounded, hoverable part of the row
    RECT ChevronRect(int row) const; // the 16px chevron column
    RECT EjectRect(int row) const;   // the 24px eject decorator (removable drives)
    bool HasEject(int row) const;

    // Rows and the tree
    Entry* EntryOf(int row);
    const Entry* EntryOf(int row) const;
    bool EnsurePidl(Entry& e); // tree children parse their path when first needed
    Node* NodeOf(int row, bool create);
    bool Expandable(int row) const;
    bool Expanded(int row) const;
    void SetExpanded(int row, bool expanded);
    void LoadChildren(Node& node);
    void OnChildren(ChildrenPayload* payload);
    void ForEachNode(const std::wstring& key, const std::function<void(Node&)>& fn);
    void ReloadNode(Node& node);
    void Eject(int row);
    int ContentHeight() const;
    void ScrollTo(int offset);
    void EnsureVisible(int row);
    void SetHot(int row);
    void Activate(int row, OpenMode mode);
    void OpenLazy(const std::wstring& name, const std::wstring& path, OpenMode mode);
    void ShowEntryMenu(int row, POINT screen);
    void ShowTagMenu(int row, POINT screen);
    void AppendOpenItems(HMENU menu);
    void ShowSectionMenu(int section, POINT screen);
    void ToggleSection(Section section);
    void BeginDrag(int row);
    void UpdateDrop(POINTL screen, DWORD keys, DWORD* effect);
    void ClearDropTarget();
    std::wstring TipFor(int row) const;
    bool ThumbRect(RECT& thumb) const;

    LONG m_ref = 1;
    SidebarEvents* m_events;
    const Theme* m_theme = nullptr;
    HWND m_hwnd = nullptr;
    HWND m_tooltip = nullptr;
    HIMAGELIST m_images = nullptr;
    ULONG m_notify = 0;

    std::vector<Entry> m_entries; // pinned + home first, then device entries
    std::vector<Row> m_rows;
    std::wstring m_pinFile;
    std::wstring m_location; // lowercase parsing name of the active tab's folder
    int m_selected = -1;     // row index of the highlighted item (deepest visible match of m_location)
    HFONT m_headerFont = nullptr; // section headers: body text, SemiBold (Files' NoChildren/Expanded/Collapsed states)
    // Tree-view state keyed by the lowercase folder path of the expandable top-level item (survives device rescans)
    std::unordered_map<std::wstring, std::unique_ptr<Node>> m_trees;
    unsigned m_treeGeneration = 0;
    std::vector<std::wstring> m_pendingTreeReloads; // lowercase parent folders that changed (debounced)
    int m_chevronPressed = -1, m_ejectPressed = -1;
    bool m_showHidden = false, m_showProtected = false, m_showDots = true;
    bool m_ejectHot = false;
    unsigned m_hidden = 1u << SecLibraries;
    // Files' defaults: Libraries, Network, WSL and Tags start collapsed (IsNetworkSectionExpanded = false, ...)
    unsigned m_collapsed = 1u << SecLibraries | 1u << SecNetwork | 1u << SecWsl | 1u << SecTags;
    unsigned m_generation = 0;
    bool m_scanning = false;
    bool m_rescan = false;
    bool m_pendingPins = false;    // shell change seen; reload pins when the debounce timer fires
    bool m_pendingDevices = false; // ... rescan drives

    int m_scroll = 0;
    SmoothScroll m_smooth; // the wheel's animation (the scrollbar thumb drag stays immediate)
    int m_hot = -1, m_pressed = -1, m_focus = -1;
    bool m_keyboardFocus = false;
    bool m_tracking = false;
    bool m_thumbDrag = false;
    int m_thumbGrab = 0;
    bool m_overBar = false;
    POINT m_downPt{};
    int m_tipRow = -1;
    std::wstring m_tipText;

    // Drag and drop
    ComPtr<IDropTargetHelper> m_dropHelper;
    ComPtr<IDataObject> m_dragData;
    ComPtr<IDropTarget> m_itemTarget;
    int m_itemTargetRow = -1;
    DropMode m_dropMode = DropMode::None;
    bool m_dragFolders = false; // every dragged item is a folder (can be pinned)
    int m_dragRow = -1;         // >= 0 while an entry is being dragged out of this control
    std::wstring m_dragKey;     // its parsing name (rows may be rebuilt mid-drag)
    bool m_dragPinned = false;  // dragging a pin: dropping on the Pinned section reorders
    int m_insertAt = -1;        // pin insertion index while hovering the Pinned section
    int m_insertY = 0;          // content y of the insertion line
    int m_dropRow = -1;         // row highlighted as the drop target
    DWORD m_lastKeys = 0;
    DWORD m_allowed = 0;        // effects the drag source allows

    ComPtr<IContextMenu2> m_menuHandler;
};
