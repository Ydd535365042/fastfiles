// Sidebar context menus and drag and drop.
#include "Sidebar.h"
#include "Drives.h"
#include "Lang.h"
#include "Sandbox.h"
#include "Tags.h"
#include "fastfiles_core.h"

namespace
{
enum : UINT { MenuNewTab = 1, MenuNewWindow, MenuPin, MenuUnpin, MenuCopyPath, MenuCollapse, MenuRefresh, MenuOtherPane, MenuEject, MenuFormat,
    MenuCopy,
    MenuSectionBase = 50, MenuShellFirst = 100, MenuShellLast = 0x7FFF };

bool IsSeparator(HMENU menu, int index)
{
    MENUITEMINFOW mii{ sizeof(mii), MIIM_FTYPE };
    return GetMenuItemInfoW(menu, index, TRUE, &mii) && (mii.fType & MFT_SEPARATOR);
}

// Files' sidebar menu has its own open commands; drop the shell's duplicates
void RemoveOpenVerbs(HMENU menu, IContextMenu* cm)
{
    for (int i = GetMenuItemCount(menu) - 1; i >= 0; --i)
    {
        const UINT id = GetMenuItemID(menu, i);
        if (id < MenuShellFirst || id > MenuShellLast)
            continue;
        wchar_t verb[64]{};
        if (FAILED(cm->GetCommandString(id - MenuShellFirst, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(verb), 64)))
            continue;
        for (const wchar_t* v : { L"open", L"explore", L"opennewwindow", L"opennewprocess", L"opennewtab" })
            if (_wcsicmp(verb, v) == 0)
            {
                DeleteMenu(menu, i, MF_BYPOSITION);
                break;
            }
    }
}
} // namespace

void RemoveShellVerbs(HMENU menu, IContextMenu* cm, UINT first, UINT last, std::initializer_list<const wchar_t*> verbs)
{
    if (!menu || !cm)
        return;
    for (int i = GetMenuItemCount(menu) - 1; i >= 0; --i)
    {
        const UINT id = GetMenuItemID(menu, i);
        if (id == static_cast<UINT>(-1) || id < first || id > last)
            continue;
        wchar_t verb[64]{};
        if (FAILED(cm->GetCommandString(id - first, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(verb), 64)))
            continue;
        for (const wchar_t* v : verbs)
            if (_wcsicmp(verb, v) == 0)
            {
                DeleteMenu(menu, i, MF_BYPOSITION);
                break;
            }
    }
}

void RemoveHiddenShellItems(HMENU menu, IContextMenu* cm, UINT first, UINT last)
{
    using namespace MenuOptions;
    if (!menu || !cm || (Shown(SendTo) && Shown(Shortcut) && Shown(PinStart)))
        return;
    // Files' ShellContextFlyoutHelper: the Send to submenu is the popup labelled with shell32's string 30312
    auto plain = [](std::wstring s)
    {
        s.erase(std::remove(s.begin(), s.end(), L'&'), s.end());
        return s;
    };
    wchar_t sendTo[128]{};
    if (HMODULE shell32 = Shown(SendTo) ? nullptr : GetModuleHandleW(L"shell32.dll"))
        LoadStringW(shell32, 30312, sendTo, ARRAYSIZE(sendTo));
    const std::wstring sendToPlain = plain(sendTo);
    for (int i = GetMenuItemCount(menu) - 1; i >= 0; --i)
    {
        MENUITEMINFOW mii{ sizeof(mii) };
        wchar_t text[128]{};
        mii.fMask = MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
        mii.dwTypeData = text;
        mii.cch = ARRAYSIZE(text);
        if (!GetMenuItemInfoW(menu, i, TRUE, &mii))
            continue;
        if (mii.hSubMenu)
        {
            if (!sendToPlain.empty() && plain(text) == sendToPlain)
                DeleteMenu(menu, i, MF_BYPOSITION); // destroys the submenu too
            continue;
        }
        if (mii.wID < first || mii.wID > last)
            continue;
        wchar_t verb[64]{};
        if (FAILED(cm->GetCommandString(mii.wID - first, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(verb), ARRAYSIZE(verb))))
            continue;
        if ((!Shown(Shortcut) && _wcsicmp(verb, L"link") == 0) || (!Shown(PinStart) && _wcsicmp(verb, L"pintostartscreen") == 0))
            DeleteMenu(menu, i, MF_BYPOSITION);
    }
}

