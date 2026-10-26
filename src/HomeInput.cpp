// Home page input: mouse, keyboard, menus, opening items.
#include "HomeView.h"
#include "Drives.h"
#include "Lang.h"
#include "Sandbox.h"
#include <thread>

struct HomeView::Resolved
{
    UniquePidl pidl;
    OpenMode mode = OpenMode::Here;
    bool reveal = false;
};

namespace
{
enum : UINT { MOpen = 1, MNewTab, MNewWindow, MOtherPane, MPin, MUnpin, MCopyPath, MOpenLocation, MRemoveRecent, MClearRecent,
    MProperties, MToggle, MRefresh, MEject, MFormat, MCopy,
    MShellFirst = 100, MShellLast = 0x7FFF };

// Files' Home widgets replace the "打开方式"/"发送到" placeholders with the shell's own submenu for the item
// (ShellContextFlyoutFactory.LoadShellMenuItemsAsync). FastFiles' Home cards aren't backed by a real IContextMenu the
// way the file list and sidebar are, so build one for just this item and keep only its "openas"/"sendto" child menu
// - the rest of the shell's items (which Files also wouldn't show here) are discarded.
struct ShellSubmenu
{
    ComPtr<IContextMenu> cm; // kept alive until the host menu closes: GetCommandString/InvokeCommand need it
};

// "Open with" is found by verb ("openas"); "Send to" (like Sidebar.cpp's RemoveHiddenShellItems) by its label
// matching shell32's string 30312, because the shell doesn't reliably hand back a "sendto" verb for that submenu.
HMENU ShellChildMenu(const std::wstring& path, const wchar_t* verb, ShellSubmenu& owner)
{
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (path.empty() || FAILED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl)
        return nullptr;
    UniquePidl guard(pidl);
    ComPtr<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;
    if (FAILED(SHBindToParent(pidl, IID_PPV_ARGS(&parent), &child)))
        return nullptr;
    if (FAILED(parent->GetUIObjectOf(nullptr, 1, &child, IID_IContextMenu, nullptr, reinterpret_cast<void**>(owner.cm.GetAddressOf()))))
        return nullptr;
    HMENU host = CreatePopupMenu();
    if (!host || FAILED(owner.cm->QueryContextMenu(host, 0, MShellFirst, MShellLast, CMF_NORMAL | CMF_EXPLORE)))
    {
        if (host)
            DestroyMenu(host);
        owner.cm.Reset();
        return nullptr;
    }
    const bool byLabel = _wcsicmp(verb, L"sendto") == 0;
    wchar_t sendTo[128]{};
    if (byLabel)
        if (HMODULE shell32 = GetModuleHandleW(L"shell32.dll"))
            LoadStringW(shell32, 30312, sendTo, ARRAYSIZE(sendTo));
    auto plain = [](std::wstring s) { s.erase(std::remove(s.begin(), s.end(), L'&'), s.end()); return s; };
    const std::wstring sendToPlain = plain(sendTo);

    HMENU found = nullptr;
    for (int i = 0, n = GetMenuItemCount(host); i < n && !found; ++i)
    {
        // "Open with"/"Send to" are MF_POPUP items that still carry a real command id (the shell's QueryContextMenu
        // sets both MIIM_ID and MIIM_SUBMENU on them); GetMenuItemID always answers -1 for an item with a submenu,
        // so read MIIM_ID (and the text, for the by-label case) directly via MIIM_SUBMENU | MIIM_STRING instead.
        MENUITEMINFOW mii{ sizeof(mii) };
        wchar_t text[128]{};
        mii.fMask = MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
        mii.dwTypeData = text;
        mii.cch = ARRAYSIZE(text);
        if (!GetMenuItemInfoW(host, i, TRUE, &mii) || !mii.hSubMenu)
            continue;
        bool match = false;
        if (byLabel)
            match = !sendToPlain.empty() && plain(text) == sendToPlain;
        else if (mii.wID >= MShellFirst && mii.wID <= MShellLast)
        {
            wchar_t cmdVerb[64]{};
            match = SUCCEEDED(owner.cm->GetCommandString(mii.wID - MShellFirst, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(cmdVerb), ARRAYSIZE(cmdVerb))) &&
                _wcsicmp(cmdVerb, verb) == 0;
        }
        if (match)
        {
            // RemoveMenu (unlike DeleteMenu) detaches the submenu without destroying it, so `menu` becomes sole
            // owner once we attach it there; `host`'s own DestroyMenu below then can't touch it a second time.
            found = mii.hSubMenu;
            RemoveMenu(host, i, MF_BYPOSITION);
        }
    }
    DestroyMenu(host); // only ever held the items we weren't after; `found` (if any) is already detached
    return found;
}

void CopyText(HWND owner, const std::wstring& text)
{
    if (text.empty() || Sandbox::CopyText(text) || !OpenClipboard(owner))
        return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes))
    {
        memcpy(GlobalLock(mem), text.c_str(), bytes);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem))
            GlobalFree(mem);
    }
    CloseClipboard();
}

