#include "FileList.h"
#include "Lang.h"

// Dropping files into the own list. Files (BaseLayoutPage.Item_DragOver, BaseLayoutViewModel.DragOverAsync): a folder
// row takes the drop into that folder ("移动到 / 复制到 / 在 … 中创建快捷方式 <folder>", Ctrl copies, Shift moves, Alt links,
// another drive copies) and opens after 1.3 s of hovering; anywhere else drops into the folder shown, except items
// that are already there. The effect is the folder's own drop target's (the shell knows drives and zip folders);
// plain copies and moves run in the window's status center, everything else (right drag, links) in that target.

namespace
{
constexpr int kHoverToOpenMs = 1300; // Files' Constants.DragAndDrop.HoverToOpenTimespan

std::vector<std::wstring> DataPathsOf(IDataObject* data)
{
    std::vector<std::wstring> out;
    ComPtr<IShellItemArray> items;
    DWORD count = 0;
    if (!data || FAILED(SHCreateShellItemArrayFromDataObject(data, IID_PPV_ARGS(&items))) || FAILED(items->GetCount(&count)))
        return out;
    for (DWORD i = 0; i < count && i < 4096; ++i)
    {
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        {
            out.emplace_back(path);
            CoTaskMemFree(path);
        }
    }
    return out;
}

std::wstring Parent(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos)
        return {};
    return slash == 2 && path.size() > 1 && path[1] == L':' ? path.substr(0, 3) : path.substr(0, slash);
}

bool Same(const std::wstring& a, const std::wstring& b)
{
    auto trim = [](std::wstring s) { while (s.size() > 3 && s.back() == L'\\') s.pop_back(); return s; };
    return CompareStringOrdinal(trim(a).c_str(), -1, trim(b).c_str(), -1, TRUE) == CSTR_EQUAL;
}

// Files' ContainsDestinationOrAncestor: the folder is one of the dragged items or inside one
bool IntoItself(const std::vector<std::wstring>& dragged, const std::wstring& folder)
{
    for (const std::wstring& d : dragged)
        if (Same(d, folder) || (folder.size() > d.size() && CompareStringOrdinal(folder.c_str(), static_cast<int>(d.size()), d.c_str(),
                static_cast<int>(d.size()), TRUE) == CSTR_EQUAL && folder[d.size()] == L'\\'))
            return true;
    return false;
}
}

IFACEMETHODIMP FileList::DropTarget::QueryInterface(REFIID riid, void** ppv)
{
    if (riid == IID_IUnknown || riid == IID_IDropTarget)
    {
        *ppv = static_cast<IDropTarget*>(this);
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

IFACEMETHODIMP FileList::DropTarget::DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect)
{
    FileList& l = *owner;
    l.m_dragData = data;
    l.m_dragPaths = DataPathsOf(data);
    l.m_dragKeys = keys;
    if (!l.m_dropHelper)
        CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&l.m_dropHelper));
    if (l.m_dropHelper)
    {
        POINT p{ pt.x, pt.y };
        l.m_dropHelper->DragEnter(l.m_list, data, &p, *effect);
    }
    l.UpdateDrop(pt, keys, effect);
    return S_OK;
}

IFACEMETHODIMP FileList::DropTarget::DragOver(DWORD keys, POINTL pt, DWORD* effect)
{
    FileList& l = *owner;
    l.m_dragKeys = keys;
    l.UpdateDrop(pt, keys, effect);
    if (l.m_dropHelper)
    {
        POINT p{ pt.x, pt.y };
        l.m_dropHelper->DragOver(&p, *effect);
    }
    return S_OK;
}

IFACEMETHODIMP FileList::DropTarget::DragLeave()
{
    FileList& l = *owner;
    if (l.m_dropHelper)
        l.m_dropHelper->DragLeave();
    l.ClearDrop();
    l.m_dragData.Reset();
    l.m_dragPaths.clear();
    return S_OK;
}

IFACEMETHODIMP FileList::DropTarget::Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect)
{
    FileList& l = *owner;
    l.UpdateDrop(pt, keys, effect);
    if (l.m_dropHelper)
    {
        POINT p{ pt.x, pt.y };
        l.m_dropHelper->Drop(data, &p, *effect);
    }
    if (l.m_target && *effect != DROPEFFECT_NONE)
    {
        if (l.m_targetPidl && RouteFileDrop(l.m_list, data, l.m_targetPidl.get(), effect, (l.m_dragKeys & MK_RBUTTON) != 0))
            l.m_target->DragLeave();
        else
            l.m_target->Drop(data, keys, pt, effect);
        l.m_target.Reset(); // consumed by Drop (no DragLeave)
    }
    else
        *effect = DROPEFFECT_NONE;
    l.ClearDrop();
    l.m_dragData.Reset();
    l.m_dragPaths.clear();
    return S_OK;
}

void FileList::ClearDrop()
{
    if (m_target)
        m_target->DragLeave();
    m_target.Reset();
    m_targetRow = -2;
    m_targetPidl.reset();
    KillTimer(m_hwnd, kDragOpenTimer);
    SetDropRow(-1);
}

