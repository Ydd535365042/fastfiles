#include "FileList.h"
#include "Tab.h"
#include "Lang.h"
#include "DarkMode.h"
#include "Format.h"

// Columns, sorting by header, hover / pressed rows, rename in place and dragging items out.

void FileList::SetupColumns()
{
    for (int i = 0; i < kColumnCount; ++i)
    {
        m_width96[i] = kColumns[i].width;
        HDITEMW item{ HDI_WIDTH | HDI_TEXT | HDI_FORMAT };
        item.cxy = 0; // ApplyColumnWidths
        item.pszText = const_cast<LPWSTR>(Tr(kColumns[i].title));
        item.fmt = HDF_STRING | HDF_LEFT;
        Header_InsertItem(m_header, i, &item);
    }
    ApplyColumnWidths();
}

// The columns at this DPI; a collapsed one is 0 wide and can't be dragged open (Files: Visibility Collapsed)
void FileList::ApplyColumnWidths()
{
    if (!m_header)
        return;
    m_settingWidths = true;
    for (int i = 0; i < kColumnCount; ++i)
    {
        const bool hidden = (m_hiddenColumns >> i) & 1;
        HDITEMW hi{ HDI_WIDTH | HDI_FORMAT };
        Header_GetItem(m_header, i, &hi);
        hi.mask = HDI_WIDTH | HDI_FORMAT;
        hi.cxy = hidden ? 0 : m_theme ? m_theme->S(m_width96[i]) : m_width96[i];
        hi.fmt = hidden || i == ColIcon ? (hi.fmt | HDF_FIXEDWIDTH) : (hi.fmt & ~HDF_FIXEDWIDTH);
        Header_SetItem(m_header, i, &hi);
    }
    m_settingWidths = false;
    m_columnDpi = m_theme ? static_cast<int>(m_theme->dpi) : 96;
    Relayout();
    InvalidateRect(m_header, nullptr, FALSE);
}

namespace
{
constexpr const wchar_t* kColumnKeys[] = { L"", L"", L"tag", L"modified", L"created", L"type", L"size" };
}

unsigned FileList::ColumnBits(const std::vector<std::wstring>& names)
{
    unsigned bits = 0;
    for (int i = ColTag; i < kColumnCount; ++i)
        if (std::find(names.begin(), names.end(), kColumnKeys[i]) != names.end())
            bits |= 1u << i;
    return bits;
}

std::vector<std::wstring> FileList::ColumnNames(unsigned bits)
{
    std::vector<std::wstring> names;
    for (int i = ColTag; i < kColumnCount; ++i)
        if ((bits >> i) & 1)
            names.emplace_back(kColumnKeys[i]);
    return names;
}

void FileList::SetHiddenColumns(unsigned hidden)
{
    hidden &= ~((1u << ColIcon) | (1u << ColName)); // (the name is always there)
    if (hidden == m_hiddenColumns)
        return;
    m_hiddenColumns = hidden;
    ApplyColumnWidths();
}

