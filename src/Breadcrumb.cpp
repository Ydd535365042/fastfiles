#include "Breadcrumb.h"
#include "Lang.h"

namespace
{
constexpr wchar_t kClass[] = L"FastFiles.Breadcrumb";
constexpr wchar_t kChevron[] = L"";  // ChevronRight (MDL2)
constexpr wchar_t kEllipsis[] = L""; // More (MDL2)
constexpr int kPad = 6, kChevronW = 16, kOverflowW = 24;
}

bool Breadcrumb::Create(HWND parent, int id, BreadcrumbEvents* events)
{
    m_events = events;
    static const bool registered = []
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        return RegisterClassExW(&wc) != 0;
    }();
    if (!registered)
        return false;
    m_hwnd = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent, MenuId(id), GetModuleHandleW(nullptr), this);
    if (!m_hwnd)
        return false;
    m_dropTarget.owner = this;
    CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_dropHelper));
    RegisterDragDrop(m_hwnd, &m_dropTarget);
    return true;
}

void Breadcrumb::SetLocation(PCIDLIST_ABSOLUTE pidl)
{
    ClearDrop(); // a drag in progress re-targets on its next DragOver
    m_segments.clear();
    m_hot = {};
    for (UniquePidl cur = ClonePidl(pidl); cur && !ILIsEmpty(cur.get());)
    {
        Segment seg;
        seg.name = LocationName(cur.get());
        seg.pidl = ClonePidl(cur.get());
        m_segments.push_back(std::move(seg));
        if (!ILRemoveLastID(cur.get()))
            break;
    }
    std::reverse(m_segments.begin(), m_segments.end());
    m_icon = IsSettingsPidl(pidl) ? -2 : pidl && IsHomePidl(pidl) ? -3 : PidlIcon(pidl);
    Measure();
    Arrange();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void Breadcrumb::SetTheme(const Theme* theme)
{
    m_theme = theme;
    if (!m_images)
    {
        SHFILEINFOW sfi{};
        m_images = reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(L".txt", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
            SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON));
    }
    Measure();
    Arrange();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void Breadcrumb::Measure()
{
    if (!m_theme)
        return;
    HDC hdc = GetDC(m_hwnd);
    for (auto& seg : m_segments)
        seg.textWidth = TextWidth(hdc, m_theme->ui, seg.name);
    ReleaseDC(m_hwnd, hdc);
}

void Breadcrumb::Arrange()
{
    if (!m_theme)
        return;
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const Theme& t = *m_theme;
    const int pad = t.S(kPad), chevron = t.S(kChevronW), overflow = t.S(kOverflowW);
    const int inset = t.S(3);
    const int icon = GetSystemMetrics(SM_CXSMICON);
    m_iconRect = { t.S(6), 0, t.S(6) + icon, rc.bottom };
    const int start = m_iconRect.right + t.S(4);
    const int avail = rc.right - start - pad;
    const int n = static_cast<int>(m_segments.size());
    auto segWidth = [&](int i) { return m_segments[i].textWidth + pad * 2 + chevron; };

    // Keep as many trailing segments as fit; always keep the current folder
    int used = 0;
    m_first = n;
    while (m_first > 0)
    {
        const int w = segWidth(m_first - 1) + (m_first - 1 > 0 ? overflow : 0);
        if (m_first < n && used + w > avail)
            break;
        used += segWidth(--m_first);
    }

    int x = start;
    m_overflow = {};
    if (m_first > 0)
    {
        m_overflow = { x, inset, x + overflow, rc.bottom - inset };
        x += overflow;
    }
    for (int i = 0; i < n; ++i)
    {
        auto& seg = m_segments[i];
        if (i < m_first)
        {
            seg.text = seg.chevron = {};
            continue;
        }
        seg.text = { x, inset, x + seg.textWidth + pad * 2, rc.bottom - inset };
        x = seg.text.right;
        seg.chevron = { x, inset, x + chevron, rc.bottom - inset };
        x = seg.chevron.right;
    }
}

void Breadcrumb::Paint(HDC hdc)
{
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    if (!m_theme)
        return;
    const Theme& t = *m_theme;
    FillSolid(hdc, rc, t.field);
    if (m_icon == -2) // Settings: its glyph, like the tab
        DrawGlyph(hdc, t.icons, Glyph::Settings, { m_iconRect.left, 0, m_iconRect.right, rc.bottom }, t.ink);
    else if (m_icon == -3) // Home: Files' GetIconForPathAsync uses the Home sidebar icon
    {
        const int size = GetSystemMetrics(SM_CXSMICON), top = (rc.bottom - size) / 2;
        DrawSectionIcon(hdc, 0, { m_iconRect.left, top, m_iconRect.left + size, top + size }, t.dpi);
    }
    else if (m_images && m_icon >= 0)
    {
        const int size = GetSystemMetrics(SM_CXSMICON);
        ImageList_Draw(m_images, m_icon, hdc, m_iconRect.left, (rc.bottom - size) / 2, ILD_TRANSPARENT);
    }
    auto hot = [&](Part part, int index) { return m_hot.part == part && m_hot.index == index; };
    if (m_first > 0)
    {
        if (hot(Part::Overflow, -1))
            FillRound(hdc, m_overflow, t.S(4), t.hover);
        DrawGlyph(hdc, t.iconsSmall, kEllipsis, m_overflow, t.muted);
    }
    const int last = static_cast<int>(m_segments.size()) - 1;
    for (int i = m_first; i <= last; ++i)
    {
        const auto& seg = m_segments[i];
        if (hot(Part::Text, i) || (i == m_dropIndex && m_itemTarget))
            FillRound(hdc, seg.text, t.S(4), t.hover);
        DrawLabel(hdc, t.ui, seg.name, seg.text, i == last ? t.ink : Mix(t.ink, t.muted, 45), DT_CENTER);
        if (hot(Part::Chevron, i))
            FillRound(hdc, seg.chevron, t.S(4), t.hover);
        DrawGlyph(hdc, t.iconsSmall, kChevron, seg.chevron, t.muted);
    }
}
Breadcrumb::Hit Breadcrumb::HitTest(POINT pt) const
{
    if (m_first > 0 && PtInRect(&m_overflow, pt))
        return { Part::Overflow, -1 };
    for (int i = m_first; i < static_cast<int>(m_segments.size()); ++i)
    {
        if (PtInRect(&m_segments[i].text, pt))
            return { Part::Text, i };
        if (PtInRect(&m_segments[i].chevron, pt))
            return { Part::Chevron, i };
    }
    return { Part::Blank, -1 };
}

void Breadcrumb::Navigate(PCIDLIST_ABSOLUTE pidl, bool newTab)
{
    // The owner may rebuild our segments in response; never touch m_segments after this call
    m_events->OnCrumbNavigate(pidl, newTab);
}

void Breadcrumb::ShowChildren(int index)
{
    ComPtr<IShellFolder> folder;
    if (FAILED(SHBindToObject(nullptr, m_segments[index].pidl.get(), nullptr, IID_PPV_ARGS(&folder))))
        return;
    std::vector<std::pair<std::wstring, UniquePidl>> children;
    ComPtr<IEnumIDList> items;
    if (folder->EnumObjects(m_hwnd, SHCONTF_FOLDERS | SHCONTF_NAVIGATION_ENUM, &items) == S_OK)
    {
        PITEMID_CHILD child = nullptr;
        while (children.size() < 500 && items->Next(1, &child, nullptr) == S_OK)
        {
            SFGAOF attrs = SFGAO_FOLDER | SFGAO_STREAM | SFGAO_HIDDEN;
            PCUITEMID_CHILD one = child;
            folder->GetAttributesOf(1, &one, &attrs);
            if ((attrs & SFGAO_FOLDER) && !(attrs & SFGAO_STREAM) && !(attrs & SFGAO_HIDDEN))
            {
                PIDLIST_ABSOLUTE full = ILCombine(m_segments[index].pidl.get(), child);
                children.emplace_back(PidlName(full, SIGDN_NORMALDISPLAY), UniquePidl(full));
            }
            CoTaskMemFree(child);
        }
    }
    std::sort(children.begin(), children.end(), [](const auto& a, const auto& b) { return StrCmpLogicalW(a.first.c_str(), b.first.c_str()) < 0; });

    HMENU menu = CreatePopupMenu();
    for (size_t i = 0; i < children.size(); ++i)
    {
        std::wstring label = children[i].first;
        for (size_t p = label.find(L'&'); p != std::wstring::npos; p = label.find(L'&', p + 2))
            label.insert(p, 1, L'&');
        AppendMenuW(menu, MF_STRING, i + 1, label.c_str());
    }
    if (children.empty())
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, Tr(L"没有子文件夹"));
    POINT pt{ m_segments[index].chevron.left, m_segments[index].chevron.bottom };
    ClientToScreen(m_hwnd, &pt);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd > 0)
        Navigate(children[cmd - 1].second.get(), false);
}