void LaunchFile(HWND from, const std::wstring& path)
{
    const size_t slash = path.find_last_of(L'\\');
    const std::wstring dir = slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask = SEE_MASK_ASYNCOK;
    sei.hwnd = GetAncestor(from, GA_ROOT);
    sei.lpFile = path.c_str();
    sei.lpDirectory = dir.empty() ? nullptr : dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei); // the shell reports failures itself ("Windows can't find ...")
}

void ShowProperties(HWND owner, PCIDLIST_ABSOLUTE pidl, const std::wstring& path)
{
    ComPtr<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;
    ComPtr<IContextMenu> menu;
    if (pidl && SUCCEEDED(SHBindToParent(pidl, IID_PPV_ARGS(&parent), &child)) &&
        SUCCEEDED(parent->GetUIObjectOf(owner, 1, &child, IID_IContextMenu, nullptr, reinterpret_cast<void**>(menu.GetAddressOf()))))
    {
        HMENU popup = CreatePopupMenu();
        menu->QueryContextMenu(popup, 0, 1, 0x7FFF, CMF_NORMAL);
        CMINVOKECOMMANDINFOEX info{ sizeof(info) };
        info.fMask = CMIC_MASK_UNICODE;
        info.hwnd = owner;
        info.lpVerb = "properties";
        info.lpVerbW = L"properties";
        info.nShow = SW_SHOWNORMAL;
        menu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info));
        DestroyMenu(popup);
    }
    else if (!path.empty())
    {
        SHObjectProperties(owner, SHOP_FILEPATH, path.c_str(), nullptr);
    }
}
}