void FileList::SetDropRow(int row)
{
    if (row == m_dropRow)
        return;
    InvalidateRow(m_dropRow);
    m_dropRow = row;
    InvalidateRow(m_dropRow);
}

void FileList::UpdateDrop(POINTL screen, DWORD keys, DWORD* effect)
{
    const DWORD allowed = *effect;
    POINT pt{ screen.x, screen.y };
    ScreenToClient(m_list, &pt);
    // Near the top / bottom edge the list scrolls (as the shell's view does)
    RECT client{};
    GetClientRect(m_list, &client);
    const int edge = m_theme ? m_theme->S(24) : 24;
    if (pt.y >= 0 && pt.y < edge)
        ScrollTo(m_scroll.x, m_scroll.y - edge);
    else if (pt.y > client.bottom - edge && pt.y <= client.bottom)
        ScrollTo(m_scroll.x, m_scroll.y + edge);
    int row = RowAt(pt);
    if (row >= Count())
        row = -1;
    const std::wstring dir = m_path.empty() || m_path.back() == L'\\' ? m_path : m_path + L'\\';
    // A folder row (not one of the dragged items) is the target; any other spot is the folder shown
    if (row >= 0)
    {
        const Item& item = m_items[m_order[row]];
        if (!(item.attrs & FILE_ATTRIBUTE_DIRECTORY) || IntoItself(m_dragPaths, dir + item.name))
            row = -1;
    }
    const std::wstring folder = row >= 0 ? dir + m_items[m_order[row]].name : m_path;
    bool refused = m_dragPaths.empty() || IntoItself(m_dragPaths, folder);
    if (row < 0 && !refused) // into the folder shown: not when every item is in it already
        refused = std::all_of(m_dragPaths.begin(), m_dragPaths.end(), [&](const std::wstring& p) { return Same(Parent(p), m_path); });
    if (refused)
    {
        ClearDrop();
        *effect = DROPEFFECT_NONE;
        SetDropDescription(m_dragData.Get(), DROPIMAGE_INVALID, L"", L"");
        return;
    }
    if (row != m_targetRow)
    {
        ClearDrop();
        m_targetRow = row;
        if (row >= 0)
        {
            PCUITEMID_CHILD child = ChildOf(m_order[row]);
            if (child && m_shellFolder)
                m_shellFolder->GetUIObjectOf(m_hwnd, 1, &child, IID_IDropTarget, nullptr, reinterpret_cast<void**>(m_target.GetAddressOf()));
            if (child)
                m_targetPidl.reset(ILCombine(m_folder.get(), reinterpret_cast<PCUIDLIST_RELATIVE>(child)));
        }
        else if (m_folder)
        {
            ComPtr<IShellFolder> parent;
            PCUITEMID_CHILD self = nullptr;
            if (SUCCEEDED(SHBindToParent(m_folder.get(), IID_PPV_ARGS(&parent), &self)))
                parent->GetUIObjectOf(m_hwnd, 1, &self, IID_IDropTarget, nullptr, reinterpret_cast<void**>(m_target.GetAddressOf()));
            m_targetPidl = ClonePidl(m_folder.get());
        }
        *effect = allowed;
        if (m_target && FAILED(m_target->DragEnter(m_dragData.Get(), keys, screen, effect)))
            m_target.Reset();
        if (row >= 0 && m_target)
            SetTimer(m_hwnd, kDragOpenTimer, kHoverToOpenMs, nullptr); // Files: a folder row opens after resting on it
    }
    else if (m_target)
    {
        *effect = allowed;
        m_target->DragOver(keys, screen, effect);
    }
    if (!m_target)
        *effect = DROPEFFECT_NONE;
    SetDropRow(m_target ? row : -1);
    const std::wstring name = row >= 0 ? m_items[m_order[row]].display : std::wstring(PathFindFileNameW(m_path.c_str()));
    if (*effect & DROPEFFECT_MOVE)
        SetDropDescription(m_dragData.Get(), DROPIMAGE_MOVE, Tr(L"移动到 %1"), name);
    else if (*effect & DROPEFFECT_COPY)
        SetDropDescription(m_dragData.Get(), DROPIMAGE_COPY, Tr(L"复制到 %1"), name);
    else if (*effect & DROPEFFECT_LINK)
        SetDropDescription(m_dragData.Get(), DROPIMAGE_LINK, Tr(L"在 %1 中创建快捷方式"), name);
    else
        SetDropDescription(m_dragData.Get(), DROPIMAGE_INVALID, L"", L"");
}

// Files' DragOverTimer: the folder row the drag rests on opens (the drag goes on in it)
void FileList::OpenDropRow()
{
    KillTimer(m_hwnd, kDragOpenTimer);
    if (m_targetRow < 0 || m_targetRow >= Count() || !m_targetPidl)
        return;
    const UniquePidl folder = ClonePidl(m_targetPidl.get());
    ClearDrop();
    m_events->OnListOpen(folder.get());
}