void Breadcrumb::ShowOverflow()
{
    HMENU menu = CreatePopupMenu();
    for (int i = m_first - 1; i >= 0; --i)
        AppendMenuW(menu, MF_STRING, i + 1, m_segments[i].name.c_str());
    POINT pt{ m_overflow.left, m_overflow.bottom };
    ClientToScreen(m_hwnd, &pt);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd > 0)
    {
        UniquePidl target = ClonePidl(m_segments[cmd - 1].pidl.get());
        Navigate(target.get(), false);
    }
}
// --- Drop target ---

IFACEMETHODIMP Breadcrumb::DropTarget::QueryInterface(REFIID riid, void** ppv)
{
    if (riid == IID_IUnknown || riid == IID_IDropTarget)
    {
        *ppv = static_cast<IDropTarget*>(this);
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

IFACEMETHODIMP Breadcrumb::DropTarget::DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect)
{
    Breadcrumb& b = *owner;
    b.m_dragData = data;
    b.m_dragItems.clear();
    ComPtr<IShellItemArray> items;
    DWORD count = 0;
    if (SUCCEEDED(SHCreateShellItemArrayFromDataObject(data, IID_PPV_ARGS(&items))) && SUCCEEDED(items->GetCount(&count)))
        for (DWORD i = 0; i < count && i < 256; ++i)
        {
            ComPtr<IShellItem> item;
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(SHGetIDListFromObject(item.Get(), &pidl)))
                b.m_dragItems.emplace_back(pidl);
        }
    const DWORD allowed = *effect;
    b.m_dragKeys = keys;
    if (b.m_dropHelper)
    {
        POINT p{ pt.x, pt.y };
        b.m_dropHelper->DragEnter(b.m_hwnd, data, &p, allowed);
    }
    b.UpdateDrop(pt, keys, effect);
    return S_OK;
}