// Files' ResizeColumnToFit: the widest of the shown texts (+ the sort icon and padding), between the column's limits
void FileList::SizeColumnToFit(int column)
{
    if (column <= ColIcon || column >= kColumnCount || ((m_hiddenColumns >> column) & 1) || !m_theme)
        return;
    const Theme& t = *m_theme;
    HDC dc = GetDC(m_list);
    int widest = 0;
    for (int row = 0; row < std::min(Count(), 1000); ++row)
    {
        const Item& item = m_items[m_order[row]];
        int w = 0;
        switch (column)
        {
        case ColName: w = TextWidth(dc, t.ui, item.display) + t.S(18 + 12); break;
        case ColTag:
            for (const std::wstring& id : item.tags)
                if (const FileTag* tag = Tags::Find(m_tags, id))
                    w += t.S(8 + 16 + 8 + 8 + 4) + std::min(TextWidth(dc, t.caption, tag->name), t.S(200));
            w += w ? t.S(12 + 12) : 0;
            break;
        case ColModified: w = TextWidth(dc, t.caption, Format::DateShort(item.modified)) + t.S(10); break;
        case ColCreated: w = TextWidth(dc, t.caption, Format::DateShort(item.created)) + t.S(10); break;
        case ColType: w = TextWidth(dc, t.caption, item.type) + t.S(10); break;
        case ColSize: w = (item.attrs & FILE_ATTRIBUTE_DIRECTORY) ? 0 : TextWidth(dc, t.caption, Format::Size(item.size)) + t.S(10); break;
        default: break;
        }
        widest = std::max(widest, w);
    }
    ReleaseDC(m_list, dc);
    // NormalMinLength 50, NormalMaxLength (name 1000, others 800), + 36 for the sort icon and padding, + the splitter
    const int max = column == ColName ? 1000 : 800;
    const int fit = std::min(std::max(MulDiv(widest, 96, static_cast<int>(t.dpi)), 50) + 36, max) + 12;
    m_width96[column] = fit;
    ApplyColumnWidths();
}

// Files' column flyout (DetailsLayoutPage HeaderGrid.ContextFlyout)
void FileList::ShowHeaderMenu(POINT screen)
{
    enum : UINT { MFit = 1, MSetDefault, MToggle = 10 };
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, MFit, Tr(L"自动调整所有列宽"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // Files' order and texts: 文件标签, 修改日期, 创建日期列, 类型, 大小列
    const struct { int column; const wchar_t* text; } toggles[] = { { ColTag, L"文件标签" }, { ColModified, L"修改日期" },
        { ColCreated, L"创建日期列" }, { ColType, L"类型" }, { ColSize, L"大小列" } };
    for (const auto& c : toggles)
        AppendMenuW(menu, MF_STRING | (((m_hiddenColumns >> c.column) & 1) ? 0 : MF_CHECKED), MToggle + c.column, Tr(c.text));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, MSetDefault, Tr(L"设为默认"));
    const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, m_hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd == MFit)
        for (int i = ColName; i < kColumnCount; ++i)
            SizeColumnToFit(i);
    else if (cmd == MSetDefault)
        m_events->OnListColumnsDefault(m_hiddenColumns);
    else if (cmd >= MToggle && cmd < MToggle + static_cast<UINT>(kColumnCount))
        SetHiddenColumns(m_hiddenColumns ^ (1u << (cmd - MToggle)));
}

// Files' UpdateSortOptionsCommand: the sorted column again flips the direction, another one sorts ascending by it
void FileList::OnColumnClick(int column)
{
    if (column <= ColIcon || column >= kColumnCount || kColumns[column].key < 0)
        return;
    const auto key = static_cast<SortKey>(kColumns[column].key);
    SetSort(key, key == m_sortKey ? !m_sortDesc : false);
    m_events->OnListSorted(this);
}

void FileList::InvalidateRow(int row)
{
    RECT rc = row >= 0 && row < Count() ? RowRect(row) : RECT{};
    if (rc.bottom > rc.top)
    {
        RECT client{};
        GetClientRect(m_list, &client);
        if (!m_layout.Metrics().wrap)
        {
            rc.left = client.left;
            rc.right = std::max(rc.right, client.right);
        }
        InflateRect(&rc, m_theme->S(4), m_theme->S(4)); // (the focus frame: outside the cells)
        InvalidateRect(m_list, &rc, FALSE);
    }
}

void FileList::SetHot(int row)
{
    if (row == m_hot)
        return;
    InvalidateRow(m_hot);
    m_hot = row;
    InvalidateRow(m_hot);
    // Files' SelectFilesOnHover: a second on the same item
    if (const ViewPrefs* prefs = m_events->ListPrefs(); prefs && prefs->hoverSelect && row >= 0)
        SetTimer(m_hwnd, 2, 1000, nullptr);
    else
        KillTimer(m_hwnd, 2);
}

