#pragma once
#include "Theme.h"
#include "Lang.h"

struct TabStripEvents
{
    virtual void OnTabSelect(int index) = 0;
    virtual void OnTabClose(int index) = 0;
    virtual void OnTabMoved(int from, int to) = 0; // the strip has already reordered itself
    virtual void OnTabNew() = 0;
    virtual void OnTabMenu(int index, POINT screen) = 0;
    // Files' TabView: the wheel over the strip selects the next/previous tab (delta as in WM_MOUSEWHEEL)
    virtual void OnTabWheel(int) {}
    // Dragging a tab out of the strip (another window, or nowhere = a new window there)
    virtual std::vector<BYTE> TabPayload(int) { return {}; }  // what the drag carries
    virtual void OnTabTornOut(int, POINT) {}                   // dropped outside every FastFiles window
    virtual void OnTabMovedAway(int) {}                         // another window took it
    virtual void OnTabArrived(const std::vector<BYTE>&, int) {} // a tab from another window, insert at index
    // Files dragged over the strip: the tab's folder takes the drop; folders dropped on "+" open as new tabs
    virtual PCIDLIST_ABSOLUTE TabFolder(int) { return nullptr; }
    virtual void OnFoldersToNewTabs(const std::vector<UniquePidl>&) {}
    virtual void OnTabActions(const RECT&) {} // Files' "标签操作菜单" button left of the tabs (screen rect)
};

// Files-style tab row drawn into the title bar (Files' TabBar over a WinUI TabView): rounded tabs 100-240 px wide,
// scroll buttons once they no longer fit, a "+" button and the window's caption buttons.
// Empty space answers HTTRANSPARENT so the frame can report HTCAPTION (drag, double-click maximize, system menu).
class TabStrip final : public IDropTarget
{
public:
    bool Create(HWND parent, int id, const Theme* theme, TabStripEvents* events);
    HWND Hwnd() const { return m_hwnd; }
    int Count() const { return static_cast<int>(m_items.size()); }
    static constexpr int kSettingsIcon = -2; // icon value: the settings glyph instead of a system image
    static constexpr int kHomeIcon = -3;     // icon value: Files' Home sidebar icon (SidebarSectionIcons Home)
    void Insert(int index, const std::wstring& text, int icon);
    void Set(int index, const std::wstring& text, int icon);
    void SetTip(int index, const std::wstring& tip); // Files: ToolTipText (the tab's full path)
    void SetNewTabTip(const std::wstring& tip) { m_newTabTip = tip; } // "+": Commands.NewTab.LabelWithHotKey
    void Remove(int index);
    void Clear();
    void SetActive(int index);
    void SetWindowActive(bool active);
    void SetMaximized(bool maximized);
    RECT ItemRect(int index) const;
    void Relayout();
    void SetActionsVisible(bool visible); // Files: 在标题栏中显示标签操作按钮 (default on)
    void SetLoading(int index, bool loading); // Files' LoadingStates: a ProgressRing instead of the tab's icon
    // Keyboard focus (the window's Tab order): kFocusActions (the tab actions button, when shown), kFocusTabs (the
    // tab list: Left / Right / Home / End select, Ctrl+F4 / Delete close, as WinUI's TabView) and kFocusNew ("+")
    static constexpr int kFocusActions = 0, kFocusTabs = 1, kFocusNew = 2;
    std::vector<int> FocusStops() const;
    void FocusPart(int part); // takes the focus with the keyboard ring on that part
    int FocusedPart() const;  // -1 when the strip doesn't have the focus
    // Clipboard format of a dragged tab (registered; shared by every FastFiles process)
    static CLIPFORMAT TabFormat();

    // IDropTarget (not reference counted: lives as long as the window)
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }
    IFACEMETHODIMP_(ULONG) Release() override { return 1; }
    IFACEMETHODIMP DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override;
    IFACEMETHODIMP DragOver(DWORD keys, POINTL pt, DWORD* effect) override;
    IFACEMETHODIMP DragLeave() override;
    IFACEMETHODIMP Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override;