IFACEMETHODIMP Breadcrumb::DropTarget::DragOver(DWORD keys, POINTL pt, DWORD* effect)
{
    owner->m_dragKeys = keys;
    owner->UpdateDrop(pt, keys, effect);
    if (owner->m_dropHelper)
    {
        POINT p{ pt.x, pt.y };
        owner->m_dropHelper->DragOver(&p, *effect);
    }
    return S_OK;
}

IFACEMETHODIMP Breadcrumb::DropTarget::DragLeave()
{
    Breadcrumb& b = *owner;
    if (b.m_dropHelper)
        b.m_dropHelper->DragLeave();
    b.ClearDrop();
    b.m_dragData.Reset();
    b.m_dragItems.clear();
    return S_OK;
}

IFACEMETHODIMP Breadcrumb::DropTarget::Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect)
{
    Breadcrumb& b = *owner;
    b.UpdateDrop(pt, keys, effect);
    if (b.m_dropHelper)
    {
        POINT p{ pt.x, pt.y };
        b.m_dropHelper->Drop(data, &p, *effect);
    }
    if (ComPtr<IDropTarget> target = std::move(b.m_itemTarget))
    {
        // Files' PathBoxItem_Drop: copy / move through the status center (right drags keep the shell's menu)
        if (b.m_dropIndex >= 0 && b.m_dropIndex < static_cast<int>(b.m_segments.size()) &&
            RouteFileDrop(b.m_hwnd, data, b.m_segments[b.m_dropIndex].pidl.get(), effect, (b.m_dragKeys & MK_RBUTTON) != 0))
            target->DragLeave();
        else
            target->Drop(data, keys, pt, effect); // consumes the target: no DragLeave
    }
    else
        *effect = DROPEFFECT_NONE;
    b.ClearDrop();
    b.m_dragData.Reset();
    b.m_dragItems.clear();
    return S_OK;
}