namespace
{
void TidySeparators(HMENU menu)
{
    bool last = true;
    for (int i = 0; i < GetMenuItemCount(menu);)
    {
        const bool sep = IsSeparator(menu, i);
        if (sep && last)
        {
            DeleteMenu(menu, i, MF_BYPOSITION);
            continue;
        }
        last = sep;
        ++i;
    }
    if (const int n = GetMenuItemCount(menu); n > 0 && IsSeparator(menu, n - 1))
        DeleteMenu(menu, n - 1, MF_BYPOSITION);
}
void CopyText(HWND owner, const std::wstring& text)
{
    if (Sandbox::CopyText(text) || !OpenClipboard(owner))
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

ComPtr<IShellItemArray> DraggedItems(IDataObject* data)
{
    ComPtr<IShellItemArray> items;
    if (data)
        SHCreateShellItemArrayFromDataObject(data, IID_PPV_ARGS(&items));
    return items;
}

// True when every dragged item is a real folder (zip files report SFGAO_FOLDER too, but also SFGAO_STREAM)
bool AllFolders(IDataObject* data)
{
    const auto items = DraggedItems(data);
    DWORD count = 0;
    if (!items || FAILED(items->GetCount(&count)) || count == 0 || count > 64)
        return false;
    SFGAOF all = 0, any = 0;
    return SUCCEEDED(items->GetAttributes(SIATTRIBFLAGS_AND, SFGAO_FOLDER, &all)) && (all & SFGAO_FOLDER) &&
        SUCCEEDED(items->GetAttributes(SIATTRIBFLAGS_OR, SFGAO_STREAM, &any)) && !(any & SFGAO_STREAM);
}

std::vector<std::wstring> DraggedKeys(IDataObject* data)
{
    std::vector<std::wstring> keys;
    const auto items = DraggedItems(data);
    DWORD count = 0;
    if (!items || FAILED(items->GetCount(&count)))
        return keys;
    for (DWORD i = 0; i < count; ++i)
    {
        ComPtr<IShellItem> item;
        PWSTR name = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &name)))
        {
            keys.emplace_back(name);
            CoTaskMemFree(name);
        }
    }
    return keys;
}

void SetDescription(IDataObject* data, DROPIMAGETYPE type, const wchar_t* message, const std::wstring& insert)
{
    SetDropDescription(data, type, message, insert);
}
}

// The caption under the drag image ("固定到侧边栏", "移动到 下载"); shown by Explorer-style drag sources
void SetDropDescription(IDataObject* data, DROPIMAGETYPE type, const wchar_t* message, const std::wstring& insert)
{
    static const CLIPFORMAT format = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_DROPDESCRIPTION));
    if (!data)
        return;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPDESCRIPTION));
    if (!mem)
        return;
    auto* desc = static_cast<DROPDESCRIPTION*>(GlobalLock(mem));
    desc->type = type;
    wcsncpy_s(desc->szMessage, message, _TRUNCATE);
    wcsncpy_s(desc->szInsert, insert.c_str(), _TRUNCATE);
    GlobalUnlock(mem);
    FORMATETC fmt{ format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium{ TYMED_HGLOBAL };
    medium.hGlobal = mem;
    if (FAILED(data->SetData(&fmt, &medium, TRUE)))
        GlobalFree(mem);
}

// --- Context menus ---

