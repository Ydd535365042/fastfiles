#include "ColumnView.h"
#include "DarkMode.h"
#include "Tab.h" // FolderTarget: folders and shortcuts to folders, as the tab itself opens them

namespace
{
constexpr wchar_t kClass[] = L"FastFiles.Columns";
// Files' BladeItem: DEFAULT_WIDTH 200, MINIMUM_WIDTH 150, optimal = widest name + 120 (icon, margins, chevron)
constexpr int kDefaultWidth = 200, kMinWidth = 150, kPadding = 120, kGap = 5;

// A child of `folder` by its file system name (what the column lists select by)
std::wstring ChildName(PCIDLIST_ABSOLUTE folder, PCUITEMID_CHILD child)
{
    if (!folder || !child)
        return {};
    const UniquePidl full(ILCombine(folder, reinterpret_cast<PCUIDLIST_RELATIVE>(child)));
    const std::wstring path = PidlName(full.get(), SIGDN_FILESYSPATH);
    return path.empty() ? std::wstring() : std::wstring(PathFindFileNameW(path.c_str()));
}

bool SameFolder(PCIDLIST_ABSOLUTE a, PCIDLIST_ABSOLUTE b)
{
    if (!a || !b)
        return false;
    if (ILIsEqual(a, b))
        return true;
    const std::wstring pa = PidlName(a, SIGDN_FILESYSPATH);
    return !pa.empty() && CompareStringOrdinal(pa.c_str(), -1, PidlName(b, SIGDN_FILESYSPATH).c_str(), -1, TRUE) == CSTR_EQUAL;
}

}

// One column: its list (Files' ColumnShellPage + ColumnLayoutPage), and what it tells the column view
struct ColumnView::Column final : FileListEvents
{
    ColumnView* owner = nullptr; // null once closed (late events are dropped)
    std::unique_ptr<FileList> list;
    UniquePidl folder;
    std::wstring select;     // name to select once listed (the last column: --select, the folder we came up from)
    std::wstring traced;     // the row leading to the next column (Files' openedFolderPresenter)
    int width = 200;
    bool userWidth = false;  // resized by hand: no more automatic sizing until it shows another folder
    bool measured = false;   // sized for this folder already (a reload keeps the width)
    RECT rc{};

    const ViewPrefs* ListPrefs() const override { return owner ? owner->Prefs() : nullptr; }
    void OnListOpen(PCIDLIST_ABSOLUTE target) override
    {
        if (owner)
            owner->OnOpen(this, target);
    }
    void OnListUp() override
    {
        if (owner && owner->TabEvents())
            owner->TabEvents()->OnListUp();
    }
    void OnListMenu(HWND hwnd, POINT screen, bool background) override
    {
        if (owner && owner->TabEvents())
            owner->TabEvents()->OnListMenu(hwnd, screen, background);
    }
    void OnListLoaded() override
    {
        if (owner)
            owner->OnLoaded(this);
    }
    void OnListColumnsDefault(unsigned) override {}
    void OnListZoom(int direction) override
    {
        if (owner && owner->TabEvents())
            owner->TabEvents()->OnListZoom(direction);
    }
    void OnListUserSelection() override
    {
        if (owner)
            owner->OnUserSelection(this);
    }
    bool OnListKey(WPARAM key) override { return owner && owner->OnKey(this, key); }
    void OnListFocus() override
    {
        if (owner)
            owner->OnFocus(this);
    }
    void OnListSorted(FileList* sorted) override
    {
        if (owner && owner->TabEvents())
            owner->TabEvents()->OnListSorted(sorted); // stored for this column's folder
    }
};

ColumnView::ColumnView(ColumnViewEvents* events, const Theme* theme) : m_events(events), m_theme(theme) {}

ColumnView::~ColumnView()
{
    Destroy();
}