private:
    enum class Part { None, Tab, TabClose, NewTab, ScrollLeft, ScrollRight, Actions, Minimize, Maximize, Close };
    struct Hit
    {
        Part part = Part::None;
        int index = -1;
        bool operator==(const Hit&) const = default;
    };
    struct Item
    {
        std::wstring text;
        int icon = -1;
        std::wstring tip;
        bool loading = false;
        RECT rc{};   // client coordinates (already scrolled)
        RECT close{};
    };
    enum class DropKind { None, Tab, Files, NewTab };
    static constexpr UINT_PTR kScrollTimer = 1, kHoverTimer = 2, kSpinTimer = 3;
    void DrawProgressRing(HDC hdc, const RECT& r) const;
    static constexpr UINT kHoverMs = 1300; // Files: Constants.DragAndDrop.HoverToOpenTimespan
    static constexpr UINT kTipMask = 0x8000; // TCM_GETITEMW mask bit (tests): pszText receives the tooltip instead

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    void Paint(HDC hdc, const RECT& rc);
    void PaintCaption(HDC hdc, const RECT& r, Part part, const wchar_t* glyph);
    Hit HitTest(POINT pt) const;
    void Activate(const Hit& hit);
    void DragTo(POINT pt);
    void BeginTearOut(int index);
    void ScrollBy(int delta);
    void EnsureVisible(int index);
    int InsertIndexAt(int x) const;
    void MoveItem(int from, int to);
    void UpdateDrop(POINTL screen, DWORD keys, DWORD* effect);
    void ClearDropTarget();
    void SetHot(const Hit& hit);
    std::wstring TipFor(const Hit& hit) const;
    void Invalidate() { InvalidateRect(m_hwnd, nullptr, FALSE); }

    HWND m_hwnd = nullptr;
    const Theme* m_theme = nullptr;
    TabStripEvents* m_events = nullptr;
    HIMAGELIST m_images = nullptr;
    HICON m_logo = nullptr; // the tab actions button's FastFiles logo, loaded at m_logoSize (follows the DPI)
    int m_logoSize = 0;
    std::vector<Item> m_items;
    int m_active = -1;
    RECT m_new{}, m_min{}, m_max{}, m_close{}, m_left{}, m_right{}, m_actions{};
    bool m_showActions = true;
    RECT m_view{};         // the part of the strip tabs are drawn in
    int m_tabWidth = 0;
    int m_scroll = 0;      // pixels the tabs are scrolled left
    bool m_overflow = false;
    Hit m_hot, m_pressed;
    int m_focusPart = kFocusTabs; // the part that has the keyboard focus while the strip has it
    bool m_ring = false;          // the focus came from the keyboard (a mouse press hides the ring)
    bool m_tracking = false;
    bool m_windowActive = true;
    bool m_maximized = false;
    int m_dragIndex = -1;
    POINT m_dragStart{};
    POINT m_pressScreen{};          // Files measures a torn-out drag from the press (TabDragStarting)
    ULONGLONG m_pressTime = 0;
    bool m_dragging = false;
    HWND m_tooltip = nullptr;
    std::wstring m_tipText;
    std::wstring m_newTabTip = Tr(L"新建标签页");

    // Drag and drop
    ComPtr<IDataObject> m_dragData;
    DropKind m_dropKind = DropKind::None;
    int m_dropIndex = -1;           // tab under the cursor (files) or insertion index (tabs)
    int m_hoverIndex = -1;          // tab the hover timer will select
    ComPtr<IDropTarget> m_itemTarget;
    int m_itemTargetIndex = -1;
    DWORD m_dragKeys = 0; // last DragEnter / DragOver keys (Drop's have lost the mouse button)
    bool m_selfDrop = false;        // a tab of this window was dropped back on its own strip
    int m_tearIndex = -1;           // tab being dragged out (while DoDragDrop runs)
    bool m_dragFolders = false;     // the files being dragged include a folder ("+" accepts them)
    ComPtr<IDropTargetHelper> m_dropHelper;
};