// 在新标签页/新窗口/另一个窗格中打开, as far as Files' 右键菜单选项 show them
void Sidebar::AppendOpenItems(HMENU menu)
{
    if (MenuOptions::Shown(MenuOptions::NewTab))
        AppendMenuW(menu, MF_STRING, MenuNewTab, Tr(L"在新标签页中打开"));
    if (MenuOptions::Shown(MenuOptions::NewWindow))
        AppendMenuW(menu, MF_STRING, MenuNewWindow, Tr(L"在新窗口中打开"));
    if (MenuOptions::Shown(MenuOptions::NewPane))
        AppendMenuW(menu, MF_STRING, MenuOtherPane, Tr(L"在另一个窗格中打开"));
}

void Sidebar::ShowTagMenu(int row, POINT screen)
{
    const Entry& e = m_entries[m_rows[row].entry];
    const std::wstring id = e.tagId, name = e.name;
    m_pressed = row;
    InvalidateRect(m_hwnd, nullptr, FALSE);
    // Files: a tag is a location item (open in new tab / new window / other pane)
    HMENU menu = CreatePopupMenu();
    AppendOpenItems(menu);
    if (GetMenuItemCount(menu) == 0)
    {
        DestroyMenu(menu);
        m_pressed = -1;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }
    const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, m_hwnd, nullptr);
    DestroyMenu(menu);
    m_pressed = -1;
    InvalidateRect(m_hwnd, nullptr, FALSE);
    if (cmd == MenuNewTab)
        m_events->OnSidebarTag(id, name, OpenMode::NewTab);
    else if (cmd == MenuNewWindow)
        m_events->OnSidebarTag(id, name, OpenMode::NewWindow);
    else if (cmd == MenuOtherPane)
        m_events->OnSidebarTag(id, name, OpenMode::OtherPane);
}