void Breadcrumb::ClearDrop()
{
    KillTimer(m_hwnd, kHoverTimer);
    m_hoverIndex = -1;
    if (m_itemTarget)
        m_itemTarget->DragLeave();
    m_itemTarget.Reset();
    if (m_dropIndex >= 0)
        InvalidateRect(m_hwnd, nullptr, FALSE);
    m_dropIndex = -1;
}

// Files' PathBoxItem_DragOver: refused when the items are already in that folder or the folder is one of them (or
// inside one of them)
bool Breadcrumb::CanDropOn(int index) const
{
    PCIDLIST_ABSOLUTE folder = m_segments[index].pidl.get();
    if (IsPagePidl(folder) || m_dragItems.empty())
        return false;
    bool anyElsewhere = false;
    for (const auto& item : m_dragItems)
    {
        if (ILIsParent(item.get(), folder, FALSE)) // the folder is the item or below it
            return false;
        if (!ILIsParent(folder, item.get(), TRUE)) // not already directly in the folder
            anyElsewhere = true;
    }
    return anyElsewhere;
}

void Breadcrumb::UpdateDrop(POINTL screen, DWORD keys, DWORD* effect)
{
    POINT pt{ screen.x, screen.y };
    ScreenToClient(m_hwnd, &pt);
    const Hit hit = HitTest(pt);
    const int index = hit.part == Part::Text || hit.part == Part::Chevron ? hit.index : -1;

    // Hover to open: any segment but the current folder
    if (index != m_hoverIndex)
    {
        KillTimer(m_hwnd, kHoverTimer);
        m_hoverIndex = index >= 0 && index + 1 < static_cast<int>(m_segments.size()) && !IsPagePidl(m_segments[index].pidl.get()) ? index : -1;
        if (m_hoverIndex >= 0)
            SetTimer(m_hwnd, kHoverTimer, kHoverMs, nullptr);
    }

    const bool can = index >= 0 && CanDropOn(index);
    if (!can || index != m_dropIndex)
    {
        if (m_itemTarget)
            m_itemTarget->DragLeave();
        m_itemTarget.Reset();
        if (m_dropIndex != (can ? index : -1))
            InvalidateRect(m_hwnd, nullptr, FALSE);
        m_dropIndex = -1;
        if (can)
        {
            // The folder's own drop target: the same move/copy/link rules (and Shift/Ctrl/Alt) as Explorer
            ComPtr<IShellFolder> parent;
            PCUITEMID_CHILD child = nullptr;
            if (SUCCEEDED(SHBindToParent(m_segments[index].pidl.get(), IID_PPV_ARGS(&parent), &child)))
                parent->GetUIObjectOf(m_hwnd, 1, &child, IID_IDropTarget, nullptr, reinterpret_cast<void**>(m_itemTarget.GetAddressOf()));
            m_dropIndex = index;
            if (m_itemTarget && FAILED(m_itemTarget->DragEnter(m_dragData.Get(), keys, screen, effect)))
                m_itemTarget.Reset();
        }
    }
    else if (m_itemTarget)
        m_itemTarget->DragOver(keys, screen, effect);

    if (!m_itemTarget)
    {
        *effect = DROPEFFECT_NONE;
        SetDropDescription(m_dragData.Get(), DROPIMAGE_INVALID, L"", L"");
        return;
    }
    const std::wstring& name = m_segments[m_dropIndex].name;
    if (*effect & DROPEFFECT_MOVE)
        SetDropDescription(m_dragData.Get(), DROPIMAGE_MOVE, Tr(L"移动到 %1"), name);
    else if (*effect & DROPEFFECT_COPY)
        SetDropDescription(m_dragData.Get(), DROPIMAGE_COPY, Tr(L"复制到 %1"), name);
    else if (*effect & DROPEFFECT_LINK)
        SetDropDescription(m_dragData.Get(), DROPIMAGE_LINK, Tr(L"在 %1 中创建快捷方式"), name);
    else
        SetDropDescription(m_dragData.Get(), DROPIMAGE_INVALID, L"", L"");
}

