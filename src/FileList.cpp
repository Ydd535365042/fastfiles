#include "FileList.h"
#include "Tab.h"
#include "DarkMode.h"
#include "Format.h"
#include "Sandbox.h"
#include <commoncontrols.h>

namespace
{
constexpr wchar_t kClass[] = L"FastFiles.FileList", kItemsClass[] = L"FastFiles.FileItems";
constexpr UINT_PTR kReloadTimer = 1, kHoverTimer = 2, kScrollTimer = 4; // (3: kDragOpenTimer) on the container / the items
constexpr UINT_PTR kHeaderSubclass = 0xFF22, kEditSubclass = 0xFF23;

bool NavigationKey(WPARAM key)
{
    return key == VK_UP || key == VK_DOWN || key == VK_PRIOR || key == VK_NEXT || key == VK_HOME || key == VK_END ||
        key == VK_LEFT || key == VK_RIGHT;
}
}

FileList::FileList(FileListEvents* events, const Theme* theme) : m_events(events), m_theme(theme), m_sortKey(SortKey::Name) {}

FileList::~FileList()
{
    m_generation->fetch_add(1); // a worker still listing posts nothing usable now
    StopThumbs();
    ReleaseAccessible();
    Unwatch();
    if (m_list)
        RevokeDragDrop(m_list);
    ClearDrop();
    if (m_hwnd)
    {
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
        DestroyWindow(m_hwnd); // the list, its header and a rename box with it
    }
    FreeItems();
    if (m_rowImages)
        ImageList_Destroy(m_rowImages);
    for (HGDIOBJ o : { static_cast<HGDIOBJ>(m_headerFont), static_cast<HGDIOBJ>(m_glyphFont), static_cast<HGDIOBJ>(m_groupFont),
             static_cast<HGDIOBJ>(m_strongFont), static_cast<HGDIOBJ>(m_editBrush) })
        if (o)
            DeleteObject(o);
}

void FileList::FreeItems()
{
    for (Item& item : m_items)
        if (item.child)
            CoTaskMemFree(item.child);
    m_items.clear();
    m_order.clear();
    m_selection.Reset();
    m_selectionDirty = true;
}

bool FileList::Create(HWND parent)
{
    static const bool registered = []
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = HostProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        WNDCLASSEXW items = wc;
        items.style = CS_DBLCLKS;
        items.lpfnWndProc = ItemsProc;
        items.lpszClassName = kItemsClass;
        return RegisterClassExW(&wc) != 0 && RegisterClassExW(&items) != 0;
    }();
    if (!registered)
        return false;
    RECT rc{};
    GetClientRect(parent, &rc);
    m_hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, kClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, rc.right, rc.bottom,
        parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!m_hwnd)
        return false;
    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    // The column header (details) above the items; both children of the container
    m_header = CreateWindowExW(0, WC_HEADERW, L"", WS_CHILD | HDS_HORZ | HDS_BUTTONS | HDS_FULLDRAG, 0, 0, rc.right, 0, m_hwnd, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    m_list = CreateWindowExW(0, kItemsClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0,
        rc.right, rc.bottom, m_hwnd, nullptr, GetModuleHandleW(nullptr), this);
    if (!m_list || !m_header)
        return false;
    SetWindowSubclass(m_header, HeaderProc, kHeaderSubclass, reinterpret_cast<DWORD_PTR>(this));
    SHFILEINFOW sfi{};
    m_icons = reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(L"x", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
        SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON));
    m_linkOverlay = SHGetIconOverlayIndexW(nullptr, IDO_SHGIOI_LINK);
    // The larger system icons for the larger layouts (the same indexes as the small ones)
    const int kLists[4] = { SHIL_SMALL, SHIL_LARGE, SHIL_EXTRALARGE, SHIL_JUMBO };
    for (int i = 0; i < 4; ++i)
        SHGetImageList(kLists[i], IID_IImageList, reinterpret_cast<void**>(&m_iconLists[i]));
    m_dropTarget.owner = this;
    RegisterDragDrop(m_list, &m_dropTarget);
    SetupColumns();
    ApplyTheme();
    return true;
}