void Sidebar::ShowEntryMenu(int row, POINT screen)
{
    Entry* entry = EntryOf(row);
    if (!entry)
        return;
    if (!entry->tagId.empty())
    {
        ShowTagMenu(row, screen);
        return;
    }
    if (m_rows[row].child)
        EnsurePidl(*entry);
    // Copy what we need: the menu loop pumps messages and a device scan may rebuild m_entries meanwhile
    const Entry& e = *entry;
    const UniquePidl pidl = ClonePidl(e.pidl.get());
    const std::wstring parsing = e.parsing, lazyPath = e.lazyPath, name = e.name;
    const Section section = e.section;
    const bool pinned = section == SecPinned || (pidl && IsPinned(pidl.get()));
    m_pressed = row;
    InvalidateRect(m_hwnd, nullptr, FALSE);

    // Files' SidebarViewModel.GetLocationItemMenuItems: 复制 (CopyItemFromSidebar, the item itself, not its path
    // text) comes right after the open-in group, same as 在新标签页/窗口/面板中打开. Not gated by a 右键菜单选项
    // setting there (only IsExecutable), and not shown for tag rows, Home, or non-filesystem shell places.
    const bool canCopy = pidl && section != SecHome && section != SecTags && parsing.rfind(L"::", 0) != 0;
    HMENU menu = CreatePopupMenu();
    AppendOpenItems(menu);
    if (canCopy)
        AppendMenuW(menu, MF_STRING, MenuCopy, Tr(L"复制"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (pinned)
        AppendMenuW(menu, MF_STRING, MenuUnpin, Tr(L"从侧边栏取消固定"));
    else if (pidl && section != SecHome)
        AppendMenuW(menu, MF_STRING, MenuPin, Tr(L"固定到侧边栏"));
    if (parsing.rfind(L"::", 0) != 0)
        AppendMenuW(menu, MF_STRING, MenuCopyPath, Tr(L"复制路径"));
    // Files' SidebarViewModel: 弹出 (ShowEjectDevice), then a separator and 格式化 (FormatDriveFromSidebar)
    const std::wstring drivePath = pidl ? PidlName(pidl.get(), SIGDN_FILESYSPATH) : std::wstring();
    if (Drives::CanEject(drivePath))
        AppendMenuW(menu, MF_STRING, MenuEject, Tr(L"弹出"));
    if (Drives::CanFormat(drivePath))
    {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, MenuFormat, Tr(L"格式化"));
    }

    // The item's real shell menu below ours: Pin to Quick access, Send to, Properties, extensions (its own eject and
    // format go: ours are above)
    ComPtr<IContextMenu> shellMenu;
    ComPtr<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;
    if (pidl && SUCCEEDED(SHBindToParent(pidl.get(), IID_PPV_ARGS(&parent), &child)) &&
        SUCCEEDED(parent->GetUIObjectOf(m_hwnd, 1, &child, IID_IContextMenu, nullptr, reinterpret_cast<void**>(shellMenu.GetAddressOf()))))
    {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        const UINT flags = CMF_NORMAL | CMF_EXPLORE | ((GetKeyState(VK_SHIFT) & 0x8000) ? CMF_EXTENDEDVERBS : 0);
        if (SUCCEEDED(shellMenu->QueryContextMenu(menu, GetMenuItemCount(menu), MenuShellFirst, MenuShellLast, flags)))
        {
            RemoveOpenVerbs(menu, shellMenu.Get());
            RemoveHiddenShellItems(menu, shellMenu.Get(), MenuShellFirst, MenuShellLast);
            RemoveShellVerbs(menu, shellMenu.Get(), MenuShellFirst, MenuShellLast, { L"eject", L"format" });
        }
        shellMenu.As(&m_menuHandler);
    }
    TidySeparators(menu);
    const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, m_hwnd, nullptr);
    m_menuHandler.Reset();
    DestroyMenu(menu);
    m_pressed = -1;
    InvalidateRect(m_hwnd, nullptr, FALSE);

    auto open = [&](OpenMode mode)
    {
        if (pidl)
            m_events->OnSidebarOpen(pidl.get(), mode);
        else if (!lazyPath.empty())
            OpenLazy(name, lazyPath, mode);
    };
    switch (cmd)
    {
    case 0: break;
    case MenuNewTab: open(OpenMode::NewTab); break;
    case MenuNewWindow: open(OpenMode::NewWindow); break;
    case MenuOtherPane: open(OpenMode::OtherPane); break;
    case MenuPin:
        Pin(pidl.get());
        break;
    case MenuUnpin:
        UnpinKey(parsing);
        break;
    case MenuCopyPath:
    {
        std::wstring path = pidl ? PidlName(pidl.get(), SIGDN_FILESYSPATH) : std::wstring();
        CopyText(m_hwnd, path.empty() ? parsing : path);
        break;
    }
    case MenuCopy:
    {
        // Files' CopyItemFromSidebar: the object itself (falls back to the path as text when that's not possible)
        std::wstring path = pidl ? PidlName(pidl.get(), SIGDN_FILESYSPATH) : std::wstring();
        CopyPathsToClipboard(m_hwnd, { path.empty() ? parsing : path });
        break;
    }
    case MenuEject: Drives::Eject(m_hwnd, drivePath); break;
    case MenuFormat: Drives::Format(m_hwnd, drivePath); break;
    default:
        wchar_t verb[64]{};
        if (shellMenu && cmd >= MenuShellFirst &&
            FAILED(shellMenu->GetCommandString(cmd - MenuShellFirst, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(verb), ARRAYSIZE(verb))))
            verb[0] = L'\0';
        if (shellMenu && cmd >= MenuShellFirst && Sandbox::VerbAllowed(verb)) // test mode: no system-changing shell items
        {
            CMINVOKECOMMANDINFOEX info{ sizeof(info) };
            info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE |
                ((GetKeyState(VK_CONTROL) & 0x8000) ? CMIC_MASK_CONTROL_DOWN : 0) | ((GetKeyState(VK_SHIFT) & 0x8000) ? CMIC_MASK_SHIFT_DOWN : 0);
            info.hwnd = GetAncestor(m_hwnd, GA_ROOT);
            info.lpVerb = MAKEINTRESOURCEA(cmd - MenuShellFirst);
            info.lpVerbW = MAKEINTRESOURCEW(cmd - MenuShellFirst);
            info.nShow = SW_SHOWNORMAL;
            info.ptInvoke = screen;
            shellMenu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info));
        }
        break;
    }
}