LRESULT CALLBACK Breadcrumb::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
    {
        auto* created = static_cast<Breadcrumb*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        created->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
    }
    auto* self = reinterpret_cast<Breadcrumb*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self && self->m_hwnd == hwnd ? self->HandleMessage(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Breadcrumb::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_TIMER:
        if (wp == kHoverTimer)
        {
            // Hovered long enough while dragging: open that folder (Files' ItemDraggedOverPathItem)
            KillTimer(m_hwnd, kHoverTimer);
            const int index = m_hoverIndex;
            m_hoverIndex = -1;
            if (index >= 0 && index + 1 < static_cast<int>(m_segments.size()))
            {
                UniquePidl target = ClonePidl(m_segments[index].pidl.get());
                Navigate(target.get(), false);
            }
            return 0;
        }
        break;
    case WM_DESTROY:
        RevokeDragDrop(m_hwnd);
        break;
    case WM_SIZE:
        Arrange();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        BufferedPaint paint(m_hwnd);
        Paint(paint.dc());
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        if (!m_tracking)
        {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, m_hwnd, 0 };
            m_tracking = TrackMouseEvent(&tme) != FALSE;
        }
        const Hit hit = HitTest({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
        if (hit.part != m_hot.part || hit.index != m_hot.index)
        {
            m_hot = hit;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        m_tracking = false;
        m_hot = {};
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
    {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(m_hwnd, &pt);
        SetCursor(LoadCursorW(nullptr, HitTest(pt).part == Part::Blank ? IDC_IBEAM : IDC_HAND));
        return TRUE;
    }
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    {
        const Hit hit = HitTest({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
        const bool middle = msg == WM_MBUTTONUP;
        if (hit.part == Part::Text)
        {
            UniquePidl target = ClonePidl(m_segments[hit.index].pidl.get());
            Navigate(target.get(), middle);
        }
        else if (middle)
            return 0;
        else if (hit.part == Part::Chevron)
            ShowChildren(hit.index);
        else if (hit.part == Part::Overflow)
            ShowOverflow();
        else if (hit.part == Part::Blank)
            m_events->OnCrumbEdit();
        return 0;
    }
    case WM_RBUTTONUP:
    {
        // Files' BreadcrumbBarItem_RightTapped: open the segment in a new tab / window / pane
        const Hit hit = HitTest({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
        if (hit.part != Part::Text || IsPagePidl(m_segments[hit.index].pidl.get()))
            return 0;
        UniquePidl target = ClonePidl(m_segments[hit.index].pidl.get());
        POINT screen{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ClientToScreen(m_hwnd, &screen);
        m_events->OnCrumbMenu(target.get(), screen);
        return 0;
    }
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}