LRESULT CALLBACK HomeView::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
    {
        auto* self = reinterpret_cast<HomeView*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    }
    auto* self = reinterpret_cast<HomeView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == kMsgResolved && !self)
    {
        delete reinterpret_cast<Resolved*>(lp); // view went away while a lazy path was being parsed
        return 0;
    }
    return self ? self->HandleMessage(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT HomeView::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
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
        Paint(paint.dc(), paint.rect());
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        if (!m_tracking)
        {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, m_hwnd, 0 };
            m_tracking = TrackMouseEvent(&tme) != FALSE;
            InvalidateRect(m_hwnd, nullptr, FALSE); // overlay scrollbar appears
        }
        if (m_thumbDrag)
        {
            RECT client, thumb;
            GetClientRect(m_hwnd, &client);
            if (ThumbRect(thumb))
            {
                const int track = client.bottom - m_theme->S(4) - (thumb.bottom - thumb.top);
                const int offset = track > 0 ? MulDiv(pt.y - m_theme->S(2) - m_thumbGrab, m_content - client.bottom, track) : 0;
                m_smooth.Reset(offset); // dragging the thumb itself: no wheel animation to fight it
                ScrollTo(offset);
            }
            return 0;
        }
        SetHot(CellAt(pt));
        return 0;
    }
    case WM_MOUSELEAVE:
        m_tracking = false;
        SetHot(-1);
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    {
        SetFocus(m_hwnd);
        m_keyboardFocus = false;
        RECT thumb;
        if (msg == WM_LBUTTONDOWN && ThumbRect(thumb) && pt.x >= thumb.right - m_theme->S(10))
        {
            m_thumbDrag = true;
            m_thumbGrab = PtInRect(&thumb, pt) ? pt.y - thumb.top : (thumb.bottom - thumb.top) / 2;
            SetCapture(m_hwnd);
            SendMessageW(m_hwnd, WM_MOUSEMOVE, wp, lp);
            return 0;
        }
        m_pressed = CellAt(pt);
        if (m_pressed >= 0)
        {
            m_focus = m_pressed;
            SetCapture(m_hwnd);
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    {
        const int pressed = m_pressed;
        const bool dragging = m_thumbDrag;
        if (GetCapture() == m_hwnd)
            ReleaseCapture();
        m_pressed = -1;
        m_thumbDrag = false;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        if (!dragging && pressed >= 0 && CellAt(pt) == pressed)
            Activate(pressed, msg == WM_MBUTTONUP || (wp & MK_CONTROL) ? OpenMode::NewTab : OpenMode::Here);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lp) != m_hwnd)
        {
            m_pressed = -1;
            m_thumbDrag = false;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_CONTEXTMENU:
    {
        POINT screen{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int cell = -1;
        if (screen.x == -1 && screen.y == -1)
        {
            // Keyboard (Apps / Shift+F10): menu at the focused card
            cell = m_focus;
            RECT r = cell >= 0 ? ClientRect(cell) : RECT{ m_theme->S(20), m_theme->S(20), 0, 0 };
            screen = { r.left + m_theme->S(16), r.top + m_theme->S(16) };
            ClientToScreen(m_hwnd, &screen);
        }
        else
        {
            POINT client = screen;
            ScreenToClient(m_hwnd, &client);
            cell = CellAt(client);
        }
        ShowMenu(cell, screen);
        return 0;
    }
    case WM_MOUSEWHEEL:
    {
        RECT client{};
        GetClientRect(m_hwnd, &client);
        ScrollTo(m_smooth.OnWheel(m_hwnd, GET_WHEEL_DELTA_WPARAM(wp), m_theme->S(120), m_content - client.bottom));
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
        m_keyboardFocus = true;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        switch (wp)
        {
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_HOME: case VK_END:
            MoveFocus(static_cast<int>(wp));
            return 0;
        case VK_PRIOR: case VK_NEXT:
        {
            RECT client;
            GetClientRect(m_hwnd, &client);
            ScrollTo(m_scroll + (wp == VK_NEXT ? 1 : -1) * (client.bottom - m_theme->S(40)));
            return 0;
        }
        case VK_RETURN: case VK_SPACE:
            if (m_focus >= 0)
                Activate(m_focus, ctrl ? OpenMode::NewTab : OpenMode::Here);
            return 0;
        case VK_DELETE:
            if (const HomeItem* item = ItemAt(m_focus))
            {
                const Section section = m_cells[m_focus].section;
                const std::wstring key = section == SecRecent ? item->link : item->path;
                if (section == SecRecent)
                    m_events->OnHomeAction(HomeAction::RemoveRecent, key, nullptr, section);
                else if (section == SecQuick)
                    m_events->OnHomeAction(HomeAction::Unpin, key, nullptr, section);
            }
            return 0;
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
        break;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_NOTIFY:
        if (auto* info = reinterpret_cast<NMTTDISPINFOW*>(lp); info->hdr.hwndFrom == m_tooltip && info->hdr.code == TTN_GETDISPINFOW)
        {
            m_tipText = TipFor(m_hot);
            info->lpszText = m_tipText.empty() ? nullptr : m_tipText.data();
        }
        return 0;
    case kMsgResolved:
    {
        std::unique_ptr<Resolved> result(reinterpret_cast<Resolved*>(lp));
        if (!result->pidl)
            MessageBeep(MB_ICONWARNING);
        else if (result->reveal)
            m_events->OnHomeReveal(result->pidl.get());
        else
            m_events->OnHomeOpen(result->pidl.get(), result->mode);
        return 0;
    }
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}
void HomeView::MoveFocus(int key)
{
    const int n = static_cast<int>(m_cells.size());
    auto usable = [&](int i) { return i >= 0 && i < n && Interactive(m_cells[i].item); };
    int next = m_focus;
    if (!usable(next))
    {
        next = -1;
        for (int i = 0; i < n && next < 0; ++i)
            if (usable(i))
                next = i;
    }
    else if (key == VK_HOME || key == VK_END)
    {
        for (int i = key == VK_HOME ? 0 : n - 1; i >= 0 && i < n; i += key == VK_HOME ? 1 : -1)
            if (usable(i))
            {
                next = i;
                break;
            }
    }
    else if (key == VK_LEFT || key == VK_RIGHT)
    {
        for (int i = next + (key == VK_RIGHT ? 1 : -1); i >= 0 && i < n; i += key == VK_RIGHT ? 1 : -1)
            if (usable(i))
            {
                next = i;
                break;
            }
    }
    else
    {
        // Up/Down: nearest cell in the next row, preferring the same column
        const RECT& from = m_cells[next].rc;
        const int cx = (from.left + from.right) / 2;
        long long best = -1;
        int pick = next;
        for (int i = 0; i < n; ++i)
        {
            if (!usable(i) || i == next)
                continue;
            const RECT& r = m_cells[i].rc;
            const int dy = key == VK_DOWN ? r.top - from.bottom : from.top - r.bottom;
            if (dy < 0 - m_theme->S(2))
                continue;
            const int dx = r.left > cx ? r.left - cx : r.right < cx ? cx - r.right : 0;
            const long long score = (static_cast<long long>(dy) << 32) + dx;
            if (best < 0 || score < best)
            {
                best = score;
                pick = i;
            }
        }
        next = pick;
    }
    if (next >= 0)
    {
        m_focus = next;
        EnsureVisible(next);
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void HomeView::OpenLazy(const std::wstring& path, OpenMode mode, bool reveal)
{
    // UNC pins and recent files on shares: parse off the UI thread (may wake a server)
    const HWND target = m_hwnd;
    std::thread([target, path, mode, reveal]
    {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SetThreadErrorMode(SEM_FAILCRITICALERRORS, nullptr);
        PIDLIST_ABSOLUTE pidl = nullptr;
        auto result = new Resolved{ UniquePidl(SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) ? pidl : nullptr), mode, reveal };
        if (!PostMessageW(target, kMsgResolved, 0, reinterpret_cast<LPARAM>(result)))
            delete result;
        if (SUCCEEDED(co))
            CoUninitialize();
    }).detach();
}

void HomeView::Activate(int cell, OpenMode mode)
{
    if (cell < 0 || cell >= static_cast<int>(m_cells.size()))
        return;
    const Section section = m_cells[cell].section;
    if (m_cells[cell].item == kHeader)
    {
        m_events->OnHomeAction(HomeAction::ToggleSection, {}, nullptr, section);
        return;
    }
    if (m_cells[cell].item == kTagTitle || m_cells[cell].item == kTagMore)
    {
        const HomeTagGroup* group = GroupAt(cell);
        if (!group)
            return;
        if (m_cells[cell].item == kTagTitle)
        {
            const std::wstring id = group->id; // ViewMoreCommand: the tag's search results
            m_events->OnHomeAction(HomeAction::OpenTag, id, nullptr, mode == OpenMode::Here ? 0 : 1);
        }
        else
        {
            RECT r = ClientRect(cell);
            POINT pt{ r.left, r.bottom };
            ClientToScreen(m_hwnd, &pt);
            ShowTagMenu(cell, pt);
        }
        return;
    }
    const HomeItem* item = ItemAt(cell);
    if (!item)
        return;
    // Copies: opening can rebuild the home data (and free *item) before the callee is done with it
    const UniquePidl pidl = ClonePidl(item->pidl.get());
    const std::wstring path = item->path;
    if (section == SecRecent || (section == SecTags && !item->folder))
    {
        if (mode != OpenMode::Here)
        {
            OpenLazy(path, mode, true); // Ctrl/middle click: go to the containing folder with the file selected
            return;
        }
        LaunchFile(m_hwnd, path);
        return;
    }
    if (pidl)
        m_events->OnHomeOpen(pidl.get(), mode);
    else if (!path.empty())
        OpenLazy(path, mode);
}
// The container's "..." (and right-click on its title): Files' OpenAllTaggedItems
void HomeView::ShowTagMenu(int cell, POINT screen)
{
    const HomeTagGroup* group = GroupAt(cell);
    if (!group)
        return;
    const std::wstring id = group->id;
    const bool any = !group->items.empty();
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (any ? 0 : MF_GRAYED), MOpen, Tr(L"全部打开"));
    m_pressed = cell;
    InvalidateRect(m_hwnd, nullptr, FALSE);
    const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, m_hwnd, nullptr);
    DestroyMenu(menu);
    m_pressed = -1;
    InvalidateRect(m_hwnd, nullptr, FALSE);
    if (cmd == MOpen)
        m_events->OnHomeAction(HomeAction::OpenAllTagged, id, nullptr, SecTags);
}

void HomeView::ShowMenu(int cell, POINT screen)
{
    if (cell >= 0 && cell < static_cast<int>(m_cells.size()) && m_cells[cell].section == SecTags &&
        (m_cells[cell].item == kTagTitle || m_cells[cell].item == kTagMore))
    {
        ShowTagMenu(cell, screen);
        return;
    }
    // Copy everything first: the menu loop pumps messages and a scan may replace m_data meanwhile
    const bool onItem = cell >= 0 && cell < static_cast<int>(m_cells.size()) && m_cells[cell].item >= 0;
    const Section section = cell >= 0 && cell < static_cast<int>(m_cells.size()) ? m_cells[cell].section : SecQuick;
    const HomeItem* item = onItem ? ItemAt(cell) : nullptr;
    const bool taggedFile = item && section == SecTags && !item->folder;
    const UniquePidl pidl = item ? ClonePidl(item->pidl.get()) : nullptr;
    const std::wstring path = item ? item->path : std::wstring(), link = item ? item->link : std::wstring();
    bool pinned = section == SecQuick;
    if (item && !pinned && m_data)
        for (const auto& q : m_data->quick)
            pinned |= _wcsicmp(q.path.c_str(), path.c_str()) == 0;
    const bool shellPath = path.rfind(L"::", 0) == 0;

    ShellSubmenu openWith, sendTo;
    HMENU menu = CreatePopupMenu();
    if (taggedFile)
    {
        // Files' FileTagsWidgetViewModel (a file): 打开方式, 打开文件位置, 发送到 - no 复制路径, no properties
        // (those are folder-only there)
        AppendMenuW(menu, MF_STRING, MOpen, Tr(L"打开"));
        if (HMENU sub = ShellChildMenu(path, L"openas", openWith))
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), Tr(L"打开方式"));
        AppendMenuW(menu, MF_STRING, MOpenLocation, Tr(L"打开文件位置"));
        if (MenuOptions::Shown(MenuOptions::SendTo))
            if (HMENU sub = ShellChildMenu(path, L"sendto", sendTo))
                AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), Tr(L"发送到"));
        SetMenuDefaultItem(menu, MOpen, FALSE);
    }
    else if (item && section == SecTags) // a tagged folder: open, pin/unpin, send to, properties (Files: isFolder)
    {
        AppendMenuW(menu, MF_STRING, MOpen, Tr(L"打开"));
        if (pinned)
            AppendMenuW(menu, MF_STRING, MUnpin, Tr(L"从侧边栏取消固定"));
        else if (pidl && MenuOptions::Shown(MenuOptions::Pin))
            AppendMenuW(menu, MF_STRING, MPin, Tr(L"固定到侧边栏"));
        if (MenuOptions::Shown(MenuOptions::SendTo))
            if (HMENU sub = ShellChildMenu(path, L"sendto", sendTo))
                AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), Tr(L"发送到"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, MProperties, Tr(L"属性"));
        SetMenuDefaultItem(menu, MOpen, FALSE);
    }
    else if (item && section == SecRecent)
    {
        // Files' RecentFilesWidgetViewModel order: 复制, 属性, 打开方式, 删除此项目, 清空所有项目, 打开文件位置,
        // 发送到. Files has no "打开" item here (double click / Enter already open it); kept as FastFiles' own
        // addition at the front since it's a harmless convenience, not a system change (see diff report).
        AppendMenuW(menu, MF_STRING, MOpen, Tr(L"打开"));
        AppendMenuW(menu, MF_STRING, MCopy, Tr(L"复制"));
        AppendMenuW(menu, MF_STRING, MProperties, Tr(L"属性"));
        if (HMENU sub = ShellChildMenu(path, L"openas", openWith))
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), Tr(L"打开方式"));
        AppendMenuW(menu, MF_STRING, MRemoveRecent, Tr(L"删除此项目"));
        AppendMenuW(menu, MF_STRING, MClearRecent, Tr(L"清空所有项目"));
        AppendMenuW(menu, MF_STRING, MOpenLocation, Tr(L"打开文件所在的位置"));
        if (MenuOptions::Shown(MenuOptions::SendTo))
            if (HMENU sub = ShellChildMenu(path, L"sendto", sendTo))
                AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), Tr(L"发送到"));
        SetMenuDefaultItem(menu, MOpen, FALSE);
    }
    else if (item)
    {
        // Files' QuickAccessWidgetViewModel / DrivesWidgetViewModel / NetworkLocationsWidgetViewModel
        if (MenuOptions::Shown(MenuOptions::NewTab))
            AppendMenuW(menu, MF_STRING, MNewTab, Tr(L"在新标签页中打开"));
        if (MenuOptions::Shown(MenuOptions::NewWindow))
            AppendMenuW(menu, MF_STRING, MNewWindow, Tr(L"在新窗口中打开"));
        if (MenuOptions::Shown(MenuOptions::NewPane))
            AppendMenuW(menu, MF_STRING, MOtherPane, Tr(L"在另一个窗格中打开"));
        // 复制 (the object itself, Files' CopyItemFromHome) - Files doesn't have 复制路径 here
        if (!shellPath && !path.empty())
            AppendMenuW(menu, MF_STRING, MCopy, Tr(L"复制"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        if (pidl || !shellPath)
            AppendMenuW(menu, MF_STRING, MProperties, Tr(L"属性"));
        if (pinned && (section == SecQuick || MenuOptions::Shown(MenuOptions::Pin)))
            AppendMenuW(menu, MF_STRING, MUnpin, section == SecQuick ? Tr(L"从快速访问取消固定\tDel") : Tr(L"从侧边栏取消固定"));
        else if (pidl && !pinned && MenuOptions::Shown(MenuOptions::Pin))
            AppendMenuW(menu, MF_STRING, MPin, Tr(L"固定到侧边栏"));
        if (MenuOptions::Shown(MenuOptions::SendTo))
            if (HMENU sub = ShellChildMenu(path, L"sendto", sendTo))
                AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), Tr(L"发送到"));
        // Files' DrivesWidgetViewModel: 弹出 (ShowEjectDevice), then a separator and 格式化 (FormatDriveFromHome)
        if (section == SecDrives && Drives::CanEject(path))
            AppendMenuW(menu, MF_STRING, MEject, Tr(L"弹出"));
        if (section == SecDrives && Drives::CanFormat(path))
        {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, MFormat, Tr(L"格式化"));
        }
        SetMenuDefaultItem(menu, MOpen, FALSE);
    }
    else
    {
        if (cell >= 0)
        {
            const bool collapsed = (m_collapsed & (1u << section)) != 0;
            AppendMenuW(menu, MF_STRING, MToggle, collapsed ? Tr(L"展开") : Tr(L"折叠"));
            if (section == SecRecent)
                AppendMenuW(menu, MF_STRING, MClearRecent, Tr(L"清除最近使用的文件…"));
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        }
        AppendMenuW(menu, MF_STRING, MRefresh, Tr(L"刷新\tF5"));
    }
    m_pressed = onItem ? cell : -1;
    InvalidateRect(m_hwnd, nullptr, FALSE);
    const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, m_hwnd, nullptr);
    DestroyMenu(menu); // also destroys any openWith/sendTo submenu attached above (menu is their sole owner)
    m_pressed = -1;
    InvalidateRect(m_hwnd, nullptr, FALSE);

    auto open = [&](OpenMode mode)
    {
        if (pidl)
            m_events->OnHomeOpen(pidl.get(), mode);
        else if (!path.empty())
            OpenLazy(path, mode);
    };
    // Invoke an "打开方式"/"发送到" pick on whichever IContextMenu owns it (openWith and sendTo build separate
    // Win32ContextMenuItem id ranges on their own IContextMenu, both starting at MShellFirst, so the id alone
    // doesn't say which; GetCommandString succeeds only on the owner).
    if (cmd >= MShellFirst && cmd <= MShellLast)
    {
        IContextMenu* owner = nullptr;
        wchar_t verb[64]{};
        if (openWith.cm && SUCCEEDED(openWith.cm->GetCommandString(cmd - MShellFirst, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(verb), ARRAYSIZE(verb))))
            owner = openWith.cm.Get();
        else if (sendTo.cm && SUCCEEDED(sendTo.cm->GetCommandString(cmd - MShellFirst, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(verb), ARRAYSIZE(verb))))
            owner = sendTo.cm.Get();
        if (owner && Sandbox::VerbAllowed(verb)) // test mode: shell items that would change the system don't run
        {
            CMINVOKECOMMANDINFOEX info{ sizeof(info) };
            info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
            info.hwnd = GetAncestor(m_hwnd, GA_ROOT);
            info.lpVerb = MAKEINTRESOURCEA(cmd - MShellFirst);
            info.lpVerbW = MAKEINTRESOURCEW(cmd - MShellFirst);
            info.nShow = SW_SHOWNORMAL;
            info.ptInvoke = screen;
            owner->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info));
        }
        return;
    }

    switch (cmd)
    {
    case MOpen:
        if (section == SecRecent || taggedFile)
            LaunchFile(m_hwnd, path);
        else
            open(OpenMode::Here);
        break;
    case MCopy: CopyPathsToClipboard(m_hwnd, { path }); break;
    case MNewTab: open(OpenMode::NewTab); break;
    case MNewWindow: open(OpenMode::NewWindow); break;
    case MOtherPane: open(OpenMode::OtherPane); break;
    case MPin: m_events->OnHomeAction(HomeAction::Pin, path, pidl.get(), section); break;
    case MUnpin: m_events->OnHomeAction(HomeAction::Unpin, path, pidl.get(), section); break;
    case MCopyPath: CopyText(m_hwnd, path); break;
    case MOpenLocation: OpenLazy(path, OpenMode::Here, true); break;
    case MRemoveRecent: m_events->OnHomeAction(HomeAction::RemoveRecent, link, nullptr, section); break;
    case MClearRecent: m_events->OnHomeAction(HomeAction::ClearRecent, {}, nullptr, section); break;
    case MProperties: ShowProperties(GetAncestor(m_hwnd, GA_ROOT), pidl.get(), shellPath ? std::wstring() : path); break;
    case MToggle: m_events->OnHomeAction(HomeAction::ToggleSection, {}, nullptr, section); break;
    case MRefresh: m_events->OnHomeAction(HomeAction::Refresh, {}, nullptr, section); break;
    case MEject: Drives::Eject(m_hwnd, path); break;
    case MFormat: Drives::Format(m_hwnd, path); break;
    }
}