// Files' SelectionCheckbox: Width 20, centered in the icon column (details: after 24; list: after the Padding 12) with the
// CheckBox's MinHeight 32 as its hit area; cards / grid: Width 32 at the top left, Margin 6 (cards: on the picture box)
RECT FileList::CheckRect(const RECT& row) const
{
    const Theme& t = *m_theme;
    const int mid = (row.top + row.bottom) / 2;
    switch (m_mode)
    {
    case Mode::Column:
        return {}; // ColumnLayoutPage's ListView: SelectionMode Extended, no item checkboxes
    case Mode::List:
    {
        const int column = std::max(t.S(IconSize() + 4), t.S(20)), cx = row.left + t.S(12) + column / 2;
        return { cx - t.S(10), std::max<int>(row.top, mid - t.S(16)), cx + t.S(10), std::min<int>(row.bottom, mid + t.S(16)) };
    }
    case Mode::Cards: case Mode::Grid:
    {
        const int inset = (m_mode == Mode::Cards ? t.S(1) : 0) + t.S(6);
        return { row.left + inset, row.top + inset, row.left + inset + t.S(32), row.top + inset + t.S(32) };
    }
    default:
    {
        const int cx = row.left + t.S(24) + t.S(IconSize() + 4) / 2;
        return { cx - t.S(10), mid - t.S(16), cx + t.S(10), mid + t.S(16) };
    }
    }
}

int FileList::CheckAt(POINT client) const
{
    const int row = RowAt(client);
    if (row < 0)
        return -1;
    const RECT box = CheckRect(RowRect(row));
    return PtInRect(&box, client) ? row : -1;
}

// Files' SelectAllCheckbox: 20x20, Margin -4,14,0,0, centered in the header's icon column (after its Padding 24)
RECT FileList::HeaderCheckRect() const
{
    const Theme& t = *m_theme;
    RECT rc{};
    if (!m_header || !Header_GetItemRect(m_header, ColIcon, &rc))
        return {};
    return { rc.left + t.S(22), t.S(14), rc.left + t.S(42), t.S(34) };
}

// Files' UpdateSelectAllCheckboxState: none -> unchecked, all -> checked, some -> indeterminate
FileList::Check FileList::HeaderCheckState() const
{
    const int selected = SelectedCount();
    return selected == 0 || Count() == 0 ? Check::Off : selected >= Count() ? Check::On : Check::Mixed;
}

void FileList::UpdateHeaderCheck()
{
    if (m_header && HeaderCheckState() != m_headerCheck)
    {
        const RECT box = HeaderCheckRect();
        InvalidateRect(m_header, &box, FALSE);
    }
}

void FileList::SetHotCheck(int row)
{
    if (row == m_hotCheck)
        return;
    InvalidateRow(m_hotCheck);
    m_hotCheck = row;
    InvalidateRow(m_hotCheck);
}

// ItemSelected_Checked / _Unchecked: SelectedItems.Add / Remove, the rest of the selection and the focus stay
void FileList::ToggleRow(int row)
{
    if (row < 0 || row >= Count())
        return;
    UserSelected();
    m_sel.Set(row, !m_sel.Selected(row));
    InvalidateRow(row);
    SelectionChanged();
}

