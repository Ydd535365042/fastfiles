// Sidebar window: painting, mouse/keyboard, tooltips, scrolling. Data lives in SidebarModel.cpp, menus and drag/drop in SidebarDrop.cpp.
#include "Sidebar.h"
#include "Format.h"
#include "Lang.h"

namespace
{
constexpr wchar_t kClass[] = L"FastFiles.Sidebar";

std::wstring FormatBytes(ULONGLONG bytes)
{
    return Format::Size(bytes); // Files' ToSizeString
}
}

IFACEMETHODIMP Sidebar::QueryInterface(REFIID riid, void** ppv)
{
    static const QITAB qit[] = { QITABENT(Sidebar, IDropTarget), {} };
    return QISearch(this, qit, riid, ppv);
}

IFACEMETHODIMP_(ULONG) Sidebar::AddRef() { return InterlockedIncrement(&m_ref); }

IFACEMETHODIMP_(ULONG) Sidebar::Release()
{
    const ULONG ref = InterlockedDecrement(&m_ref);
    if (ref == 0)
        delete this;
    return ref;
}

bool Sidebar::Create(HWND parent, int id, const Theme* theme)
{
    static const bool registered = []
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    m_theme = theme;
    if (!registered || !CreateWindowExW(0, kClass, Tr(L"侧边栏"), WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP, 0, 0, 0, 0,
            parent, MenuId(id), GetModuleHandleW(nullptr), this))
        return false;
    m_bar.Attach(m_hwnd, true, kBarTimer, m_theme, [this](int y) { m_smooth.Reset(y); ScrollTo(y); });

    SHFILEINFOW sfi{};
    m_images = reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(L".txt", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
        SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON));

    m_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, m_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    TTTOOLINFOW ti{ sizeof(ti) };
    ti.uFlags = TTF_SUBCLASS;
    ti.hwnd = m_hwnd;
    ti.uId = 1;
    ti.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(m_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    SendMessageW(m_tooltip, TTM_SETMAXTIPWIDTH, 0, m_theme->S(420));

    CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_dropHelper));
    RegisterDragDrop(m_hwnd, this);

    // Drives, media, shares, and renamed/removed folders (pins follow renames like Explorer's Quick Access)
    SHChangeNotifyEntry entry{ nullptr, TRUE };
    m_notify = SHChangeNotifyRegister(m_hwnd, SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
        SHCNE_DRIVEADD | SHCNE_DRIVEREMOVED | SHCNE_MEDIAINSERTED | SHCNE_MEDIAREMOVED | SHCNE_NETSHARE | SHCNE_NETUNSHARE |
        SHCNE_FREESPACE | SHCNE_RENAMEFOLDER | SHCNE_RMDIR | SHCNE_MKDIR | SHCNE_UPDATEIMAGE, kMsgChange, 1, &entry);

    LoadPins();
    LoadTags();
    ScanDevices();
    Relayout();
    return true;
}

void Sidebar::Destroy()
{
    m_smooth.Reset(m_scroll); // drops it from the frame clock before the window goes
    if (m_notify)
        SHChangeNotifyDeregister(m_notify);
    m_notify = 0;
    if (m_hwnd)
    {
        RevokeDragDrop(m_hwnd);
        DestroyWindow(m_hwnd);
    }
    m_hwnd = nullptr;
    if (m_headerFont)
        DeleteObject(m_headerFont);
    m_headerFont = nullptr;
    m_dropHelper.Reset();
    m_dragData.Reset();
    m_itemTarget.Reset();
}