void Sidebar::ShowSectionMenu(int section, POINT screen)
{
    HMENU menu = CreatePopupMenu();
    if (section > SecHome && section < SecCount)
    {
        const bool collapsed = (m_collapsed & (1u << section)) != 0;
        AppendMenuW(menu, MF_STRING, MenuCollapse, collapsed ? Tr(L"展开此分区") : Tr(L"折叠此分区"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    HMENU show = CreatePopupMenu();
    for (int s = 0; s < SecCount; ++s)
        AppendMenuW(show, MF_STRING | ((m_hidden & (1u << s)) ? 0 : MF_CHECKED) | (s == SecPinned ? MF_GRAYED : 0),
            MenuSectionBase + s, SectionName(s));
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(show), Tr(L"显示分区"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, MenuRefresh, Tr(L"刷新"));
    const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, m_hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd == MenuCollapse)
        ToggleSection(static_cast<Section>(section));
    else if (const int s = static_cast<int>(cmd) - MenuSectionBase; cmd >= MenuSectionBase && s < SecCount && s != SecPinned)
    {
        m_hidden ^= 1u << s;
        Rebuild();
        MatchLocation();
    }
    else if (cmd == MenuRefresh)
    {
        LoadPins();
        LoadTags();
        ScanDevices();
    }
}

// --- Drag source: any entry can be dragged out (to the file view, other apps); pinned ones also reorder ---

void Sidebar::BeginDrag(int row)
{
    Entry* entry = EntryOf(row);
    if (!entry)
        return;
    if (m_rows[row].child)
        EnsurePidl(*entry);
    const Entry& e = *entry;
    ComPtr<IShellItem> item;
    ComPtr<IDataObject> data;
    if (!e.pidl || FAILED(SHCreateItemFromIDList(e.pidl.get(), IID_PPV_ARGS(&item))) ||
        FAILED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data))))
        return;
    m_dragRow = row;
    m_dragKey = e.parsing;
    m_dragPinned = e.section == SecPinned;
    DWORD effect = DROPEFFECT_NONE;
    // Copy/link only: a pinned Desktop must never be moved by a slip of the mouse
    SHDoDragDrop(m_hwnd, data.Get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
    m_dragRow = -1;
    m_dragKey.clear();
    m_dragPinned = false;
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

// --- Drop target ---

IFACEMETHODIMP Sidebar::DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect)
{
    m_dragData = data;
    m_allowed = *effect;
    m_lastKeys = keys;
    m_dragFolders = AllFolders(data);
    m_dropMode = DropMode::None;
    if (m_dropHelper)
    {
        POINT p{ pt.x, pt.y };
        m_dropHelper->DragEnter(m_hwnd, data, &p, *effect);
    }
    SetTimer(m_hwnd, kDragScrollTimer, 60, nullptr);
    UpdateDrop(pt, keys, effect);
    return S_OK;
}

IFACEMETHODIMP Sidebar::DragOver(DWORD keys, POINTL pt, DWORD* effect)
{
    m_allowed = *effect;
    m_lastKeys = keys;
    UpdateDrop(pt, keys, effect);
    if (m_dropHelper)
    {
        POINT p{ pt.x, pt.y };
        m_dropHelper->DragOver(&p, *effect);
    }
    return S_OK;
}

IFACEMETHODIMP Sidebar::DragLeave()
{
    KillTimer(m_hwnd, kDragScrollTimer);
    if (m_dropHelper)
        m_dropHelper->DragLeave();
    ClearDropTarget();
    m_dragData.Reset();
    m_dropMode = DropMode::None;
    m_insertAt = -1;
    m_dropRow = -1;
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return S_OK;
}

void Sidebar::ClearDropTarget()
{
    if (m_itemTarget)
        m_itemTarget->DragLeave();
    m_itemTarget.Reset();
    m_itemTargetRow = -1;
}