bool ColumnView::Create(HWND parent)
{
    static const bool registered = []
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        return RegisterClassExW(&wc) != 0;
    }();
    RECT rc{};
    GetClientRect(parent, &rc);
    // Hidden until the first Open; the horizontal scroll bar only shows when the columns don't fit
    if (!registered || !CreateWindowExW(WS_EX_CONTROLPARENT, kClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, rc.right, rc.bottom, parent, nullptr, GetModuleHandleW(nullptr), this))
        return false;
    m_bar.Attach(m_hwnd, false, kBarTimer, m_theme, [this](int x) { ScrollTo(x); });
    ApplyTheme();
    return true;
}

LRESULT CALLBACK ColumnView::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
    {
        auto* self = static_cast<ColumnView*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<ColumnView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->HandleMessage(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

void ColumnView::Destroy()
{
    for (auto& column : m_columns)
        column->owner = nullptr; // (the lists' windows go with them)
    m_columns.clear();
    m_active = 0;
    if (m_hwnd)
    {
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

void ColumnView::Move(const RECT& rc)
{
    if (m_hwnd)
        MoveChild(m_hwnd, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
}

bool ColumnView::AddColumn()
{
    auto column = std::make_unique<Column>();
    column->owner = this;
    column->width = m_theme->S(kDefaultWidth);
    column->list = std::make_unique<FileList>(column.get(), m_theme);
    if (!column->list->Create(m_hwnd))
        return false;
    column->list->SetMode(FileList::Mode::Column);
    m_columns.push_back(std::move(column));
    return true;
}

// The column lists `folder`; one it can't list (a zip folder, a folder that went away) ends the column view there:
// the columns from this one on close and the tab opens the folder itself
void ColumnView::Browse(int index, PCIDLIST_ABSOLUTE folder)
{
    Column* c = m_columns[index].get();
    UniquePidl copy = ClonePidl(folder); // folder may be c->folder itself
    c->folder = std::move(copy);
    c->select.clear();
    c->traced.clear();
    c->userWidth = c->measured = false;
    c->list->SetTraced(L"");
    c->list->SetFilter(index == m_active ? m_filter : std::wstring());
    if (m_selectionColumn == index)
        m_selectionColumn = -1; // (a selection made in what it showed before)
    if (c->folder)
        c->list->SetSortState(m_events->ColumnSortFor(c->folder.get())); // each column its folder's own
    if (c->folder && c->list->Open(c->folder.get(), L""))
        return;
    const UniquePidl failed = std::move(c->folder);
    Dismiss(index - 1);
    if (index == 0)
    {
        m_active = 0; // the first column itself: Open reports it, the tab shows the folder another way
        return;
    }
    if (failed)
        m_events->OnColumnCantShow(failed.get());
}

bool ColumnView::Open(const std::vector<UniquePidl>& chain, PCUITEMID_CHILD select, bool focus)
{
    if (chain.empty())
        return false;
    // Columns already showing the same folders stay as they are (back/forward, a layout switch)
    int keep = 0;
    while (keep < Count() && keep < static_cast<int>(chain.size()) && SameFolder(m_columns[keep]->folder.get(), chain[keep].get()))
        ++keep;
    Dismiss(keep - 1);
    const int first = Count();
    while (Count() < static_cast<int>(chain.size()) && AddColumn())
    {
    }
    if (Count() == 0)
        return false;
    Layout();
    const int target = static_cast<int>(chain.size()) - 1;
    m_active = std::min(target, Count() - 1);
    for (int i = first; i < Count(); ++i)
    {
        Browse(i, chain[i].get());
        if (Count() <= i)
            return Count() > 0; // (that folder can't be a column: the tab takes over; none at all: the caller does)
    }
    // Each column's row leading to the next one keeps the selected background (Files' HighlightPathDirectory, not a
    // selection); the last selects `select` (without opening it)
    for (int i = 0; i < Count(); ++i)
    {
        Column* c = m_columns[i].get();
        const bool last = i + 1 == Count();
        c->traced = last ? std::wstring() : ChildName(chain[i].get(), ILFindLastID(chain[i + 1].get()));
        c->list->SetTraced(c->traced);
        if (last)
        {
            c->select = ChildName(chain[i].get(), select);
            if (!c->select.empty())
                c->list->SelectWhenListed(c->select);
        }
    }
    m_active = Count() - 1;
    if (focus && m_active >= 0)
        Activate(m_active, true);
    ScrollTo(m_content);
    m_events->OnColumnActive(Folder(m_active));
    return true;
}

void ColumnView::Activate(int index, bool focus)
{
    if (index < 0 || index >= Count())
        return;
    const bool changed = index != m_active;
    m_active = index;
    if (changed)
    {
        // The tab's filter is the active column's only
        for (int i = 0; i < Count(); ++i)
            m_columns[i]->list->SetFilter(i == index ? m_filter : std::wstring());
    }
    if (focus && GetFocus() != m_columns[index]->list->List())
        SetFocus(m_columns[index]->list->List()); // (OnFocus comes back here: changed is false by then)
    if (changed)
    {
        EnsureVisible(index);
        m_events->OnColumnActive(Folder(index));
    }
}

void ColumnView::Dismiss(int keep)
{
    keep = std::max(keep, -1);
    if (keep + 1 >= Count())
        return;
    // Keyboard focus in a column that's about to go: hand it to the one that stays first
    const HWND focus = GetFocus();
    bool lostFocus = false;
    for (int i = keep + 1; i < Count(); ++i)
        if (const HWND w = m_columns[i]->list->Hwnd(); focus && (w == focus || IsChild(w, focus)))
            lostFocus = true;
    const bool activeGoes = m_active > keep;
    if (lostFocus && keep >= 0)
        Activate(keep, true);
    for (int i = Count() - 1; i > keep; --i)
        m_columns[i]->owner = nullptr;
    if (m_selectionColumn > keep)
        m_selectionColumn = -1; // its column is gone (the timer would look at whichever comes there next)
    m_columns.resize(static_cast<size_t>(keep + 1));
    // Files' CloseFolder: the row that led to a closed column is just a row again
    if (keep >= 0)
    {
        m_columns[keep]->traced.clear();
        m_columns[keep]->list->SetTraced(L"");
    }
    Layout();
    if (activeGoes && keep >= 0)
        Activate(keep, false);
}

void ColumnView::Select(PCUITEMID_CHILD child)
{
    if (m_active < 0 || m_active >= Count() || !child)
        return;
    Column* c = m_columns[m_active].get();
    c->select = ChildName(c->folder.get(), child);
    if (!c->select.empty())
        c->list->SelectWhenListed(c->select); // (not the user's: nothing opens)
}

void ColumnView::DismissLast()
{
    if (Count() > 1)
        Dismiss(Count() - 2);
}

void ColumnView::OpenAfter(int index, PCIDLIST_ABSOLUTE folder)
{
    if (index < 0 || index >= Count() || !folder)
        return;
    const UniquePidl target = ClonePidl(folder);
    // The row leading there (Files' SetOpenedFolder: the selected item's row, a shortcut's included)
    std::wstring traced;
    if (FileList* list = m_columns[index]->list.get(); list->SelectedCount() == 1)
        if (const auto paths = list->SelectedPaths(); paths.size() == 1)
            traced = PathFindFileNameW(paths[0].c_str());
    if (index + 1 < Count() && SameFolder(m_columns[index + 1]->folder.get(), target.get()))
    {
        Dismiss(index + 1); // already open: only what's after it goes
    }
    else
    {
        if (index + 1 < Count())
            Dismiss(index + 1); // the next column shows the new folder; the ones after it close
        else if (!AddColumn())
            return;
        m_columns[index + 1]->width = m_theme->S(kDefaultWidth);
        Layout();
        Browse(index + 1, target.get());
        if (index + 1 >= Count())
            return; // (that folder can't be a column: the tab took over)
        Layout();
        ScrollTo(m_content);
        if (m_active == index + 1)
            m_events->OnColumnActive(Folder(m_active)); // the active column now shows another folder
    }
    m_columns[index]->traced = traced;
    m_columns[index]->list->SetTraced(traced);
}

// Columns left to right, each followed by a 5 px gap (the resize grip, with a 1 px separator)
void ColumnView::Layout()
{
    if (!m_hwnd || m_inLayout)
        return;
    m_inLayout = true;
    const int gap = m_theme->S(kGap);
    m_content = 0;
    for (const auto& c : m_columns)
        m_content += c->width + gap;
    RECT client{};
    GetClientRect(m_hwnd, &client);
    m_scroll = std::clamp(m_scroll, 0, std::max(0, m_content - static_cast<int>(client.right)));
    const bool bar = m_content > client.right;
    const int strip = bar ? m_theme->S(12) : 0; // ScrollBarSize
    m_bar.Set(bar ? m_content : 0, client.right, m_scroll, { 0, client.bottom - strip, client.right, client.bottom },
        m_theme->S(40));
    int x = -m_scroll;
    for (int i = 0; i < Count(); ++i)
    {
        Column* c = m_columns[i].get();
        c->rc = { x, 0, x + c->width, std::max<LONG>(0, client.bottom - strip) };
        c->list->Move(c->rc);
        ShowWindow(c->list->Hwnd(), SW_SHOWNA);
        x += c->width + gap;
    }
    m_inLayout = false;
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void ColumnView::ScrollTo(int x)
{
    RECT client{};
    if (m_hwnd)
        GetClientRect(m_hwnd, &client);
    const int pos = std::clamp(x, 0, std::max(0, m_content - static_cast<int>(client.right)));
    if (pos != m_scroll)
    {
        m_scroll = pos;
        Layout();
    }
}

void ColumnView::EnsureVisible(int index)
{
    if (index < 0 || index >= Count() || !m_hwnd)
        return;
    RECT client{};
    GetClientRect(m_hwnd, &client);
    const RECT& r = m_columns[index]->rc;
    if (r.left < 0)
        ScrollTo(m_scroll + r.left);
    else if (r.right > client.right)
        ScrollTo(m_scroll + (r.right - client.right) + m_theme->S(kGap));
}

int ColumnView::GripAt(int x) const
{
    for (int i = 0; i < Count(); ++i)
        if (x >= m_columns[i]->rc.right && x < m_columns[i]->rc.right + m_theme->S(kGap))
            return i;
    return -1;
}

int ColumnView::IndexOf(const Column* column) const
{
    for (int i = 0; i < Count(); ++i)
        if (m_columns[i].get() == column)
            return i;
    return -1;
}

PCIDLIST_ABSOLUTE ColumnView::Folder(int index) const
{
    return index >= 0 && index < Count() ? m_columns[index]->folder.get() : nullptr;
}

FileList* ColumnView::ListAt(int index) const
{
    return index >= 0 && index < Count() ? m_columns[index]->list.get() : nullptr;
}

int ColumnView::IndexOfList(HWND list) const
{
    for (int i = 0; i < Count(); ++i)
        if (list && (m_columns[i]->list->List() == list || m_columns[i]->list->Hwnd() == list))
            return i;
    return -1;
}

// Files' BladeItem.SetWidth once the folder has loaded: widest name + padding (the icon box past 16 widens it), at least
// the minimum; an empty folder keeps the default
void ColumnView::Measure(int index, bool scroll)
{
    if (index < 0 || index >= Count())
        return;
    Column* c = m_columns[index].get();
    if (c->userWidth)
        return;
    const ViewPrefs* prefs = Prefs();
    const int size = std::clamp(prefs ? prefs->layoutSize[static_cast<int>(FileList::Mode::Column)] : 2, 1, 5);
    constexpr int kIcon[5] = { 16, 16, 20, 24, 32 };
    const int width = c->list->Count() > 0
        ? std::max(c->list->NameWidth() + m_theme->S(kPadding + kIcon[size - 1] - 16), m_theme->S(kMinWidth))
        : m_theme->S(kDefaultWidth);
    c->measured = true;
    if (width != c->width)
    {
        c->width = width;
        Layout();
        if (scroll)
            ScrollTo(m_content);
    }
}

void ColumnView::SetFilter(const std::wstring& lower)
{
    m_filter = lower;
    if (FileList* list = ActiveList())
        list->SetFilter(lower);
}

void ColumnView::ReloadViews()
{
    for (const auto& c : m_columns)
        c->list->ApplyPrefs(); // (hidden items: re-filtered from the listing)
}

void ColumnView::ApplyPrefs()
{
    for (const auto& c : m_columns)
        c->list->ApplyPrefs();
}

void ColumnView::Resized()
{
    for (int i = 0; i < Count(); ++i)
        Measure(i, false);
    Layout();
}

void ColumnView::ApplyTheme()
{
    if (!m_hwnd)
        return;
    for (const auto& c : m_columns)
        c->list->ApplyTheme();
    InvalidateRect(m_hwnd, nullptr, TRUE);
}

// A folder opened in a column's list (Enter, double click, 打开, a drag hovering over it): into the next column
// (Files' ColumnViewBase_ItemInvoked)
void ColumnView::OnOpen(Column* column, PCIDLIST_ABSOLUTE folder)
{
    const int index = IndexOf(column);
    if (index < 0 || !folder)
        return;
    const UniquePidl target = ClonePidl(folder);
    OpenAfter(index, target.get());
}

// The folder's items are in: sized to its names once (Files' OnItemLoadStatusChanged -> SetWidth)
void ColumnView::OnLoaded(Column* column)
{
    const int index = IndexOf(column);
    if (index < 0)
        return;
    if (!column->measured)
        Measure(index, true);
    if (TabEvents() && index == m_active)
        TabEvents()->OnListLoaded();
}

// The user changed a column's selection (a programmatic select / Unselect doesn't come here)
void ColumnView::OnUserSelection(Column* column)
{
    const int index = IndexOf(column);
    if (index < 0)
        return;
    // Files' HandleSelectionChange: something selected here clears the selection in every other column (their rows
    // leading on keep the traced background: ClearSelectionIndicator leaves openedFolderPresenter alone)
    if (column->list->SelectedCount() > 0)
        for (int i = 0; i < Count(); ++i)
            if (i != index && m_columns[i]->list->SelectedCount() > 0)
                m_columns[i]->list->Unselect();
    // Decided a moment later, once the click or drag is over; how it was made is read now
    m_selectionColumn = index;
    m_selectionRight = (GetKeyState(VK_RBUTTON) & 0x8000) || (GetKeyState(VK_APPS) & 0x8000) ||
        ((GetKeyState(VK_F10) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000));
    m_selectionMouse = (GetKeyState(VK_LBUTTON) & 0x8000) != 0;
    SetTimer(m_hwnd, kSelectionTimer, 30, nullptr);
}

void ColumnView::OnFocus(Column* column)
{
    if (const int index = IndexOf(column); index >= 0)
        Activate(index, false); // Files: the column that gets focus is the current instance
}

// Files' ColumnLayoutPage.OnSelectionChanged / FileList_ItemTapped
void ColumnView::EvaluateSelection(int index)
{
    if (index < 0 || index >= Count())
        return;
    Column* c = m_columns[index].get();
    FileList* list = c->list.get();
    const int selected = list->SelectedCount();
    if (selected == 1)
    {
        ComPtr<IShellItemArray> items = list->Selection();
        ComPtr<IShellItem> item;
        UniquePidl target;
        if (items && SUCCEEDED(items->GetItemAt(0, &item)) && FolderTarget(item.Get(), target))
        {
            // Files' TryOpenSelectedFolder: the row already leading to the next column stays as it is; a right click
            // never opens a folder, it closes the stale column after it (#18584)
            const auto paths = list->SelectedPaths();
            const std::wstring name = paths.size() == 1 ? std::wstring(PathFindFileNameW(paths[0].c_str())) : std::wstring();
            if (!c->traced.empty() && CompareStringOrdinal(name.c_str(), -1, c->traced.c_str(), -1, TRUE) == CSTR_EQUAL)
                return;
            if (m_selectionRight)
                Dismiss(index);
            else
                OpenAfter(index, target.get());
            return;
        }
        Dismiss(index); // a file
    }
    else if (selected > 1 || m_selectionMouse)
        Dismiss(index); // several items, or a click on empty space
}

// Files' ColumnLayoutPage.FileList_PreviewKeyDown (the keys it handles itself; the rest go to the list)
bool ColumnView::OnKey(Column* column, WPARAM key)
{
    const int index = IndexOf(column);
    if (index < 0 || (GetKeyState(VK_MENU) & 0x8000)) // Alt+arrows: the window's commands (back, forward, up)
        return false;
    FileList* list = column->list.get();
    if (list->Renaming() || list->SelectedCount() > 1)
        return false;
    switch (key)
    {
    case VK_SPACE:
        return true; // handled (no selection toggling in a column)
    case VK_UP:
    case VK_DOWN:
        // ClearOpenedFolderSelectionIndicator; nothing selected yet: the first item
        column->traced.clear();
        list->SetTraced(L"");
        if (list->SelectedCount() == 0 && list->Count() > 0)
        {
            list->SelectFirst();
            return true;
        }
        return false;
    case VK_LEFT:
        // MoveFocusToPreviousBlade: the columns after this one close, the previous one gets the keyboard (none in the first)
        if (index > 0)
        {
            Dismiss(index);
            Activate(index - 1, true);
        }
        list->Unselect(); // stays open, nothing selected in it
        column->traced.clear();
        list->SetTraced(L"");
        return true;
    case VK_RIGHT:
        // MoveFocusToNextBlade: into the next column, its first item selected
        if (index + 1 < Count())
        {
            Activate(index + 1, true);
            if (FileList* next = ListAt(index + 1); next && next->Count() > 0)
                next->SelectFirst();
        }
        return true;
    }
    return false;
}

LRESULT ColumnView::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    const int x = GET_X_LPARAM(lp);
    switch (msg)
    {
    case WM_SIZE:
        Layout();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        BufferedPaint paint(m_hwnd);
        const HDC hdc = paint.dc();
        RECT client{};
        GetClientRect(m_hwnd, &client);
        FillSolid(hdc, client, m_theme->content);
        for (int i = 0; i < Count(); ++i)
        {
            const RECT& r = m_columns[i]->rc;
            const int mid = r.right + m_theme->S(kGap) / 2;
            // Files' BladeItem: a resting 1px BorderThickness="0,0,1,0" in DividerStrokeColorDefault; its separate
            // BladeResizer hit zone (4px, Transparent at rest) fills with CardStrokeColorDefaultSolid under the mouse
            // and SolidBackgroundFillColorSecondary while dragging (ResizerPointerOver / ResizerPressed) - both
            // neutral grays, never the accent color
            const bool dragging = i == m_dragGrip;
            const bool hot = dragging || i == m_hotGrip;
            const int w = hot ? m_theme->S(4) : std::max(1, m_theme->S(1));
            const COLORREF color = dragging ? m_theme->columnGripDrag : hot ? m_theme->columnGrip : m_theme->line;
            FillSolid(hdc, { mid - w / 2, 0, mid - w / 2 + w, client.bottom }, color);
        }
        m_bar.Paint(hdc, paint.rect());
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT)
        {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(m_hwnd, &pt);
            if (m_dragGrip >= 0 || (!m_bar.Over(pt) && !m_bar.Captured() && GripAt(pt.x) >= 0))
            {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
        }
        break;
    case WM_MOUSEMOVE:
    {
        if (m_dragGrip >= 0 && m_dragGrip < Count())
        {
            // Files' BladeResizer: no smaller than MINIMUM_WIDTH; a hand-sized column keeps its width
            Column* c = m_columns[m_dragGrip].get();
            c->width = std::max(m_dragStartWidth + (x - m_dragStartX), m_theme->S(kMinWidth));
            c->userWidth = true;
            Layout();
            return 0;
        }
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, m_hwnd, 0 };
        TrackMouseEvent(&tme);
        // The scroll bar first (dragging it, or the pointer on its strip)
        const bool held = m_bar.Captured();
        const bool onBar = m_bar.OnMouseMove({ x, GET_Y_LPARAM(lp) }, (wp & MK_LBUTTON) != 0) || held;
        const int hot = onBar ? -1 : GripAt(x);
        if (hot != m_hotGrip)
        {
            m_hotGrip = hot;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        m_bar.OnMouseLeave();
        if (m_hotGrip >= 0)
        {
            m_hotGrip = -1;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (const POINT pt{ x, GET_Y_LPARAM(lp) }; m_bar.Over(pt))
            m_bar.OnButtonDown(pt);
        else if (const int grip = GripAt(x); grip >= 0)
        {
            m_dragGrip = grip;
            m_dragStartX = x;
            m_dragStartWidth = m_columns[grip]->width;
            SetCapture(m_hwnd);
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (msg == WM_LBUTTONUP ? m_bar.OnButtonUp() : (m_bar.OnCaptureLost(), false))
            return 0;
        if (m_dragGrip >= 0)
        {
            m_dragGrip = -1;
            if (msg == WM_LBUTTONUP && GetCapture() == m_hwnd)
                ReleaseCapture();
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        // Files' BladeResizer_DoubleTapped: back to the width that fits the names
        if (const POINT pt{ x, GET_Y_LPARAM(lp) }; m_bar.Over(pt))
            m_bar.OnButtonDown(pt);
        else if (const int grip = GripAt(x); grip >= 0)
        {
            m_columns[grip]->userWidth = false;
            Measure(grip, false);
        }
        return 0;
    case WM_HSCROLL:
    {
        // (Sent by accessibility tools; the overlay bar scrolls through ScrollTo itself)
        RECT client{};
        GetClientRect(m_hwnd, &client);
        int pos = m_scroll;
        const int line = m_theme->S(40);
        switch (LOWORD(wp))
        {
        case SB_LINELEFT: pos -= line; break;
        case SB_LINERIGHT: pos += line; break;
        case SB_PAGELEFT: pos -= client.right; break;
        case SB_PAGERIGHT: pos += client.right; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = HIWORD(wp); break;
        case SB_LEFT: pos = 0; break;
        case SB_RIGHT: pos = m_content; break;
        }
        ScrollTo(pos);
        return 0;
    }
    case WM_MOUSEHWHEEL:
        ScrollTo(m_scroll + GET_WHEEL_DELTA_WPARAM(wp) * m_theme->S(40) / WHEEL_DELTA);
        return 0;
    case WM_PARENTNOTIFY:
        // A click inside a column makes it the active one (also when its view doesn't report the focus change)
        if (LOWORD(wp) == WM_LBUTTONDOWN || LOWORD(wp) == WM_RBUTTONDOWN)
        {
            const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            for (int i = 0; i < Count(); ++i)
                if (PtInRect(&m_columns[i]->rc, pt))
                {
                    Activate(i, false);
                    break;
                }
        }
        break;
    case WM_TIMER:
        if (wp == kBarTimer)
            m_bar.OnTimer(wp);
        else if (wp == kSelectionTimer)
        {
            if (GetKeyState(VK_LBUTTON) & 0x8000)
                return 0; // still clicking or drawing a selection rectangle: looked at again shortly
            KillTimer(m_hwnd, kSelectionTimer);
            const int column = m_selectionColumn;
            m_selectionColumn = -1;
            if (column >= 0 && column < Count())
                EvaluateSelection(column);
            m_selectionRight = m_selectionMouse = false;
        }
        return 0;
    case WM_DESTROY:
        KillTimer(m_hwnd, kSelectionTimer);
        return 0;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}