// Files' rename: a text box over the name, the name without its extension selected
bool FileList::BeginRename()
{
    if (m_edit)
        return true;
    int row = FocusedIndex();
    if (row < 0 || !m_sel.Selected(row))
    {
        const std::vector<int> rows = SelectedRows();
        row = rows.empty() ? -1 : rows[0];
    }
    if (row < 0)
        return false;
    EnsureVisible(row);
    UpdateWindow(m_list);
    const RECT rc = RowRect(row);
    if (rc.bottom <= rc.top)
        return false;
    CacheColumns();
    const Theme& t = *m_theme;
    const Item& item = m_items[m_order[row]];
    const RECT box = NameRect(row);
    // In the items window (it scrolls away with it: a scroll commits the rename); centered in the grid (TextAlignment Center)
    const DWORD align = m_mode == Mode::Grid ? ES_CENTER : ES_LEFT;
    m_edit = CreateWindowExW(0, L"EDIT", item.display.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | align, box.left, box.top,
        box.right - box.left, box.bottom - box.top, m_list, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!m_edit)
        return false;
    m_renameFrom = item.name;
    SetWindowSubclass(m_edit, EditProc, 0xFF23, reinterpret_cast<DWORD_PTR>(this));
    SendMessageW(m_edit, WM_SETFONT, reinterpret_cast<WPARAM>(t.ui), FALSE);
    SendMessageW(m_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(t.S(6), t.S(6)));
    DarkMode::ApplyControl(m_edit, t.dark);
    SetWindowPos(m_edit, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    // The name without its extension selected (all of it for folders)
    const size_t dot = (item.attrs & FILE_ATTRIBUTE_DIRECTORY) ? std::wstring::npos : item.display.find_last_of(L'.');
    const int end = dot == std::wstring::npos || dot == 0 ? -1 : static_cast<int>(dot);
    SetFocus(m_edit);
    SendMessageW(m_edit, EM_SETSEL, 0, end);
    return true;
}

void FileList::CommitRename(bool apply)
{
    if (!m_edit)
        return;
    const HWND edit = m_edit;
    m_edit = nullptr; // (DestroyWindow's focus change posts kMsgRename: nothing to do by then)
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(edit)) + 1, L'\0');
    text.resize(GetWindowTextW(edit, text.data(), static_cast<int>(text.size())));
    const bool hadFocus = GetFocus() == edit;
    DestroyWindow(edit);
    if (hadFocus)
        SetFocus(m_list);
    const std::wstring from = m_renameFrom;
    m_renameFrom.clear();
    const int row = RowOf(from);
    if (!apply || row < 0)
        return;
    const Item& item = m_items[m_order[row]];
    while (!text.empty() && (text.back() == L' ' || text.back() == L'.'))
        text.pop_back();
    while (!text.empty() && text.front() == L' ')
        text.erase(text.begin());
    if (text.empty() || text == item.display || text.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos)
        return; // (Files shows "名称不能包含..." and keeps the box open; here nothing happens)
    // A hidden extension stays (Explorer appends it back)
    std::wstring newName = text;
    if (item.display.size() < item.name.size() && item.name.compare(0, item.display.size(), item.display) == 0)
        newName += item.name.substr(item.display.size());
    ComPtr<IFileOperation> op;
    ComPtr<IShellItem> shellItem;
    PCUITEMID_CHILD child = ChildOf(m_order[row]);
    if (!child || FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op))) ||
        FAILED(SHCreateItemWithParent(m_folder.get(), m_shellFolder.Get(), child, IID_PPV_ARGS(&shellItem))))
        return;
    op->SetOwnerWindow(GetAncestor(m_hwnd, GA_ROOT));
    op->SetOperationFlags(FOF_ALLOWUNDO | FOFX_ADDUNDORECORD);
    if (SUCCEEDED(op->RenameItem(shellItem.Get(), newName.c_str(), nullptr)) && SUCCEEDED(op->PerformOperations()))
    {
        SelectWhenListed(newName);
        Reload();
    }
}

void FileList::DragOut()
{
    std::vector<PCUITEMID_CHILD> children;
    for (int row : SelectedRows())
        if (PCUITEMID_CHILD child = ChildOf(m_order[row]))
            children.push_back(child);
    ComPtr<IDataObject> data;
    if (children.empty() || FAILED(m_shellFolder->GetUIObjectOf(m_hwnd, static_cast<UINT>(children.size()), children.data(), IID_IDataObject,
            nullptr, reinterpret_cast<void**>(data.GetAddressOf()))))
        return;
    DWORD effect = 0;
    SHDoDragDrop(m_hwnd, data.Get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
}