// Pinned section: insert between rows (or on the header/hint = end/start). Other entries: forward to the item's own
// IDropTarget so dropping on a drive or folder copies/moves exactly like Explorer's navigation pane.
void Sidebar::UpdateDrop(POINTL screen, DWORD keys, DWORD* effect)
{
    POINT pt{ screen.x, screen.y };
    ScreenToClient(m_hwnd, &pt);
    const int row = RowAt(pt.y);
    const Row* r = row >= 0 ? &m_rows[row] : nullptr;
    const bool internal = m_dragRow >= 0 && m_dragPinned;
    const int from = internal ? DragOrdinal() : -1;
    int insert = -1;
    bool onItem = false;
    if (r && r->section == SecPinned && (internal || m_dragFolders))
    {
        if (r->header)
            insert = PinnedCount();
        else if (r->entry < 0) // the hint (Pinned has no tree rows)
            insert = 0;
        else
        {
            const int rel = pt.y + m_scroll - r->top, h = r->height, ordinal = PinnedOrdinal(r->entry);
            if (internal)
                insert = rel < h / 2 ? ordinal : ordinal + 1;
            else if (rel < h / 4)
                insert = ordinal;
            else if (rel >= h * 3 / 4)
                insert = ordinal + 1;
            else
                onItem = true;
        }
        if (internal && (insert == from || insert == from + 1))
            insert = -1; // would land where it already is
    }
    Entry* target = EntryOf(row);
    if (target && r->child)
        EnsurePidl(*target);
    const bool pinBranch = r && r->section == SecPinned && (internal || m_dragFolders);
    if (!pinBranch && target)
        onItem = !(m_dragRow >= 0 && target->parsing == m_dragKey);

    const DWORD allowed = *effect;
    const bool onTag = target && r->section == SecTags && m_dragRow < 0;
    if (onTag)
    {
        // Dropping files on a tag tags them (nothing is moved or copied)
        ClearDropTarget();
        m_dropMode = DropMode::Tag;
        *effect = (allowed & DROPEFFECT_LINK) ? DROPEFFECT_LINK : (allowed & DROPEFFECT_COPY) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        SetDescription(m_dragData.Get(), DROPIMAGE_LABEL, Tr(L"添加标签 %1"), target->name);
    }
    else if (insert >= 0)
    {
        ClearDropTarget();
        m_dropMode = DropMode::Pin;
        *effect = (allowed & DROPEFFECT_LINK) ? DROPEFFECT_LINK : (allowed & DROPEFFECT_COPY) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        SetDescription(m_dragData.Get(), DROPIMAGE_LINK, internal ? Tr(L"移到这里") : Tr(L"固定到侧边栏"), L"");
    }
    else if (onItem && target->pidl)
    {
        m_dropMode = DropMode::Item;
        POINTL p = screen;
        if (row != m_itemTargetRow)
        {
            ClearDropTarget();
            ComPtr<IShellFolder> parent;
            PCUITEMID_CHILD child = nullptr;
            if (SUCCEEDED(SHBindToParent(target->pidl.get(), IID_PPV_ARGS(&parent), &child)))
                parent->GetUIObjectOf(m_hwnd, 1, &child, IID_IDropTarget, nullptr, reinterpret_cast<void**>(m_itemTarget.GetAddressOf()));
            m_itemTargetRow = row;
            if (m_itemTarget && FAILED(m_itemTarget->DragEnter(m_dragData.Get(), keys, p, effect)))
                m_itemTarget.Reset();
        }
        else if (m_itemTarget)
            m_itemTarget->DragOver(keys, p, effect);
        if (!m_itemTarget)
            *effect = DROPEFFECT_NONE;
        const std::wstring& name = target->name;
        if (*effect & DROPEFFECT_MOVE)
            SetDescription(m_dragData.Get(), DROPIMAGE_MOVE, Tr(L"移动到 %1"), name);
        else if (*effect & DROPEFFECT_COPY)
            SetDescription(m_dragData.Get(), DROPIMAGE_COPY, Tr(L"复制到 %1"), name);
        else if (*effect & DROPEFFECT_LINK)
            SetDescription(m_dragData.Get(), DROPIMAGE_LINK, Tr(L"在 %1 中创建快捷方式"), name);
        else
            SetDescription(m_dragData.Get(), DROPIMAGE_INVALID, L"", L"");
    }
    else
    {
        ClearDropTarget();
        m_dropMode = DropMode::None;
        *effect = DROPEFFECT_NONE;
        SetDescription(m_dragData.Get(), DROPIMAGE_INVALID, L"", L"");
    }

    // Insertion line: top of the insert-th pinned row, bottom of the last one, or under the header when empty
    int insertY = 0;
    if (insert >= 0)
    {
        int seen = 0, lastBottom = -1;
        for (const Row& pr : m_rows)
        {
            if (pr.section != SecPinned)
                continue;
            if (pr.header)
            {
                insertY = pr.top + pr.height; // empty Pinned: right under its header
                continue;
            }
            if (pr.entry < 0)
            {
                insertY = pr.top + pr.height / 2;
                break;
            }
            if (seen++ == insert)
            {
                insertY = pr.top;
                break;
            }
            lastBottom = pr.top + pr.height;
            insertY = lastBottom;
        }
    }
    const int dropRow = (m_dropMode == DropMode::Item && m_itemTarget) || m_dropMode == DropMode::Tag ? row : -1;
    if (insert != m_insertAt || insertY != m_insertY || dropRow != m_dropRow)
    {
        m_insertAt = insert;
        m_insertY = insertY;
        m_dropRow = dropRow;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

IFACEMETHODIMP Sidebar::Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect)
{
    KillTimer(m_hwnd, kDragScrollTimer);
    UpdateDrop(pt, keys, effect);
    if (m_dropHelper)
    {
        POINT p{ pt.x, pt.y };
        m_dropHelper->Drop(data, &p, *effect);
    }
    if (m_dropMode == DropMode::Pin && m_insertAt >= 0)
    {
        if (m_dragRow >= 0 && m_dragPinned)
        {
            const int from = DragOrdinal();
            if (from >= 0)
                MovePin(from, m_insertAt > from ? m_insertAt - 1 : m_insertAt);
        }
        else
            PinKeys(DraggedKeys(data), m_insertAt);
    }
    else if (m_dropMode == DropMode::Item && m_itemTarget)
    {
        // Files' SidebarViewModel drop: copy / move through the status center; anything else (right drag, links,
        // virtual items) stays with the folder's own drop target
        const Entry* target = EntryOf(m_itemTargetRow);
        if (target && target->pidl && RouteFileDrop(m_hwnd, data, target->pidl.get(), effect, (m_lastKeys & MK_RBUTTON) != 0))
            m_itemTarget->DragLeave();
        else
            m_itemTarget->Drop(data, keys, pt, effect);
        m_itemTarget.Reset(); // consumed by Drop; no DragLeave
        m_itemTargetRow = -1;
    }
    else if (m_dropMode == DropMode::Tag && m_dropRow >= 0 && m_rows[m_dropRow].entry >= 0)
    {
        const Entry& e = m_entries[m_rows[m_dropRow].entry];
        std::vector<std::wstring> paths;
        for (auto& key : DraggedKeys(data))
            if (key.size() >= 2 && (key[1] == L':' || key.rfind(L"\\\\", 0) == 0)) // file system items only
                paths.push_back(std::move(key));
        if (!paths.empty() && Tags::Set(paths, e.tagId, true))
            Tags::Broadcast();
        *effect = DROPEFFECT_LINK;
    }
    else
        *effect = DROPEFFECT_NONE;
    ClearDropTarget();
    m_dragData.Reset();
    m_dropMode = DropMode::None;
    m_insertAt = -1;
    m_dropRow = -1;
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return S_OK;
}