void FileList::Move(const RECT& rc)
{
    if (m_hwnd)
        SetWindowPos(m_hwnd, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

int FileList::SelectedCount() const { return m_sel.SelectedCount(); }

int FileList::FocusedIndex() const { return m_sel.focus >= 0 && m_sel.focus < Count() ? m_sel.focus : -1; }

std::vector<int> FileList::SelectedRows() const
{
    std::vector<int> rows;
    for (int i = m_sel.Next(-1); i >= 0 && i < Count(); i = m_sel.Next(i))
        rows.push_back(i);
    return rows;
}

LRESULT CALLBACK FileList::HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* self = reinterpret_cast<FileList*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg)
    {
    case WM_SIZE:
        if (self && self->m_list)
            self->Relayout();
        return 0;
    case WM_ERASEBKGND:
        return 1; // the list covers it
    case WM_SETFOCUS:
        if (self && self->m_list)
            SetFocus(self->m_list);
        return 0;
    case WM_NOTIFY:
        if (self && self->m_header && reinterpret_cast<NMHDR*>(lp)->hwndFrom == self->m_header)
            return self->OnHeaderNotify(reinterpret_cast<NMHEADERW*>(lp));
        break;
    case WM_CONTEXTMENU:
        // The header's right click (Files' HeaderGrid.ContextFlyout)
        if (self && self->m_header && reinterpret_cast<HWND>(wp) == self->m_header)
        {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (pt.x == -1 && pt.y == -1)
            {
                pt = { 8, 8 };
                ClientToScreen(self->m_header, &pt);
            }
            self->ShowHeaderMenu(pt);
            return 0;
        }
        break;
    case WM_CTLCOLOREDIT:
        if (self && self->m_theme)
        {
            SetTextColor(reinterpret_cast<HDC>(wp), self->m_theme->ink);
            SetBkColor(reinterpret_cast<HDC>(wp), self->m_theme->field);
            return reinterpret_cast<LRESULT>(self->m_editBrush);
        }
        break;
    case kMsgListed:
    {
        std::unique_ptr<Listing> listing(reinterpret_cast<Listing*>(lp));
        if (self)
            self->TakeListing(listing.release());
        return 0;
    }
    case kMsgIcons:
    {
        std::unique_ptr<std::vector<std::pair<int, int>>> icons(reinterpret_cast<std::vector<std::pair<int, int>>*>(lp));
        if (self)
            self->TakeIcons(icons.release(), static_cast<unsigned>(wp));
        return 0;
    }
    case kMsgThumb:
        if (self)
            self->TakeThumb(lp, static_cast<unsigned>(wp));
        else
            DropThumb(lp);
        return 0;
    case kMsgChanged:
    {
        // Something in the folder changed: list it again shortly (a copy sends many of these)
        PIDLIST_ABSOLUTE* pidls = nullptr;
        LONG event = 0;
        if (HANDLE lock = SHChangeNotification_Lock(reinterpret_cast<HANDLE>(wp), static_cast<DWORD>(lp), &pidls, &event))
            SHChangeNotification_Unlock(lock);
        if (self)
            SetTimer(hwnd, kReloadTimer, 300, nullptr);
        return 0;
    }
    case kMsgRename:
        if (self && self->m_edit)
            self->CommitRename(true);
        return 0;
    case WM_TIMER:
        KillTimer(hwnd, wp);
        if (self && wp == kReloadTimer)
        {
            if (self->m_edit || self->m_loading)
                SetTimer(hwnd, kReloadTimer, 300, nullptr); // after the rename box / the listing under way
            else
                self->Reload();
        }
        if (self && wp == kDragOpenTimer)
            self->OpenDropRow();
        if (self && wp == kRevealTimer && self->m_held)
        {
            self->m_blank = false;
            self->TakeListing(self->m_held.release());
        }
        if (self && wp == kHoverTimer && self->m_hot >= 0 && self->SelectedCount() <= 1 && self->m_hot < self->Count())
            self->FocusRow(self->m_hot, true, false, false); // Files' SelectFilesOnHover: a second on the same item selects it
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK FileList::ItemsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* self = reinterpret_cast<FileList*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self || !self->m_theme)
        return DefWindowProcW(hwnd, msg, wp, lp);
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    bool handled = false;
    if (const LRESULT r = self->ListMessage(msg, wp, lp, handled); handled)
        return r;
    switch (msg)
    {
    case WM_PAINT:
        self->Paint(hwnd, false);
        return 0;
    case WM_PRINTCLIENT:
    {
        // PrintWindow: the same picture into the caller's DC
        RECT client{};
        GetClientRect(hwnd, &client);
        self->DrawItems(reinterpret_cast<HDC>(wp), client);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; // Paint fills everything
    case WM_SIZE:
        self->Relayout();
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_CTLCOLOREDIT:
        return SendMessageW(self->m_hwnd, msg, wp, lp); // the rename box: the container's colors
    case WM_SETFOCUS: case WM_KILLFOCUS:
        if (self->FocusedIndex() >= 0)
            self->InvalidateRow(self->FocusedIndex());
        if (msg == WM_SETFOCUS)
            self->m_events->OnListFocus();
        return 0;
    case WM_VSCROLL: case WM_HSCROLL:
    {
        const bool vertical = msg == WM_VSCROLL;
        SCROLLINFO si{ sizeof(si), SIF_ALL };
        GetScrollInfo(hwnd, vertical ? SB_VERT : SB_HORZ, &si);
        const int line = self->m_theme->S(vertical ? 36 : 24);
        int pos = si.nPos;
        switch (LOWORD(wp))
        {
        case SB_LINEUP: pos -= line; break;
        case SB_LINEDOWN: pos += line; break;
        case SB_PAGEUP: pos -= static_cast<int>(si.nPage); break;
        case SB_PAGEDOWN: pos += static_cast<int>(si.nPage); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = si.nMax; break;
        default: break;
        }
        (vertical ? self->m_smoothY : self->m_smoothX).Reset(pos); // the scrollbar itself: no wheel animation to fight
        self->ScrollTo(vertical ? self->m_scroll.x : pos, vertical ? pos : self->m_scroll.y);
        return 0;
    }
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
    {
        if (msg == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL))
        {
            // Files: Ctrl+wheel up / down is 增加大小 / 减小大小
            if (const int delta = GET_WHEEL_DELTA_WPARAM(wp); delta != 0)
                self->m_events->OnListZoom(delta > 0 ? 1 : -1);
            return 0;
        }
        // (the list layout scrolls only sideways: the wheel moves it that way)
        const bool sideways = self->m_layout.Metrics().wrap && self->m_layout.Metrics().vertical;
        const bool horizontal = msg == WM_MOUSEHWHEEL || shift || sideways;
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        const int delta = GET_WHEEL_DELTA_WPARAM(wp) * (msg == WM_MOUSEHWHEEL ? -1 : 1);
        RECT client{};
        GetClientRect(hwnd, &client);
        const int step = lines == WHEEL_PAGESCROLL ? client.bottom : static_cast<int>(lines) * self->m_theme->S(36) / 3;
        const int maxX = std::max(0, self->m_layout.ContentWidth() - static_cast<int>(client.right));
        const int maxY = std::max(0, self->m_layout.ContentHeight() - static_cast<int>(client.bottom));
        // Files' ScrollViewer: a ~250 ms eased animation per notch, retargeted (not queued) by the next one; a
        // touchpad's high-resolution delta follows the finger with no added lag (SmoothScroll::OnWheel).
        if (horizontal)
            self->ScrollTo(self->m_smoothX.OnWheel(hwnd, delta, step, maxX), self->m_scroll.y);
        else
            self->ScrollTo(self->m_scroll.x, self->m_smoothY.OnWheel(hwnd, delta, step, maxY));
        self->OnMouseMove(self->m_downRow >= 0 || self->m_marquee ? self->m_downAt : POINT{ -1, -1 }, 0);
        return 0;
    }
    case WM_KEYDOWN:
        if (self->m_events->OnListKey(wp))
            return 0; // (the column view's Left / Right / Space)
        if (wp == VK_F2 && !ctrl && !alt)
            return self->BeginRename(), 0;
        if (wp == 'A' && ctrl && !shift && !alt)
            return self->SelectAll(), 0;
        if ((wp == 'C' || wp == 'X') && ctrl && !shift && !alt)
            return self->Invoke(wp == 'C' ? "copy" : "cut", false), 0;
        if (wp == VK_RETURN && !ctrl && !alt)
        {
            if (self->SelectedCount() > 0)
                self->OpenSelection();
            return 0;
        }
        if (!alt && self->OnKey(wp))
            return 0;
        break;
    case WM_CHAR:
        if (wp >= 32 && !ctrl)
            self->OnChar(static_cast<wchar_t>(wp));
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && alt)
            return self->Invoke("properties", self->SelectedCount() == 0), 0; // Files' OpenProperties
        break;
    case WM_CONTEXTMENU:
    {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (self->m_header && reinterpret_cast<HWND>(wp) == self->m_header)
            return SendMessageW(self->m_hwnd, msg, wp, lp); // the header's (the container shows its flyout)
        if (pt.x == -1 && pt.y == -1)
        {
            // Apps key / Shift+F10: at the focused item, else the top left
            const int focused = self->FocusedIndex();
            const RECT rc = focused >= 0 ? self->RowRect(focused) : RECT{};
            if (self->SelectedCount() && rc.bottom > rc.top)
                pt = { rc.left + self->m_theme->S(60), rc.bottom };
            else
                pt = { 8, 8 };
            ClientToScreen(hwnd, &pt);
        }
        self->m_events->OnListMenu(hwnd, pt, self->SelectedCount() == 0);
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (self->m_pressedCheck >= 0)
        {
            // A row's box held down: it alone follows the mouse (no marquee, no drag)
            const int check = self->CheckAt(pt);
            self->SetHotCheck(check == self->m_pressedCheck ? check : -1);
            return 0;
        }
        self->OnMouseMove(pt, wp);
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        self->SetHot(-1);
        if (self->m_pressedCheck < 0)
            self->SetHotCheck(-1);
        return 0;
    case WM_LBUTTONUP:
        if (self->m_pressedCheck >= 0)
        {
            const int row = self->m_pressedCheck;
            self->m_pressedCheck = -1; // (before ReleaseCapture's WM_CAPTURECHANGED)
            ReleaseCapture();
            if (self->CheckAt({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }) == row)
                self->ToggleRow(row);
            self->InvalidateRow(row);
            return 0;
        }
        self->OnButtonUp({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
        return 0;
    case WM_CAPTURECHANGED:
        if (self->m_pressedCheck >= 0)
        {
            self->InvalidateRow(self->m_pressedCheck);
            self->m_pressedCheck = -1;
        }
        if (self->m_marquee || self->m_downRow >= 0 || self->m_pressed >= 0)
        {
            KillTimer(hwnd, kScrollTimer);
            self->InvalidateRow(self->m_pressed);
            self->m_marquee = false;
            self->m_downRow = self->m_pressed = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_TIMER:
        if (wp == kScrollTimer && self->m_marquee)
        {
            // The band at an edge scrolls the items under it
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            RECT client{};
            GetClientRect(hwnd, &client);
            const int edge = self->m_theme->S(20);
            const int step = self->m_theme->S(18);
            const int dy = pt.y < edge ? -step : pt.y > client.bottom - edge ? step : 0;
            const int dx = pt.x < edge ? -step : pt.x > client.right - edge ? step : 0;
            if (dy || dx)
            {
                if (dx)
                    self->m_smoothX.Reset(self->m_scroll.x + dx);
                if (dy)
                    self->m_smoothY.Reset(self->m_scroll.y + dy);
                self->ScrollTo(self->m_scroll.x + dx, self->m_scroll.y + dy);
                self->UpdateMarquee(pt);
            }
        }
        return 0;
    case SmoothFrame::kMsgTick:
    {
        // One frame of the wheel's ease-out, paced to the monitor's refresh rate (SmoothFrame), not a 15 ms timer.
        const int x = self->m_smoothX.Animating() ? self->m_smoothX.Tick() : self->m_scroll.x;
        const int y = self->m_smoothY.Animating() ? self->m_smoothY.Tick() : self->m_scroll.y;
        self->ScrollTo(x, y);
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_LBUTTONDBLCLK:
    {
        if (self->m_edit)
            self->CommitRename(true);
        // Files' SelectionCheckbox takes the press (its double tap is handled too: no open)
        if (const int check = msg == WM_RBUTTONDOWN ? -1 : self->CheckAt({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }); check >= 0)
        {
            SetFocus(hwnd);
            SetCapture(hwnd);
            self->m_pressedCheck = check;
            self->SetHotCheck(check);
            self->InvalidateRow(check);
            return 0;
        }
        self->OnButtonDown(msg, { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }, wp);
        return 0;
    }
    case WM_RBUTTONUP:
        if (self->m_downRow >= 0)
            self->m_downRow = -1;
        break; // WM_CONTEXTMENU follows
    case WM_RBUTTONDBLCLK:
        return 0;
    case WM_GETOBJECT:
        return self->OnGetObject(wp, lp);
    case WM_NCDESTROY:
        self->ReleaseAccessible();
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT FileList::OnHeaderNotify(NMHEADERW* nm)
{
    switch (nm->hdr.code)
    {
    case HDN_BEGINTRACKW:
        return nm->iItem == 0 ? TRUE : FALSE; // the icon column is fixed
    case HDN_DIVIDERDBLCLICKW:
        SizeColumnToFit(nm->iItem); // Files' GridSplitter_DoubleTapped
        return 0;
    case HDN_ITEMCLICKW:
        OnColumnClick(nm->iItem);
        return 0;
    case HDN_ITEMCHANGINGW:
        if (nm->pitem && (nm->pitem->mask & HDI_WIDTH) && !m_settingWidths) // (only what the user drags)
        {
            if (nm->iItem == 0)
                return TRUE;
            // Files' NormalMinLength (50) plus the grid splitter
            nm->pitem->cxy = std::max(nm->pitem->cxy, m_theme ? m_theme->S(62) : 62);
        }
        return FALSE;
    case HDN_ITEMCHANGEDW:
        // A width dragged by hand is the column's width from now on (also while it's hidden)
        if (nm->pitem && (nm->pitem->mask & HDI_WIDTH) && !m_settingWidths && nm->iItem > 0 && nm->iItem < kColumnCount &&
            !((m_hiddenColumns >> nm->iItem) & 1) && nm->pitem->cxy > 0 && m_theme)
            m_width96[nm->iItem] = MulDiv(nm->pitem->cxy, 96, static_cast<int>(m_theme->dpi));
        if (!m_settingWidths)
            Relayout();
        return 0;
    }
    return 0;
}

LRESULT CALLBACK FileList::HeaderProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
{
    auto* self = reinterpret_cast<FileList*>(data);
    switch (msg)
    {
    case WM_PAINT:
        self->Paint(hwnd, true);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wp != kSortTurnTimer)
            break;
        // The sort icon's next frame; the last one draws it at rest
        if (self->m_sortTurnStart && GetTickCount64() - self->m_sortTurnStart >= static_cast<ULONGLONG>(kSortTurnMs))
        {
            self->m_sortTurnStart = 0;
            KillTimer(hwnd, kSortTurnTimer);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE:
    {
        const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        const RECT box = self->HeaderCheckRect();
        const bool onCheck = PtInRect(&box, pt) != FALSE;
        if (onCheck != self->m_headerCheckHot)
        {
            self->m_headerCheckHot = onCheck;
            InvalidateRect(hwnd, &box, FALSE);
        }
        if (self->m_headerCheckPressed)
            return 0;
        HDHITTESTINFO hit{ pt };
        int item = static_cast<int>(SendMessageW(hwnd, HDM_HITTEST, 0, reinterpret_cast<LPARAM>(&hit)));
        if (!(hit.flags & HHT_ONHEADER) || item <= ColIcon || item >= kColumnCount || kColumns[item].key < 0)
            item = -1;
        if (item != self->m_headerHot)
        {
            self->m_headerHot = item;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        break;
    }
    case WM_MOUSELEAVE:
        if (self->m_headerHot >= 0 || self->m_headerCheckHot)
        {
            self->m_headerHot = -1;
            self->m_headerCheckHot = self->m_headerCheckPressed && self->m_headerCheckHot;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    // Files' SelectAllCheckbox: a click (press and release on it) checks it from unchecked, else unchecks it
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
    {
        const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        const RECT box = self->HeaderCheckRect();
        if (!PtInRect(&box, pt))
            break;
        SetCapture(hwnd);
        self->m_headerCheckPressed = self->m_headerCheckHot = true;
        InvalidateRect(hwnd, &box, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
        if (self->m_headerCheckPressed)
        {
            self->m_headerCheckPressed = false;
            ReleaseCapture();
            const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            const RECT box = self->HeaderCheckRect();
            InvalidateRect(hwnd, &box, FALSE);
            if (PtInRect(&box, pt))
            {
                if (self->HeaderCheckState() == Check::Off)
                    self->SelectAll();
                else
                    self->ClearSelection();
                SetFocus(self->m_list);
            }
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (self->m_headerCheckPressed)
        {
            self->m_headerCheckPressed = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, HeaderProc, kHeaderSubclass);
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK FileList::EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
{
    auto* self = reinterpret_cast<FileList*>(data);
    switch (msg)
    {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        if (wp == VK_RETURN || wp == VK_ESCAPE)
        {
            self->CommitRename(wp == VK_RETURN);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp == VK_RETURN || wp == VK_ESCAPE)
            return 0; // no beep
        break;
    case WM_KILLFOCUS:
        PostMessageW(self->m_hwnd, kMsgRename, 0, 0);
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, EditProc, kEditSubclass);
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

std::wstring FileList::CellText(int row, int column) const
{
    if (row < 0 || row >= Count())
        return {};
    const Item& item = m_items[m_order[row]];
    switch (column)
    {
    case ColIcon: case ColName: return item.display; // (sub-item 0 is the name, as in the shell's list)
    case ColModified: return Format::DateShort(item.modified);
    case ColCreated: return Format::DateShort(item.created);
    case ColType: return item.type;
    case ColSize: return (item.attrs & FILE_ATTRIBUTE_DIRECTORY) ? std::wstring() : Format::Size(item.size);
    default: return {};
    }
}

// The list view messages scripts and tools read (the tests, from another process: the pointers are in our memory)
LRESULT FileList::ListMessage(UINT msg, WPARAM wp, LPARAM lp, bool& handled)
{
    handled = true;
    const int row = static_cast<int>(static_cast<INT_PTR>(wp));
    switch (msg)
    {
    case LVM_GETITEMCOUNT: return Count();
    case LVM_GETVIEW:
        switch (m_mode)
        {
        case Mode::List: return LV_VIEW_LIST;
        case Mode::Cards: return LV_VIEW_TILE;
        case Mode::Grid: return LV_VIEW_ICON;
        default: return LV_VIEW_DETAILS; // (a column: rows, like the shell's name-only details view it replaced)
        }
    case LVM_GETHEADER: return reinterpret_cast<LRESULT>(m_header);
    case LVM_GETSELECTEDCOUNT: return SelectedCount();
    case LVM_GETNEXTITEM:
    {
        const UINT flags = LOWORD(lp);
        if (flags & LVNI_FOCUSED)
            return FocusedIndex() > row ? FocusedIndex() : -1;
        if (flags & LVNI_SELECTED)
            return m_sel.Next(row);
        return row + 1 < Count() ? row + 1 : -1;
    }
    case LVM_GETITEMSTATE:
        return (m_sel.Selected(row) ? LVIS_SELECTED : 0) | (row == FocusedIndex() ? LVIS_FOCUSED : 0);
    case LVM_SETITEMSTATE:
    {
        const auto* item = reinterpret_cast<const LVITEMW*>(lp);
        if (!item)
            return FALSE;
        UserSelected();
        if (item->stateMask & LVIS_SELECTED)
        {
            if (row < 0)
                m_sel.All((item->state & LVIS_SELECTED) != 0);
            else
                m_sel.Set(row, (item->state & LVIS_SELECTED) != 0);
        }
        if ((item->stateMask & LVIS_FOCUSED) && row >= 0 && row < Count())
            m_sel.focus = (item->state & LVIS_FOCUSED) ? row : (m_sel.focus == row ? -1 : m_sel.focus);
        SelectionChanged();
        return TRUE;
    }
    case LVM_GETITEMTEXTW:
    {
        auto* item = reinterpret_cast<LVITEMW*>(lp);
        if (!item || !item->pszText || item->cchTextMax <= 0)
            return 0;
        const std::wstring text = CellText(row, item->iSubItem);
        lstrcpynW(item->pszText, text.c_str(), item->cchTextMax);
        return static_cast<LRESULT>(std::min<size_t>(text.size(), static_cast<size_t>(item->cchTextMax - 1)));
    }
    case LVM_GETITEMRECT:
    {
        auto* rc = reinterpret_cast<RECT*>(lp);
        if (!rc || row < 0 || row >= Count())
            return FALSE;
        *rc = RowRect(row);
        return TRUE;
    }
    case LVM_ENSUREVISIBLE:
        EnsureVisible(row);
        return TRUE;
    case LVM_GETGROUPCOUNT:
        return static_cast<LRESULT>(m_groups.size());
    case LVM_GETGROUPINFOBYINDEX:
    {
        // The group's header text (its title) and its first row as the id
        auto* group = reinterpret_cast<LVGROUP*>(lp);
        if (!group || row < 0 || row >= static_cast<int>(m_groupInfo.size()))
            return FALSE;
        if ((group->mask & LVGF_HEADER) && group->pszHeader && group->cchHeader > 0)
            lstrcpynW(group->pszHeader, m_groupInfo[static_cast<size_t>(row)].title.c_str(), group->cchHeader);
        if (group->mask & LVGF_GROUPID)
            group->iGroupId = m_groups[static_cast<size_t>(row)].first;
        return TRUE;
    }
    }
    handled = false;
    return 0;
}

// --- Layout and scrolling ---

void FileList::Relayout(bool keepFocusVisible)
{
    if (!m_list || !m_theme || m_inLayout)
        return;
    m_inLayout = true;
    const Theme& t = *m_theme;
    CacheColumns();
    int columns = 0;
    for (int i = 0; i < kColumnCount; ++i)
        columns = std::max(columns, m_colRight[i]);
    // Two passes: a scroll bar coming or going changes the client size
    for (int pass = 0; pass < 2; ++pass)
    {
        RECT client{};
        GetClientRect(m_list, &client);
        ItemsMetrics m = Metrics(client.right);
        if (!m.wrap)
            m.rowWidth = m_mode == Mode::Column ? client.right : std::max<int>(columns, client.right); // (a column: no columns)
        m_layout.Build(Count(), m_groups, m, client.right, client.bottom);
        // Details: both ways (the columns can be wider); cards / grid: down only; list: sideways only
        const bool sideways = m.wrap && m.vertical;
        SCROLLINFO si{ sizeof(si), SIF_RANGE | SIF_PAGE };
        si.nMax = sideways ? 0 : std::max(0, m_layout.ContentHeight() - 1);
        si.nPage = static_cast<UINT>(std::max<LONG>(0, client.bottom));
        SetScrollInfo(m_list, SB_VERT, &si, TRUE);
        si.nMax = std::max(0, (sideways ? m_layout.ContentWidth() : m.wrap || m_mode == Mode::Column ? 0 : columns) - 1);
        si.nPage = static_cast<UINT>(std::max<LONG>(0, client.right));
        SetScrollInfo(m_list, SB_HORZ, &si, TRUE);
    }
    m_inLayout = false;
    {
        RECT client{};
        GetClientRect(m_list, &client);
        // A relayout (resize, item count changed) while the wheel's still animating: keep the target in range
        // instead of letting Tick() carry it past the new (possibly shorter) content.
        m_smoothX.ClampTarget(std::max(0, m_layout.ContentWidth() - static_cast<int>(client.right)));
        m_smoothY.ClampTarget(std::max(0, m_layout.ContentHeight() - static_cast<int>(client.bottom)));
    }
    ScrollTo(m_scroll.x, m_scroll.y);
    PlaceHeader();
    if (keepFocusVisible && FocusedIndex() >= 0)
        EnsureVisible(FocusedIndex());
    InvalidateRect(m_list, nullptr, FALSE);
    (void)t;
}
// Files' details rows (36 high, ListView Padding 8,0,8,12) and its group headers: ListViewHeaderItem MinHeight 44,
// Padding 12,8,12,0, Margin 0,0,0,4 around the template (16px SemiBold title, StackPanel Margin 0,0,0,4) and the 1px
// divider 8 under it; with the count below the title (tags) the template is a line taller
// (GridViewHeaderItem: the same MinHeight 44 and Margin 4)
ItemsMetrics FileList::Metrics(int clientWidth) const
{
    const Theme& t = *m_theme;
    ItemsMetrics m;
    m.rowWidth = clientWidth;
    LayoutMetrics(m);
    if (!m_groups.empty())
        m.headerHeight = t.S(m_groupKey == kGroupTag ? 69 : 48);
    return m;
}

void FileList::PlaceHeader()
{
    if (!m_header || !m_theme)
        return;
    RECT host{};
    GetClientRect(m_hwnd, &host);
    const int h = m_mode == Mode::Details ? m_theme->S(40) : 0; // Files' HeaderGrid (details only)
    if (!h)
    {
        ShowWindow(m_header, SW_HIDE);
        RECT items{};
        GetWindowRect(m_list, &items);
        MapWindowPoints(nullptr, m_hwnd, reinterpret_cast<POINT*>(&items), 2);
        if (items.top != 0 || items.right - items.left != host.right || items.bottom - items.top != host.bottom)
            SetWindowPos(m_list, nullptr, 0, 0, host.right, host.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
    int columns = 0;
    for (int i = 0; i < kColumnCount; ++i)
        columns = std::max(columns, m_colRight[i]);
    // The header moves with the horizontal scroll (it's as wide as the columns, at least the window)
    SetWindowPos(m_header, nullptr, -m_scroll.x, 0, std::max<int>(columns, host.right + m_scroll.x), h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RECT items{};
    GetWindowRect(m_list, &items);
    MapWindowPoints(nullptr, m_hwnd, reinterpret_cast<POINT*>(&items), 2);
    if (items.top != h || items.right - items.left != host.right || items.bottom - items.top != host.bottom - h)
        SetWindowPos(m_list, nullptr, 0, h, host.right, std::max<LONG>(0, host.bottom - h), SWP_NOZORDER | SWP_NOACTIVATE);
}

void FileList::ScrollTo(int x, int y)
{
    if (!m_list)
        return;
    RECT client{};
    GetClientRect(m_list, &client);
    const int maxX = std::max(0, m_layout.ContentWidth() - static_cast<int>(client.right));
    const int maxY = std::max(0, m_layout.ContentHeight() - static_cast<int>(client.bottom));
    const ItemsMetrics& m = m_layout.Metrics();
    const bool sideways = m.wrap && m.vertical;
    x = m.wrap && !sideways ? 0 : std::clamp(x, 0, maxX);
    y = sideways ? 0 : std::clamp(y, 0, maxY);
    if (x == m_scroll.x && y == m_scroll.y)
        return;
    const bool moved = x != m_scroll.x;
    const int dx = x - m_scroll.x, dy = y - m_scroll.y;
    m_scroll = { x, y };
    SetScrollPos(m_list, SB_VERT, y, TRUE);
    SetScrollPos(m_list, SB_HORZ, x, TRUE);
    // A per-frame wheel scroll (vertical-only, smaller than a screenful): slide the already-painted rows with
    // ScrollWindowEx and repaint only the strip it exposes, instead of the whole window every tick. Correct only
    // when nothing composed (the backdrop material is fixed to the window, not the content: Paint() still repaints
    // everything then, same as before) and the move is small enough that most of the picture carries over.
    if (!Backdrop::Active() && dx == 0 && dy != 0 && std::abs(dy) < client.bottom && !m_marquee)
        ScrollWindowEx(m_list, 0, -dy, nullptr, nullptr, nullptr, nullptr, SW_INVALIDATE | SW_ERASE);
    else
        InvalidateRect(m_list, nullptr, FALSE); // painted whole (the material doesn't scroll with the rows)
    if (m_edit)
        CommitRename(true);
    if (moved)
        PlaceHeader();
}

void FileList::EnsureVisible(int row)
{
    const RECT r = m_layout.ItemRect(row);
    if (r.bottom <= r.top || !m_list)
        return;
    RECT client{};
    GetClientRect(m_list, &client);
    int top = r.top;
    // The first row of a group brings its header along
    if (const int g = m_layout.GroupOf(row); g >= 0 && m_layout.Group(g).first == row)
        if (const RECT h = m_layout.HeaderRect(g); h.bottom > h.top)
            top = h.top;
    if (m_layout.Metrics().wrap && m_layout.Metrics().vertical)
    {
        // The list layout: its column into view sideways
        int x = m_scroll.x;
        if (r.left < x)
            x = r.left - m_layout.Metrics().padLeft;
        else if (r.right > x + client.right)
            x = r.right + m_layout.Metrics().padRight - client.right;
        m_smoothX.Reset(x); // a key / selection jump, not the wheel: goes straight there, no animation to fight it
        ScrollTo(x, 0);
        return;
    }
    int y = m_scroll.y;
    if (top < y)
        y = top;
    else if (r.bottom > y + client.bottom)
        y = r.bottom - client.bottom;
    m_smoothY.Reset(y);
    ScrollTo(m_scroll.x, y);
}

RECT FileList::RowRect(int row) const
{
    RECT r = m_layout.ItemRect(row);
    if (r.bottom > r.top)
        OffsetRect(&r, -m_scroll.x, -m_scroll.y);
    return r;
}

int FileList::RowAt(POINT client) const
{
    return m_layout.HitTest({ client.x + m_scroll.x, client.y + m_scroll.y });
}
// --- Mouse and keyboard (WinUI ListViewBase, SelectionMode Extended) ---

void FileList::OnButtonDown(UINT msg, POINT pt, WPARAM keys)
{
    UserSelected();
    SetFocus(m_list);
    m_keyboardFocus = false;
    const int row = RowAt(pt);
    const bool ctrl = (keys & MK_CONTROL) != 0, shift = (keys & MK_SHIFT) != 0;
    if (msg == WM_LBUTTONDBLCLK)
    {
        if (row >= 0)
            OpenSelection();
        else if (const ViewPrefs* prefs = m_events->ListPrefs(); prefs && prefs->doubleClickUp)
            m_events->OnListUp(); // Files' DoubleClickToGoUp
        return;
    }
    if (msg == WM_RBUTTONDOWN)
    {
        // The right click acts on what's under it: an unselected row alone, empty space nothing
        if (row >= 0 && !m_sel.Selected(row))
            FocusRow(row, true, false, false);
        else if (row < 0 && !ctrl && !shift)
        {
            m_sel.All(false);
            SelectionChanged();
        }
        return;
    }
    m_downAt = pt;
    m_downKeys = keys;
    m_downRow = row;
    m_downSelected = row >= 0 && m_sel.Selected(row);
    SetCapture(m_list);
    if (row >= 0)
    {
        // Files' pressed row while the button is down
        m_pressed = row;
        InvalidateRow(row);
        if (shift)
            FocusRow(row, true, true, ctrl);
        else if (!ctrl && !m_downSelected) // (Ctrl: toggled on release, a drag keeps it)
            FocusRow(row, true, false, false);
        return;
    }
    // Empty space: the rubber band (Files' RectangleSelection); without Ctrl / Shift it starts from nothing
    if (!ctrl && !shift)
    {
        m_sel.All(false);
        SelectionChanged();
    }
    m_marquee = true;
    m_marqueeFrom = { pt.x + m_scroll.x, pt.y + m_scroll.y };
    m_marqueeRect = { m_marqueeFrom.x, m_marqueeFrom.y, m_marqueeFrom.x, m_marqueeFrom.y };
    m_marqueeBase = m_sel.Bits();
    SetTimer(m_list, kScrollTimer, 50, nullptr);
}

void FileList::OnButtonUp(POINT pt)
{
    KillTimer(m_list, kScrollTimer);
    const int down = m_downRow, pressed = m_pressed;
    const bool wasMarquee = m_marquee;
    m_downRow = m_pressed = -1;
    m_marquee = false;
    if (GetCapture() == m_list)
        ReleaseCapture();
    InvalidateRow(pressed);
    if (wasMarquee)
    {
        InvalidateRect(m_list, nullptr, FALSE);
        return;
    }
    if (down < 0 || RowAt(pt) != down)
        return;
    if ((m_downKeys & MK_CONTROL) && !(m_downKeys & MK_SHIFT))
        FocusRow(down, true, false, true);   // Ctrl+click: just this row on / off
    else if (!(m_downKeys & MK_SHIFT) && m_downSelected)
        FocusRow(down, true, false, false);  // a click on a selected row (not a drag): it alone
}
void FileList::OnMouseMove(POINT pt, WPARAM keys)
{
    if (pt.x == -1 && pt.y == -1)
    {
        GetCursorPos(&pt);
        ScreenToClient(m_list, &pt);
    }
    if (m_marquee)
    {
        UpdateMarquee(pt);
        return;
    }
    if (m_downRow >= 0 && (keys & MK_LBUTTON))
    {
        // Past the drag distance: the selection goes out as a drag (with the pressed row in it)
        if (std::abs(pt.x - m_downAt.x) > GetSystemMetrics(SM_CXDRAG) || std::abs(pt.y - m_downAt.y) > GetSystemMetrics(SM_CYDRAG))
        {
            const int row = m_downRow;
            m_downRow = -1;
            InvalidateRow(m_pressed);
            m_pressed = -1;
            ReleaseCapture();
            if (!m_sel.Selected(row))
                FocusRow(row, true, false, false);
            DragOut();
        }
        return;
    }
    SetHot(RowAt(pt));
    SetHotCheck(CheckAt(pt));
}

void FileList::UpdateMarquee(POINT client)
{
    const POINT p{ client.x + m_scroll.x, client.y + m_scroll.y };
    m_marqueeRect = { std::min(p.x, m_marqueeFrom.x), std::min(p.y, m_marqueeFrom.y), std::max(p.x, m_marqueeFrom.x), std::max(p.y, m_marqueeFrom.y) };
    // Ctrl adds to (toggles against) the selection from before the band; otherwise it's what the band covers
    const bool ctrl = (m_downKeys & MK_CONTROL) != 0;
    std::vector<unsigned char> bits = m_marqueeBase;
    bits.resize(static_cast<size_t>(Count()), 0);
    for (int row : m_layout.RowsIn(m_marqueeRect))
        bits[static_cast<size_t>(row)] = ctrl ? !m_marqueeBase[static_cast<size_t>(row)] : 1;
    m_sel.SetBits(bits);
    SelectionChanged();
    InvalidateRect(m_list, nullptr, FALSE);
}

void FileList::FocusRow(int row, bool select, bool extend, bool toggleCtrl)
{
    if (row < 0 || row >= Count())
        return;
    const int before = m_sel.focus;
    if (select)
    {
        if (extend)
        {
            // Shift: anchor .. row (Ctrl+Shift adds it to the selection)
            const int anchor = m_sel.anchor >= 0 && m_sel.anchor < Count() ? m_sel.anchor : row;
            if (!toggleCtrl)
                m_sel.All(false);
            m_sel.Range(anchor, row, true);
        }
        else if (toggleCtrl)
        {
            m_sel.Set(row, !m_sel.Selected(row));
            m_sel.anchor = row;
        }
        else
        {
            m_sel.All(false);
            m_sel.Set(row, true);
            m_sel.anchor = row;
        }
    }
    else if (!extend)
        m_sel.anchor = row;
    m_sel.focus = row;
    InvalidateRow(before);
    InvalidateRow(row);
    EnsureVisible(row);
    SelectionChanged();
}
bool FileList::OnKey(WPARAM key)
{
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (key == VK_SPACE || NavigationKey(key))
        UserSelected();
    if (key == VK_SPACE)
    {
        // Space selects the focused row (Ctrl+Space toggles it)
        if (const int f = FocusedIndex(); f >= 0)
            FocusRow(f, true, false, ctrl);
        return true;
    }
    if (!NavigationKey(key))
        return false;
    RECT client{};
    GetClientRect(m_list, &client);
    const int from = FocusedIndex();
    const bool sideways = m_layout.Metrics().wrap && m_layout.Metrics().vertical;
    const int to = m_layout.Move(from, static_cast<UINT>(key), sideways ? client.right : client.bottom);
    if (to < 0)
        return true;
    m_keyboardFocus = true;
    // Arrows select; Shift extends from the anchor; Ctrl moves the focus only
    if (shift)
        FocusRow(to, true, true, ctrl);
    else
        FocusRow(to, !ctrl, false, false);
    return true;
}

// Type-ahead: the next item whose name starts with what was typed (the same letter again: the next one with it)
void FileList::OnChar(wchar_t c)
{
    const ULONGLONG now = GetTickCount64();
    if (now - m_typedAt > 1000)
        m_typed.clear();
    m_typedAt = now;
    if (m_typed.empty() && c == L' ')
        return;
    UserSelected();
    m_typed += c;
    const int n = Count();
    if (n == 0)
        return;
    const bool repeat = m_typed.size() > 1 && m_typed.find_first_not_of(m_typed[0]) == std::wstring::npos;
    const std::wstring prefix = repeat ? m_typed.substr(0, 1) : m_typed;
    const int length = static_cast<int>(prefix.size());
    const int focus = FocusedIndex();
    const int start = focus < 0 ? 0 : (repeat || m_typed.size() == 1 ? focus + 1 : focus);
    for (int k = 0; k < n; ++k)
    {
        const int row = (start + k) % n;
        const std::wstring& name = m_items[m_order[row]].display;
        if (static_cast<int>(name.size()) >= length && CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, name.c_str(), length,
                prefix.c_str(), length, nullptr, nullptr, 0) == CSTR_EQUAL)
        {
            FocusRow(row, true, false, false);
            return;
        }
    }
}

void FileList::SelectionChanged()
{
    if (m_sel.version == m_paintedSel)
        return;
    m_paintedSel = m_sel.version;
    m_selectionDirty = true;
    InvalidateRect(m_list, nullptr, FALSE);
    UpdateHeaderCheck();
    NotifyWinEvent(EVENT_OBJECT_SELECTIONWITHIN, m_list, OBJID_CLIENT, CHILDID_SELF);
    if (const int f = FocusedIndex(); f >= 0 && GetFocus() == m_list)
        NotifyWinEvent(EVENT_OBJECT_FOCUS, m_list, OBJID_CLIENT, f + 1);
    if (m_userSel)
    {
        m_userSel = false;
        m_events->OnListUserSelection();
    }
}