void Sidebar::Move(const RECT& rc)
{
    if (m_hwnd)
        MoveChild(m_hwnd, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
}

void Sidebar::Relayout()
{
    if (m_tooltip)
        SendMessageW(m_tooltip, TTM_SETMAXTIPWIDTH, 0, m_theme->S(420));
    if (m_headerFont)
        DeleteObject(m_headerFont);
    LOGFONTW lf{};
    GetObjectW(m_theme->ui, sizeof(lf), &lf);
    lf.lfWeight = FW_SEMIBOLD;
    m_headerFont = CreateFontIndirectW(&lf);
    Rebuild();
}

void Sidebar::SetSectionState(unsigned hidden, unsigned collapsed)
{
    m_hidden = hidden & ((1u << SecCount) - 1) & ~(1u << SecPinned); // Pinned can't be hidden: it's the drop target
    m_collapsed = collapsed & ((1u << SecCount) - 1);
    Rebuild();
}

void Sidebar::ToggleSection(Section section)
{
    m_collapsed ^= 1u << section;
    Rebuild();
}

// --- Geometry (rows are stored in content coordinates; the client shows [m_scroll, m_scroll + height)) ---

int Sidebar::RowAt(int y) const
{
    const int cy = y + m_scroll;
    for (size_t i = 0; i < m_rows.size(); ++i)
        if (cy >= m_rows[i].top && cy < m_rows[i].top + m_rows[i].height)
            return static_cast<int>(i);
    return -1;
}

// Files: MenuItemsHost Margin 4 + RootPanel Padding 4 on each side; ElementGrid 2px inside the 32px row, 1px border.
// Columns: [3 indicator][depth*16 indent][16 chevron][4 + 16 icon][12 text 12][28 decorator]
RECT Sidebar::RowRect(int row) const
{
    RECT client;
    GetClientRect(m_hwnd, &client);
    const Row& r = m_rows[row];
    return { m_theme->S(8), r.top - m_scroll, client.right - m_theme->S(8), r.top - m_scroll + r.height };
}

RECT Sidebar::ItemBox(int row) const
{
    RECT box = RowRect(row);
    box.top += m_theme->S(2);
    box.bottom -= m_theme->S(2);
    return box;
}

RECT Sidebar::ChevronRect(int row) const
{
    const RECT box = ItemBox(row);
    const int left = box.left + m_theme->S(1 + 3 + 16 * m_rows[row].depth);
    return { left, box.top, left + m_theme->S(16), box.bottom };
}

bool Sidebar::HasEject(int row) const
{
    // The decorator shows in the NoExpansion state only (SidebarStyles.xaml), i.e. removable drives without subfolders
    const Entry* e = EntryOf(row);
    return e && e->removable && !m_rows[row].child && !Expandable(row);
}

RECT Sidebar::EjectRect(int row) const
{
    const RECT box = ItemBox(row);
    const int cx = box.right - m_theme->S(1 + 14), cy = (box.top + box.bottom) / 2, h = m_theme->S(12);
    return { cx - h, cy - h, cx + h, cy + h };
}

void Sidebar::ScrollTo(int offset)
{
    RECT client{};
    if (m_hwnd)
        GetClientRect(m_hwnd, &client);
    const int maxScroll = std::max(0, ContentHeight() - static_cast<int>(client.bottom));
    offset = std::clamp(offset, 0, maxScroll);
    if (offset != m_scroll)
    {
        m_scroll = offset;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    if (m_hwnd && m_theme)
    {
        const RECT strip{ client.right - m_theme->S(12), 0, client.right, client.bottom };
        m_bar.Set(maxScroll > 0 ? ContentHeight() : 0, client.bottom, m_scroll, strip, m_theme->S(32));
    }
}

void Sidebar::EnsureVisible(int row)
{
    if (row < 0 || row >= static_cast<int>(m_rows.size()))
        return;
    RECT client;
    GetClientRect(m_hwnd, &client);
    const Row& r = m_rows[row];
    int y = m_scroll;
    if (r.top < m_scroll)
        y = r.top - m_theme->S(4);
    else if (r.top + r.height > m_scroll + client.bottom)
        y = r.top + r.height - client.bottom + m_theme->S(4);
    if (y == m_scroll)
        return;
    m_smooth.Reset(y); // a key / selection jump, not the wheel: goes straight there
    ScrollTo(y);
}

void Sidebar::SetHot(int row)
{
    if (row == m_hot)
        return;
    m_hot = row;
    InvalidateRect(m_hwnd, nullptr, FALSE);
    // One tool covers the window; move it to the hot row so the tip re-queries its text
    if (m_tooltip)
    {
        SendMessageW(m_tooltip, TTM_POP, 0, 0);
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.hwnd = m_hwnd;
        ti.uId = 1;
        if (row >= 0)
            ti.rect = RowRect(row);
        SendMessageW(m_tooltip, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
    }
}

std::wstring Sidebar::TipFor(int row) const
{
    if (row < 0 || row >= static_cast<int>(m_rows.size()))
        return {};
    const Row& r = m_rows[row];
    if (r.header)
        return {};
    const Entry* e = EntryOf(row);
    if (!e)
        return {};
    if (m_ejectHot && row == m_hot && HasEject(row))
        return Tr(L"弹出");
    if (!e->tagId.empty())
        return e->name;
    // Files: drives show "{free} 可用，共 {total}", locations their path (shell locations their name)
    std::wstring tip = e->totalBytes && r.section != SecCloud ? FormatBytes(e->freeBytes) + Tr(L" 可用，共 ") + FormatBytes(e->totalBytes)
                     : e->parsing.rfind(L"::", 0) == 0 ? e->name : e->parsing;
    if (e->missing)
        tip += Tr(L"\n找不到此位置");
    return tip;
}

void Sidebar::Activate(int row, OpenMode mode)
{
    if (row < 0 || row >= static_cast<int>(m_rows.size()))
        return;
    const Row& r = m_rows[row];
    if (r.header)
    {
        ToggleSection(r.section);
        return;
    }
    Entry* e = EntryOf(row);
    if (!e)
        return;
    if (r.child)
        EnsurePidl(*e); // already enumerated, so parsing is quick
    if (!e->tagId.empty())
        m_events->OnSidebarTag(e->tagId, e->name, mode);
    else if (e->pidl)
        m_events->OnSidebarOpen(e->pidl.get(), mode);
    else if (!e->lazyPath.empty())
        OpenLazy(e->name, e->lazyPath, mode);
}

// --- Painting (blueprint .side / .sh / .si: muted section titles, rounded hover/selected rows, accent pill) ---

void Sidebar::Paint(HDC hdc, const RECT& rc)
{
    const Theme& t = *m_theme;
    FillSolid(hdc, rc, t.layer);
    int iconW = 16, iconH = 16;
    if (m_images)
        ImageList_GetIconSize(m_images, &iconW, &iconH);
    const bool focused = GetFocus() == m_hwnd;
    const int radius = t.S(4); // ControlCornerRadius
    for (size_t i = 0; i < m_rows.size(); ++i)
    {
        const int index = static_cast<int>(i);
        const Row& r = m_rows[i];
        const RECT row = RowRect(index);
        if (row.bottom < rc.top || row.top > rc.bottom)
            continue;
        const RECT box = ItemBox(index);
        const Entry* e = EntryOf(index);
        const bool hint = !r.header && !e;
        const bool hot = index == m_hot && m_dragRow < 0 && !hint;
        const bool selected = index == m_selected;
        const bool down = hot && (index == m_pressed || index == m_chevronPressed);
        const bool drop = index == m_dropRow;

        // SidebarItem PointerStates / SelectionStates: subtle fills for hover/press, a raised control fill when selected
        if (drop)
        {
            FillRound(hdc, box, radius, t.controlFillPressed);
            FrameRound(hdc, box, radius, t.accent);
            FrameRound(hdc, { box.left + 1, box.top + 1, box.right - 1, box.bottom - 1 }, radius, t.accent);
        }
        else if (selected)
        {
            FillRound(hdc, box, radius, down ? t.controlFillPressed : hot ? t.controlFillHover : t.controlFill);
            FrameRound(hdc, box, radius, t.controlStroke);
        }
        else if (down || hot)
            FillRound(hdc, box, radius, down ? t.subtlePressed : t.subtleHover);
        if (selected)
        {
            const int cy = (box.top + box.bottom) / 2;
            FillRound(hdc, { box.left + t.S(1), cy - t.S(8), box.left + t.S(4), cy + t.S(8) }, t.S(2), t.accent);
        }

        const RECT chevron = ChevronRect(index);
        if (hint)
            continue;
        // Chevron E76C, TextFillColorSecondary, 10px; pointing down when expanded
        const bool homeRow = r.section == SecHome;
        if (r.header || Expandable(index))
        {
            const bool open = r.header ? !(m_collapsed & (1u << r.section)) : Expanded(index);
            DrawGlyph(hdc, t.iconsSmall, open ? Glyph::ChevronDown : Glyph::ChevronRight, chevron, t.muted);
        }
        const bool dim = e && (e->missing || e->hidden);
        const int icon = t.S(16);
        const int iconX = chevron.right + t.S(4), iconY = (box.top + box.bottom - icon) / 2;
        if (r.header || homeRow)
            DrawSectionIcon(hdc, r.section, { iconX, iconY, iconX + icon, iconY + icon }, t.dpi);
        else if (e->section == SecTags)
        {
            // Files draws a tag as a colored dot
            const int d = t.S(10), cx = iconX + icon / 2, cy = (box.top + box.bottom) / 2;
            FillRound(hdc, { cx - d / 2, cy - d / 2, cx - d / 2 + d, cy - d / 2 + d }, d / 2, e->tagColor);
        }
        else if (m_images && e->icon >= 0)
            ImageList_Draw(m_images, e->icon, hdc, iconX + (icon - iconW) / 2, (box.top + box.bottom - iconH) / 2,
                ILD_TRANSPARENT | (dim ? ILD_BLEND50 : 0));

        // Text: Margin 12,0,12,2; section headers SemiBold with an 8px margin
        const bool eject = HasEject(index);
        const int textLeft = iconX + icon + t.S(r.header ? 8 : 12);
        const int textRight = box.right - t.S(1) - (eject ? t.S(28) : 0) - t.S(12);
        const RECT text{ textLeft, box.top, textRight, box.bottom - t.S(2) };
        if (r.header)
            DrawLabel(hdc, m_headerFont ? m_headerFont : t.ui, SectionName(r.section), text, t.ink);
        else
            DrawLabel(hdc, t.ui, e->name, text, dim ? Mix(t.ink, t.layer, 50) : t.ink);
        if (eject)
        {
            const RECT er = EjectRect(index);
            if (hot && m_ejectHot)
                FillRound(hdc, er, radius, index == m_ejectPressed ? t.subtlePressed : t.subtleHover);
            DrawEjectIcon(hdc, er, t.ink);
        }
        if (focused && m_keyboardFocus && index == m_focus)
            FrameRound(hdc, box, radius, t.ink);
    }
    if (m_insertAt >= 0)
    {
        RECT client;
        GetClientRect(m_hwnd, &client);
        const int y = m_insertY - m_scroll;
        FillRound(hdc, { t.S(10), y - t.S(1), client.right - t.S(10), y + t.S(2) }, t.S(1), t.accent);
    }
    m_bar.Paint(hdc, rc);
}

LRESULT CALLBACK Sidebar::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
    {
        auto* self = reinterpret_cast<Sidebar*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    }
    auto* self = reinterpret_cast<Sidebar*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCDESTROY)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    return self && self->m_hwnd == hwnd ? self->HandleMessage(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Sidebar::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    // Owner-drawn submenus of the shell context menu ("Send to", "Open with")
    if (m_menuHandler && (msg == WM_INITMENUPOPUP || msg == WM_DRAWITEM || msg == WM_MEASUREITEM || msg == WM_MENUCHAR))
    {
        LRESULT result = 0;
        ComPtr<IContextMenu3> menu3;
        if (SUCCEEDED(m_menuHandler.As(&menu3)) ? SUCCEEDED(menu3->HandleMenuMsg2(msg, wp, lp, &result))
                                                 : SUCCEEDED(m_menuHandler->HandleMenuMsg(msg, wp, lp)))
            return msg == WM_MENUCHAR ? result : (msg == WM_INITMENUPOPUP ? 0 : TRUE);
    }
    const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    switch (msg)
    {
    case WM_SIZE:
        ScrollTo(m_scroll);
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        BufferedPaint paint(m_hwnd);
        Paint(paint.dc(), paint.rect());
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        if (!m_tracking)
        {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, m_hwnd, 0 };
            m_tracking = TrackMouseEvent(&tme) != FALSE;
        }
        // The scroll bar first (dragging it, or the pointer on its strip) unless a row is being held
        const bool held = m_bar.Captured();
        if (m_pressed < 0 && m_chevronPressed < 0 && m_ejectPressed < 0 &&
            (m_bar.OnMouseMove(pt, (wp & MK_LBUTTON) != 0) || held))
        {
            SetHot(-1);
            return 0;
        }
        SetHot(RowAt(pt.y));
        if (const bool ejectHot = m_hot >= 0 && HasEject(m_hot) && [&] { const RECT er = EjectRect(m_hot); return PtInRect(&er, pt) != FALSE; }();
            ejectHot != m_ejectHot)
        {
            m_ejectHot = ejectHot;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            if (m_tooltip)
                SendMessageW(m_tooltip, TTM_UPDATE, 0, 0);
        }
        if (m_pressed >= 0 && !m_rows[m_pressed].header && (wp & MK_LBUTTON) &&
            (abs(pt.x - m_downPt.x) > GetSystemMetrics(SM_CXDRAG) || abs(pt.y - m_downPt.y) > GetSystemMetrics(SM_CYDRAG)))
        {
            const int row = m_pressed;
            m_pressed = -1;
            ReleaseCapture();
            BeginDrag(row);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        m_tracking = false;
        m_bar.OnMouseLeave();
        SetHot(-1);
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    {
        SetFocus(m_hwnd);
        m_keyboardFocus = false;
        if (m_bar.Over(pt))
        {
            m_bar.OnButtonDown(pt);
            return 0;
        }
        const int row = RowAt(pt.y);
        m_downPt = pt;
        // The chevron has its own pointer surface: it toggles without invoking the row (Files' ChevronContainer)
        if (row >= 0 && !m_rows[row].header && Expandable(row))
        {
            const RECT chevron = ChevronRect(row);
            if (PtInRect(&chevron, pt))
            {
                m_chevronPressed = m_focus = row;
                SetCapture(m_hwnd);
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return 0;
            }
            // Folder rows navigate on click; a double click toggles them
            if (msg == WM_LBUTTONDBLCLK)
            {
                m_focus = row;
                SetExpanded(row, !Expanded(row));
                return 0;
            }
        }
        if (row >= 0 && HasEject(row))
            if (const RECT er = EjectRect(row); PtInRect(&er, pt))
            {
                m_ejectPressed = row;
                SetCapture(m_hwnd);
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return 0;
            }
        m_pressed = row;
        if (m_pressed >= 0 && !m_rows[m_pressed].header && !EntryOf(m_pressed))
            m_pressed = -1; // the empty-Pinned hint isn't clickable
        m_focus = m_pressed >= 0 ? m_pressed : m_focus;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        if (m_pressed >= 0)
            SetCapture(m_hwnd);
        return 0;
    }
    case WM_LBUTTONUP:
    {
        if (m_bar.OnButtonUp())
            return 0;
        const int pressed = m_pressed, chevron = m_chevronPressed, eject = m_ejectPressed;
        m_pressed = m_chevronPressed = m_ejectPressed = -1;
        if (GetCapture() == m_hwnd)
            ReleaseCapture();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        if (chevron >= 0)
        {
            if (const RECT cr = ChevronRect(chevron); PtInRect(&cr, pt))
                SetExpanded(chevron, !Expanded(chevron));
        }
        else if (eject >= 0)
        {
            if (const RECT er = EjectRect(eject); PtInRect(&er, pt))
                Eject(eject);
        }
        else if (pressed >= 0 && RowAt(pt.y) == pressed)
            Activate(pressed, (wp & MK_CONTROL) ? OpenMode::NewTab : OpenMode::Here);
        return 0;
    }
    case WM_MBUTTONUP:
        if (const int row = RowAt(pt.y); row >= 0 && !m_rows[row].header)
            Activate(row, OpenMode::NewTab);
        return 0;
    case WM_RBUTTONUP:
    {
        if (m_bar.Over(pt))
            return 0; // the scroll bar's strip has no menu (as in the file list)
        POINT screen = pt;
        ClientToScreen(m_hwnd, &screen);
        const int row = RowAt(pt.y);
        if (EntryOf(row))
            ShowEntryMenu(row, screen);
        else
            ShowSectionMenu(row >= 0 ? m_rows[row].section : -1, screen);
        return 0;
    }
    case WM_CONTEXTMENU: // keyboard (Apps / Shift+F10); right clicks are handled above
    {
        if (m_focus < 0 || m_focus >= static_cast<int>(m_rows.size()))
            return 0;
        const RECT row = RowRect(m_focus);
        POINT screen{ row.left + m_theme->S(24), row.bottom };
        ClientToScreen(m_hwnd, &screen);
        if (EntryOf(m_focus))
            ShowEntryMenu(m_focus, screen);
        else
            ShowSectionMenu(m_rows[m_focus].section, screen);
        return 0;
    }
    case WM_CAPTURECHANGED:
        m_bar.OnCaptureLost();
        m_pressed = m_chevronPressed = m_ejectPressed = -1;
        return 0;
    case WM_MOUSEWHEEL:
    {
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        const int step = m_theme->S(32) * static_cast<int>(lines == WHEEL_PAGESCROLL ? 10 : std::max(1u, lines));
        RECT client{};
        GetClientRect(m_hwnd, &client);
        ScrollTo(m_smooth.OnWheel(m_hwnd, GET_WHEEL_DELTA_WPARAM(wp), step, ContentHeight() - client.bottom));
        POINT clientPt = pt;
        ScreenToClient(m_hwnd, &clientPt);
        SetHot(RowAt(clientPt.y));
        return 0;
    }
    case SmoothFrame::kMsgTick:
        if (m_smooth.Animating())
            ScrollTo(m_smooth.Tick());
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_KEYDOWN:
    {
        const int rows = static_cast<int>(m_rows.size());
        auto focusable = [&](int i) { return m_rows[i].header || EntryOf(i) != nullptr; };
        auto step = [&](int from, int dir)
        {
            for (int i = from + dir; i >= 0 && i < rows; i += dir)
                if (focusable(i))
                    return i;
            return from;
        };
        int next = m_focus;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        switch (wp)
        {
        case VK_UP: next = m_focus < 0 ? step(-1, 1) : step(m_focus, -1); break;
        case VK_DOWN: next = step(m_focus, 1); break;
        case VK_HOME: next = step(-1, 1); break;
        case VK_END: next = step(rows, -1); break;
        case VK_LEFT:
        case VK_RIGHT:
        {
            // Right expands, Left collapses (sections and folder rows); Left on anything else jumps to its parent row
            if (m_focus < 0)
                break;
            const Section section = m_rows[m_focus].section;
            const bool collapsed = (m_collapsed & (1u << section)) != 0;
            if (m_rows[m_focus].header)
            {
                if (collapsed == (wp == VK_RIGHT))
                    ToggleSection(section);
            }
            else if (Expandable(m_focus) && Expanded(m_focus) == (wp == VK_LEFT))
                SetExpanded(m_focus, wp == VK_RIGHT);
            else if (wp == VK_LEFT)
                for (int i = m_focus - 1, depth = m_rows[m_focus].depth; i >= 0; --i)
                    if (m_rows[i].depth < depth && (m_rows[i].header || EntryOf(i)))
                    {
                        next = i;
                        break;
                    }
            break;
        }
        case VK_RETURN:
        case VK_SPACE:
            Activate(m_focus, ctrl ? OpenMode::NewTab : OpenMode::Here);
            break;
        case VK_DELETE:
            if (m_focus >= 0 && m_focus < rows && m_rows[m_focus].entry >= 0 && m_rows[m_focus].section == SecPinned)
                UnpinKey(m_entries[m_rows[m_focus].entry].parsing);
            break;
        default:
            return DefWindowProcW(m_hwnd, msg, wp, lp);
        }
        m_keyboardFocus = true;
        if (next != m_focus && next >= 0)
        {
            m_focus = next;
            EnsureVisible(m_focus);
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        if (msg == WM_SETFOCUS && m_focus < 0)
            m_focus = m_selected;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_TIMER:
        if (wp == kBarTimer)
            m_bar.OnTimer(wp);
        else if (wp == kRefreshTimer)
        {
            KillTimer(m_hwnd, kRefreshTimer);
            if (m_pendingPins)
                LoadPins();
            if (m_pendingDevices)
                ScanDevices();
            m_pendingPins = m_pendingDevices = false;
            for (const std::wstring& key : std::exchange(m_pendingTreeReloads, {}))
                ForEachNode(key, [this](Node& node) { if (node.expanded) LoadChildren(node); });
        }
        else if (wp == kDragScrollTimer && m_dragData)
        {
            POINT cursor;
            GetCursorPos(&cursor);
            ScreenToClient(m_hwnd, &cursor);
            RECT client;
            GetClientRect(m_hwnd, &client);
            const int edge = m_theme->S(24);
            const int delta = cursor.y < edge ? -m_theme->S(12) : cursor.y > client.bottom - edge ? m_theme->S(12) : 0;
            if (delta)
            {
                const int before = m_scroll;
                if (m_dropHelper)
                    m_dropHelper->Show(FALSE);
                ScrollTo(m_scroll + delta);
                if (m_scroll != before)
                {
                    POINT screen = cursor;
                    ClientToScreen(m_hwnd, &screen);
                    DWORD effect = m_allowed;
                    UpdateDrop({ screen.x, screen.y }, m_lastKeys, &effect);
                }
                UpdateWindow(m_hwnd);
                if (m_dropHelper)
                    m_dropHelper->Show(TRUE);
            }
        }
        return 0;
    case WM_NOTIFY:
        if (auto* info = reinterpret_cast<NMTTDISPINFOW*>(lp); info->hdr.hwndFrom == m_tooltip && info->hdr.code == TTN_GETDISPINFOW)
        {
            m_tipText = TipFor(m_hot);
            info->lpszText = m_tipText.empty() ? nullptr : m_tipText.data();
        }
        return 0;
    case kMsgDevices:
        OnDevices(reinterpret_cast<DevicePayload*>(lp));
        return 0;
    case kMsgResolved:
        OnResolved(reinterpret_cast<ResolvePayload*>(lp));
        return 0;
    case kMsgChildren:
        OnChildren(reinterpret_cast<ChildrenPayload*>(lp));
        return 0;
    case kMsgChange:
        OnShellChange(reinterpret_cast<HANDLE>(wp), static_cast<DWORD>(lp));
        return 0;
    case WM_DESTROY:
        KillTimer(m_hwnd, kRefreshTimer);
        KillTimer(m_hwnd, kDragScrollTimer);
        if (m_tooltip)
            DestroyWindow(m_tooltip);
        m_tooltip = nullptr;
        return 0;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}