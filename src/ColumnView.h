#pragma once
#include "FileList.h"

// Files' column view (Views/Layouts/ColumnsLayoutPage + ColumnLayoutPage, a BladeView of ColumnShellPages): one
// column per folder from a root (drive, pinned or cloud folder) down to the current one, side by side in a
// horizontally scrolling strip. Each column is FastFiles' own list (FileList::Mode::Column: Files' ColumnLayoutPage
// rows at ColumnsViewSize); the shell still supplies menus, drag data and invoking through it. Selecting a single
// folder opens it in the next column (the row leading there keeps the selected background); selecting a file, several
// items or empty space closes the columns after it. The active column (Files' ActivePaneOrColumn) is the one with
// focus; the tab shows its folder and its commands go to its list.

struct ViewPrefs;
// FastFiles' hidden-item settings on a shell view (TabView.cpp): ICommDlgBrowser2::GetViewFlags, then IncludeObject
DWORD HiddenViewFlags(const ViewPrefs* prefs);
HRESULT FilterHidden(const ViewPrefs* prefs, IShellView* view, PCUITEMID_CHILD child);
// The browser's own window (ExplorerBrowserControl) shows between the old view going away and the new one appearing:
// erase it with the palette instead of the system's white brush (TabView.cpp)
void ThemeBrowserHost(IExplorerBrowser* browser, const ViewPrefs* prefs);

struct ColumnViewEvents
{
    virtual const ViewPrefs* ColumnPrefs() const = 0;
    virtual void OnColumnActive(PCIDLIST_ABSOLUTE folder) = 0; // the active column changed, or the active column's folder
    virtual void OnColumnCantShow(PCIDLIST_ABSOLUTE folder) = 0; // a folder a column can't list (the tab opens it itself)
    virtual ListSort ColumnSortFor(PCIDLIST_ABSOLUTE folder) const = 0; // the folder's sort / grouping (each column its own)
    // The tab's handling of what isn't the column view's own: menus, double click on empty space, Ctrl+wheel
    virtual FileListEvents* ColumnListEvents() = 0;
};

class ColumnView
{
public:
    ColumnView(ColumnViewEvents* events, const Theme* theme);
    ~ColumnView();
    bool Create(HWND parent);
    void Destroy();
    HWND Hwnd() const { return m_hwnd; }
    void Move(const RECT& rc);

    // chain[0] is the root, chain.back() the folder shown last; `select` is a child of the last one (or null)
    // false: not even the first folder could be listed (no columns)
    bool Open(const std::vector<UniquePidl>& chain, PCUITEMID_CHILD select, bool focus);
    void OpenAfter(int index, PCIDLIST_ABSOLUTE folder); // folder in column index+1, columns after it closed
    void Dismiss(int keep);                              // close every column after `keep`
    void Activate(int index, bool focus);
    void DismissLast();                                  // Files' NavigateUp with more than one column
    void Select(PCUITEMID_CHILD child);                  // in the active column, without opening it

    int Count() const { return static_cast<int>(m_columns.size()); }
    int Active() const { return m_active; }
    PCIDLIST_ABSOLUTE Folder(int index) const;
    FileList* ListAt(int index) const;
    FileList* ActiveList() const { return ListAt(m_active); }
    int IndexOfList(HWND list) const; // column whose items window this is, -1 if none

    void ReloadViews();                          // every column lists its folder again (hidden-item settings changed)
    void SetFilter(const std::wstring& lower);   // the tab's name filter, on the active column only
    void ApplyTheme();                           // palette change: the lists, separators, scroll bar
    void ApplyPrefs();                           // hidden / sort priority settings: every column re-filters and re-sorts
    void Resized();                              // ColumnsViewSize changed: the widths follow the icon box

    struct Column;
    // From a column's list (FileListEvents)
    void OnOpen(Column* column, PCIDLIST_ABSOLUTE folder);
    void OnLoaded(Column* column);
    void OnUserSelection(Column* column);
    bool OnKey(Column* column, WPARAM key);
    void OnFocus(Column* column);

private:
    static constexpr UINT_PTR kSelectionTimer = 1;
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    bool AddColumn();
    void Browse(int index, PCIDLIST_ABSOLUTE folder);
    void Layout();
    void ScrollTo(int x);
    void EnsureVisible(int index);
    int GripAt(int x) const;
    int IndexOf(const Column* column) const;
    void EvaluateSelection(int index);
    void Measure(int index, bool scroll);

    ColumnViewEvents* m_events;
public:
    const ViewPrefs* Prefs() const { return m_events ? m_events->ColumnPrefs() : nullptr; }
    FileListEvents* TabEvents() const { return m_events ? m_events->ColumnListEvents() : nullptr; }
    const Theme* GetTheme() const { return m_theme; }
private:
    const Theme* m_theme;
    HWND m_hwnd = nullptr;
    std::vector<std::unique_ptr<Column>> m_columns;
    int m_active = 0;
    int m_scroll = 0;          // pixels scrolled right
    int m_content = 0;         // total width of the columns
    bool m_inLayout = false;
    int m_dragGrip = -1;       // column whose right edge is being dragged
    int m_dragStartX = 0, m_dragStartWidth = 0;
    int m_hotGrip = -1;
    int m_selectionColumn = -1;    // column whose selection the user changed (evaluated when the timer fires)
    bool m_selectionRight = false; // ... with the right button or the menu key (never opens: Files #18584)
    bool m_selectionMouse = false; // ... by a click (an empty-area click closes the columns after it)
    std::wstring m_filter;         // the tab's filter (on the active column)
};
